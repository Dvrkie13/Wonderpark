/* The free camera (F1, Controls: Free camera, Camera speed, Stick speed,
 * Invert left / right and up / down, Recenter after) and the field of view
 * (Display: Field of view).
 *
 * Adapted from MildRain479/PokePark-2-Wiicompiled's
 * recomp/patches/S2LE01/hle_camera.c (CC0): its design, addresses and
 * offsets, our hooks (docs/camera-and-extras.md checked the addresses
 * against our build). How it works, in its words, shortened:
 *
 * The camera that follows the player (vtable 0x80434938, update 0x80024EE0)
 * keeps its direction as Euler angles in degrees, and each update works the
 * yaw out from where the eye was, so that it trails behind. On the Wii
 * nothing turns it. The mouse (or right stick) turns the yaw, and swings the
 * eye around the point followed by the same angle, so the game goes on from
 * there. The pitch is the game's own, eased toward a target
 * (0x80024C50), so the tilt is an offset carried on the stored value only
 * while the eye is placed: the game sees its own value when it eases it.
 * Once turned, the eye is brought in to the first wall between it and the
 * point looked at, and kept above the ground; that too only while the frame
 * is drawn: the game goes on from its own eye, or it would take the
 * shortened distance as its own. B (which looks around in the park) also
 * eases the camera back behind the player, by the game's own easing of the
 * yaw.
 *
 * Field of view: the world's cameras are widened while a camera is applied
 * to the scene (0x80019B20, the fovy in f1) and while its frustum is worked
 * out for culling (0x8001A390, the fovy at +0x3C), so what's culled matches
 * what's seen. Only cameras with a perspective projection (+0x38 0) and a
 * far plane (+0x44) of 5000 or more, and not in minigames.
 *
 * WONDERPARK_MOUSE=<dx>,<dy>[,<updates>] (tests): that many mouse pixels
 * every update (only for that many updates, when given). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "host_input.h"
#include "host_settings.h"

int hle_in_minigame(void);         /* hle_scene.c */
int hle_in_park(void);             /* hle_scene.c */
unsigned hle_scene_changes(void);  /* hle_scene.c */
extern uint32_t g_wpad_held;       /* hle_wpad.c */

static float ldf(u32 a) {
    union { float f; u32 u; } x;
    x.u = LD32(a);
    return x.f;
}

static void stf(u32 a, float v) {
    union { float f; u32 u; } x;
    x.f = v;
    ST32(a, x.u);
}

/* ---- Field of view */

enum { CAM_PROJECTION = 0x38, CAM_FOVY = 0x3C, CAM_FAR = 0x44 };
#define WORLD_FAR 5000.0f
#define APPLY_TO_G3D 0x80019B20u
#define FRUSTUM_FOR_CAMERA 0x8001A390u

static float fov_scale(void) { return g_settings.fov > 0 ? g_settings.fov / 100.0f : 1.0f; }

static int widens(u32 cam, float fovy) {
    return fov_scale() != 1.0f && (!hle_in_minigame() || g_x[X_FOV_ATTRACTIONS]) && wp_mem_ptr(cam, CAM_FAR + 4) &&
           LD32(cam + CAM_PROJECTION) == 0 && ldf(cam + CAM_FAR) >= WORLD_FAR && fovy > 1.0f && fovy < 170.0f;
}

static float widened(float fovy) {
    float w = atanf(tanf(fovy * 3.14159265f / 360.0f) * fov_scale()) * 360.0f / 3.14159265f;
    return w < 170.0f ? w : 170.0f;
}

/* void Camera::ApplyToG3dCamera(Camera*, g3d::Camera*, ..., f32 fovy, ...) */
WP_HOOK(0x80019B20, Camera_ApplyToG3dCamera) {
    if (widens(c->gpr[3], (float)c->fpr[1])) c->fpr[1] = c->ps1[1] = widened((float)c->fpr[1]);
    return 0;
}

/* void FrustumForCamera(const Camera*, Frustum*, f32 far) */
WP_HOOK(0x8001A390, FrustumForCamera) {
    u32 cam = c->gpr[3], own;
    float fovy;
    if (!wp_mem_ptr(cam, CAM_FAR + 4)) return 0;
    own = LD32(cam + CAM_FOVY);
    fovy = ldf(cam + CAM_FOVY);
    if (!widens(cam, fovy)) return 0;
    stf(cam + CAM_FOVY, widened(fovy));
    wp_call_original(c, FRUSTUM_FOR_CAMERA);
    ST32(cam + CAM_FOVY, own);
    return 1;
}

/* ---- The free camera */

enum {
    CAM_ID = 0x04,
    CAM_EYE = 0x0C,
    CAM_AT = 0x28,
    CAM_PITCH = 0xF0,   /* degrees */
    CAM_YAW = 0xF4,     /* degrees, -180 to 180 */
    CAM_YAW_TO = 0x100, /* where the yaw eases to while CAM_EASING counts down */
    CAM_EASING = 0x10C,
    CAM_FOCUS = 0x110,      /* the point followed */
    MANAGER_CURRENT = 0x58 /* the id of the camera on screen */
};
#define FOLLOW_UPDATE 0x80024EE0u
#define EASE_PITCH 0x80024C50u
#define COLLISION_GET 0x80076EE0u
#define COLLISION_RAY 0x8012DD80u

#define PI 3.14159265f
#define MOUSE_DEGREES 0.3f /* degrees per mouse pixel, at Camera speed 100% */
/* A tilt swings the eye up or down around the player, who stays in the
 * middle of the screen: the view moves less than for a turn of the same
 * angle, and at the same rate the tilt felt slow. */
#define TILT_DEGREES (MOUSE_DEGREES * 1.5f)
#define MIN_TILT -20.0f
#define MAX_TILT 45.0f
#define RECENTER_UPDATES 30
#define WALL_MARGIN 0.9f    /* of the way to a wall */
#define WALL_WIDTH 5.0f     /* to each side of the eye: a wide view reaches past it */
#define WALL_RISE 5.0f      /* the rays start this far above the point looked at, off the ground */
#define WALL_NEAREST 0.4f   /* of the way to the eye, whatever is there */
#define WALL_EASE_IN 0.08f  /* of the distance to where a wall allows, each 60th of a second */
#define WALL_EASE_OUT 0.01f /* of the way to the eye, each 60th of a second, once clear */
#define GROUND_CLEARANCE 4.0f

static float sTurn, sTilt;        /* degrees: ours so far */
static float sYawStep, sTiltStep; /* this update's */
static u32 sCurrent;              /* the id of the camera on screen */
static int sRecenter;
static u32 sGamePitch, sOurPitch; /* as stored (float bits) */
static u32 sTilted;               /* the camera carrying the tilt */
static u32 sLogged;
static float sPull = 1;            /* how far toward the eye it is placed */
static u32 sPlaced;                /* the camera carrying a placed eye */
static u32 sGameEye[3], sOurEye[3]; /* as stored (float bits) */
static uint64_t sPlacedAt;
static double sLastInput; /* wall seconds of the last turn, for Recenter after */
double wp_wall_seconds(void);

/* bool CollisionManager::Ray(this, const VEC3* from, VEC3* hit, const VEC3* along, mask, BOOL, 0, 0),
 * with the mask and flag the camera's own checks use. How far along the ray
 * the first wall is, or 1 with none. The vectors go in guest memory just
 * below the stack pointer, above where the call's own frames start
 * (wp_call_guest8 leaves 0x100 bytes). */
static float wall_along(CPUState* c, const float from[3], const float along[3]) {
    u32 scratch = (c->gpr[1] - 0x80) & ~7u, args[8], mgr;
    int i, found;
    float hit[3], dot = 0, len = 0;
    for (i = 0; i < 3; i++) {
        stf(scratch + i * 4, from[i]);
        stf(scratch + 12 + i * 4, from[i]);
        stf(scratch + 24 + i * 4, along[i]);
    }
    memset(args, 0, sizeof(args));
    mgr = wp_call_guest8(c, COLLISION_GET, args);
    if (!mgr) return 1;
    args[0] = mgr;
    args[1] = scratch;
    args[2] = scratch + 12;
    args[3] = scratch + 24;
    args[4] = 0x0C;
    args[5] = 1;
    found = wp_call_guest8(c, COLLISION_RAY, args) == 1;
    for (i = 0; i < 3; i++) hit[i] = ldf(scratch + 12 + i * 4);
    if (!found) return 1;
    for (i = 0; i < 3; i++) {
        dot += (hit[i] - from[i]) * along[i];
        len += along[i] * along[i];
    }
    return len > 0 && dot < len ? (dot > 0 ? dot / len : 0) : 1;
}

/* WONDERPARK_MOUSE's updates left: counted where a turn is applied (the
 * follow camera's update), so none are spent while it isn't on screen. */
static int sTestLeft = -1;

static void read_turn(float* dx, float* dy) {
    static int env = -1;
    static float ex, ey;
    if (env < 0) {
        const char* e = getenv("WONDERPARK_MOUSE");
        env = e && sscanf(e, "%f,%f,%d", &ex, &ey, &sTestLeft) >= 2;
    }
    *dx = *dy = 0;
    if (env && sTestLeft != 0) {
        *dx = ex;
        *dy = ey;
    } else if (g_host_mouse_read) {
        g_host_mouse_read(dx, dy); /* sensitivity and inverting already applied (input_win32.c) */
    }
}

/* void CameraManager::Update(this): every update, before the cameras' own. */
WP_HOOK(0x8001EFD0, CameraManager_Update) {
    static uint32_t lastHeld;
    static u32 lastCurrent;
    static unsigned lastScene;
    float dx, dy, tilt;
    sCurrent = LD32(c->gpr[3] + MANAGER_CURRENT);
    if (sCurrent != lastCurrent || hle_scene_changes() != lastScene) {
        /* Another camera, or another scene: start over behind the player. */
        WP_LOG("camera", "camera %u on screen", sCurrent);
        lastCurrent = sCurrent;
        lastScene = hle_scene_changes();
        sTurn = sTilt = 0;
        sPull = 1;
    }
    read_turn(&dx, &dy);
    if (!g_x[X_FREE_CAMERA]) dx = dy = 0;
    {
        /* Camera smoothing: the turn eases toward the input instead of
         * following it at once (0%: at once; 90%: a tenth of the way each
         * 60th of a second). */
        static float sx, sy;
        float keep = g_x[X_CAMERA_SMOOTHING] / 100.0f;
        if (keep <= 0) {
            sx = sy = 0;
        } else {
            /* What's still to come of earlier input, plus the new. */
            sx += dx, sy += dy;
            dx = sx * (1 - keep), dy = sy * (1 - keep);
            sx -= dx, sy -= dy;
            if (fabsf(sx) < 0.01f) sx = 0;
            if (fabsf(sy) < 0.01f) sy = 0;
        }
    }
    if (dx != 0 || dy != 0) sLastInput = wp_wall_seconds();
    if (g_wpad_held & ~lastHeld & HOST_PAD_B) sRecenter = 1;
    lastHeld = g_wpad_held;
    if (g_x[X_CAMERA_RECENTER] > 0 && (sTurn != 0 || sTilt != 0) &&
        wp_wall_seconds() - sLastInput > g_x[X_CAMERA_RECENTER]) {
        sRecenter = 1;
        sLastInput = wp_wall_seconds();
    }
    if (!g_x[X_FREE_CAMERA] && (sTurn != 0 || sTilt != 0)) sRecenter = 1; /* turned off: back behind */
    sYawStep = -dx * MOUSE_DEGREES;
    tilt = sTilt + dy * TILT_DEGREES; /* moving the mouse down raises the eye */
    tilt = tilt < MIN_TILT ? MIN_TILT : tilt > MAX_TILT ? MAX_TILT : tilt;
    sTiltStep = tilt - sTilt;
    return 0;
}

static float wrap180(float a) { return a >= 180.0f ? a - 360.0f : a < -180.0f ? a + 360.0f : a; }

/* Swings the eye around the point followed. */
static void swing_eye(u32 cam, float deg) {
    float r = deg * PI / 180.0f, s = sinf(r), co = cosf(r);
    float fx = ldf(cam + CAM_FOCUS), fz = ldf(cam + CAM_FOCUS + 8);
    float x = ldf(cam + CAM_EYE) - fx, z = ldf(cam + CAM_EYE + 8) - fz;
    stf(cam + CAM_EYE, fx + x * co + z * s);
    stf(cam + CAM_EYE + 8, fz + z * co - x * s);
}

/* Takes the tilt off the stored pitch, if it is still the one put there. */
static void untilt(u32 cam) {
    if (cam == sTilted && LD32(cam + CAM_PITCH) == sOurPitch) ST32(cam + CAM_PITCH, sGamePitch);
}

/* void fn_80024C50(this, ...): eases the pitch. The update has no other need
 * of the game's own value after this. */
WP_HOOK(0x80024C50, CameraFollow_EasePitch) {
    u32 cam = c->gpr[3];
    wp_call_original(c, EASE_PITCH);
    if (sTilt == 0 || !wp_mem_ptr(cam, 0x120) || LD32(cam + CAM_ID) != sCurrent || hle_in_minigame()) return 1;
    sTilted = cam;
    sGamePitch = LD32(cam + CAM_PITCH);
    stf(cam + CAM_PITCH, ldf(cam + CAM_PITCH) + sTilt);
    sOurPitch = LD32(cam + CAM_PITCH);
    return 1;
}

/* Puts the game's eye back, if it is still the one put there. */
static void unplace(u32 cam) {
    int i;
    if (cam != sPlaced) return;
    sPlaced = 0;
    for (i = 0; i < 3; i++)
        if (LD32(cam + CAM_EYE + i * 4) != sOurEye[i]) return;
    for (i = 0; i < 3; i++) ST32(cam + CAM_EYE + i * 4, sGameEye[i]);
}

/* How far in a wall allows, of the way to the eye: 1 with none, past each
 * side of the eye as well as to it. Only what is level with the rays' start
 * or above counts: the ground is seen to on its own. */
static float wall_limit(CPUState* c, const float at[3], const float along[3]) {
    float from[3], ray[3], side[3], t, nearest = 1;
    float len = sqrtf(along[0] * along[0] + along[2] * along[2]);
    int i, s;
    if (len < 0.001f) return 1;
    memcpy(from, at, sizeof(from));
    from[1] += WALL_RISE;
    side[0] = -along[2] / len * WALL_WIDTH;
    side[1] = 0;
    side[2] = along[0] / len * WALL_WIDTH;
    for (s = -1; s <= 1; s++) {
        for (i = 0; i < 3; i++) ray[i] = at[i] + along[i] + side[i] * s - from[i];
        if (ray[1] < 0) ray[1] = 0;
        t = wall_along(c, from, ray);
        if (t < nearest) nearest = t;
    }
    if (nearest < 1) nearest *= WALL_MARGIN;
    return nearest < WALL_NEAREST ? WALL_NEAREST : nearest;
}

/* The follow camera's update, virt0x24. */
WP_HOOK(0x80024EE0, CameraFollow_Update) {
    u32 cam = c->gpr[3];
    int ours, i, turned;
    float at[3], along[3], ray[3], down[3], eye[3], t, limit, steps, dist;
    uint64_t now;
    if (!wp_mem_ptr(cam, 0x120)) return 0;
    ours = LD32(cam + CAM_ID) == sCurrent && !hle_in_minigame();
    if (ours && cam != sLogged) {
        sLogged = cam;
        WP_LOG("camera", "camera %u turns (%08X)", sCurrent, cam);
    }
    untilt(cam);
    unplace(cam);
    if (ours && (sYawStep != 0 || sTiltStep != 0)) {
        stf(cam + CAM_YAW, wrap180(ldf(cam + CAM_YAW) + sYawStep));
        if (sYawStep != 0) swing_eye(cam, sYawStep);
        sTurn = wrap180(sTurn + sYawStep);
        sTilt += sTiltStep;
        sYawStep = sTiltStep = 0;
        if (sTestLeft > 0) sTestLeft--; /* WONDERPARK_MOUSE=<dx>,<dy>,<updates>: only that many */
    }
    if (ours && sRecenter) {
        sRecenter = 0;
        /* Recenter speed: slow, normal (the game's own pace), fast. */
        static const u32 kUpdates[3] = {RECENTER_UPDATES * 2, RECENTER_UPDATES, RECENTER_UPDATES * 2 / 5};
        stf(cam + CAM_YAW_TO, wrap180(ldf(cam + CAM_YAW) - sTurn));
        ST32(cam + CAM_EASING, kUpdates[g_x[X_RECENTER_SPEED] < 0 || g_x[X_RECENTER_SPEED] > 2 ? 1 : g_x[X_RECENTER_SPEED]]);
        sTurn = sTilt = 0;
    }
    wp_call_original(c, FOLLOW_UPDATE);
    /* Camera distance: the eye further back along its line, in the park
     * (walls and the ground still bring it in, as for a turn). */
    dist = g_x[X_CAMERA_DISTANCE] / 100.0f;
    turned = sTurn != 0 || sTilt != 0 || dist != 1.0f;
    if (!ours || (!turned && sPull >= 1)) {
        sPull = 1;
        return 1;
    }
    for (i = 0; i < 3; i++) {
        at[i] = ldf(cam + CAM_AT + i * 4);
        along[i] = (ldf(cam + CAM_EYE + i * 4) - at[i]) * dist;
    }
    /* Smoothly either way: a wall's edge passing a ray is a jump in the limit. */
    now = wp_timebase();
    steps = (float)((double)(now - sPlacedAt) / TB_HZ * 60);
    steps = steps > 4 ? 4 : steps;
    sPlacedAt = now;
    limit = turned ? wall_limit(c, at, along) : 1;
    if (limit < sPull) sPull += (limit - sPull) * (steps * WALL_EASE_IN > 1 ? 1 : steps * WALL_EASE_IN);
    else sPull = sPull + WALL_EASE_OUT * steps > limit ? limit : sPull + WALL_EASE_OUT * steps;
    for (i = 0; i < 3; i++) eye[i] = at[i] + along[i] * sPull;
    /* The ground: the eye stays above it, by looking down from over the eye. */
    for (i = 0; i < 3; i++) ray[i] = eye[i];
    if (ray[1] < at[1] + WALL_RISE) ray[1] = at[1] + WALL_RISE;
    down[0] = down[2] = 0;
    down[1] = eye[1] - GROUND_CLEARANCE - ray[1];
    t = wall_along(c, ray, down);
    if (t < 1 && eye[1] < ray[1] + down[1] * t + GROUND_CLEARANCE) eye[1] = ray[1] + down[1] * t + GROUND_CLEARANCE;
    sPlaced = cam;
    for (i = 0; i < 3; i++) {
        sGameEye[i] = LD32(cam + CAM_EYE + i * 4);
        stf(cam + CAM_EYE + i * 4, eye[i]);
        sOurEye[i] = LD32(cam + CAM_EYE + i * 4);
    }
    return 1;
}
