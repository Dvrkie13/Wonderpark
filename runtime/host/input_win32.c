/* From MildRain479/PokePark-Wiicompiled recomp/boot/input_win32.c (CC0). */
/* Wii Remote 0 from the keyboard and the first controller (SDL3), for
 * hle_wpad.c. PokePark is played with the remote held sideways, like a
 * classic pad: the d-pad moves, 1 and 2 are the face buttons. By default:
 *
 *                    keyboard / mouse           controller
 *   d-pad (move)     WASD / arrows              d-pad / left stick
 *   2 (jump)         Space / X                  A
 *   1 (dash)         left click / Z             X
 *   A (Thunderbolt)  right click / Enter        B
 *   B                V / Left Ctrl              Y / right trigger
 *   +                P                          Start
 *   -                M                          Back
 *   HOME             Home / H                   right stick click
 *   camera           mouse                      right stick
 *   shake (held)     Q / Left Shift             LB
 *   shake 2          E                          RB
 *   (in minigames a shake is shake and shake 2 pressed by turns, hle_wpad.c)
 *
 * The keyboard counts only while the game's window has focus. While it
 * has focus the mouse is captured: hidden, held in the window, and its
 * movement turns the camera (hle_camera.c). Esc lets it go; a click in
 * the window captures it again. Mouse buttons count only while captured,
 * and not the click that captures it. The settings menu (menu_win32.c)
 * changes the bindings; while it is open the game gets no input. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "host_input.h"
#include "host_settings.h"

/* Wonderpark: controllers through SDL3 (pad_sdl.c) instead of XInput, so
 * PlayStation, Switch and other pads work natively; the readings come in
 * XInput's form, so the rest of this file is as it was. */
#include "pad_sdl.h"
extern int g_wp_headless; /* main.c */

static int has_focus(void) {
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (!w) return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static int sCaptured, sReleased, sSwallow, sButtonsLive;
static POINT sCenter;
static float sMouseX, sMouseY, sPointerX, sPointerY;

/* Keeps the cursor in the window while captured, checked on every poll:
 * going fullscreen moves the window. */
static void capture(int on) {
    if (on) {
        RECT r;
        POINT c;
        HWND w = GetForegroundWindow();
        GetClientRect(w, &r);
        MapWindowPoints(w, NULL, (POINT*)&r, 2);
        c.x = (r.left + r.right) / 2;
        c.y = (r.top + r.bottom) / 2;
        if (!sCaptured || c.x != sCenter.x || c.y != sCenter.y) {
            ClipCursor(&r);
            sCenter = c;
            SetCursorPos(sCenter.x, sCenter.y);
        }
        if (!sCaptured) while (ShowCursor(FALSE) >= 0) {}
    } else if (sCaptured) {
        ClipCursor(NULL);
        while (ShowCursor(TRUE) < 0) {}
    }
    sCaptured = on;
}

/* Lets the cursor go now (the F1 tab opening: the game is paused then and
 * doesn't poll input, which would release it). */
void input_release_mouse(void) { capture(0); }

/* Captures or releases the mouse and collects its movement; works out
 * whether its buttons count (sButtonsLive). */
static void mouse(void) {
    int left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    POINT p;
    sButtonsLive = 0;
    {
        /* Our menus over the game (the F1 tab, the Options page's PC rows)
         * want the cursor free and shown, to point and click. */
        extern int settings_ui_wants_cursor(void);
        if (!has_focus() || menu_is_open() || settings_ui_wants_cursor()) {
            capture(0);
            return;
        }
    }
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) sReleased = 1;
    if (sReleased && (left || right)) {
        HWND w = GetForegroundWindow();
        RECT r;
        GetCursorPos(&p);
        ScreenToClient(w, &p);
        GetClientRect(w, &r);
        if (PtInRect(&r, p)) sReleased = 0, sSwallow = 1;
    }
    capture(!sReleased);
    if (!sCaptured) return;
    GetCursorPos(&p);
    sMouseX += (float)(p.x - sCenter.x);
    sPointerX += (float)(p.x - sCenter.x);
    sPointerY += (float)(p.y - sCenter.y);
    sMouseY += (float)(p.y - sCenter.y);
    if (p.x != sCenter.x || p.y != sCenter.y) SetCursorPos(sCenter.x, sCenter.y);
    if (sSwallow) {
        if (left || right) return;
        sSwallow = 0;
    }
    sButtonsLive = 1;
}

static void read_sticks(float* lx, float* ly, float* rx, float* ry);

/* Right stick (Controls; [controls] right_stick): 0 the camera, 1 the
 * pointer, 2 auto: the pointer while the game has it on (minigames that
 * aim, menus pointed at), else the camera. */
static int stick_is_pointer(void) {
    extern int hle_pointer_on(void); /* hle_wpad.c */
    return g_x[X_RIGHT_STICK] == 1 || (g_x[X_RIGHT_STICK] == 2 && hle_pointer_on());
}

/* The right stick moves the pointer this many mouse pixels a second at
 * full push (across the screen in about 2 s), times Aim speed; the push
 * squared, so small pushes aim finely. */
#define AIM_STICK_PIXELS_PER_SECOND 900.0f

static void read_pointer(float* dx, float* dy) {
    static LARGE_INTEGER last;
    LARGE_INTEGER now, freq;
    float lx, ly, rx, ry, dt = 0;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (last.QuadPart) dt = (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
    if (dt > 0.1f) dt = 0.1f;
    last = now;
    *dx = sPointerX * g_settings.mouse_sensitivity;
    *dy = sPointerY * g_settings.mouse_sensitivity;
    sPointerX = sPointerY = 0;
    if (stick_is_pointer() && has_focus()) {
        float speed = AIM_STICK_PIXELS_PER_SECOND * dt * g_x[X_AIM_SPEED] / 100.0f;
        read_sticks(&lx, &ly, &rx, &ry);
        *dx += rx * fabsf(rx) * speed;
        *dy -= ry * fabsf(ry) * speed;
    }
    if (g_x[X_MIRROR_WORLD]) *dx = -*dx;
}

/* A stick axis, -32768 to 32767, as -1 to 1 past a dead zone. */
static float stick_axis(SHORT v) {
    const float kDead = 7849.0f; /* XInput's own suggestion for the left stick */
    float f = (float)v;
    if (f > kDead) return (f - kDead) / (32767.0f - kDead);
    if (f < -kDead) return (f + kDead) / (32768.0f - kDead);
    return 0.0f;
}

static void read_sticks(float* lx, float* ly, float* rx, float* ry) {
    PadState s;
    *lx = *ly = *rx = *ry = 0;
    if (!pad_sdl_state(0, &s) || menu_is_open()) return;
    *lx = stick_axis(s.lx);
    *ly = stick_axis(s.ly);
    *rx = stick_axis(s.rx);
    *ry = stick_axis(s.ry);
}

/* The right stick turns the camera as the mouse does: full over turns it
 * as this many pixels a second would (180 degrees a second), up looking
 * up. */
#define LOOK_STICK_PIXELS_PER_SECOND 600.0f

/* The camera's turn since the last call (hle_camera.c), in mouse pixels:
 * the mouse's movement and the right stick's push over the time. */
static void read_mouse(float* dx, float* dy) {
    static LARGE_INTEGER last;
    LARGE_INTEGER now, freq;
    float lx, ly, rx, ry, dt = 0;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (last.QuadPart) dt = (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
    if (dt > 0.1f) dt = 0.1f; /* not a jump after a stall */
    last = now;
    read_sticks(&lx, &ly, &rx, &ry);
    if (!has_focus() || stick_is_pointer()) rx = ry = 0; /* the stick moving the pointer instead */
    {
        /* Wonderpark: the camera's settings (hle_camera.c): Camera speed for
         * the mouse, Stick speed for the stick, both inverts; Mirror world
         * turns the other way, as the picture is flipped. */
        float stick = LOOK_STICK_PIXELS_PER_SECOND * dt * g_x[X_STICK_SENSITIVITY] / 100.0f;
        int flipX = (g_x[X_INVERT_X] != 0) != (g_x[X_MIRROR_WORLD] != 0);
        *dx = (sMouseX * g_settings.mouse_sensitivity + rx * stick) * (flipX ? -1 : 1);
        *dy = (sMouseY * g_settings.mouse_sensitivity - ry * stick) * (g_settings.invert_tilt ? -1 : 1);
    }
    sMouseX = sMouseY = 0;
}

static int is_mouse_button(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

/* The pad bit of each action (ACT_*). */
static const uint32_t kActionBit[ACT_COUNT] = {
    HOST_PAD_SCREEN_UP, HOST_PAD_SCREEN_DOWN, HOST_PAD_SCREEN_LEFT, HOST_PAD_SCREEN_RIGHT,
    HOST_PAD_2, HOST_PAD_1, HOST_PAD_A, HOST_PAD_B, HOST_PAD_PLUS, HOST_PAD_MINUS, HOST_PAD_HOME, HOST_PAD_SHAKE, HOST_PAD_SHAKE2,
};

static uint32_t keyboard(void) {
    uint32_t held = 0;
    int i, k;
    if (!has_focus()) return 0;
    for (i = 0; i < ACT_COUNT; i++) {
        for (k = 0; k < BIND_SLOTS; k++) {
            int vk = g_settings.key[i][k];
            if (vk && (!is_mouse_button(vk) || sButtonsLive) && (GetAsyncKeyState(vk) & 0x8000)) held |= kActionBit[i];
        }
    }
    return held;
}

/* Half the stick's travel, then the d-pad direction. */
#define STICK_THRESHOLD 16384

/* The controller's buttons (XPAD_*), and separately the directions the
 * left stick is pushed, as d-pad bits. Read once per poll, and the menu
 * gets the same reading. (A pad connecting or going away is noticed in
 * pad_sdl.c, which tells the button icons.) */
static void controller_state(uint32_t* buttons, uint32_t* stick) {
    PadState s;
    *buttons = *stick = 0;
    if (!pad_sdl_state(0, &s)) return;
    *buttons = s.buttons;
    if (s.lt > 128) *buttons |= XPAD_LT;
    if (s.rt > 128) *buttons |= XPAD_RT;
    if (s.ly > STICK_THRESHOLD) *stick |= XPAD_UP;
    if (s.ly < -STICK_THRESHOLD) *stick |= XPAD_DOWN;
    if (s.lx < -STICK_THRESHOLD) *stick |= XPAD_LEFT;
    if (s.lx > STICK_THRESHOLD) *stick |= XPAD_RIGHT;
}

/* The left stick always moves, besides what the d-pad is bound to. */
static uint32_t controller(uint32_t b, uint32_t stick) {
    uint32_t held = 0;
    int i, k;
    for (i = 0; i < ACT_COUNT; i++) {
        for (k = 0; k < BIND_SLOTS; k++) {
            if (b & g_settings.pad[i][k]) held |= kActionBit[i];
        }
    }
    if (stick & XPAD_UP) held |= HOST_PAD_SCREEN_UP;
    if (stick & XPAD_DOWN) held |= HOST_PAD_SCREEN_DOWN;
    if (stick & XPAD_LEFT) held |= HOST_PAD_SCREEN_LEFT;
    if (stick & XPAD_RIGHT) held |= HOST_PAD_SCREEN_RIGHT;
    return held;
}

/* While the menu is open, and after it closes until everything held is
 * let go, nothing reaches the game: not the press that closed it. */
static int sControllerLast;

/* RECOMP_PROMPTS=controller: as if the controller had been used last (for
 * test runs, whose scripted input is no device's). */
int input_controller_last(void) {
    static int forced = -1;
    if (forced < 0) {
        const char* e = getenv("RECOMP_PROMPTS");
        forced = e && !strcmp(e, "controller");
    }
    return forced || sControllerLast;
}

static void preview_tick(void);

/* Speed-up (Gameplay: Speed-up, Speed-up key): Tab, or the controller's
 * left trigger when no action is bound to it, runs the park 2 or 3 times as
 * fast (the game's clock, so it runs that many more of its fixed steps),
 * while held or switched by presses. Only in the park (hle_scene.c), never
 * in an attraction, a movie, or while a story scene or talk runs; the
 * sound is silent meanwhile (audio_win32.c). Our speed only: one set from
 * the debug overlay (F7) is left alone. */
static void speed_up(uint32_t buttons) {
    extern int hle_in_park(void), hle_event_running(void);
    extern const char* wp_movie_playing(void);
    extern void wp_set_game_speed(int times);
    extern int wp_game_speed(void);
    static int on, prevHeld, ours = 1, test = -1;
    int held = 0, want, i, k, ltBound = 0;
    if (test < 0) {
        const char* e = getenv("WONDERPARK_SPEED_HELD"); /* tests: as if the key were held */
        test = e && *e == '1';
    }
    if (test) held = 1;
    for (i = 0; i < ACT_COUNT; i++)
        for (k = 0; k < BIND_SLOTS; k++) ltBound |= g_settings.pad[i][k] == XPAD_LT;
    if (has_focus() && !menu_is_open() && (GetAsyncKeyState(VK_TAB) & 0x8000)) held = 1;
    if (!ltBound && (buttons & XPAD_LT)) held = 1;
    if (g_x[X_SPEED_TOGGLE]) {
        if (held && !prevHeld) on = !on;
    } else {
        on = held;
    }
    prevHeld = held;
    want = on && g_x[X_SPEED_UP] && hle_in_park() && !hle_event_running() && !wp_movie_playing() ? g_x[X_SPEED_UP] + 1 : 1;
    if (wp_game_speed() != ours) return; /* someone else's (F7) */
    if (want != ours) {
        wp_set_game_speed(want);
        ours = want;
    }
}

static int read_pad(int chan, uint32_t* held) {
    static int menuHeld;
    uint32_t buttons, stick, keys, pad;
    if (chan != 0) return 0;
    preview_tick();
    mouse();
    controller_state(&buttons, &stick);
    speed_up(buttons);
    menu_controller(buttons, buttons | stick);
    keys = keyboard();
    pad = controller(buttons, stick);
    if (pad || buttons || stick) sControllerLast = 1; /* any button, or the stick past half */
    else if (keys) sControllerLast = 0;
    *held = keys | pad;
    if (g_x[X_MIRROR_WORLD]) {
        /* The picture is flipped: left on screen is right in the game. */
        uint32_t lr = *held & (HOST_PAD_SCREEN_LEFT | HOST_PAD_SCREEN_RIGHT);
        if (lr == HOST_PAD_SCREEN_LEFT || lr == HOST_PAD_SCREEN_RIGHT) *held ^= HOST_PAD_SCREEN_LEFT | HOST_PAD_SCREEN_RIGHT;
    }
    if (menu_is_open()) menuHeld = 1;
    else if (menuHeld && !*held) menuHeld = 0;
    if (menuHeld) *held = 0;
    return 1;
}

/* The Wii Remote's motor on or off, as the controller's at the strength
 * the setting says (normal: as before there was a setting). */
static int sRumbling;

static void set_motors(int on) {
    static const WORD kSpeed[4] = {0, 0x3000, 0x6000, 0xC000};
    WORD s = on ? kSpeed[g_settings.rumble & 3] : 0;
    pad_sdl_rumble(0, s, s);
}

static void rumble(int chan, int on) {
    if (chan != 0) return;
    sRumbling = on;
    set_motors(on);
}

/* A short buzz at the new strength, when the setting is changed: ended
 * at an input poll (read_pad) once it's been long enough. */
static ULONGLONG sPreviewEnd;

void input_rumble_preview(void) {
    set_motors(1);
    sPreviewEnd = GetTickCount64() + 200;
}

static void preview_tick(void) {
    if (sPreviewEnd && GetTickCount64() >= sPreviewEnd) {
        sPreviewEnd = 0;
        set_motors(sRumbling);
    }
}

/* Scripted input goes to the Options tab first, as the controller's does. */
static int script_input(uint32_t held) { return options_script(held) || menu_is_open(); }

void input_init(void) {
    if (!g_wp_headless) pad_sdl_init(); /* (no controllers in tests run headless) */
    g_host_pad_read = read_pad;
    g_host_script_input = script_input;
    g_host_pad_rumble = rumble;
    g_host_mouse_read = read_mouse;
    prompts_init();
    g_host_pointer_read = read_pointer;
    g_host_sticks_read = read_sticks;
}
