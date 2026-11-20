/* From MildRain479/PokePark-Wiicompiled recomp/boot/settings.c (CC0). */
/* Settings: settings.ini and bindings.ini, read at start and saved whenever one changes,
 * from the Options tab of the pause menu (options.c) and, on Windows, the
 * settings menu (menu_win32.c). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_settings.h"

void hle_set_frame_rate(int fps);   /* hle_frame.c */
void gxb_set_output_rate(int fps);  /* gx_frames.c */

/* The frame rates offered. Above 60 the game still draws 60 frames a
 * second and the ones between are interpolated (gl_backend.c). */
static const int kFrameRates[] = {30, 60, 120, 144, 165, 240};
#define FRAME_RATES ((int)(sizeof(kFrameRates) / sizeof(kFrameRates[0])))

static int valid_frame_rate(int fps) {
    int i;
    for (i = FRAME_RATES - 1; i > 0 && kFrameRates[i] > fps; i--) {}
    return kFrameRates[i];
}

void hle_set_fov(int percent); /* hle_camera.c */
void hle_set_text_speed(int steps);  /* hle_modifiers.c */
void hle_set_berry_rate(int percent);
void hle_set_prices(int percent);
void hle_set_damage(int dealt, int taken);
void hle_set_chase_speed(int percent);

/* The gameplay settings' choices, in order. */
static const int kTextSpeeds[] = {1, 2, 3, 0};
static const int kBerries[] = {100, 150, 200, 300, 50};
static const int kPrices[] = {100, 50, 0, 200};
static const int kDamageDealt[] = {100, 150, 200, 300, 50};
static const int kDamageTaken[] = {100, 50, 0, 200, 300};
static const int kChaseSpeeds[] = {100, 75, 50, 125, 150};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int choice(const int* list, int n, int v) {
    int i;
    for (i = 0; i < n && list[i] != v; i++) {}
    return i < n ? i : 0;
}

static void apply_gameplay(void) {
    hle_set_text_speed(g_settings.text_speed);
    hle_set_berry_rate(g_settings.berries);
    hle_set_prices(g_settings.prices);
    hle_set_damage(g_settings.damage_dealt, g_settings.damage_taken);
    hle_set_chase_speed(g_settings.chase_speed);
}

#define FOV_MIN 70
#define FOV_MAX 150

static void apply_frame_rate(void) {
    hle_set_frame_rate(g_settings.frame_rate);
    gxb_set_output_rate(g_settings.frame_rate);
}
/* For the F1 notebook's Frame rate row (settings_ui.cpp): at once, not
 * at the next start. */
void settings_apply_frame_rate(void) { apply_frame_rate(); }

HostSettings g_settings;

/* The PC settings table (host_settings.h X_*): section, key, default, range, comment. */
static const struct {
    const char* section;
    const char* key;
    int def, min, max;
    const char* note;
} kX[X_COUNT] = {
    {"display", "window_mode", 0, 0, 2, "0 windowed, 1 borderless fullscreen, 2 fullscreen (the screen set to the resolution)"},
    {"display", "resolution", 4, 0, 4, "window size: 0 1280x720, 1 1600x900, 2 1920x1080, 3 2560x1440, 4 match screen"},
    {"display", "vsync", 0, 0, 1, "1: frames wait for the screen's refresh (no tearing in fullscreen)"},
    {"display", "brightness", 100, 50, 150, "percent"},
    {"graphics", "scaling_filter", 0, 0, 1, "0 smooth, 1 sharp (pixels as squares)"},
    {"graphics", "widescreen", 1, 0, 1, "16:9, from the next start"},
    {"graphics", "hud_edges", 0, 0, 1, "in game, HUD items at the sides out at the screen's edges (widescreen)"},
    {"graphics", "bloom", 1, 0, 1, "the soft glow around bright lights"},
    {"graphics", "fog", 1, 0, 1, "distance fog"},
    {"graphics", "per_pixel_lighting", 0, 0, 1, "1: lighting worked out for each pixel instead of each vertex"},
    {"graphics", "hdr", 0, 0, 1, "not yet"},
    {"graphics", "colors", 0, 0, 1, "0 original, 1 vivid"},
    {"graphics", "shader_mode", 0, 0, 2, "0 smooth (the ubershader), 1 fast (specialised shaders, from the next start), 2 precompile (not yet: as fast)"},
    {"graphics", "fast_loading", 1, 0, 1, "the disc's files into RAM at start (284 MB), from the next start"},
    {"controls", "invert_x", 0, 0, 1, "1: mouse and stick turn the camera the other way"},
    {"controls", "right_stick", 2, 0, 2, "0 camera, 1 pointer, 2 auto: the pointer while the game uses one"},
    {"controls", "aim_assist", 1, 0, 1, "not yet"},
    {"controls", "steer_sensitivity", 100, 25, 200, "percent: Flight of Fancy's turning by stick and keys"},
    {"controls", "aim_speed", 100, 25, 200, "percent: how fast the right stick moves the pointer"},
    {"controls", "rhythm_offset", 0, -100, 100, "Dance Inferno timing, ms: positive if presses feel late (whole frames)"},
    {"controls", "free_camera", 1, 0, 1, "1: the mouse and right stick turn the camera"},
    {"controls", "stick_sensitivity", 100, 10, 300, "percent: how fast the right stick turns the camera"},
    {"controls", "camera_recenter", 0, 0, 30, "seconds without turning before the camera eases back behind; 0 never"},
    {"display", "fov_attractions", 0, 0, 1, "1: the field of view applies in attractions too"},
    {"overlay", "show_fps", 0, 0, 2, "the performance overlay: 0 off, 1 compact (the frame rate), 2 detailed"},
    {"overlay", "corner", 0, 0, 3, "0 top left, 1 top right, 2 bottom left, 3 bottom right"},
    {"overlay", "size", 100, 75, 150, "percent"},
    {"overlay", "opacity", 100, 40, 100, "percent"},
    {"overlay", "screenshots", 0, 0, 1, "1: in screenshots too (no screenshot key yet)"},
    {"overlay", "key", 0x76, 0, 255, "the key that cycles Show FPS (a Windows virtual-key code; 0x76 is F7)"},
    {"input", "button_icons", 0, 0, 4, "button icons: 0 auto (the pad used last), 1 Xbox, 2 PlayStation, 3 Nintendo, 4 keyboard only"},
    {"input", "keyboard_player", 0, 0, 1, "0: keyboard and controller 1 are player 1; 1: keyboard alone is player 1, controllers are players 2-4"},
    {"audio", "volume_master", 100, 0, 100, "percent"},
    {"audio", "volume_music", 100, 0, 100, "percent: music and jingles"},
    {"audio", "volume_sfx", 100, 0, 100, "percent: sound effects"},
    {"audio", "volume_voices", 100, 0, 100, "percent: Pokemon voices"},
    {"audio", "voice_language", 0, 0, 1, "0 English, 1 Japanese (not yet)"},
    {"audio", "mute_background", 1, 0, 1, "1: silent while another window is in front"},
    {"audio", "audio_latency", 1, 0, 2, "sound delay: 0 low (48 ms), 1 normal (84 ms), 2 safe (132 ms)"},
    {"gameplay", "pause_background", 1, 0, 1, "1: the game pauses while another window is in front"},
    {"gameplay", "skip_logos", 0, 0, 1, "1: the title as soon as the park has loaded (the wrist strap screen cut short)"},
    {"gameplay", "save_backups", 1, 0, 1, "1: a copy of a save slot in backups\\ before each save (the last 10)"},
    /* Off by default: on, an unbound left trigger fast-forwarded the park
     * with the sound muted while held, which played as choppy sound. */
    {"gameplay", "speed_up", 0, 0, 2, "Tab / left trigger runs the park faster: 0 off, 1 2x, 2 3x"},
    {"gameplay", "speed_toggle", 0, 0, 1, "0: while held, 1: pressed on and off"},
    {"silly", "pokemon_size", 1, 0, 2, "0 mini (half), 1 normal, 2 giant (twice): the park's Pokemon, for looks"},
    {"silly", "voice_pitch", 0, -50, 50, "percent of the frequency: Pokemon voices higher or deeper"},
    {"silly", "mirror_world", 0, 0, 1, "the picture flipped left to right (and left and right swapped)"},
    {"silly", "victory_confetti", 0, 0, 1, "1: a burst of confetti when an attraction is cleared"},
    {"silly", "stereo_3d", 0, 0, 2, "0 off, 1 red-cyan, 2 side by side"},
};
int g_x[X_COUNT];
static int sXOverridden[X_COUNT];
static char sXFileValue[X_COUNT][32];

static int x_index(const char* section, const char* key) {
    int i;
    for (i = 0; i < X_COUNT; i++)
        if (!strcmp(kX[i].section, section) && !strcmp(kX[i].key, key)) return i;
    return -1;
}
static int x_clamp(int i, int v) { return v < kX[i].min ? kX[i].min : v > kX[i].max ? kX[i].max : v; }
int settings_x_default(int which) { return which >= 0 && which < X_COUNT ? kX[which].def : 0; }
int settings_x_overridden(int which) { return which >= 0 && which < X_COUNT && sXOverridden[which]; }

/* What a PC setting changes outside this file, done when it changes. */
static void apply_x(int which) {
    switch (which) {
    case X_WINDOW_MODE: g_settings.fullscreen = g_x[X_WINDOW_MODE] != 0; /* fall through */
    case X_RESOLUTION: settings_apply_window(); break;
    default: break;
    }
}

void settings_set_x(int which, int value) {
    if (which < 0 || which >= X_COUNT) return;
    g_x[which] = x_clamp(which, value);
    sXOverridden[which] = 0; /* changed here: saved */
    apply_x(which);
    settings_save();
}

static const char* const kActionIni[ACT_COUNT] = {
    "up", "down", "left", "right", "two", "one", "a", "b", "plus", "minus", "home", "shake", "shake2",
};

void settings_default_controls(void) {
#ifdef _WIN32
    static const int kKeys[ACT_COUNT][BIND_SLOTS] = {
        {'W', VK_UP}, {'S', VK_DOWN}, {'A', VK_LEFT}, {'D', VK_RIGHT},
        {VK_SPACE, 'X'}, {VK_LBUTTON, 'Z'}, {VK_RBUTTON, VK_RETURN}, {'V', VK_LCONTROL},
        {'P', 0}, {'M', 0}, {VK_HOME, 'H'}, {'Q', VK_LSHIFT}, {'E', 0},
    };
#else
    static const int kKeys[ACT_COUNT][BIND_SLOTS]; /* no keyboard */
#endif
    static const uint32_t kPad[ACT_COUNT][BIND_SLOTS] = {
        {XPAD_UP, 0}, {XPAD_DOWN, 0}, {XPAD_LEFT, 0}, {XPAD_RIGHT, 0},
        {XPAD_A, 0}, {XPAD_X, 0}, {XPAD_B, 0}, {XPAD_Y, XPAD_RT},
        {XPAD_START, 0}, {XPAD_BACK, 0}, {XPAD_RTHUMB, 0}, {XPAD_LB, 0}, {XPAD_RB, 0},
    };
    memcpy(g_settings.key, kKeys, sizeof(kKeys));
    memcpy(g_settings.pad, kPad, sizeof(kPad));
}

static int valid_msaa(int n) { return n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1; }
static int valid_aniso(int n) { return n >= 16 ? 16 : valid_msaa(n); }

/* ------------------------------------------------------------------------ */
/* Names */

void settings_key_name(int vk, char* out, size_t n) {
#ifdef _WIN32
    static const struct { int vk; const char* name; } kNames[] = {
        {VK_LBUTTON, "Left click"}, {VK_RBUTTON, "Right click"}, {VK_MBUTTON, "Middle click"},
        {VK_XBUTTON1, "Mouse 4"}, {VK_XBUTTON2, "Mouse 5"},
        {VK_LCONTROL, "Left Ctrl"}, {VK_RCONTROL, "Right Ctrl"}, {VK_LSHIFT, "Left Shift"},
        {VK_RSHIFT, "Right Shift"}, {VK_LMENU, "Left Alt"}, {VK_RMENU, "Right Alt"},
    };
    /* Keys whose scan code needs the extended bit to be told apart. */
    static const int kExtended[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
                                    VK_INSERT, VK_DELETE, VK_DIVIDE, VK_NUMLOCK};
    size_t i;
    LONG lp;
    if (!vk) {
        snprintf(out, n, "-");
        return;
    }
    for (i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        if (kNames[i].vk == vk) {
            snprintf(out, n, "%s", kNames[i].name);
            return;
        }
    }
    lp = (LONG)(MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC) << 16);
    for (i = 0; i < sizeof(kExtended) / sizeof(kExtended[0]); i++) {
        if (kExtended[i] == vk) lp |= 1 << 24;
    }
    if (!GetKeyNameTextA(lp, out, (int)n)) snprintf(out, n, "Key %02X", vk);
#else
    if (vk) snprintf(out, n, "Key %02X", vk);
    else snprintf(out, n, "-");
#endif
}

const char* settings_pad_name(uint32_t b) {
    static const struct { uint32_t bit; const char* name; } kNames[] = {
        {XPAD_UP, "D-pad up"}, {XPAD_DOWN, "D-pad down"}, {XPAD_LEFT, "D-pad left"}, {XPAD_RIGHT, "D-pad right"},
        {XPAD_START, "Start"}, {XPAD_BACK, "Back"}, {XPAD_LTHUMB, "L stick click"}, {XPAD_RTHUMB, "R stick click"},
        {XPAD_LB, "LB"}, {XPAD_RB, "RB"}, {XPAD_A, "A"}, {XPAD_B, "B"}, {XPAD_X, "X"}, {XPAD_Y, "Y"},
        {XPAD_LT, "LT"}, {XPAD_RT, "RT"},
    };
    size_t i;
    for (i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        if (kNames[i].bit == b) return kNames[i].name;
    }
    return "-";
}

void settings_binding_name(int act, int controller, char* out, size_t n) {
    int k;
    if (n) *out = 0;
    for (k = 0; k < BIND_SLOTS; k++) {
        if (controller && g_settings.pad[act][k]) {
            snprintf(out, n, "%s", settings_pad_name(g_settings.pad[act][k]));
            return;
        }
        if (!controller && g_settings.key[act][k]) {
            settings_key_name(g_settings.key[act][k], out, n);
            return;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* settings.ini and bindings.ini
 *
 * Both live in the data folder (%APPDATA%\Wonderpark, paths.c): settings.ini
 * holds the options, bindings.ini the keyboard and controller bindings.
 * WONDERPARK_SETTINGS and WONDERPARK_BINDINGS point at other files. Every
 * option can also be set by the environment variable WONDERPARK_<KEY> (the
 * key as in settings.ini, upper case: WONDERPARK_FRAME_RATE=60), which wins
 * over the file. An overridden value is used but never saved: the file keeps
 * its own. */

const char* wp_data_dir(void); /* paths.c */

static char sPath[1024], sBindPath[1024];

static const char* data_file(char* buf, size_t n, const char* env, const char* name) {
    if (!buf[0]) {
        const char* e = getenv(env);
        if (e && *e) snprintf(buf, n, "%s", e);
        else snprintf(buf, n, "%s/%s", wp_data_dir(), name);
    }
    return buf;
}

static const char* settings_path(void) {
    const char* old = getenv("RECOMP_SETTINGS"); /* the reference runtime's name, still accepted */
    if (!sPath[0] && old && *old && !getenv("WONDERPARK_SETTINGS")) snprintf(sPath, sizeof(sPath), "%s", old);
    return data_file(sPath, sizeof(sPath), "WONDERPARK_SETTINGS", "settings.ini");
}

static const char* bindings_path(void) {
    return data_file(sBindPath, sizeof(sBindPath), "WONDERPARK_BINDINGS", "bindings.ini");
}

/* The options that can be overridden from the environment: their section
 * and key in settings.ini, and the variable's name. */
static const struct { const char* section; const char* key; const char* env; } kOptions[] = {
    {"display", "fullscreen", "WONDERPARK_FULLSCREEN"},
    {"display", "scale", "WONDERPARK_SCALE"},
    {"display", "msaa", "WONDERPARK_MSAA"},
    {"display", "frame_rate", "WONDERPARK_FRAME_RATE"},
    {"display", "fov", "WONDERPARK_FOV"},
    {"display", "aniso", "WONDERPARK_ANISO"},
    {"display", "renderer", "WONDERPARK_RENDERER"},
    {"gameplay", "text_speed", "WONDERPARK_TEXT_SPEED"},
    {"gameplay", "berries", "WONDERPARK_BERRIES"},
    {"gameplay", "prices", "WONDERPARK_PRICES"},
    {"gameplay", "damage_dealt", "WONDERPARK_DAMAGE_DEALT"},
    {"gameplay", "damage_taken", "WONDERPARK_DAMAGE_TAKEN"},
    {"gameplay", "chase_speed", "WONDERPARK_CHASE_SPEED"},
    {"gameplay", "skip_movies", "WONDERPARK_SKIP_MOVIES"},
    {"updates", "check", "WONDERPARK_CHECK_UPDATES"},
    {"mouse", "sensitivity", "WONDERPARK_SENSITIVITY"},
    {"mouse", "invert_tilt", "WONDERPARK_INVERT_TILT"},
    {"controller", "rumble", "WONDERPARK_RUMBLE"},
    {"controller", "touch_controls", "WONDERPARK_TOUCH_CONTROLS"},
};
#define OPTIONS ((int)(sizeof(kOptions) / sizeof(kOptions[0])))

/* Per option: whether the environment overrides it, and the value the file
 * had (to write back instead of the override). */
static int sOverridden[OPTIONS];
static char sFileValue[OPTIONS][64];

static int option_index(const char* section, const char* key) {
    int i;
    for (i = 0; i < OPTIONS; i++)
        if (!strcmp(kOptions[i].section, section) && !strcmp(kOptions[i].key, key)) return i;
    return -1;
}

static uint32_t sSaves;

uint32_t settings_generation(void) { return sSaves; }

/* Saving never waits for the drive (it may be a spinning disk, and F1 runs
 * on the render thread): settings_save() only makes the files' text, and a
 * thread of its own writes it, the newest text of each file only, to a
 * temporary file that then replaces the real one (never half written).
 * settings_flush() (at exit) waits up to 2 s for a write still pending. */
#ifdef _WIN32
static CRITICAL_SECTION sWriteLock;
static HANDLE sWriteWake, sWriteIdle, sWriter;
static char* sPending[2];          /* settings.ini's, bindings.ini's text to write; NULL none */
static const char* sPendingPath[2];
static int sWriting;

static void write_now(const char* path, const char* text) {
    char tmp[1100];
    FILE* f;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!(f = fopen(tmp, "wb"))) {
        fprintf(stderr, "settings: couldn't write %s\n", tmp);
        return;
    }
    fputs(text, f);
    if (fclose(f) != 0 || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fprintf(stderr, "settings: couldn't replace %s\n", path);
        DeleteFileA(tmp);
    }
}

static DWORD WINAPI writer(LPVOID arg) {
    (void)arg;
    for (;;) {
        char* text[2];
        const char* path[2];
        int i;
        WaitForSingleObject(sWriteWake, INFINITE);
        EnterCriticalSection(&sWriteLock);
        for (i = 0; i < 2; i++) text[i] = sPending[i], path[i] = sPendingPath[i], sPending[i] = NULL;
        sWriting = text[0] || text[1];
        LeaveCriticalSection(&sWriteLock);
        for (i = 0; i < 2; i++)
            if (text[i]) write_now(path[i], text[i]), free(text[i]);
        EnterCriticalSection(&sWriteLock);
        sWriting = 0;
        if (!sPending[0] && !sPending[1]) SetEvent(sWriteIdle);
        LeaveCriticalSection(&sWriteLock);
    }
}

void settings_flush(void) {
    if (sWriter) WaitForSingleObject(sWriteIdle, 2000);
}

static void queue_write(int which, const char* path, char* text) {
    if (!sWriter) {
        InitializeCriticalSection(&sWriteLock);
        sWriteWake = CreateEventA(NULL, FALSE, FALSE, NULL);
        sWriteIdle = CreateEventA(NULL, TRUE, TRUE, NULL);
        sWriter = CreateThread(NULL, 0, writer, NULL, 0, NULL);
        if (!sWriter) {  /* no thread: written here instead */
            write_now(path, text);
            free(text);
            return;
        }
        SetThreadPriority(sWriter, THREAD_PRIORITY_BELOW_NORMAL);
        atexit(settings_flush);
    }
    EnterCriticalSection(&sWriteLock);
    free(sPending[which]); /* an older text not written yet: the newest wins */
    sPending[which] = text;
    sPendingPath[which] = path;
    ResetEvent(sWriteIdle);
    LeaveCriticalSection(&sWriteLock);
    SetEvent(sWriteWake);
}
#else
void settings_flush(void) {}
static void queue_write(int which, const char* path, char* text) {
    FILE* f = fopen(path, "w");
    (void)which;
    if (f) fputs(text, f), fclose(f);
    free(text);
}
#endif

/* Saves text to path (queue_write), replacing each overridden option's value
 * with the file's own (or leaving its line out if the file had none). */
static void write_file(const char* path, const char* text) {
    size_t cap = strlen(text) + 4096, used = 0;
    char* out = (char*)malloc(cap);
    char section[32] = "";
    const char* line = text;
    if (!out) return;
    out[0] = 0;
#define OUT(...) (used += (size_t)snprintf(out + used, used < cap ? cap - used : 0, __VA_ARGS__))
    while (*line) {
        const char* end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        char buf[512], key[32];
        int i = -1;
        snprintf(buf, sizeof(buf), "%.*s", (int)(len < sizeof(buf) ? len : sizeof(buf) - 1), line);
        int x = -1;
        if (buf[0] == '[') sscanf(buf, "[%31[^]]", section);
        else if (sscanf(buf, "%31[a-z_0-9] =", key) == 1) i = option_index(section, key), x = x_index(section, key);
        if (used + sizeof(buf) + 128 > cap) {
            char* grown = (char*)realloc(out, cap * 2);
            if (!grown) break;
            out = grown, cap *= 2;
        }
        if (i >= 0 && sOverridden[i]) {
            if (sFileValue[i][0]) OUT("%s = %s\n", key, sFileValue[i]);
        } else if (x >= 0 && sXOverridden[x]) {
            if (sXFileValue[x][0]) OUT("%s = %s\n", key, sXFileValue[x]);
        } else {
            OUT("%s", buf);
        }
        line += len;
    }
#undef OUT
    queue_write(path == sBindPath ? 1 : 0, path, out);
}

const char* settings_file_path(void) { return settings_path(); }

/* A growing text buffer for the files' contents. */
typedef struct { char* p; size_t len, cap; } Text;

static void tprintf(Text* t, const char* fmt, ...) {
    va_list ap;
    int n;
    for (;;) {
        size_t room = t->cap - t->len;
        va_start(ap, fmt);
        n = vsnprintf(t->p ? t->p + t->len : NULL, t->p ? room : 0, fmt, ap);
        va_end(ap);
        if (n >= 0 && t->p && (size_t)n < room) {
            t->len += (size_t)n;
            return;
        }
        t->cap = t->cap * 2 + (size_t)n + 256;
        t->p = (char*)realloc(t->p, t->cap);
    }
}

/* The PC settings (kX) of one section. */
static void emit_x(Text* s, const char* section) {
    int i;
    for (i = 0; i < X_COUNT; i++)
        if (!strcmp(kX[i].section, section)) tprintf(s, "%s = %d ; %s\n", kX[i].key, g_x[i], kX[i].note);
}

void settings_save(void) {
    Text s = {0}, b = {0};
    int i;
    sSaves++;
    tprintf(&s, "; Wonderpark settings. Each can be overridden by the environment variable\n");
    tprintf(&s, "; WONDERPARK_<KEY> (for example WONDERPARK_FRAME_RATE=60); see docs/DEVELOPMENT.md.\n\n");
    tprintf(&s, "[display]\n");
#ifndef __ANDROID__
    tprintf(&s, "fullscreen = %d\n", g_settings.fullscreen);
#endif
    tprintf(&s, "scale = %d ; internal resolution: 0 fits the window, or 1-%d\n", g_settings.scale, MAX_SCALE);
    tprintf(&s, "msaa = %d ; anti-aliasing samples: 1 (off), 2, 4 or 8\n", g_settings.msaa);
    tprintf(&s, "frame_rate = %d ; 30 (the game's own), 60, or 120, 144, 165, 240 (interpolated)\n",
            g_settings.frame_rate);
    tprintf(&s, "fov = %d ; field of view in the park, percent of the game's: %d-%d\n", g_settings.fov, FOV_MIN, FOV_MAX);
    tprintf(&s, "aniso = %d ; anisotropic texture filtering: 1 (off), 2, 4, 8 or 16\n", g_settings.aniso);
#ifndef __ANDROID__
    tprintf(&s, "renderer = %s ; opengl or vulkan, from the next start\n",
            g_settings.renderer == RENDERER_VULKAN ? "vulkan" : "opengl");
#endif
    emit_x(&s, "display");
    tprintf(&s, "\n[overlay]\n");
    emit_x(&s, "overlay");
    tprintf(&s, "\n[graphics]\n");
    emit_x(&s, "graphics");
    tprintf(&s, "\n[controls]\n");
    emit_x(&s, "controls");
    tprintf(&s, "\n[input]\n");
    emit_x(&s, "input");
    tprintf(&s, "\n[audio]\n");
    emit_x(&s, "audio");
    tprintf(&s, "\n[silly]\n");
    emit_x(&s, "silly");
    tprintf(&s, "\n[gameplay]\n");
    emit_x(&s, "gameplay");
    tprintf(&s, "skip_movies = %d ; 1: skip the story's video cutscenes (not working yet)\n", g_settings.skip_movies);
    tprintf(&s, "text_speed = %d ; dialogue: 1 (the game's), 2, 3 times as fast, or 0: a page at once\n",
            g_settings.text_speed);
    tprintf(&s, "berries = %d ; berries collected, percent of the game's: 50-500\n", g_settings.berries);
    tprintf(&s, "prices = %d ; berries spent buying, percent of the price: 0 (free) to 200\n", g_settings.prices);
    tprintf(&s, "damage_dealt = %d ; damage dealt in battles, percent of the game's: 25-400\n", g_settings.damage_dealt);
    tprintf(&s, "damage_taken = %d ; damage taken in battles, percent of the game's: 0 (none) to 200\n",
            g_settings.damage_taken);
    tprintf(&s, "chase_speed = %d ; the speed of the Pokemon you chase, percent of the game's: 50-150\n\n",
            g_settings.chase_speed);
    tprintf(&s, "[updates]\n");
    tprintf(&s, "check = %d ; 1: at start, ask GitHub whether a newer version is out\n\n", g_settings.check_updates);
#ifndef __ANDROID__ /* no mouse or keyboard */
    tprintf(&s, "[mouse]\n");
    tprintf(&s, "sensitivity = %.2f ; camera turn and pointer speed, times the default\n", g_settings.mouse_sensitivity);
    tprintf(&s, "invert_tilt = %d\n\n", g_settings.invert_tilt);
#endif
    tprintf(&s, "[controller]\n");
    tprintf(&s, "rumble = %d ; 0 off, 1 light, 2 normal, 3 strong\n", g_settings.rumble);
#ifdef __ANDROID__
    tprintf(&s, "touch_controls = %d ; 1 shown while the touch screen was used last, 0 never\n",
            g_settings.touch_controls);
#endif

    tprintf(&b, "; Wonderpark controls: two keys and two controller buttons per Wii Remote button.\n\n");
#ifndef __ANDROID__
    tprintf(&b, "[keyboard] ; Windows virtual-key codes (mouse buttons too), 0: none\n");
    for (i = 0; i < ACT_COUNT; i++) {
        char k1[32], k2[32];
        settings_key_name(g_settings.key[i][0], k1, sizeof(k1));
        settings_key_name(g_settings.key[i][1], k2, sizeof(k2));
        tprintf(&b, "%s = 0x%02X 0x%02X ; %s, %s\n", kActionIni[i], g_settings.key[i][0], g_settings.key[i][1], k1, k2);
    }
    tprintf(&b, "\n");
#endif
    tprintf(&b, "[controller] ; XInput buttons (0x10000: LT, 0x20000: RT), 0: none\n");
    for (i = 0; i < ACT_COUNT; i++) {
        tprintf(&b, "%s = 0x%04X 0x%04X ; %s, %s\n", kActionIni[i], g_settings.pad[i][0], g_settings.pad[i][1],
                settings_pad_name(g_settings.pad[i][0]), settings_pad_name(g_settings.pad[i][1]));
    }
    write_file(settings_path(), s.p);
    write_file(bindings_path(), b.p);
    free(s.p);
    free(b.p);
}

/* Applies one "key = value" line of section; *sawShake2 notes the second
 * shake button's lines (1 keyboard, 2 controller). */
static void parse_option(const char* section, const char* name, const char* p, int* sawShake2) {
    int i;
    char* end;
    unsigned long a, b;
    if ((i = x_index(section, name)) >= 0) {
        g_x[i] = x_clamp(i, strtol(p, NULL, 0)); /* (the overlay's key is written in hex) */
        return;
    }
    if (!strcmp(section, "display") && !strcmp(name, "show_fps")) {
        /* Before the performance overlay: 1 top left, 2 top right (the small badge). */
        int v = atoi(p);
        if (v > 0) g_x[X_SHOW_FPS] = 1, g_x[X_OVERLAY_CORNER] = v == 2 ? 1 : 0;
        return;
    }
    if (!strcmp(section, "display")) {
        if (!strcmp(name, "fullscreen")) g_settings.fullscreen = atoi(p) != 0;
        if (!strcmp(name, "scale")) g_settings.scale = atoi(p) < 0 ? 0 : atoi(p) > MAX_SCALE ? MAX_SCALE : atoi(p);
        if (!strcmp(name, "msaa")) g_settings.msaa = valid_msaa(atoi(p));
        if (!strcmp(name, "aniso")) g_settings.aniso = valid_aniso(atoi(p));
        if (!strcmp(name, "renderer")) g_settings.renderer = strstr(p, "vulkan") ? RENDERER_VULKAN : RENDERER_OPENGL;
        if (!strcmp(name, "frame_rate")) g_settings.frame_rate = valid_frame_rate(atoi(p));
        if (!strcmp(name, "fov")) {
            int v = atoi(p) / 5 * 5;
            g_settings.fov = v < FOV_MIN ? FOV_MIN : v > FOV_MAX ? FOV_MAX : v;
        }
    } else if (!strcmp(section, "gameplay")) {
        if (!strcmp(name, "skip_movies")) g_settings.skip_movies = atoi(p) != 0;
        if (!strcmp(name, "text_speed")) g_settings.text_speed = kTextSpeeds[choice(kTextSpeeds, COUNT(kTextSpeeds), atoi(p))];
        /* Wonderpark: any percent in the F1 tab's ranges (50-500, 0-200), not PokéPark 1's few choices. */
        if (!strcmp(name, "berries")) g_settings.berries = atoi(p) < 50 ? 50 : atoi(p) > 500 ? 500 : atoi(p);
        if (!strcmp(name, "prices")) g_settings.prices = atoi(p) < 0 ? 0 : atoi(p) > 200 ? 200 : atoi(p);
        /* The same for the battle and chase sliders (25-400, 0-200, 50-150; hle_modifiers.c). */
        if (!strcmp(name, "damage_dealt")) g_settings.damage_dealt = atoi(p) < 25 ? 25 : atoi(p) > 400 ? 400 : atoi(p);
        if (!strcmp(name, "damage_taken")) g_settings.damage_taken = atoi(p) < 0 ? 0 : atoi(p) > 200 ? 200 : atoi(p);
        if (!strcmp(name, "chase_speed")) g_settings.chase_speed = atoi(p) < 50 ? 50 : atoi(p) > 150 ? 150 : atoi(p);
    } else if (!strcmp(section, "mouse")) {
        if (!strcmp(name, "sensitivity")) {
            float s = (float)atof(p);
            g_settings.mouse_sensitivity = s < 0.1f ? 0.1f : s > 3.0f ? 3.0f : s;
        }
        if (!strcmp(name, "invert_tilt")) g_settings.invert_tilt = atoi(p) != 0;
    } else if (!strcmp(section, "updates") && !strcmp(name, "check")) {
        g_settings.check_updates = atoi(p) != 0;
    } else if (!strcmp(section, "controller") && !strcmp(name, "rumble")) {
        g_settings.rumble = atoi(p) < 0 ? 0 : atoi(p) > 3 ? 3 : atoi(p);
    } else if (!strcmp(section, "controller") && !strcmp(name, "touch_controls")) {
        g_settings.touch_controls = atoi(p) != 0;
    }
    if (!strcmp(section, "keyboard") || !strcmp(section, "controller")) {
        for (i = 0; i < ACT_COUNT; i++) {
            if (strcmp(name, kActionIni[i])) continue;
            if (i == ACT_SHAKE2) *sawShake2 |= section[0] == 'k' ? 1 : 2;
            a = strtoul(p, &end, 0);
            b = strtoul(end, NULL, 0);
            if (section[0] == 'k') {
                g_settings.key[i][0] = (int)(a & 0xFF);
                g_settings.key[i][1] = (int)(b & 0xFF);
            } else {
                g_settings.pad[i][0] = (uint32_t)a;
                g_settings.pad[i][1] = (uint32_t)b;
            }
        }
    }
}

/* Reads one ini file; 0 if it doesn't exist. */
static int read_file(const char* path, int* sawShake2) {
    char line[256], section[32] = "";
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line, *c, name[32];
        int i;
        if ((c = strchr(line, ';'))) *c = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            sscanf(p, "[%31[^]]", section);
            continue;
        }
        if (!(c = strchr(p, '='))) continue;
        *c = '\0';
        if (sscanf(p, "%31s", name) != 1) continue;
        p = c + 1;
        while (*p == ' ' || *p == '\t') p++;
        for (c = p + strlen(p); c > p && (c[-1] == '\n' || c[-1] == '\r' || c[-1] == ' ' || c[-1] == '\t'); c--) c[-1] = '\0';
        if ((i = option_index(section, name)) >= 0) snprintf(sFileValue[i], sizeof(sFileValue[i]), "%s", p);
        if ((i = x_index(section, name)) >= 0) snprintf(sXFileValue[i], sizeof(sXFileValue[i]), "%s", p);
        parse_option(section, name, p, sawShake2);
    }
    fclose(f);
    return 1;
}

void settings_load(void) {
    int sawShake2 = 0, i, found;
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.msaa = 1;
    g_settings.aniso = 1;
    g_settings.renderer = RENDERER_VULKAN; /* OpenGL draws if it can't start */
    g_settings.rumble = 2;
    g_settings.touch_controls = 1;
    g_settings.text_speed = 1;
    g_settings.berries = 100;
    g_settings.prices = 100;
    g_settings.damage_dealt = 100;
    g_settings.damage_taken = 100;
    g_settings.chase_speed = 100;
    g_settings.frame_rate = 30;
    g_settings.fov = 100;
    g_settings.mouse_sensitivity = 1.0f;
    g_settings.check_updates = 1;
    settings_default_controls();
    memset(sOverridden, 0, sizeof(sOverridden));
    memset(sFileValue, 0, sizeof(sFileValue));
    memset(sXOverridden, 0, sizeof(sXOverridden));
    memset(sXFileValue, 0, sizeof(sXFileValue));
    for (i = 0; i < X_COUNT; i++) g_x[i] = kX[i].def;

    found = read_file(settings_path(), &sawShake2);
    /* Files from before window_mode: the old fullscreen switch says it. */
    if (!sXFileValue[X_WINDOW_MODE][0]) g_x[X_WINDOW_MODE] = g_settings.fullscreen ? 1 : 0;
#ifdef _WIN32
    if (!found) {
        /* Before Wonderpark kept its files in %APPDATA%, settings.ini (with
         * the bindings in it) was beside the program: carried over once. */
        char old[1024], *slash;
        GetModuleFileNameA(NULL, old, sizeof(old) - 16);
        if ((slash = strrchr(old, '\\')) != NULL) {
            strcpy(slash + 1, "settings.ini");
            found = read_file(old, &sawShake2);
        }
    }
#endif
    read_file(bindings_path(), &sawShake2);

    /* WONDERPARK_<KEY> wins over the file. */
    for (i = 0; i < OPTIONS; i++) {
        const char* e = getenv(kOptions[i].env);
        if (!e || !*e) continue;
        sOverridden[i] = 1;
        parse_option(kOptions[i].section, kOptions[i].key, e, &sawShake2);
        fprintf(stderr, "settings: %s = %s (from %s)\n", kOptions[i].key, e, kOptions[i].env);
    }
    for (i = 0; i < X_COUNT; i++) {
        char env[64];
        const char* e;
        size_t k;
        snprintf(env, sizeof(env), "WONDERPARK_%s", kX[i].key);
        for (k = 11; env[k]; k++) env[k] = (char)(env[k] >= 'a' && env[k] <= 'z' ? env[k] - 32 : env[k]);
        if (!(e = getenv(env)) || !*e) continue;
        sXOverridden[i] = 1;
        g_x[i] = x_clamp(i, atoi(e));
        fprintf(stderr, "settings: %s = %d (from %s)\n", kX[i].key, g_x[i], env);
    }
    {
        /* WONDERPARK_FULLSCREEN (older) still works, as a window_mode override. */
        int f = option_index("display", "fullscreen");
        if (f >= 0 && sOverridden[f] && !sXOverridden[X_WINDOW_MODE]) {
            g_x[X_WINDOW_MODE] = g_settings.fullscreen ? 1 : 0;
            sXOverridden[X_WINDOW_MODE] = 1;
        }
    }
    g_settings.fullscreen = g_x[X_WINDOW_MODE] != 0;

    /* Files from before the second shake key gave its default (RB) to
     * shake: it moves over. */
    if (found && !(sawShake2 & 2)) {
        for (i = 0; i < ACT_COUNT; i++) {
            int k;
            for (k = 0; k < BIND_SLOTS; k++) {
                if (i != ACT_SHAKE2 && g_settings.pad[i][k] == g_settings.pad[ACT_SHAKE2][0]) g_settings.pad[i][k] = 0;
            }
        }
    }
    if (found && !(sawShake2 & 1)) {
        for (i = 0; i < ACT_COUNT; i++) {
            int k;
            for (k = 0; k < BIND_SLOTS; k++) {
                if (i != ACT_SHAKE2 && g_settings.key[i][k] == g_settings.key[ACT_SHAKE2][0]) g_settings.key[i][k] = 0;
            }
        }
    }
    /* Write the files out (both, or the one that's missing). */
    {
        FILE* f = fopen(bindings_path(), "r");
        int haveBindings = f != NULL;
        if (f) fclose(f);
        if (!found || !haveBindings) settings_save();
        else {
            f = fopen(settings_path(), "r");
            if (!f) settings_save();
            else fclose(f);
        }
    }
    apply_frame_rate();
    hle_set_fov(g_settings.fov);
    apply_gameplay();
}

void settings_step(int which, int d) {
    static const int kMsaa[] = {1, 2, 4, 8};
    int i;
    switch (which) {
    case SET_FULLSCREEN: g_settings.fullscreen = !g_settings.fullscreen; break;
    case SET_SCALE: g_settings.scale = (g_settings.scale + d + MAX_SCALE + 1) % (MAX_SCALE + 1); break;
    case SET_MSAA:
        for (i = 0; i < 4 && kMsaa[i] != g_settings.msaa; i++) {}
        g_settings.msaa = kMsaa[(i + d + 4) % 4];
        break;
    case SET_FRAME_RATE:
        for (i = 0; i < FRAME_RATES && kFrameRates[i] != g_settings.frame_rate; i++) {}
        g_settings.frame_rate = kFrameRates[(i + d + FRAME_RATES) % FRAME_RATES];
        apply_frame_rate();
        break;
    case SET_FOV:
        g_settings.fov = g_settings.fov + 5 * d < FOV_MIN ? FOV_MIN : g_settings.fov + 5 * d > FOV_MAX ? FOV_MAX : g_settings.fov + 5 * d;
        hle_set_fov(g_settings.fov);
        break;
    case SET_SENSITIVITY: {
        int tenths = (int)(g_settings.mouse_sensitivity * 10 + 0.5f) + d;
        g_settings.mouse_sensitivity = (tenths < 1 ? 1 : tenths > 30 ? 30 : tenths) / 10.0f;
        break;
    }
    case SET_INVERT: g_settings.invert_tilt = !g_settings.invert_tilt; break;
    case SET_UPDATES:
        g_settings.check_updates = !g_settings.check_updates;
        update_check();
        break;
    case SET_ANISO: {
        static const int kAniso[] = {1, 2, 4, 8, 16};
        for (i = 0; i < 5 && kAniso[i] != g_settings.aniso; i++) {}
        g_settings.aniso = kAniso[(i + d + 5) % 5];
        break;
    }
    case SET_RENDERER: g_settings.renderer = !g_settings.renderer; break;
    case SET_TOUCH: g_settings.touch_controls = !g_settings.touch_controls; break;
    case SET_RUMBLE:
        g_settings.rumble = (g_settings.rumble + d + 4) % 4;
        input_rumble_preview();
        break;
    case SET_TEXT_SPEED:
        g_settings.text_speed = kTextSpeeds[(choice(kTextSpeeds, COUNT(kTextSpeeds), g_settings.text_speed) + d +
                                             COUNT(kTextSpeeds)) % COUNT(kTextSpeeds)];
        apply_gameplay();
        break;
    case SET_BERRIES:
        g_settings.berries =
            kBerries[(choice(kBerries, COUNT(kBerries), g_settings.berries) + d + COUNT(kBerries)) % COUNT(kBerries)];
        apply_gameplay();
        break;
    case SET_PRICES:
        g_settings.prices =
            kPrices[(choice(kPrices, COUNT(kPrices), g_settings.prices) + d + COUNT(kPrices)) % COUNT(kPrices)];
        apply_gameplay();
        break;
    case SET_DAMAGE_DEALT:
        g_settings.damage_dealt = kDamageDealt[(choice(kDamageDealt, COUNT(kDamageDealt), g_settings.damage_dealt) + d +
                                                COUNT(kDamageDealt)) % COUNT(kDamageDealt)];
        apply_gameplay();
        break;
    case SET_DAMAGE_TAKEN:
        g_settings.damage_taken = kDamageTaken[(choice(kDamageTaken, COUNT(kDamageTaken), g_settings.damage_taken) + d +
                                                COUNT(kDamageTaken)) % COUNT(kDamageTaken)];
        apply_gameplay();
        break;
    case SET_CHASE_SPEED:
        g_settings.chase_speed = kChaseSpeeds[(choice(kChaseSpeeds, COUNT(kChaseSpeeds), g_settings.chase_speed) + d +
                                               COUNT(kChaseSpeeds)) % COUNT(kChaseSpeeds)];
        apply_gameplay();
        break;
    default: return;
    }
    settings_save();
}

void settings_bind(int act, int slot, int vk, uint32_t pad) {
    int i, k;
    for (i = 0; i < ACT_COUNT; i++) {
        for (k = 0; k < BIND_SLOTS; k++) {
            if (vk && g_settings.key[i][k] == vk) g_settings.key[i][k] = 0;
            if (pad && g_settings.pad[i][k] == pad) g_settings.pad[i][k] = 0;
        }
    }
    if (vk) g_settings.key[act][slot] = vk;
    else g_settings.pad[act][slot] = pad;
    settings_save();
}
