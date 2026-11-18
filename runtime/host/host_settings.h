/* From MildRain479/PokePark-Wiicompiled recomp/runtime/host_settings.h (CC0). */
/* User settings, kept in settings.ini and bindings.ini in the data folder
 * (%APPDATA%\Wonderpark; settings.c), overridable by WONDERPARK_<KEY>, and changed in
 * Options in the pause menu (and the settings menu, menu_win32.c;
 * settings.c reads and writes them). For the
 * platform layer (recomp/boot, recomp/gpu); the game patches don't see
 * them. RECOMP_SETTINGS=<file> reads and writes another file instead. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* What can be bound: the Wii Remote's buttons, held sideways. */
enum {
    ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT,
    ACT_2, ACT_1, ACT_A, ACT_B, ACT_PLUS, ACT_MINUS, ACT_HOME, ACT_SHAKE, ACT_SHAKE2,
    ACT_COUNT
};
#define BIND_SLOTS 2

/* Controller buttons: XInput's, plus the triggers above its 16 bits. */
enum {
    XPAD_UP = 0x0001, XPAD_DOWN = 0x0002, XPAD_LEFT = 0x0004, XPAD_RIGHT = 0x0008,
    XPAD_START = 0x0010, XPAD_BACK = 0x0020, XPAD_LTHUMB = 0x0040, XPAD_RTHUMB = 0x0080,
    XPAD_LB = 0x0100, XPAD_RB = 0x0200, XPAD_A = 0x1000, XPAD_B = 0x2000, XPAD_X = 0x4000, XPAD_Y = 0x8000,
    XPAD_LT = 0x10000, XPAD_RT = 0x20000,
};

#define MAX_SCALE 6
enum { RENDERER_OPENGL, RENDERER_VULKAN };
#define MAX_MSAA 8

typedef struct {
    int fullscreen;          /* borderless, on the window's monitor */
    int scale;               /* internal resolution multiplier; 0 picks one for the window */
    int msaa;                /* anti-aliasing samples: 1 (off), 2, 4 or 8 */
    int frame_rate;          /* 30 (the game's own), 60, or 120-240 (interpolated) */
    int fov;                 /* field of view, percent of the game's (tangent of half the angle) */
    int aniso;               /* anisotropic filtering: 1 (off), 2, 4, 8 or 16 */
    int renderer;            /* RENDERER_*: what draws, from the next start */
    int rumble;              /* controller rumble: 0 off, 1 light, 2 normal, 3 strong */
    int touch_controls;      /* Android: the touch screen's controls, shown while it was used last; 0 never */
    int text_speed;          /* dialogue: 1 (the game's), 2, 3, or 0 (a page at once) */
    int berries;             /* berries collected, percent of the game's */
    int prices;              /* berries spent, percent of the game's */
    int damage_dealt;        /* damage Pikachu deals in battles, percent of the game's */
    int damage_taken;        /* damage Pikachu takes in battles, percent of the game's */
    int chase_speed;         /* the speed of the Pokémon Pikachu chases, percent of the game's */
    float mouse_sensitivity; /* camera turn per pixel, times the default */
    int invert_tilt;         /* mouse up looks down */
    int check_updates;       /* ask GitHub for a newer release at start (update.c) */
    int skip_movies;         /* skip the story's video cutscenes (hle_movie.c) */
    int key[ACT_COUNT][BIND_SLOTS];      /* Windows virtual-key codes (mouse buttons too); 0 = none */
    uint32_t pad[ACT_COUNT][BIND_SLOTS]; /* controller buttons (XPAD_*); 0 = none */
} HostSettings;

extern HostSettings g_settings;

/* Wonderpark's PC settings beyond the ones above (docs/handoff/SETTINGS.md,
 * the F1 tab's six categories), each a whole number in settings.ini under
 * its section, overridable by WONDERPARK_<KEY>. Choices are 0-based
 * indexes; percents are percents. Some are stored but not working yet (the
 * F1 tab shows those as "Coming soon"). */
enum {
    /* [display] */
    X_WINDOW_MODE,      /* 0 windowed, 1 borderless, 2 fullscreen (the screen set to X_RESOLUTION's size) */
    X_RESOLUTION,       /* window size: 0 1280x720, 1 1600x900, 2 1920x1080, 3 2560x1440, 4 match screen */
    X_VSYNC,            /* 0, 1 */
    X_BRIGHTNESS,       /* 50-150 % */
    /* [graphics] */
    X_SCALING_FILTER,   /* 0 smooth, 1 sharp */
    X_WIDESCREEN,       /* 0, 1 (from the next start) */
    X_HUD_EDGES, X_BLOOM, X_FOG, X_PER_PIXEL_LIGHTING, X_HDR,
    X_COLORS,           /* 0 original, 1 vivid */
    X_SHADER_MODE,      /* 0 smooth, 1 fast, 2 precompile */
    X_FAST_LOADING,
    /* [controls] */
    X_INVERT_X,
    X_RIGHT_STICK,      /* 0 camera, 1 pointer, 2 auto */
    X_AIM_ASSIST,
    X_STEER_SENSITIVITY, X_AIM_SPEED, /* 25-200 % */
    X_RHYTHM_OFFSET,    /* -100..100 ms */
    X_FREE_CAMERA,      /* the mouse and right stick turn the camera (hle_camera.c) */
    X_STICK_SENSITIVITY, /* 10-300 %: the right stick's turning */
    X_CAMERA_RECENTER,  /* seconds without turning before the camera eases back; 0 never */
    /* [input] */
    X_BUTTON_ICONS,     /* 0 auto, 1 Xbox, 2 PlayStation, 3 Nintendo, 4 keyboard only (pad_icons.cpp) */
    /* [audio] */
    X_VOLUME_MASTER, X_VOLUME_MUSIC, X_VOLUME_SFX, X_VOLUME_VOICES, /* 0-100 % */
    X_VOICE_LANGUAGE,   /* 0 English, 1 Japanese */
    X_MUTE_BACKGROUND,
    X_AUDIO_LATENCY,    /* 0 low, 1 normal, 2 safe */
    /* [gameplay] */
    X_PAUSE_BACKGROUND, X_SKIP_LOGOS,
    X_SAVE_BACKUPS,     /* copy a save slot to backups\ before it's written (nand.c) */
    X_SPEED_UP,         /* the speed-up key's speed: 0 off, 1 2x, 2 3x (input_win32.c) */
    X_SPEED_TOGGLE,     /* 0 while held, 1 on and off by presses */
    /* [silly] */
    X_POKEMON_SIZE,     /* 0 mini, 1 normal, 2 giant */
    X_VOICE_PITCH,      /* -50..50 % */
    X_MIRROR_WORLD, X_VICTORY_CONFETTI,
    X_STEREO_3D,        /* 0 off, 1 red-cyan, 2 side by side */
    X_COUNT
};
extern int g_x[X_COUNT];
/* Sets one (clamped to its range), applies it and saves. */
void settings_set_x(int which, int value);
int settings_x_default(int which);
/* Whether WONDERPARK_<KEY> overrides it (the F1 tab says so). */
int settings_x_overridden(int which);
/* Called when a setting changes, for parts that apply it (window, audio). */
void settings_apply_window(void); /* window_win32.c */

void settings_load(void);
void settings_save(void);
void settings_default_controls(void);

/* The name of the first key (or, with controller, controller button)
 * bound to action act, as the menu shows it; "" if none is. */
void settings_binding_name(int act, int controller, char* out, size_t n);
/* How many times the settings have been saved (so changed). */
uint32_t settings_generation(void);
/* Whether the controller was used more recently than the keyboard and
 * mouse (input_win32.c). */
int input_controller_last(void);
/* A short buzz at the rumble setting's strength (input_win32.c). */
void input_rumble_preview(void);
/* Sets up the button prompt icons (prompts.c). */
void prompts_init(void);

/* What settings_step changes. */
enum { SET_FULLSCREEN, SET_SCALE, SET_MSAA, SET_FRAME_RATE, SET_FOV, SET_SENSITIVITY, SET_INVERT, SET_ANISO,
       SET_RUMBLE, SET_TEXT_SPEED, SET_BERRIES, SET_PRICES,
       SET_DAMAGE_DEALT, SET_DAMAGE_TAKEN, SET_CHASE_SPEED, SET_RENDERER, SET_TOUCH, SET_UPDATES };
/* Changes a setting a step (d: -1 or 1; on and off settings flip), applies
 * and saves it. */
void settings_step(int which, int d);
/* Binds key vk (or, if 0, controller button pad) to action act's slot,
 * taking it off any other (one key or button does one thing), and saves. */
void settings_bind(int act, int slot, int vk, uint32_t pad);
/* Names, as the menus show them: "-" for none. */
void settings_key_name(int vk, char* out, size_t n);
const char* settings_pad_name(uint32_t b);

/* The update check (update.c): update_check starts it (if the setting is
 * on and it hasn't found an answer), update_state says how it went, and,
 * when a newer release is out, update_tag is its version ("v1.2.3") and
 * update_open opens its page in the browser. */
enum { UPDATE_OFF, UPDATE_CHECKING, UPDATE_CURRENT, UPDATE_FAILED, UPDATE_NEWER };
void update_check(void);
int update_state(void);
const char* update_tag(void);
void update_open(void);

/* The Options tab of the pause menu (options.c), as for the settings
 * menu below; used by it while that is closed. */
int options_is_open(void);
int options_window_message(unsigned msg, uintptr_t wp, intptr_t lp);
void options_controller(uint32_t buttons, uint32_t nav);
const uint32_t* options_draw(int w, int h);
/* How many times options_draw has drawn its image again (it returns the
 * same buffer while nothing changed). */
unsigned options_draws(void);
/* Android: a tap at window pixel (x, y); returns 1 if the tab used it. */
int options_touch(int x, int y);
/* Scripted presses (HOST_PAD_* bits held), as the controller's; returns 1
 * if the tab is open. */
int options_script(uint32_t held);
/* Android: the touch controls' buttons held, as for options_script. */
int options_touch_buttons(uint32_t held);

/* The settings menu (menu_win32.c; in public builds, without
 * RECOMP_DEBUG_MENU, these are the Options tab's alone). */
int menu_is_open(void);
/* Takes the window's keyboard and mouse messages while open; returns 1 if
 * it used the message. */
int menu_window_message(void* wnd, unsigned msg, uintptr_t wp, intptr_t lp);
/* The controller's buttons held (XPAD_*), at every input poll: without
 * and with the left stick's directions as the d-pad's. Opens and closes
 * the menu on Back+Start, and works it while open. */
void menu_controller(uint32_t buttons, uint32_t nav);
/* The menu as a w x h BGRA image with alpha, top row first, to lay over
 * the picture; NULL when closed. */
const uint32_t* menu_draw(int w, int h);
/* Android: changes whenever menu_draw's image may have, so a device can
 * leave an unchanged one where it is (input_android.c). */
unsigned menu_generation(void);
