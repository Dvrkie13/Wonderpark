/* The Wii Remote. On a Wii, WPAD talks to remotes over Bluetooth: it reads
 * their calibration, sets the report format and turns on the IR camera,
 * then parses a report every few milliseconds and hands it to the
 * sampling callback (KPAD's), which KPADRead turns into button edges,
 * acceleration and pointer data.
 *
 * Here WPAD's device-facing calls are answered on the host. Remote 0,
 * held sideways with no extension, connects a second after the game
 * first sets a connect callback, the way a paired remote reconnects at
 * boot (connecting at once would come before the game has registered its
 * own callback, and the game would never count the remote as connected).
 * A timer then stands in for its reports at 200 Hz; each report reads the
 * host's controls (host_input.h). KPAD runs unchanged on top. Remotes 1-3
 * (players 2-4) connect while their players have a device (report()). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "host_input.h"
#include "host_settings.h"

/* Ported from MildRain479/PokePark-Wiicompiled recomp/patches/hle_wpad.c (CC0),
 * with S2LE01 addresses. Per-minigame motion and the button prompts
 * (hle_prompts) come with Phase 3; for now the remote reports buttons, a
 * shake as a swing, and the pointer (mouse) while the game has the IR camera
 * on (hle_motion, below). */
#define ppc_timebase wp_timebase
#define PPC_TB_HZ TB_HZ
#define MA(a) wp_mem_ptr((uint32_t)(a), 1)
#define HLE_LOG(...) WP_LOG("wpad", __VA_ARGS__)
static int hle_motion(int chan, uint32_t held, uint32_t reports, int16_t acc[3], int16_t dots[4][2]);
void hle_prompts_update(void); /* hle_prompts.c: the button prompts redrawn when they change */

int (*g_host_pad_read)(int chan, uint32_t* held);
void (*g_host_pad_rumble)(int chan, int on);
int (*g_host_pad_present)(int chan);
void (*g_host_pad_sticks)(int chan, float* lx, float* ly, float* rx, float* ry);

#define WPAD_MAX_CHANS 4
#define WPAD_ERR_NONE 0
#define WPAD_ERR_NO_CONTROLLER (-1)
#define WPAD_DEV_CORE 0
#define WPAD_DEV_NOT_FOUND 253
#define REPORT_HZ 200u

/* WPADStatus button bits. */
enum {
    WPAD_LEFT = 0x0001, WPAD_RIGHT = 0x0002, WPAD_DOWN = 0x0004, WPAD_UP = 0x0008,
    WPAD_PLUS = 0x0010, WPAD_2 = 0x0100, WPAD_1 = 0x0200, WPAD_B = 0x0400,
    WPAD_A = 0x0800, WPAD_MINUS = 0x1000, WPAD_HOME = 0x8000,
};

/* One g in the accelerometer's units, as calibrated on a typical remote. */
#define ACC_ONE_G 104
/* One swing of a shake, in reports (200 a second): 30 ms. */
#define SWING_REPORTS 6

static struct {
    uint32_t connectCb, extensionCb, samplingCb;
    uint32_t format;
    uint32_t dpdCommand;
    int speakerOn;
} sChan[WPAD_MAX_CHANS];

/* The pointer, while the game has the IR camera on (WPADControlDpd): the
 * mouse moves it (the screen is -1 to 1 across), and the remote reports the
 * sensor bar's two dots where the IR camera would see them. The constants
 * are the reference runtime's (recomp/patches/hle_motion.c, CC0), measured
 * against KPAD's pointer: x moves 1/225 of the 1024x768 view per unit, y
 * 1/226, and with the sensor bar below the screen the middle sits lower.
 * The dots move the other way to the pointer. Bit 2 of the result: dots
 * given; acc is the remote at rest. */
#define POINTER_PER_PIXEL (1.0f / 600.0f)
#define POINTER_X_LIMIT 1.6f
#define DOT_SPREAD 100
/* Flight of Fancy (attraction 4): flown by tilting the remote, held
 * sideways: left and right steer, forward and back dive and climb. Here
 * the left stick tilts it in proportion, and the d-pad keys (A/D, W/S,
 * arrows) tilt it easing in; Steering sensitivity scales how far a push
 * tilts it (so how fast it turns) and how quickly. From MildRain479/
 * PokePark-2-Wiicompiled's hle_motion.c (CC0): left and right go all the way
 * over (the game turns too wide at less), forward and back to 50 degrees;
 * each axis set on its own, so steering hard keeps most of a climb. */
#define PI_F 3.14159265f
#define MAX_STEER (90.0f * PI_F / 180.0f)
#define MAX_PITCH (50.0f * PI_F / 180.0f)
#define TILT_EASE 0.15f /* a share of the way each report (about 30 ms to get most of the way) */

/* A pair of opposite keys: +1, -1, and with both held the one pressed last. */
typedef struct { uint32_t last; int side; } KeyPair;
static float key_pair(KeyPair* k, uint32_t held, uint32_t plus, uint32_t minus) {
    uint32_t pressed = held & ~k->last;
    k->last = held;
    if (!(held & minus)) k->side = held & plus ? 1 : 0;
    else if (!(held & plus)) k->side = -1;
    else if (pressed & plus) k->side = 1;
    else if (pressed & minus) k->side = -1;
    return (float)k->side;
}

/* Player chan's sticks: player 1's as the camera reads them, the others'
 * from their own pads (input_win32.c). */
static void sticks(int chan, float* lx, float* ly, float* rx, float* ry) {
    *lx = *ly = *rx = *ry = 0;
    if (chan == 0) {
        if (g_host_sticks_read) g_host_sticks_read(lx, ly, rx, ry);
    } else if (g_host_pad_sticks) {
        g_host_pad_sticks(chan, lx, ly, rx, ry);
    }
}

static int flight_tilt(int chan, uint32_t held, int16_t acc[3]) {
    static float steerOf[WPAD_MAX_CHANS], pitchOf[WPAD_MAX_CHANS];
    static KeyPair leftRightOf[WPAD_MAX_CHANS], upDownOf[WPAD_MAX_CHANS];
    float *steer = &steerOf[chan], *pitch = &pitchOf[chan];
    float lx, ly, rx, ry, s = g_x[X_STEER_SENSITIVITY] / 100.0f, ease, ws, wp, x, y, across;
    sticks(chan, &lx, &ly, &rx, &ry);
    /* Right lowers the pointer end (to the left, held sideways); up climbs:
     * the remote tilted back. The stick over the keys when pushed. */
    ws = key_pair(&leftRightOf[chan], held, HOST_PAD_SCREEN_LEFT, HOST_PAD_SCREEN_RIGHT);
    if (lx != 0) ws = -lx;
    wp = key_pair(&upDownOf[chan], held, HOST_PAD_SCREEN_DOWN, HOST_PAD_SCREEN_UP);
    if (ly != 0) wp = -ly;
    if (g_x[X_MIRROR_WORLD]) ws = -ws; /* the picture is flipped */
    ws *= s;
    ws = ws > 1 ? 1 : ws < -1 ? -1 : ws;
    ease = TILT_EASE * s;
    ease = ease > 0.6f ? 0.6f : ease;
    *steer += (ws * MAX_STEER - *steer) * ease;
    *pitch += (wp * MAX_PITCH - *pitch) * TILT_EASE;
    /* Gravity in the remote's axes: x across its face, y along it, z out
     * of its face; both tilts together shrunk to 1 g if more. */
    x = sinf(*pitch) * ACC_ONE_G;
    y = sinf(*steer) * ACC_ONE_G;
    across = sqrtf(x * x + y * y);
    if (across > ACC_ONE_G) {
        x = x * ACC_ONE_G / across;
        y = y * ACC_ONE_G / across;
        across = (float)ACC_ONE_G;
    }
    acc[0] = (int16_t)x;
    acc[1] = (int16_t)y;
    acc[2] = (int16_t)sqrtf((float)ACC_ONE_G * ACC_ONE_G - across * across);
    return 1;
}

/* Power Bash (attraction 3): swinging the remote like a bat. The game
 * judges a swing by its size in any direction (MildRain479/PokePark-2-
 * Wiicompiled's hle_motion.c, CC0: along the remote's length too), and a
 * single short burst can fall between its readings and be missed. Here
 * each press of a shake key (Q / E, LB / RB) swings it three times back and
 * forth, a burst of acceleration on all three axes. */
#define BASH_SWINGS 3
static int bash_swing(int chan, uint32_t held, int16_t acc[3]) {
    static uint32_t lastOf[WPAD_MAX_CHANS], left[WPAD_MAX_CHANS];
    uint32_t pressed = held & ~lastOf[chan] & (HOST_PAD_SHAKE | HOST_PAD_SHAKE2);
    int sign;
    lastOf[chan] = held;
    if (pressed) left[chan] = BASH_SWINGS * 2 * SWING_REPORTS;
    acc[0] = 0;
    acc[1] = 0;
    acc[2] = ACC_ONE_G;
    if (!left[chan]) return 1;
    left[chan]--;
    sign = (left[chan] / SWING_REPORTS) & 1 ? 1 : -1;
    acc[0] = (int16_t)(sign * ACC_ONE_G * 5 / 2);
    acc[1] = (int16_t)(sign * ACC_ONE_G * 5 / 2);
    acc[2] = (int16_t)(ACC_ONE_G + sign * ACC_ONE_G * 5 / 2);
    return 1;
}

/* Players 2-4 move their pointer with their right stick: this many units a
 * second at full push (across the screen in about 0.6 s), the push squared
 * for fine aiming, times Aim speed. (Player 1's: the mouse, and the right
 * stick as the Right stick setting says, input_win32.c.) */
#define POINTER_PER_STICK_SECOND 3.5f

static int hle_motion(int chan, uint32_t held, uint32_t reports, int16_t acc[3], int16_t dots[4][2]) {
    static float pxOf[WPAD_MAX_CHANS], pyOf[WPAD_MAX_CHANS];
    float dx = 0, dy = 0, *px = &pxOf[chan], *py = &pyOf[chan];
    int mx, my;
    {
        extern int hle_minigame(void); /* hle_scene.c */
        if (hle_minigame() == 4) return flight_tilt(chan, held, acc);
        if (hle_minigame() == 3) return bash_swing(chan, held, acc);
    }
    if (!sChan[chan].dpdCommand) return 0;
    if (chan == 0) {
        if (g_host_pointer_read) g_host_pointer_read(&dx, &dy);
        *px += dx * POINTER_PER_PIXEL;
        *py += -dy * POINTER_PER_PIXEL;
    } else {
        float lx, ly, rx, ry, k = POINTER_PER_STICK_SECOND / REPORT_HZ * g_x[X_AIM_SPEED] / 100.0f;
        sticks(chan, &lx, &ly, &rx, &ry);
        *px += rx * fabsf(rx) * k;
        *py += ry * fabsf(ry) * k;
    }
    *px = *px > POINTER_X_LIMIT ? POINTER_X_LIMIT : *px < -POINTER_X_LIMIT ? -POINTER_X_LIMIT : *px;
    *py = *py > 1 ? 1 : *py < -1 ? -1 : *py;
    mx = (int)(512 - *px * 225);
    my = (int)(486 + *py * 226);
    dots[0][0] = (int16_t)(mx - DOT_SPREAD);
    dots[0][1] = (int16_t)my;
    dots[1][0] = (int16_t)(mx + DOT_SPREAD);
    dots[1][1] = (int16_t)my;
    if (wp_log_on("pointer") && (reports % 30 == 0 || dx != 0 || dy != 0))
        WP_LOG("pointer", "remote %d: dpd %u, move %+.0f %+.0f -> pointer %.2f %.2f, dots at %d,%d", chan + 1, sChan[chan].dpdCommand, dx, dy,
               *px, *py, mx, my);
    acc[0] = 0;
    acc[1] = 0;
    acc[2] = ACC_ONE_G;
    return 2;
}

/* Whether the game has remote 0's pointer on (the right stick's Auto
 * setting moves it then, input_win32.c). */
int hle_pointer_on(void) { return sChan[0].dpdCommand != 0; }

static uint32_t sReports;
/* Remote 0's buttons (HOST_PAD_*) at the last WPADRead. */
uint32_t g_wpad_held;
int (*g_host_script_input)(uint32_t held);
/* Remote 0 connects as described above; remotes 1-3 (players 2-4) connect
 * whenever the host has a device for that player (g_host_pad_present: a
 * controller, or a test script) and go away when it does, telling the
 * game through its connect callback as a real remote would. */
static int sConnecting, sConnected[WPAD_MAX_CHANS];

static int connected(int32_t chan) { return chan >= 0 && chan < WPAD_MAX_CHANS && sConnected[chan]; }

/* WONDERPARK_PAD_SCRIPT: scripted input for remote 0 on top of the host's,
 * for unattended runs. "<seconds>:<buttons> ...", buttons joined by "+"
 * (up down left right a b 1 2 plus minus home shake; on-screen
 * directions), each held from its time until the next entry, timed from
 * program start. "4:2 4.2: 9:a 9.2:" presses 2 at 4 s and A at 9 s.
 * WONDERPARK_PAD2_SCRIPT to _PAD4_SCRIPT do the same for players 2-4, and
 * connect their remotes (as if they had a controller). */
struct ScriptEntry { double t; uint32_t held; int scene; /* -1, or t counts from that scene's start */ };
static struct ScriptEntry* sScripts[WPAD_MAX_CHANS];
static int sScriptLens[WPAD_MAX_CHANS] = {-1, -1, -1, -1}, sScriptFramesOf[WPAD_MAX_CHANS];
unsigned gxb_game_frames(void); /* gx_frames.c */

static void load_script(int chan) {
    char envName[32];
    struct ScriptEntry* sScript = NULL;
    int sScriptLen, sScriptFrames = 0;
    static const struct { const char* name; uint32_t bit; } kNames[] = {
        {"up", HOST_PAD_SCREEN_UP}, {"down", HOST_PAD_SCREEN_DOWN}, {"left", HOST_PAD_SCREEN_LEFT},
        {"right", HOST_PAD_SCREEN_RIGHT}, {"a", HOST_PAD_A}, {"b", HOST_PAD_B}, {"1", HOST_PAD_1},
        {"2", HOST_PAD_2}, {"plus", HOST_PAD_PLUS}, {"minus", HOST_PAD_MINUS}, {"home", HOST_PAD_HOME},
        {"shake", HOST_PAD_SHAKE}, {"shake2", HOST_PAD_SHAKE2},
    };
    const char* p;
    int cap = 0;
    if (chan == 0) snprintf(envName, sizeof(envName), "WONDERPARK_PAD_SCRIPT");
    else snprintf(envName, sizeof(envName), "WONDERPARK_PAD%d_SCRIPT", chan + 1);
    p = getenv(envName);
    sScriptLen = 0;
    while (p && *p) {
        char* end;
        double t;
        uint32_t held = 0;
        int scene = -1;
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        if (*p == 's') {
            /* "s<scene>+<seconds>": that long after the scene first started
             * (hle_scene.c; 6 the start menu, 0 the park, 5 a lobby, 4 an
             * attraction), however long loading took. */
            scene = (int)strtol(p + 1, &end, 10);
            p = *end == '+' ? end + 1 : end;
        } else if (*p == 'f') {
            /* "f<n>": at the game's n-th finished frame instead of a time
             * (unaffected by waits for the disc, which vary a lot on a hard
             * drive). The whole script counts frames then. */
            sScriptFrames = 1;
            p++;
        }
        t = strtod(p, &end);
        if (end == p || *end != ':') {
            fprintf(stderr, "wpad: WONDERPARK_PAD_SCRIPT: ignoring everything from \"%.20s\"\n", p);
            break;
        }
        if (sScriptLen == cap) {
            struct ScriptEntry* grown = realloc(sScript, (cap ? cap * 2 : 64) * sizeof(*sScript));
            if (!grown) break;
            sScript = grown;
            cap = cap ? cap * 2 : 64;
        }
        p = end + 1;
        while (*p && *p != ' ' && *p != ',') {
            size_t n = strcspn(p, "+ ,"), i;
            for (i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
                if (strlen(kNames[i].name) == n && strncmp(p, kNames[i].name, n) == 0) held |= kNames[i].bit;
            }
            p += n;
            if (*p == '+') p++;
        }
        sScript[sScriptLen].t = t;
        sScript[sScriptLen].scene = scene;
        sScript[sScriptLen++].held = held;
    }
    sScripts[chan] = sScript;
    sScriptLens[chan] = sScriptLen;
    sScriptFramesOf[chan] = sScriptFrames;
}

/* Whether player chan has a test script (it connects their remote). */
static int has_script(int chan) {
    if (sScriptLens[chan] < 0) load_script(chan);
    return sScriptLens[chan] > 0;
}

static uint32_t script_held(int chan) {
    double now;
    uint32_t held = 0;
    int i;
    if (!has_script(chan)) return 0;
    /* Wonderpark: timed from program start (monotonic host time), so scripts
     * line up with screenshots and logs. */
    now = sScriptFramesOf[chan] ? (double)gxb_game_frames() : (double)wp_host_time() / PPC_TB_HZ;
    {
        /* The entry that came last, by when it's due (a scene-timed one only
         * once its scene has started). */
        extern double hle_scene_entered(int scene); /* hle_scene.c */
        double best = -1e30;
        for (i = 0; i < sScriptLens[chan]; i++) {
            const struct ScriptEntry* e = &sScripts[chan][i];
            double at = e->t;
            if (e->scene >= 0) {
                double s = hle_scene_entered(e->scene);
                if (s <= 0) continue;
                at = s + e->t;
            }
            if (at <= now && at >= best) best = at, held = e->held;
        }
    }
    return held;
}

/* Held sideways, d-pad on the left: the remote's UP points left on screen,
 * RIGHT points up. */
static uint16_t wpad_buttons(uint32_t held) {
    static const struct { uint32_t host; uint16_t wpad; } kMap[] = {
        {HOST_PAD_SCREEN_UP, WPAD_RIGHT}, {HOST_PAD_SCREEN_DOWN, WPAD_LEFT},
        {HOST_PAD_SCREEN_LEFT, WPAD_UP},  {HOST_PAD_SCREEN_RIGHT, WPAD_DOWN},
        {HOST_PAD_A, WPAD_A},             {HOST_PAD_B, WPAD_B},
        {HOST_PAD_1, WPAD_1},             {HOST_PAD_2, WPAD_2},
        {HOST_PAD_PLUS, WPAD_PLUS},       {HOST_PAD_MINUS, WPAD_MINUS},
        {HOST_PAD_HOME, WPAD_HOME},
    };
    uint16_t b = 0;
    size_t i;
    for (i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
        if (held & kMap[i].host) b |= kMap[i].wpad;
    }
    return b;
}

/* Reports go to the sampling callback, like the Bluetooth stack's receive
 * interrupt on a Wii. */
static void report(void) {
    int chan;
    sReports++;
    if (sReports % (REPORT_HZ / 2) == 0) {
        /* Players 2-4: a remote connects when its player has a device, and
         * goes away when the device does (looked at twice a second). Only
         * once the game listens for that remote (its connect callback). */
        for (chan = 1; chan < WPAD_MAX_CHANS; chan++) {
            int present = has_script(chan) || (g_host_pad_present && g_host_pad_present(chan));
            if (present == sConnected[chan] || !sChan[chan].connectCb) continue;
            sConnected[chan] = present;
            HLE_LOG("WPAD: remote %d %s", chan + 1, present ? "connected" : "went away");
            wp_irq_post(sChan[chan].connectCb, (uint32_t)chan, present ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER);
        }
    }
    for (chan = 0; chan < WPAD_MAX_CHANS; chan++)
        if (sConnected[chan] && sChan[chan].samplingCb) wp_irq_post(sChan[chan].samplingCb, (uint32_t)chan, 0);
}

static void connect(void) {
    sConnected[0] = 1;
    HLE_LOG("WPAD: remote 0 connected");
    wp_timer_stop(WP_TIMER_PAD_CONNECT);
    wp_timer_start(WP_TIMER_PAD, TB_HZ / REPORT_HZ, report);
    if (sChan[0].connectCb) wp_irq_post(sChan[0].connectCb, 0, WPAD_ERR_NONE);
}

/* The game's own connect callback (its input manager, under KPAD's): sets
 * its "remote chan is connected" flag, which the menus count. Logged. */
WP_HOOK(0x80033010, GameConnectCallback) {
    HLE_LOG("game's connect callback: remote %d, result %d", (int)c->gpr[3] + 1, (int)c->gpr[4]);
    return 0;
}

/* Debugging (WONDERPARK_LOG=wpadflags): the game input manager's flags per
 * remote, every two seconds. */
static void log_flags(CPUState* c) {
    static uint32_t last;
    u32 mgr;
    int i;
    if (!wp_log_on("wpadflags") || sReports - last < 2 * REPORT_HZ) return;
    last = sReports;
    mgr = wp_call_guest(c, 0x80072600u, 0, 0, 0, 0);
    for (i = 0; i < 4; i++)
        WP_LOG("wpadflags", "remote %d: +11260 %u +11264 %u", i + 1, LD8(mgr + i + 11260), LD8(mgr + i + 11264));
}

/* s32 WPADProbe(s32 chan, u32* type) */
WP_HOOK(0x802D4050, WPADProbe) {
    int32_t chan = (int32_t)c->gpr[3];
    int on = chan >= 0 && chan < WPAD_MAX_CHANS && connected(chan);
    if (c->gpr[4]) ST32(c->gpr[4], on ? WPAD_DEV_CORE : WPAD_DEV_NOT_FOUND);
    c->gpr[3] = on ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER;
    wp_return(c);
    return 1;
}

/* WPADConnectCallback WPADSetConnectCallback(s32 chan, WPADConnectCallback cb) */
WP_HOOK(0x802D4170, WPADSetConnectCallback) {
    int32_t chan = (int32_t)c->gpr[3];
    uint32_t cb = c->gpr[4], old = 0;
    if (chan >= 0 && chan < WPAD_MAX_CHANS) {
        old = sChan[chan].connectCb;
        sChan[chan].connectCb = cb;
        if (cb && chan == 0 && !sConnecting) {
            sConnecting = 1;
            wp_timer_start(WP_TIMER_PAD_CONNECT, TB_HZ, connect);
        }
    }
    c->gpr[3] = old;
    wp_return(c);
    return 1;
}

/* WPADExtensionCallback WPADSetExtensionCallback(s32 chan, WPADExtensionCallback cb) */
WP_HOOK(0x802D4200, WPADSetExtensionCallback) {
    int32_t chan = (int32_t)c->gpr[3];
    uint32_t old = 0;
    if (chan >= 0 && chan < WPAD_MAX_CHANS) {
        old = sChan[chan].extensionCb;
        sChan[chan].extensionCb = c->gpr[4];
    }
    c->gpr[3] = old;
    wp_return(c);
    return 1;
}

/* WPADSamplingCallback WPADSetSamplingCallback(s32 chan, WPADSamplingCallback cb) */
WP_HOOK(0x802D40E0, WPADSetSamplingCallback) {
    int32_t chan = (int32_t)c->gpr[3];
    uint32_t old = 0;
    if (chan >= 0 && chan < WPAD_MAX_CHANS) {
        old = sChan[chan].samplingCb;
        sChan[chan].samplingCb = c->gpr[4];
    }
    c->gpr[3] = old;
    wp_return(c);
    return 1;
}

/* u32 WPADGetDataFormat(s32 chan) */
WP_HOOK(0x802D4260, WPADGetDataFormat) {
    int32_t chan = (int32_t)c->gpr[3];
    c->gpr[3] = chan >= 0 && chan < WPAD_MAX_CHANS ? sChan[chan].format : 0;
    wp_return(c);
    return 1;
}

/* s32 WPADSetDataFormat(s32 chan, u32 format) */
WP_HOOK(0x802D42B0, WPADSetDataFormat) {
    int32_t chan = (int32_t)c->gpr[3];
    if (chan >= 0 && chan < WPAD_MAX_CHANS) sChan[chan].format = c->gpr[4];
    c->gpr[3] = connected(chan) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER;
    wp_return(c);
    return 1;
}

/* void WPADRead(s32 chan, WPADStatus* status): the latest report.
 * WPADStatus: u16 button; s16 accX, accY, accZ; DPDObject obj[4] (s16 x,
 * y; u16 size; u8 traceId; pad); u8 dev; s8 err. */
WP_HOOK(0x802D49E0, WPADRead) {
    int32_t chan = (int32_t)c->gpr[3];
    uint32_t st = c->gpr[4], held = 0;
    int16_t ax = 0, ay = 0, az = ACC_ONE_G;
    int on = chan >= 0 && chan < WPAD_MAX_CHANS && connected(chan), motion = 0, i;
    int16_t acc[3], dots[4][2];
    hle_prompts_update();
    if (c->gpr[3] == 0) log_flags(c);
    if (on && g_host_pad_read && !g_host_pad_read(chan, &held)) held = 0;
    if (on && chan == 0) {
        uint32_t scripted = script_held(0);
        extern void options_rows_filter(uint32_t * held); /* settings_ui.cpp: the Options page's PC rows */
        if (!g_host_script_input || !g_host_script_input(scripted)) held |= scripted;
        options_rows_filter(&held);
    } else if (on) {
        held |= script_held(chan); /* players 2-4's test scripts */
    }
    if (on && (motion = hle_motion(chan, held, sReports, acc, dots)) != 0) {
        /* In minigames, the remote's motion as each asks (hle_motion.c). */
        ax = acc[0];
        ay = acc[1];
        az = acc[2];
    } else if (held & (HOST_PAD_SHAKE | HOST_PAD_SHAKE2)) {
        /* Swing along the remote's length, which is left-right on screen
         * held sideways: +-2.5 g, about 17 times a second. */
        ay = (sReports / SWING_REPORTS) & 1 ? ACC_ONE_G * 5 / 2 : -ACC_ONE_G * 5 / 2;
    }
    if (on && chan == 0) {
        extern uint32_t g_wpad_held; /* the camera's B (hle_camera.c) */
        g_wpad_held = held;
    }
    if (held && wp_log_on("wpadbtn")) WP_LOG("wpadbtn", "chan %d held %04X -> buttons %04X (lr %08X)", (int)chan, held, wpad_buttons(held), c->lr);
    memset(MA(st), 0, 0x2A);
    ST16(st + 0x00, wpad_buttons(held));
    ST16(st + 0x02, (uint16_t)ax);
    ST16(st + 0x04, (uint16_t)ay);
    ST16(st + 0x06, (uint16_t)az);
    /* IR points in view: the sensor bar's two dots, where a minigame
     * uses the pointer; otherwise none, and the pointer is off. */
    if (motion & 2) {
        for (i = 0; i < 2; i++) {
            ST16(st + 0x08 + 8 * i, (uint16_t)dots[i][0]);
            ST16(st + 0x0A + 8 * i, (uint16_t)dots[i][1]);
            ST16(st + 0x0C + 8 * i, 4); /* size */
            ST8(st + 0x0E + 8 * i, (uint8_t)i);
        }
    }
    ST8(st + 0x28, on ? WPAD_DEV_CORE : WPAD_DEV_NOT_FOUND);
    ST8(st + 0x29, (uint8_t)(on ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER));
    wp_return(c);
    return 1;
}

/* void WPADGetAccGravityUnit(s32 chan, u32 type, WPADAcc* acc) */
WP_HOOK(0x802D3DE0, WPADGetAccGravityUnit) {
    ST16(c->gpr[5] + 0, ACC_ONE_G);
    ST16(c->gpr[5] + 2, ACC_ONE_G);
    ST16(c->gpr[5] + 4, ACC_ONE_G);
    wp_return(c);
    return 1;
}

/* s32 WPADControlDpd(s32 chan, u32 command, WPADCallback cb) */
WP_HOOK(0x802D6990, WPADControlDpd) {
    int32_t chan = (int32_t)c->gpr[3];
    if (chan >= 0 && chan < WPAD_MAX_CHANS) sChan[chan].dpdCommand = c->gpr[4];
    if (c->gpr[5]) wp_irq_post(c->gpr[5], (uint32_t)chan, connected(chan) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER);
    c->gpr[3] = connected(chan) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER;
    wp_return(c);
    return 1;
}

/* BOOL WPADIsDpdEnabled(s32 chan). (Matched to PP1's by its loads and stores:
 * it returns the channel block's +0x838 word.) */
WP_HOOK(0x802D6900, WPADIsDpdEnabled) {
    int32_t chan = (int32_t)c->gpr[3];
    c->gpr[3] = chan >= 0 && chan < WPAD_MAX_CHANS && sChan[chan].dpdCommand != 0;
    wp_return(c);
    return 1;
}

/* u8 WPADGetDpdFormat(s32 chan) */
/* (not hooked yet: PP1 8024A8F0; its S2LE01 counterpart is laid out differently) */

/* void WPADControlMotor(s32 chan, u32 command): 1 starts the rumble. */
WP_HOOK(0x802D45F0, WPADControlMotor) {
    int32_t chan = (int32_t)c->gpr[3];
    if (connected(chan) && g_host_pad_rumble) g_host_pad_rumble(chan, c->gpr[4] == 1);
    wp_return(c);
    return 1;
}

/* s32 WPADControlLed(s32 chan, u8 pattern, WPADCallback cb) */
WP_HOOK(0x802D4740, WPADControlLed) {
    int32_t chan = (int32_t)c->gpr[3];
    if (c->gpr[5]) wp_irq_post(c->gpr[5], (uint32_t)chan, connected(chan) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER);
    c->gpr[3] = connected(chan) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER;
    wp_return(c);
    return 1;
}

/* s32 WPADGetInfoAsync(s32 chan, WPADInfo* info, WPADCallback cb).
 * WPADInfo: BOOL dpd, speaker, attach, lowBat, nearempty; u8 battery,
 * led, protocol, firmware. */
WP_HOOK(0x802D43D0, WPADGetInfoAsync) {
    int32_t chan = (int32_t)c->gpr[3];
    uint32_t info = c->gpr[4];
    if (!connected(chan)) {
        c->gpr[3] = (uint32_t)WPAD_ERR_NO_CONTROLLER;
        wp_return(c);
        return 1;
    }
    memset(MA(info), 0, 0x18);
    ST32(info + 0x00, sChan[chan].dpdCommand != 0);
    ST32(info + 0x04, (uint32_t)sChan[chan].speakerOn);
    ST8(info + 0x14, 4);             /* battery: full */
    ST8(info + 0x15, (uint8_t)(1 << chan)); /* player LED */
    if (c->gpr[5]) wp_irq_post(c->gpr[5], (uint32_t)chan, WPAD_ERR_NONE);
    c->gpr[3] = WPAD_ERR_NONE;
    wp_return(c);
    return 1;
}

/* u8 WPADGetRadioSensitivity(s32 chan): a strong signal. */
WP_HOOK(0x802D28B0, WPADGetRadioSensitivity) { c->gpr[3] = connected((int32_t)c->gpr[3]) ? 100 : 0; wp_return(c); return 1; }

/* s32 WPADDisconnect(s32 chan): the remote stays connected. */
WP_HOOK(0x802D3EF0, WPADDisconnect) { c->gpr[3] = WPAD_ERR_NONE; wp_return(c); return 1; }

/* The speaker. Sound sent to it is dropped. */

/* s32 WPADControlSpeaker(s32 chan, u32 command, WPADCallback cb):
 * command 0 turns it off, 1 on. */
WP_HOOK(0x802D4E50, WPADControlSpeaker) {
    int32_t chan = (int32_t)c->gpr[3];
    if (!connected(chan)) {
        c->gpr[3] = (uint32_t)WPAD_ERR_NO_CONTROLLER;
        wp_return(c);
        return 1;
    }
    if (c->gpr[4] == 0) sChan[chan].speakerOn = 0;
    else if (c->gpr[4] == 1) sChan[chan].speakerOn = 1;
    if (c->gpr[5]) wp_irq_post(c->gpr[5], (uint32_t)chan, WPAD_ERR_NONE);
    c->gpr[3] = WPAD_ERR_NONE;
    wp_return(c);
    return 1;
}

/* BOOL WPADIsSpeakerEnabled(s32 chan) */
WP_HOOK(0x802D4E00, WPADIsSpeakerEnabled) {
    int32_t chan = (int32_t)c->gpr[3];
    c->gpr[3] = chan >= 0 && chan < WPAD_MAX_CHANS && sChan[chan].speakerOn;
    wp_return(c);
    return 1;
}

/* BOOL WPADCanSendStreamData(s32 chan) */
WP_HOOK(0x802D6620, WPADCanSendStreamData) { c->gpr[3] = connected((int32_t)c->gpr[3]); wp_return(c); return 1; }

/* s32 WPADSendStreamData(s32 chan, void* data, u16 len) */
WP_HOOK(0x802D66B0, WPADSendStreamData) { c->gpr[3] = connected((int32_t)c->gpr[3]) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER; wp_return(c); return 1; }
