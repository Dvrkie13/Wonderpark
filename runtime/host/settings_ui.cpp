// The F1 settings tab (docs/settings/ingame-f1-settings.html): drawn by the
// runtime over the game, no game assets. F1 (or a controller's Back + Start)
// opens it; the game pauses behind it (gx_frames.c show(): its clock stops
// and the last frame is shown again); changes apply at once and are saved to
// settings.ini / bindings.ini (settings.c). F1, Esc, the Close button or the
// controller's B close it.
//
// Drawn with Direct2D and DirectWrite in software into an image the size of
// the window, which the renderer lays over the frame (overlay_win32.c
// menu_draw, gl_backend.c); redrawn only when something changes. Fonts:
// Fredoka and Nunito (SIL OFL), built into the executable (wonderpark.rc).
//
// The look: the "Explorer's Notebook" (a green cover with gold corners, a
// brown leather page with paw prints, colored index tabs, washi-tape section
// titles, sticker buttons with navy ink outlines and hard shadows, small
// tilts: one rotation matrix per element). The game's own Options page rows
// (below) keep the game's look.
//
// Settings whose feature isn't built yet stay visible, dimmed, with a red
// "Coming soon" stamp (a section title can carry it too); a choice that
// isn't is a dashed pill marked "soon".
//
// The Friend Book (the pink ribbon, F2, or Y on a controller) is a view of
// its own: the index tabs become the park's areas, the Pokémon are cards,
// and a card opens a hint card with the Pad's own notes from the disc.
//
// Controls: mouse (click, drag sliders, wheel scrolls); keyboard Up/Down
// move, Left/Right change, Enter/Space press, Q/E (or PageUp/PageDown)
// switch tab; controller d-pad / left stick, A, B (close), LB/RB, Y.
// Rebinding: press a key cap, then the key, mouse button or controller
// button; Esc cancels, Backspace clears.
//
// WONDERPARK_F1_SHOT=<prefix> (tests): opens the tab WONDERPARK_F1_SHOT_AT
// seconds after the game first shows a frame (default 1) and saves
// <prefix>-<n>[b]-<category>.bmp for each category (b: scrolled to its end),
// then the Friend Book (<n> 8 everyone, 9 the Cove Area, 10 a hint card,
// 10b its "show more"), the frame as shown, then closes it.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite_3.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "host_input.h"
#include "host_settings.h"
#include "pad_icons.h"
#include "pad_sdl.h"
char g_dump_next[512];  // a frame for gl_backend.c to save as shown (test shots)
extern int g_picture_rect[4];  // gl_backend.c: where the game's picture is in the window
int win_has_focus(void);
int hle_widescreen(void);  // nand.c: the game runs in its 16:9 mode
extern int g_wp_headless;
void input_release_mouse(void);          /* input_win32.c */
unsigned gxb_game_frames(void);          /* gx_frames.c */
int hle_game_text_speed(void);           /* hle_options.c: 0 Normal, 1 Fast, -1 no save loaded */
void hle_set_game_text_speed(int fast);
int dvd_voices_ready(void);              /* dvd.c: the Japanese voices are set up */
void* win_handle(void);
}

using Microsoft::WRL::ComPtr;
using D2D1::ColorF;
using D2D1::Matrix3x2F;
using D2D1::Point2F;
using D2D1::RectF;

namespace {

// ------------------------------------------------------------------------
// Basics

const float PI = 3.14159265f;

D2D1_COLOR_F rgb(uint32_t c, float a = 1) {
    return ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a);
}
float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
float bezier(float x1, float y1, float x2, float y2, float x) {
    x = clamp01(x);
    float t = x;
    for (int i = 0; i < 8; i++) {
        float cx = 3 * x1 * t * (1 - t) * (1 - t) + 3 * x2 * t * t * (1 - t) + t * t * t - x;
        float dx = 3 * x1 * (1 - t) * (1 - t) + 6 * (x2 - x1) * t * (1 - t) + 3 * (1 - x2) * t * t;
        if (std::fabs(cx) < 1e-5f || std::fabs(dx) < 1e-6f) break;
        t = clamp01(t - cx / dx);
    }
    return 3 * y1 * t * (1 - t) * (1 - t) + 3 * y2 * t * t * (1 - t) + t * t * t;
}
double now_s() {
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f), QueryPerformanceCounter(&t0);
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
}
std::wstring widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}
bool reduced_motion() {
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    return !anim;
}

// ------------------------------------------------------------------------
// The settings, as rows

// Kinds of line. NOTE: a line of text (with a label: a row with nothing to
// change). TEXTBOX: a text field (shown only). PLAYERS: the four player
// slots (Multiplayer). The Friend Book's (built in build_fb_rows, below
// with the friends list): FBTOP (the ring and counts), PLACES (the place
// chips; items: place ids, -1 all), PLACEHEAD (a place's title; act: the
// place), CARDS (up to three Pokémon cards; items: indices in g_friends),
// TIP (a sticky note).
enum Kind { HEAD, NOTE, CHOICE, TOGGLE, SLIDER, TEXTBOX, BINDHEAD, BIND, FIXEDBIND, PLAYERS, FBTOP, PLACES, PLACEHEAD, CARDS, TIP };

struct Row {
    Kind kind = HEAD;
    std::wstring label, desc;
    std::vector<std::wstring> opts;
    unsigned soonOpts = 0;  // choices not available yet (bit per option)
    int min = 0, max = 100, step = 5;
    std::wstring unit;
    bool sign = false;
    bool soon = false;      // the whole row (or a HEAD's section): "Coming soon"
    bool mini = false;        // CHOICE: the Friend Book's smaller filter pills
    int act = -1;           // BIND: the Wii Remote button (ACT_*); PLACEHEAD: the place
    uint32_t color = 0;     // PLACEHEAD: the place's color
    std::vector<int> items; // CARDS, PLACES
    std::function<int()> get;
    std::function<void(int)> set;
    std::function<void()> reset;
    // Available only sometimes (the game's own settings need a loaded save):
    // while off() is true the row is greyed with tag on its stamp instead of
    // "Coming soon".
    std::function<bool()> off;
    std::wstring tag;
};

struct Category {
    std::wstring id, name, desc;
    uint32_t gem, acc, ink;
    std::vector<Row> rows;
};

std::vector<Category> g_cats;
Category g_fb;        // the Friend Book's page (build_fb_rows)
bool g_fbDirty = true;  // its rows want building again

Row head(const wchar_t* t, bool soon = false) {
    Row r;
    r.kind = HEAD;
    r.label = t;
    r.soon = soon;
    return r;
}
Row note(const wchar_t* t) {
    Row r;
    r.kind = NOTE;
    r.desc = t;
    return r;
}

// A PC setting from the table in settings.c.
Row xrow(Kind k, int x, const wchar_t* label, const wchar_t* desc, std::vector<std::wstring> opts = {},
         bool soon = false) {
    Row r;
    r.kind = k;
    r.label = label;
    r.desc = desc ? desc : L"";
    r.opts = opts;
    r.soon = soon;
    r.get = [x] { return g_x[x]; };
    r.set = [x](int v) { settings_set_x(x, v); };
    r.reset = [x] { settings_set_x(x, settings_x_default(x)); };
    return r;
}
Row xslider(int x, const wchar_t* label, const wchar_t* desc, int mn, int mx, int step, const wchar_t* unit,
            bool soon, bool sign = false) {
    Row r = xrow(SLIDER, x, label, desc, {}, soon);
    r.min = mn, r.max = mx, r.step = step, r.unit = unit, r.sign = sign;
    return r;
}
// A setting that isn't built yet: shown, "Coming soon", value v.
Row soon_row(Kind k, const wchar_t* label, const wchar_t* desc, std::vector<std::wstring> opts = {}, int v = 0) {
    Row r;
    r.kind = k;
    r.label = label;
    r.desc = desc ? desc : L"";
    r.opts = opts;
    r.soon = true;
    r.get = [v] { return v; };
    return r;
}
// A choice backed by g_settings: values[] are what the field holds.
Row vchoice(const wchar_t* label, const wchar_t* desc, std::vector<std::wstring> opts, std::vector<int> values,
            int* field, int def, std::function<void()> after = nullptr) {
    Row r;
    r.kind = CHOICE;
    r.label = label;
    r.desc = desc ? desc : L"";
    r.opts = opts;
    r.get = [=] {
        for (size_t i = 0; i < values.size(); i++)
            if (values[i] == *field) return (int)i;
        return 0;
    };
    r.set = [=](int i) {
        *field = values[i];
        if (after) after();
        settings_save();
    };
    r.reset = [=] {
        *field = def;
        if (after) after();
        settings_save();
    };
    return r;
}

const wchar_t* const kBindNames[ACT_COUNT] = {
    L"Move up", L"Move down", L"Move left", L"Move right", L"Jump (2)", L"Attack / charge (1)",
    L"Talk / confirm (A)", L"Back (B)", L"Menu (+)", L"− button", L"Home", L"Special move (shake)",
    L"Shake, second key",
};

void build_rows() {
    g_cats.clear();
    // Display
    {
        Category c{L"display", L"Display", L"Window, resolution and frame rate", 0x4a7fe0, 0x79a5f0, 0xffffff, {}};
        c.rows.push_back(xrow(CHOICE, X_WINDOW_MODE, L"Window",
                              L"Fullscreen sets the screen to the resolution below; Borderless keeps the desktop's.",
                              {L"Windowed", L"Borderless", L"Fullscreen"}));
        c.rows.push_back(xrow(CHOICE, X_RESOLUTION, L"Resolution", L"The window's size, when in a window.",
                              {L"1280×720", L"1600×900", L"1920×1080", L"2560×1440", L"Match screen"}));
        Row fr = vchoice(L"Frame rate",
                         L"60 draws every step of the game. Above 60 the frames between are blended from the last two (smoother turning); never more than your screen shows. Movies stay at 30.",
                         {L"30 (original)", L"60", L"120", L"144", L"240"}, {30, 60, 120, 144, 240}, &g_settings.frame_rate, 30);
        c.rows.push_back(fr);
        c.rows.push_back(xrow(TOGGLE, X_VSYNC, L"V-Sync", L"Stops the picture from tearing in fullscreen. A window never tears."));
        Row rd;
        rd.kind = CHOICE;
        rd.label = L"Renderer";
        rd.desc = L"OpenGL draws the game. Vulkan isn't built yet.";
        rd.opts = {L"OpenGL", L"Vulkan"};
        rd.soonOpts = 2;
        rd.get = [] { return 0; };
        rd.set = [](int) {};
        rd.reset = [] {};
        c.rows.push_back(rd);
        c.rows.push_back(xrow(CHOICE, X_SHOW_FPS, L"Show FPS", L"The frames shown each second, in a corner: green at your frame rate, yellow a little under, red well under.",
                              {L"Off", L"Top left", L"Top right"}));
        c.rows.push_back(xslider(X_BRIGHTNESS, L"Brightness", nullptr, 50, 150, 5, L"%", false));
        g_cats.push_back(c);
    }
    // Graphics
    {
        Category c{L"graphics", L"Graphics", L"How sharp and shiny the park looks", 0x8b5bd2, 0xb596f0, 0xffffff, {}};
        c.rows.push_back(head(L"View"));
        Row fov;
        fov.kind = SLIDER;
        fov.label = L"Field of view";
        fov.desc = L"How wide the camera sees in the park and its story scenes. 100% is the game's own.";
        fov.min = 70, fov.max = 150, fov.step = 5, fov.unit = L"%";
        fov.get = [] { return g_settings.fov; };
        fov.set = [](int v) {
            g_settings.fov = v;
            settings_save();
        };
        fov.reset = [] {
            g_settings.fov = 100;
            settings_save();
        };
        c.rows.push_back(fov);
        c.rows.push_back(xrow(TOGGLE, X_FOV_ATTRACTIONS, L"Field of view in attractions",
                              L"Uses it in the attractions too (they keep their own view when off)."));
        {
            Row dd = soon_row(SLIDER, L"Draw distance", L"How far away the park's Pokémon and props still show.", {}, 100);
            dd.min = 50, dd.max = 300, dd.step = 10, dd.unit = L"%";
            c.rows.push_back(dd);
        }
        c.rows.push_back(head(L"Sharpness"));
        c.rows.push_back(vchoice(L"Internal resolution",
                                 L"How many pixels the game draws before fitting your screen. Takes effect when the tab closes.",
                                 {L"Auto", L"Native", L"2×", L"3×", L"4×", L"6×"}, {0, 1, 2, 3, 4, 6}, &g_settings.scale, 0));
        c.rows.push_back(vchoice(L"Anti-aliasing", L"Smooths jagged edges. Takes effect when the tab closes.",
                                 {L"Off", L"2×", L"4×", L"8×"}, {1, 2, 4, 8}, &g_settings.msaa, 1));
        c.rows.push_back(vchoice(L"Texture filtering", L"Keeps paths and floors crisp at an angle.",
                                 {L"Default", L"4×", L"16×"}, {1, 4, 16}, &g_settings.aniso, 1));
        c.rows.push_back(xrow(CHOICE, X_SCALING_FILTER, L"Scaling filter", nullptr, {L"Smooth", L"Sharp"}));
        c.rows.push_back(head(L"Effects"));
        c.rows.push_back(xrow(TOGGLE, X_WIDESCREEN, L"Widescreen",
                              L"The game's own 16:9 mode; wider screens see more to the sides. From the next start."));
        c.rows.push_back(xrow(TOGGLE, X_HUD_EDGES, L"HUD at screen edges",
                              L"In game, moves HUD items at the sides (the minimap) out to the screen's edges, in widescreen."));
        c.rows.push_back(xrow(TOGGLE, X_BLOOM, L"Bloom", L"The soft glow around bright lights."));
        c.rows.push_back(xrow(TOGGLE, X_FOG, L"Fog", L"The haze that softens faraway parts of the park."));
        c.rows.push_back(xrow(TOGGLE, X_PER_PIXEL_LIGHTING, L"Per-pixel lighting",
                              L"Lights each pixel instead of each corner: smoother shading on Pokémon and round things. A little more work for the graphics card."));
        c.rows.push_back(xrow(TOGGLE, X_HDR, L"HDR", L"Brighter highlights on an HDR screen.", {}, true));
        c.rows.push_back(xrow(CHOICE, X_COLORS, L"Colors", nullptr, {L"Original", L"Vivid"}));
        c.rows.push_back(head(L"Performance"));
        {
            Row sm = xrow(CHOICE, X_SHADER_MODE, L"Shader compilation",
                          L"Smooth: one shader for everything, never stutters. Fast: a shader made for each look, lighter on the graphics card, but the first sight of each may hitch (AMD cards). From the next start.",
                          {L"Smooth", L"Fast"});
            sm.get = [] { return std::min(g_x[X_SHADER_MODE], 1); };
            c.rows.push_back(sm);
        }
        c.rows.push_back(xrow(TOGGLE, X_FAST_LOADING, L"Fast loading",
                              L"Reads the game's files into RAM at start (about 280 MB), so areas load without waiting for the drive. From the next start."));
        g_cats.push_back(c);
    }
    // Controls
    {
        Category c{L"controls", L"Controls", L"Keyboard, mouse and controller", 0xf6d44a, 0xfbe27a, 0x1b2440, {}};
        c.rows.push_back(head(L"Camera and aiming"));
        c.rows.push_back(xrow(TOGGLE, X_FREE_CAMERA, L"Free camera",
                              L"The mouse and right stick turn the camera in the park. B puts it back behind you."));
        Row cam;
        cam.kind = SLIDER;
        cam.label = L"Camera speed";
        cam.desc = L"For the mouse (and the pointer in minigames).";
        cam.min = 25, cam.max = 200, cam.step = 5, cam.unit = L"%";
        cam.get = [] { return (int)std::lround(g_settings.mouse_sensitivity * 100); };
        cam.set = [](int v) {
            g_settings.mouse_sensitivity = v / 100.0f;
            settings_save();
        };
        cam.reset = [] {
            g_settings.mouse_sensitivity = 1.0f;
            settings_save();
        };
        c.rows.push_back(cam);
        c.rows.push_back(xslider(X_STICK_SENSITIVITY, L"Stick speed", L"How fast the right stick turns the camera.", 10, 300,
                                 10, L"%", false));
        c.rows.push_back(xrow(TOGGLE, X_INVERT_X, L"Invert left / right", nullptr));
        Row iy;
        iy.kind = TOGGLE;
        iy.label = L"Invert up / down";
        iy.get = [] { return g_settings.invert_tilt; };
        iy.set = [](int v) {
            g_settings.invert_tilt = v;
            settings_save();
        };
        iy.reset = [] {
            g_settings.invert_tilt = 0;
            settings_save();
        };
        c.rows.push_back(iy);
        c.rows.push_back(xslider(X_CAMERA_RECENTER, L"Recenter after", L"Seconds without turning before the camera eases back. Off: never.",
                                 0, 30, 1, L" s", false));
        c.rows.push_back(xrow(CHOICE, X_RIGHT_STICK, L"Right stick", L"Auto moves the pointer when the game uses one (aiming, pointing at menus), else the camera.",
                              {L"Camera", L"Pointer", L"Auto"}));
        c.rows.push_back(xrow(TOGGLE, X_AIM_ASSIST, L"Aim assist", L"Controller aiming leans toward targets (Cake Contraption).", {}, true));
        c.rows.push_back(vchoice(L"Rumble", nullptr, {L"Off", L"Light", L"Normal", L"Strong"}, {0, 1, 2, 3},
                                 &g_settings.rumble, 2, [] { input_rumble_preview(); }));
        c.rows.push_back(head(L"Buttons"));
        c.rows.push_back(xrow(CHOICE, X_BUTTON_ICONS, L"Button icons",
                              L"The buttons the game's prompts and this page show. Auto: your controller's own. Keys show while you use the keyboard.",
                              {L"Auto", L"Xbox", L"PlayStation", L"Nintendo", L"Keyboard only"}));
        Row bh;
        bh.kind = BINDHEAD;
        c.rows.push_back(bh);
        for (int a = 0; a < ACT_COUNT; a++) {
            Row b;
            b.kind = BIND;
            b.act = a;
            b.label = kBindNames[a];
            c.rows.push_back(b);
        }
        Row fb;
        fb.kind = FIXEDBIND;
        fb.label = L"Settings";
        c.rows.push_back(fb);
        c.rows.push_back(head(L"Feel"));
        c.rows.push_back(xslider(X_STEER_SENSITIVITY, L"Steering sensitivity",
                                 L"Flight of Fancy: how hard the stick or keys turn you, and how quickly.", 25, 200, 5, L"%", false));
        c.rows.push_back(xslider(X_AIM_SPEED, L"Aim speed", L"How fast the right stick moves the pointer.", 25, 200, 5, L"%", false));
        c.rows.push_back(xslider(X_RHYTHM_OFFSET, L"Dance timing offset", L"Dance Inferno: raise it if your presses feel late, lower it if early. In steps of a frame (17 ms).", -100, 100,
                                 5, L" ms", false, true));
        g_cats.push_back(c);
    }
    // Audio
    {
        Category c{L"audio", L"Audio", L"Music, sound effects and voices", 0x5bc26a, 0x7ad68a, 0x1b2440, {}};
        c.rows.push_back(xslider(X_VOLUME_MASTER, L"Master volume", nullptr, 0, 100, 5, L"%", false));
        c.rows.push_back(xslider(X_VOLUME_MUSIC, L"Music", L"Music and jingles.", 0, 100, 5, L"%", false));
        c.rows.push_back(xslider(X_VOLUME_SFX, L"Sound effects", nullptr, 0, 100, 5, L"%", false));
        c.rows.push_back(xslider(X_VOLUME_VOICES, L"Pokémon voices", nullptr, 0, 100, 5, L"%", false));
        {
            // Japanese voices (dvd.c): set up once from the player's own Japanese
            // disc (drop it on the launcher, or tools/extract_voices.py); greyed
            // until then. The disc's file table is read at boot: from the next start.
            Row vl = xrow(CHOICE, X_VOICE_LANGUAGE, L"Voice language",
                          L"The Pokémon's voices. Japanese comes from your own Japanese disc (drop it on the launcher). From the next start.",
                          {L"English", L"Japanese"});
            vl.off = [] { return !dvd_voices_ready() && g_x[X_VOICE_LANGUAGE] == 0; };
            vl.tag = L"Needs your Japanese disc";
            c.rows.push_back(vl);
        }
        c.rows.push_back(xrow(TOGGLE, X_MUTE_BACKGROUND, L"Mute in the background", L"Goes quiet when you switch to another window."));
        c.rows.push_back(xrow(CHOICE, X_AUDIO_LATENCY, L"Audio delay", L"Raise it if the sound crackles.", {L"Low", L"Normal", L"Safe"}));
        g_cats.push_back(c);
    }
    // Gameplay
    {
        Category c{L"gameplay", L"Gameplay", L"Tune the pace and the challenge", 0xf39a2b, 0xf7b45c, 0x1b2440, {}};
        c.rows.push_back(head(L"Balance"));
        {
            // The game's own Text Speed (Options page), read and written in
            // place in its loaded save (hle_options.c).
            Row ts;
            ts.kind = CHOICE;
            ts.label = L"Text speed";
            ts.desc = L"The game's own setting, as on its Options page; saved with your game.";
            ts.opts = {L"Normal", L"Fast"};
            ts.get = [] { return std::max(0, hle_game_text_speed()); };
            ts.set = [](int v) { hle_set_game_text_speed(v); };
            ts.reset = [] { hle_set_game_text_speed(0); };
            ts.off = [] { return hle_game_text_speed() < 0; };
            ts.tag = L"Load a save first";
            c.rows.push_back(ts);
        }
        // Berries collected and Shop prices (hle_balance.c): what is gained and spent.
        auto pct = [](const wchar_t* label, const wchar_t* desc, int mn, int mx, int step, int* field) {
            Row r;
            r.kind = SLIDER;
            r.label = label;
            r.desc = desc;
            r.min = mn, r.max = mx, r.step = step, r.unit = L"%";
            r.get = [field] { return *field; };
            r.set = [field](int v) {
                *field = v;
                settings_save();
            };
            r.reset = [field] {
                *field = 100;
                settings_save();
            };
            return r;
        };
        c.rows.push_back(pct(L"Berries collected", L"Berries you get from boxes, rewards and friends.", 50, 500, 25,
                             &g_settings.berries));
        c.rows.push_back(pct(L"Shop prices", L"What buying takes from your Berries. Shops still show their own price.", 0,
                             200, 10, &g_settings.prices));
        c.rows.push_back(pct(L"Damage you deal", L"In battles.", 25, 400, 25, &g_settings.damage_dealt));
        c.rows.push_back(pct(L"Damage you take", L"In battles. 0% and nothing can hurt you.", 0, 200, 10, &g_settings.damage_taken));
        c.rows.push_back(pct(L"Chase speed", L"How fast the Pokémon you chase runs.", 50, 150, 5, &g_settings.chase_speed));
        c.rows.push_back(head(L"Comfort"));
        c.rows.push_back(xrow(TOGGLE, X_PAUSE_BACKGROUND, L"Pause in the background", L"Pauses when the window loses focus."));
        c.rows.push_back(xrow(TOGGLE, X_SKIP_LOGOS, L"Skip startup logos",
                              L"Goes to the title as soon as the park has loaded, instead of waiting out the wrist strap screen."));
        c.rows.push_back(xrow(CHOICE, X_SPEED_UP, L"Speed-up",
                              L"Tab or the left trigger runs the park faster. Not in attractions or story scenes; quiet meanwhile.",
                              {L"Off", L"2×", L"3×"}));
        c.rows.push_back(xrow(CHOICE, X_SPEED_TOGGLE, L"Speed-up key", nullptr, {L"Hold", L"Toggle"}));
        c.rows.push_back(xrow(TOGGLE, X_SAVE_BACKUPS, L"Save backups",
                              L"Keeps a copy of your slot before each save (the last 10), in the backups folder."));
        g_cats.push_back(c);
    }
    // Multiplayer
    {
        Category c{L"multi", L"Multiplayer", L"Play together, on one PC or online", 0x3fb6c8, 0x7fd3e0, 0x1b2440, {}};
        c.rows.push_back(head(L"Local"));
        // The four players (input_win32.c pad_of): player 1 is the keyboard
        // and mouse (with controller 1 unless the keyboard plays alone); the
        // controllers, in the order they connected, are the next players.
        // Choosing P1 or P2 swaps who has controller 1.
        Row pl;
        pl.kind = PLAYERS;
        pl.label = L"Players";
        pl.get = [] { return g_x[X_KEYBOARD_PLAYER]; };
        pl.set = [](int v) { settings_set_x(X_KEYBOARD_PLAYER, v); };
        pl.reset = [] { settings_set_x(X_KEYBOARD_PLAYER, settings_x_default(X_KEYBOARD_PLAYER)); };
        c.rows.push_back(pl);
        c.rows.push_back(note(L"Each controller plays as the next player once it's connected. Choose P1 or P2 to move controller 1. In Multiplayer Attractions, add players with Add Wii Remote Controllers."));
        c.rows.push_back(xrow(TOGGLE, X_KEYBOARD_PLAYER, L"Keyboard can be a player",
                              L"On: the keyboard and mouse are player 1 by themselves, and controller 1 is player 2. Off: they share player 1."));
        c.rows.push_back(soon_row(TOGGLE, L"Join by pressing a button", L"A controller takes the first open slot when one of its buttons is pressed."));
        c.rows.push_back(soon_row(TOGGLE, L"Player markers", L"P1-P4 tags over each player's Pokémon."));
        {
            Row remote = note(L"Steam Remote Play Together and Parsec give your friend's controller to this PC, so it should take the next slot like one plugged in. Not tested yet.");
            remote.label = L"Friends far away";
            c.rows.push_back(remote);
        }
        c.rows.push_back(head(L"Online", true));
        c.rows.push_back(note(L"Play attractions with friends over the internet. Not ready yet: local play and Remote Play work today."));
        c.rows.push_back(soon_row(CHOICE, L"Play online", nullptr, {L"Host a room", L"Join with a code"}, -1));
        c.rows.push_back(soon_row(TEXTBOX, L"Nickname", L"What other players see."));
        c.rows.push_back(soon_row(CHOICE, L"Region", nullptr, {L"Auto", L"Americas", L"Europe", L"Asia"}, 0));
        c.rows.push_back(soon_row(TOGGLE, L"Voice chat", nullptr));
        g_cats.push_back(c);
    }
    // Silly
    {
        Category c{L"silly", L"Silly", L"Just for fun", 0xe2457f, 0xf27aa5, 0xffffff, {}};
        c.rows.push_back(xrow(CHOICE, X_POKEMON_SIZE, L"Pokémon size", L"The park's Pokémon at half or twice their size. Just for looks: they bump into things as before.",
                              {L"Mini", L"Normal", L"Giant"}));
        c.rows.push_back(xslider(X_VOICE_PITCH, L"Voice pitch", L"From deep and grumbly to tiny and squeaky. Changes the next thing a Pokémon says.", -50, 50, 5, L"%", false, true));
        c.rows.push_back(xrow(TOGGLE, X_MIRROR_WORLD, L"Mirror world", L"Flips the whole park left to right. Left and right swap too."));
        c.rows.push_back(xrow(TOGGLE, X_VICTORY_CONFETTI, L"Victory confetti", L"A burst of confetti when you clear an attraction."));
        c.rows.push_back(xrow(CHOICE, X_STEREO_3D, L"3D glasses", L"Red-cyan for paper 3D glasses; side by side for 3D TVs and VR viewers.",
                              {L"Off", L"Red-cyan", L"Side by side"}));
        g_cats.push_back(c);
    }
}

// ------------------------------------------------------------------------
// State

struct UI {
    bool open = false;
    double openT = 0;
    int tab = 0;
    int view = 0;                // 0 the settings, 1 the Friend Book
    int zone = 0;                // Friend Book: 0 everyone, 1-6 an area
    int ffilter = 0;             // Friend Book: 0 all, 1 friends, 2 not yet
    int place = -1;              // Friend Book: the place chosen, -1 all
    int sheet = -1;              // the hint card open (an index in g_friends), -1 none
    bool revealed = false;       // its "show more" pressed
    float scroll = 0;            // the panel's rows, scrolled by
    int focus = -1;              // focused item (keyboard/controller), -1 none
    int focusSlot = 0;           // which part of the line
    bool keyNav = false;         // show the focus ring
    int listenRow = -1, listenSlot = -1;  // rebinding
    double listenT = 0;
    int hoverRow = -1, hoverPart = -1;
    int dragRow = -1;            // slider being dragged
    unsigned gen = 1;
    bool reduced = false;
    uint32_t prevPad = 0;
    double padRepeatAt = 0;
    uint32_t padHeldDir = 0;
    int hoverBtn = 0;            // what the mouse is over: a Hit's what (buttons only)
} U;

void changed() { U.gen++; }
Category& cur() { return U.view ? g_fb : g_cats[U.tab]; }

// ------------------------------------------------------------------------
// Drawing (software Direct2D into a WIC bitmap)

struct Gfx {
    ComPtr<ID2D1Factory> f;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<IDWriteFactory5> dw;
    ComPtr<IDWriteFontCollection1> fonts;
    ComPtr<ID2D1SolidColorBrush> solid;
    ComPtr<ID2D1StrokeStyle> round, dashed;
    std::map<std::wstring, ComPtr<IDWriteTextFormat>> formats;
    std::map<std::wstring, ComPtr<ID2D1PathGeometry>> icons;
    std::vector<uint32_t> image;
    int w = 0, h = 0;
    bool ok = false;
} G;

ID2D1SolidColorBrush* brush(D2D1_COLOR_F c) {
    G.solid->SetColor(c);
    return G.solid.Get();
}

bool load_fonts() {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                   reinterpret_cast<IUnknown**>(G.dw.GetAddressOf()))))
        return false;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    G.dw->CreateInMemoryFontFileLoader(&loader);
    G.dw->RegisterFontFileLoader(loader.Get());
    ComPtr<IDWriteFontSetBuilder1> b;
    G.dw->CreateFontSetBuilder(&b);
    for (int id : {101, 102}) {
        HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
        if (!r) return false;
        HGLOBAL hg = LoadResource(nullptr, r);
        ComPtr<IDWriteFontFile> file;
        loader->CreateInMemoryFontFileReference(G.dw.Get(), LockResource(hg), SizeofResource(nullptr, r), nullptr, &file);
        b->AddFontFile(file.Get());
    }
    ComPtr<IDWriteFontSet> set;
    b->CreateFontSet(&set);
    return SUCCEEDED(G.dw->CreateFontCollectionFromFontSet(set.Get(), &G.fonts));
}

IDWriteTextFormat* fmt(const wchar_t* fam, int weight, float size) {
    wchar_t key[64];
    swprintf(key, 64, L"%s|%d|%g", fam, weight, size);
    auto it = G.formats.find(key);
    if (it != G.formats.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    G.dw->CreateTextFormat(fam, G.fonts.Get(), (DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STYLE_NORMAL,
                           DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    G.formats[key] = f;
    return f.Get();
}

const DWRITE_TEXT_ALIGNMENT LEFT = DWRITE_TEXT_ALIGNMENT_LEADING, CENTER = DWRITE_TEXT_ALIGNMENT_CENTER,
                            RIGHT = DWRITE_TEXT_ALIGNMENT_TRAILING;

ComPtr<IDWriteTextLayout> layout(const std::wstring& s, IDWriteTextFormat* f, float w, float h = 1000,
                                 DWRITE_TEXT_ALIGNMENT align = LEFT, float spacing = 0) {
    ComPtr<IDWriteTextLayout> l;
    G.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, h, &l);
    l->SetTextAlignment(align);
    if (spacing != 0) {
        ComPtr<IDWriteTextLayout1> l1;
        if (SUCCEEDED(l.As(&l1))) l1->SetCharacterSpacing(0, spacing, 0, {0, (UINT32)s.size()});
    }
    return l;
}
DWRITE_TEXT_METRICS measure(IDWriteTextLayout* l) {
    DWRITE_TEXT_METRICS m;
    l->GetMetrics(&m);
    return m;
}
void text(const std::wstring& s, IDWriteTextFormat* f, float x, float y, float w, D2D1_COLOR_F c,
          DWRITE_TEXT_ALIGNMENT align = LEFT, float spacing = 0) {
    auto l = layout(s, f, w, 1000, align, spacing);
    G.rt->DrawTextLayout(Point2F(x, y), l.Get(), brush(c));
}
// One line, cut with "…" to fit w.
void text_fit(const std::wstring& s, IDWriteTextFormat* f, float x, float y, float w, D2D1_COLOR_F c,
              DWRITE_TEXT_ALIGNMENT align) {
    auto l = layout(s, f, w, 100, align);
    l->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING tr = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ell;
    G.dw->CreateEllipsisTrimmingSign(l.Get(), &ell);
    l->SetTrimming(&tr, ell.Get());
    G.rt->DrawTextLayout(Point2F(x, y), l.Get(), brush(c));
}
// One line, centred on cy.
void text_c(const std::wstring& s, IDWriteTextFormat* f, float x, float cy, float w, D2D1_COLOR_F c,
            DWRITE_TEXT_ALIGNMENT align = LEFT, float spacing = 0) {
    auto l = layout(s, f, w, 1000, align, spacing);
    l->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    G.rt->DrawTextLayout(Point2F(x, cy - measure(l.Get()).height / 2), l.Get(), brush(c));
}
float text_w(const std::wstring& s, IDWriteTextFormat* f, float spacing = 0) {
    auto l = layout(s, f, 4000, 1000, LEFT, spacing);
    l->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return measure(l.Get()).widthIncludingTrailingWhitespace;
}
float text_h(const std::wstring& s, IDWriteTextFormat* f, float w) { return measure(layout(s, f, w).Get()).height; }
std::wstring upper(std::wstring s) {
    for (auto& ch : s) ch = towupper(ch);
    return s;
}

// A small SVG path parser (M L H V A C Z, absolute and relative), for the
// mockup's 24x24 icons.
ComPtr<ID2D1PathGeometry> svg_path(const char* d) {
    ComPtr<ID2D1PathGeometry> p;
    ComPtr<ID2D1GeometrySink> s;
    G.f->CreatePathGeometry(&p);
    p->Open(&s);
    float cx = 0, cy = 0, sx = 0, sy = 0;
    bool open = false;
    char cmd = 0;
    const char* q = d;
    auto num = [&](float& out) -> bool {
        while (*q == ' ' || *q == ',') q++;
        if (!*q || !(isdigit((unsigned char)*q) || *q == '-' || *q == '.' || *q == '+')) return false;
        char* e;
        out = strtof(q, &e);
        if (e == q) return false;
        q = e;
        return true;
    };
    auto flag = [&](float& out) -> bool {  // arc flags may run together ("01")
        while (*q == ' ' || *q == ',') q++;
        if (*q != '0' && *q != '1') return false;
        out = (float)(*q - '0');
        q++;
        return true;
    };
    auto begin = [&](float x, float y) {
        if (open) s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->BeginFigure(Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
        open = true;
    };
    auto done = [&] {
        if (open) s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
        return p;
    };
    while (*q) {
        while (*q == ' ' || *q == ',') q++;
        if (!*q) break;
        if (isalpha((unsigned char)*q)) cmd = *q++;
        bool rel = islower((unsigned char)cmd) != 0;
        float a, b, c, d2, e, f, g;
        switch (toupper(cmd)) {
        case 'M':
            if (!num(a) || !num(b)) return done();
            if (rel) a += cx, b += cy;
            begin(a, b);
            cx = sx = a, cy = sy = b;
            cmd = rel ? 'l' : 'L';
            break;
        case 'L':
            if (!num(a) || !num(b)) return done();
            if (rel) a += cx, b += cy;
            s->AddLine(Point2F(a, b));
            cx = a, cy = b;
            break;
        case 'H':
            if (!num(a)) return done();
            cx = rel ? cx + a : a;
            s->AddLine(Point2F(cx, cy));
            break;
        case 'V':
            if (!num(a)) return done();
            cy = rel ? cy + a : a;
            s->AddLine(Point2F(cx, cy));
            break;
        case 'C':
            if (!num(a) || !num(b) || !num(c) || !num(d2) || !num(e) || !num(f)) return done();
            if (rel) a += cx, b += cy, c += cx, d2 += cy, e += cx, f += cy;
            s->AddBezier(D2D1::BezierSegment(Point2F(a, b), Point2F(c, d2), Point2F(e, f)));
            cx = e, cy = f;
            break;
        case 'A':
            if (!num(a) || !num(b) || !num(c) || !flag(d2) || !flag(e) || !num(f) || !num(g)) return done();
            if (rel) f += cx, g += cy;
            s->AddArc(D2D1::ArcSegment(Point2F(f, g), D2D1::SizeF(a, b), c,
                                       e != 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                       d2 != 0 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            cx = f, cy = g;
            break;
        case 'Z':
            if (open) s->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
            cx = sx, cy = sy;
            break;
        default: q++; break;
        }
    }
    return done();
}

ComPtr<ID2D1PathGeometry>& cached_path(const wchar_t* key, const char* d) {
    auto& g = G.icons[key];
    if (!g) g = svg_path(d);
    return g;
}

// The icons (the mockup's, 24x24, stroke 2, round); fill: filled too.
void draw_icon(const std::wstring& id, float x, float y, float size, D2D1_COLOR_F c, const D2D1_COLOR_F* fill = nullptr) {
    static const std::map<std::wstring, std::vector<const char*>> paths = {
        {L"display", {"M5 4h14a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2Z", "M8 20h8M12 16v4"}},
        {L"graphics", {"M12 3l2.2 5.8L20 11l-5.8 2.2L12 19l-2.2-5.8L4 11l5.8-2.2Z"}},
        {L"controls", {"M6 8h12a4 4 0 0 1 3.8 5.3l-1 3a2 2 0 0 1-3.5.6L15.5 14.5h-7l-1.8 2.4a2 2 0 0 1-3.5-.6l-1-3A4 4 0 0 1 6 8Z",
                       "M8 10v3M6.5 11.5h3M15.5 11h.01M17.5 12.5h.01"}},
        {L"audio", {"M4 10v4h4l5 4V6L8 10Z", "M16.5 9a4 4 0 0 1 0 6M19 6.5a7.5 7.5 0 0 1 0 11"}},
        {L"gameplay", {"M4 7h10M18 7h2M4 17h4M12 17h8", "M18 7a2 2 0 1 1-4 0a2 2 0 1 1 4 0Z", "M12 17a2 2 0 1 1-4 0a2 2 0 1 1 4 0Z"}},
        {L"multi", {"M11.5 8a3 3 0 1 1-6 0a3 3 0 1 1 6 0Z", "M18.5 9a2.5 2.5 0 1 1-5 0a2.5 2.5 0 1 1 5 0Z",
                    "M3 19c.6-3.4 2.8-5.5 5.5-5.5C11.2 13.5 13.4 15.6 14 19M14.5 14.2c.5-.4 1-.6 1.5-.6 2.2 0 3.9 1.9 4.4 5.4"}},
        {L"silly", {"M21 12a9 9 0 1 1-18 0a9 9 0 1 1 18 0Z", "M8.5 14.5a4.5 4.5 0 0 0 7 0M9 9.5h.01M15 9.5h.01"}},
        {L"heart", {"M12 20.5c0 0-7.5-4.6-7.5-10.2A4.3 4.3 0 0 1 12 7.6a4.3 4.3 0 0 1 7.5 2.7c0 5.6-7.5 10.2-7.5 10.2Z"}},
        {L"pin", {"M12 21C12 21 5 14.8 5 9.5A7 7 0 0 1 19 9.5C19 14.8 12 21 12 21Z", "M14.5 9.5a2.5 2.5 0 1 1-5 0a2.5 2.5 0 1 1 5 0Z"}},
        {L"bulb", {"M12 3a6 6 0 0 0-3.6 10.8c.7.6 1.1 1.4 1.1 2.2h5c0-.8.4-1.6 1.1-2.2A6 6 0 0 0 12 3Z", "M9.5 18.5h5M10.5 21h3"}},
        {L"close", {"M7 7l10 10M17 7L7 17"}},
    };
    auto it = paths.find(id);
    if (it == paths.end()) return;
    Matrix3x2F old;
    G.rt->GetTransform(&old);
    G.rt->SetTransform(Matrix3x2F::Scale(size / 24, size / 24) * Matrix3x2F::Translation(x, y) * old);
    int i = 0;
    for (const char* d : it->second) {
        std::wstring key = id + std::to_wstring(i++);
        auto& g = G.icons[key];
        if (!g) g = svg_path(d);
        if (fill && i == 1) G.rt->FillGeometry(g.Get(), brush(*fill));
        G.rt->DrawGeometry(g.Get(), brush(c), 2, G.round.Get());
    }
    G.rt->SetTransform(old);
}

ComPtr<ID2D1LinearGradientBrush> grad(D2D1_POINT_2F a, D2D1_POINT_2F b, std::vector<D2D1_GRADIENT_STOP> st) {
    ComPtr<ID2D1GradientStopCollection> sc;
    G.rt->CreateGradientStopCollection(st.data(), (UINT32)st.size(), &sc);
    ComPtr<ID2D1LinearGradientBrush> l;
    G.rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), sc.Get(), &l);
    return l;
}
ComPtr<ID2D1LinearGradientBrush> vgrad(float y0, float y1, D2D1_COLOR_F a, D2D1_COLOR_F b) {
    return grad(Point2F(0, y0), Point2F(0, y1), {{0, a}, {1, b}});
}
ComPtr<ID2D1RadialGradientBrush> rglow(float x, float y, float r, D2D1_COLOR_F c) {
    D2D1_GRADIENT_STOP st[2] = {{0, c}, {1, ColorF(c.r, c.g, c.b, 0)}};
    ComPtr<ID2D1GradientStopCollection> sc;
    G.rt->CreateGradientStopCollection(st, 2, &sc);
    ComPtr<ID2D1RadialGradientBrush> b;
    G.rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(Point2F(x, y), Point2F(), r, r), sc.Get(), &b);
    return b;
}
D2D1_ROUNDED_RECT rr(float x, float y, float w, float h, float r) { return D2D1::RoundedRect(RectF(x, y, x + w, y + h), r, r); }
D2D1_RECT_F shift(D2D1_RECT_F r, float dx, float dy) { return RectF(r.left + dx, r.top + dy, r.right + dx, r.bottom + dy); }

// ---- The notebook's look (docs/settings/ingame-f1-settings.html): ink
// outlines, hard offset shadows, small tilts. No glows, no blur.

const uint32_t INK = 0x1b2440, PAGE_TEXT = 0xfff3da, PAGE_SOFT = 0xe9cfa5, PINK = 0xe2457f, GOLD = 0xf3cf6b,
               LEATHER_DARK = 0x2a190d;
D2D1_COLOR_F line_color() { return rgb(0xffe2b4, .22f); }

// Turned by deg (clockwise) about (cx, cy) while it lives: one matrix per element.
struct Rot {
    Matrix3x2F old;
    bool on;
    Rot(float deg, float cx, float cy) : on(deg != 0) {
        G.rt->GetTransform(&old);
        if (on) G.rt->SetTransform(Matrix3x2F::Rotation(deg, Point2F(cx, cy)) * old);
    }
    ~Rot() {
        if (on) G.rt->SetTransform(old);
    }
};
// Drawn at an opacity while it lives.
struct Fade {
    bool on;
    explicit Fade(float op) : on(op < 1) {
        if (on)
            G.rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), op),
                            nullptr);
    }
    ~Fade() {
        if (on) G.rt->PopLayer();
    }
};

ComPtr<ID2D1PathGeometry> poly_path(const D2D1_POINT_2F* pts, int n) {
    ComPtr<ID2D1PathGeometry> p;
    ComPtr<ID2D1GeometrySink> s;
    G.f->CreatePathGeometry(&p);
    p->Open(&s);
    s->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
    s->AddLines(pts + 1, n - 1);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    return p;
}
// A rectangle with its own radius at each corner.
ComPtr<ID2D1PathGeometry> rrect_path(float x, float y, float w, float h, float tl, float tr, float br, float bl) {
    ComPtr<ID2D1PathGeometry> p;
    ComPtr<ID2D1GeometrySink> s;
    G.f->CreatePathGeometry(&p);
    p->Open(&s);
    auto arc = [&](float ex, float ey, float r) {
        if (r > 0)
            s->AddArc(D2D1::ArcSegment(Point2F(ex, ey), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    };
    s->BeginFigure(Point2F(x + tl, y), D2D1_FIGURE_BEGIN_FILLED);
    s->AddLine(Point2F(x + w - tr, y));
    arc(x + w, y + tr, tr);
    s->AddLine(Point2F(x + w, y + h - br));
    arc(x + w - br, y + h, br);
    s->AddLine(Point2F(x + bl, y + h));
    arc(x, y + h - bl, bl);
    s->AddLine(Point2F(x, y + tl));
    arc(x + tl, y, tl);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    return p;
}
void fill_geo(ID2D1Geometry* g, D2D1_COLOR_F c, float dx = 0, float dy = 0) {
    if (dx == 0 && dy == 0) {
        G.rt->FillGeometry(g, brush(c));
        return;
    }
    Matrix3x2F old;
    G.rt->GetTransform(&old);
    G.rt->SetTransform(Matrix3x2F::Translation(dx, dy) * old);
    G.rt->FillGeometry(g, brush(c));
    G.rt->SetTransform(old);
}

// A sticker: fill, ink outline (bw thick, inside the box) and a hard
// shadow sh down and right.
void sticker(D2D1_RECT_F b, float r, D2D1_COLOR_F fill, float sh = 2, float bw = 2.5f, D2D1_COLOR_F ink = rgb(INK),
             bool dashed = false, D2D1_COLOR_F shadow = rgb(INK)) {
    float w = b.right - b.left, h = b.bottom - b.top;
    if (sh > 0) G.rt->FillRoundedRectangle(rr(b.left + sh, b.top + sh, w, h, r), brush(shadow));
    if (fill.a > 0) G.rt->FillRoundedRectangle(rr(b.left, b.top, w, h, r), brush(fill));
    G.rt->DrawRoundedRectangle(rr(b.left + bw / 2, b.top + bw / 2, w - bw, h - bw, std::max(0.f, r - bw / 2)), brush(ink), bw,
                               dashed ? G.dashed.Get() : nullptr);
}
void circle(float cx, float cy, float r, D2D1_COLOR_F fill, float bw = 0, D2D1_COLOR_F ink = rgb(INK), bool dashed = false) {
    if (fill.a > 0) G.rt->FillEllipse(D2D1::Ellipse(Point2F(cx, cy), r, r), brush(fill));
    if (bw > 0)
        G.rt->DrawEllipse(D2D1::Ellipse(Point2F(cx, cy), r - bw / 2, r - bw / 2), brush(ink), bw, dashed ? G.dashed.Get() : nullptr);
}

// The five-pointed star (slider thumbs, friend stickers), size wide, centred.
void draw_star(float cx, float cy, float size, D2D1_COLOR_F fill, D2D1_COLOR_F edge, float sw) {
    auto& g = cached_path(L"star5", "M14 1.5l3.6 7.6 8.3 1-6.1 5.7 1.6 8.2L14 20l-7.4 4 1.6-8.2-6.1-5.7 8.3-1z");
    Matrix3x2F old;
    G.rt->GetTransform(&old);
    float s = size / 28;
    G.rt->SetTransform(Matrix3x2F::Scale(s, s) * Matrix3x2F::Translation(cx - size / 2, cy - size / 2 + s) * old);
    G.rt->FillGeometry(g.Get(), brush(fill));
    if (sw > 0) G.rt->DrawGeometry(g.Get(), brush(edge), sw / s, G.round.Get());
    G.rt->SetTransform(old);
}

// A strip of washi tape: the color with pale stripes, its ends torn (pts:
// its outline as fractions of the box, as the mockup's clip-path).
void tape(D2D1_RECT_F b, D2D1_COLOR_F c, float stripeA, float stripe, std::initializer_list<std::pair<float, float>> pts) {
    std::vector<D2D1_POINT_2F> p;
    float w = b.right - b.left, h = b.bottom - b.top;
    for (auto& q : pts) p.push_back(Point2F(b.left + q.first * w, b.top + q.second * h));
    auto g = poly_path(p.data(), (int)p.size());
    G.rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.Get()), nullptr);
    G.rt->FillRectangle(b, brush(c));
    for (float t = -h; t < w + h; t += stripe * 2 * 1.4142f)
        G.rt->DrawLine(Point2F(b.left + t, b.bottom), Point2F(b.left + t + h, b.top), brush(ColorF(1, 1, 1, stripeA)), stripe);
    G.rt->PopLayer();
}
const std::initializer_list<std::pair<float, float>> kSecTape = {
    {0, .08f}, {.04f, 0}, {.08f, .10f}, {.12f, 0}, {1, .06f}, {.97f, .5f}, {1, .94f}, {.96f, 1}, {.92f, .9f}, {.88f, 1}, {0, .92f}, {.03f, .5f}};

// The "Coming soon" rubber stamp (red; white on a washi title). Returns its width.
float stamp(const std::wstring& s, float x, float cy, bool onTape, bool draw = true) {
    auto l = layout(upper(s), fmt(L"Fredoka", 700, 11), 600, 100, LEFT, 1.1f);
    l->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    float w = measure(l.Get()).widthIncludingTrailingWhitespace + 16 + 5, h = 21;
    if (!draw) return w;
    Rot rot(onTape ? 3.f : -6.f, x + w / 2, cy);
    D2D1_COLOR_F ink = onTape ? ColorF(1, 1, 1) : rgb(0xff8a7a);
    G.rt->FillRoundedRectangle(rr(x, cy - h / 2, w, h, 6), brush(onTape ? ColorF(0, 0, 0, .15f) : rgb(0x3a160e, .35f)));
    G.rt->DrawRoundedRectangle(rr(x + 1.25f, cy - h / 2 + 1.25f, w - 2.5f, h - 2.5f, 5), brush(ink), 2.5f);
    G.rt->DrawTextLayout(Point2F(x + 10.5f, cy - measure(l.Get()).height / 2), l.Get(), brush(ink));
    return w;
}

// The focus ring: 3 px pink, 3 px out.
void draw_focus(D2D1_RECT_F r, float radius) {
    G.rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(r.left - 4.5f, r.top - 4.5f, r.right + 4.5f, r.bottom + 4.5f), radius + 4.5f,
                                                 radius + 4.5f),
                               brush(rgb(PINK)), 3);
}

// A sticker button (Close, Reset, Back to Settings) with an optional key
// chip: returns its width.
float stone_button(float x, float y, float h, const std::wstring& label, const wchar_t* key, bool hover, bool draw = true) {
    IDWriteTextFormat* f = fmt(L"Fredoka", 600, 15);
    float tw = text_w(label, f);
    float w = (key ? 3 + 6 + 34 + 8 : 3 + 14) + tw + 14 + 3;
    if (!draw) return w;
    float lift = hover ? -1.f : 0;
    sticker(RectF(x + lift, y + lift, x + w + lift, y + h + lift), 12, ColorF(1, 1, 1), hover ? 4.f : 3.f, 3);
    float tx = x + lift + 3 + 14;
    if (key) {
        G.rt->FillRoundedRectangle(rr(x + lift + 9, y + lift + (h - 26) / 2, 34, 26, 7), brush(rgb(INK)));
        text_c(key, fmt(L"Fredoka", 600, 14), x + lift + 9, y + lift + h / 2, 34, rgb(0xfff8e6), CENTER);
        tx = x + lift + 9 + 34 + 8;
    }
    text_c(label, f, tx, y + lift + h / 2, tw + 4, rgb(INK));
    return w;
}

// ---- Layout of the notebook (scene units: 720 high). The tab: 644 x 700,
// 10 from the window's right and top. In it: the cover from x 54, the page
// (the panel) at (76, 14), 554 x 672; the rows from 146 to 64 above the
// page's bottom, x 40, 488 wide.

const float TAB_W = 644, TAB_H = 700, COVER_X = 54;
const float PANEL_X = 76, PANEL_Y = 14, PANEL_W = 554, PANEL_H = 672;
const float ROWS_TOP = 146, ROWS_BOTTOM = 64, ROW_X = 40, ROW_W = 488, FOOT_H = 62;
const int NTABS = 7;

struct Placed {
    float y = 0, h = 0;       // in the scrolled list
    float ctrlX = 0, ctrlY = 0, ctrlW = 0, ctrlH = 0;  // the control's box (list coordinates)
    std::vector<D2D1_RECT_F> parts;  // pills / slots / cards, list coordinates
    float labelW = ROW_W;
    float stampW = 0;         // a "Coming soon" row: its stamp's width
    bool wrap = false;
};
std::vector<Placed> g_placed;
float g_listH = 0;

// The Friend Book's parts (below, with the friends list).
std::wstring place_chip_name(int place);
std::wstring place_chip_count(int place);
void draw_fb_row(int i, const Row& r, const Placed& p, float y0, float dy, bool focused);

IDWriteTextFormat* pill_font(bool mini) { return fmt(L"Nunito", 800, mini ? 12.5f : 13.5f); }
float soon_tag_w() { return text_w(L"SOON", fmt(L"Nunito", 800, 10)) + 6; }
float pill_w(const std::wstring& s, bool mini = false, bool soonOpt = false) {
    return text_w(s, pill_font(mini)) + (mini ? 24 : 26) + 5 + (soonOpt ? soon_tag_w() : 0);
}
float chip_w(int place) {
    return text_w(place_chip_name(place), fmt(L"Fredoka", 600, 14)) + 6 + text_w(place_chip_count(place), fmt(L"Nunito", 800, 11.5f)) + 14 +
           22 + 5;
}
float label_h(const Row& r, float w) {
    float h = r.label.empty() ? 0 : text_h(r.label, fmt(L"Nunito", 800, 16), w);
    if (!r.desc.empty()) h += 2 + text_h(r.desc, fmt(L"Nunito", 600, 13), w);
    return h;
}
const float PLAYER_W = (ROW_W - 12) / 2, PLAYER_H = 76;
const float CARD_W = (ROW_W - 4 - 24) / 3, CARD_H = 124;

void place_rows() {
    const Category& c = cur();
    g_placed.assign(c.rows.size(), Placed());
    float y = 0;
    for (size_t i = 0; i < c.rows.size(); i++) {
        const Row& r = c.rows[i];
        Placed& p = g_placed[i];
        p.y = y;
        switch (r.kind) {
        case HEAD: p.h = 18 + 25 + 6; break;
        case NOTE:
            if (r.label.empty()) p.h = text_h(r.desc, fmt(L"Nunito", 600, 13), ROW_W) + 8;
            else p.h = label_h(r, ROW_W) + 26;
            break;
        case BINDHEAD: p.h = 26; break;
        case BIND:
        case FIXEDBIND: {
            float fr = (ROW_W - 24) / 5.3f;
            p.h = 44;
            for (int s = 0; s < 4; s++) {
                float x = ROW_X + fr * 1.3f + 6 + s * (fr + 6);
                p.parts.push_back(RectF(x, y + 5, x + fr, y + 39));
            }
            break;
        }
        case PLAYERS:
            p.h = 10 + PLAYER_H * 2 + 12 + 8;
            for (int n = 0; n < 4; n++) {
                float x = ROW_X + (n % 2) * (PLAYER_W + 12), py = y + 10 + (n / 2) * (PLAYER_H + 12);
                p.parts.push_back(RectF(x, py, x + PLAYER_W, py + PLAYER_H));
            }
            break;
        case FBTOP: p.h = 96; break;
        case PLACES: {
            float x = ROW_X, py = y + 10;
            for (int place : r.items) {
                float w = chip_w(place);
                if (x > ROW_X && x + w > ROW_X + ROW_W) x = ROW_X, py += 34 + 7;
                p.parts.push_back(RectF(x, py, x + w, py + 34));
                x += w + 7;
            }
            p.h = py + 34 + 4 - y;
            break;
        }
        case PLACEHEAD: p.h = 18 + 26; break;
        case CARDS:
            p.h = 12 + CARD_H + 8;
            for (size_t k = 0; k < r.items.size(); k++) {
                float x = ROW_X + 2 + k * (CARD_W + 12);
                p.parts.push_back(RectF(x, y + 12, x + CARD_W, y + 12 + CARD_H));
            }
            break;
        case TIP: p.h = 14 + text_h(r.desc, fmt(L"Nunito", 700, 12.5f), ROW_W - 28) + 22 + 10; break;
        default: {
            // A row: label (and description), its control, and its stamp
            // when it's not available.
            float cw = 0, ch = 0;
            bool smallPills = r.mini;
            if (r.kind == TOGGLE) cw = 62, ch = 34;
            else if (r.kind == SLIDER) cw = 190 + 7 + 58, ch = 28;
            else if (r.kind == TEXTBOX) cw = 180, ch = 36;
            else {
                for (size_t o = 0; o < r.opts.size(); o++) cw += pill_w(r.opts[o], smallPills, r.soonOpts >> o & 1) + 7;
                cw -= 7;
                ch = smallPills ? 32 : 36;
            }
            if (r.soon) p.stampW = stamp(r.tag.empty() ? L"Coming soon" : r.tag, 0, 0, false, false);
            float reserve = r.soon ? std::max(124.f, p.stampW + 10) : 0;
            if (r.kind == CHOICE && r.label.empty()) {
                // The Friend Book's filter: pills from the left.
                float px = ROW_X;
                for (auto& o : r.opts) {
                    float pw = pill_w(o, smallPills);
                    p.parts.push_back(RectF(px, y + 8, px + pw, y + 8 + ch));
                    px += pw + 7;
                }
                p.ctrlX = ROW_X, p.ctrlY = y + 8, p.ctrlW = px - 7 - ROW_X, p.ctrlH = ch;
                p.h = 8 + ch + 6;
                break;
            }
            bool wrap = 200 + 16 + cw + reserve > ROW_W;
            p.wrap = wrap;
            p.labelW = (wrap ? ROW_W : ROW_W - 16 - cw) - reserve;
            float lh = label_h(r, p.labelW);
            float top = y + 13;
            if (r.kind == CHOICE && wrap) {
                // Pills on lines of their own under the label, from the left
                // (as the mockup's flex-wrap puts them).
                float avail = ROW_W - reserve;
                std::vector<std::vector<int>> lines(1);
                float lw = 0;
                for (size_t o = 0; o < r.opts.size(); o++) {
                    float pw = pill_w(r.opts[o], false, r.soonOpts >> o & 1);
                    if (lw > 0 && lw + 7 + pw > avail) lines.push_back({}), lw = 0;
                    lines.back().push_back((int)o);
                    lw += (lw > 0 ? 7 : 0) + pw;
                }
                float py = top + lh + 10;
                p.parts.assign(r.opts.size(), RectF());
                for (auto& line : lines) {
                    float px = ROW_X;
                    for (int o : line) {
                        float pw = pill_w(r.opts[o], false, r.soonOpts >> o & 1);
                        p.parts[o] = RectF(px, py, px + pw, py + 36);
                        px += pw + 7;
                    }
                    py += 36 + 7;
                }
                p.ctrlY = top + lh + 10;
                p.ctrlH = py - 7 - p.ctrlY;
                p.ctrlX = ROW_X, p.ctrlW = avail;
                p.h = (py - 7 + 13) - y;
            } else {
                float rowH = std::max(lh, ch);
                p.ctrlX = wrap ? ROW_X : ROW_X + ROW_W - reserve - cw, p.ctrlW = cw, p.ctrlH = ch;
                p.ctrlY = wrap ? top + lh + 10 : top + (rowH - ch) / 2;
                if (r.kind == CHOICE) {
                    float px = p.ctrlX;
                    for (size_t o = 0; o < r.opts.size(); o++) {
                        float pw = pill_w(r.opts[o], false, r.soonOpts >> o & 1);
                        p.parts.push_back(RectF(px, p.ctrlY, px + pw, p.ctrlY + 36));
                        px += pw + 7;
                    }
                }
                p.h = (wrap ? lh + 10 + ch : rowH) + 26;
            }
            break;
        }
        }
        y += p.h;
    }
    g_listH = y + 18;
}

// What can be highlighted, top to bottom: the top line (FOCUS_TOP: the
// seven index tabs, the Friend Book ribbon, Close), the rows, and the
// button at the bottom (FOCUS_RESET). Within a line, U.focusSlot says which
// part (tab, choice, key slot, card).
const int FOCUS_TOP = -3, FOCUS_RESET = -2, FB_SLOT = NTABS, CLOSE_SLOT = NTABS + 1;
bool focusable(Kind k) { return k != HEAD && k != NOTE && k != BINDHEAD && k != FBTOP && k != PLACEHEAD && k != TIP; }
std::vector<int> focusables() {
    std::vector<int> f;
    f.push_back(FOCUS_TOP);
    const Category& c = cur();
    for (size_t i = 0; i < c.rows.size(); i++)
        if (focusable(c.rows[i].kind)) f.push_back((int)i);
    f.push_back(FOCUS_RESET);
    return f;
}

float view_h() { return PANEL_H - ROWS_TOP - ROWS_BOTTOM; }
void clamp_scroll() {
    float mx = std::max(0.f, g_listH - view_h());
    U.scroll = std::max(0.f, std::min(mx, U.scroll));
}
void scroll_to(int row) {
    if (row < 0 || row >= (int)g_placed.size()) return;
    const Placed& p = g_placed[row];
    if (p.y < U.scroll) U.scroll = p.y - 4;
    if (p.y + p.h > U.scroll + view_h()) U.scroll = p.y + p.h - view_h() + 4;
    clamp_scroll();
}

std::wstring slider_text(const Row& r, int v) {
    std::wstring s = (r.sign && v > 0 ? L"+" : L"") + std::to_wstring(v) + r.unit;
    return s;
}
std::wstring key_text(int vk) {
    char buf[64];
    settings_key_name(vk, buf, sizeof(buf));
    return widen(buf);
}
std::wstring slot_text(int act, int slot) {
    if (slot < 2) {
        int vk = g_settings.key[act][slot];
        return vk ? key_text(vk) : L"";
    }
    uint32_t b = g_settings.pad[act][slot - 2];
    return b ? widen(settings_pad_name(b)) : L"";
}

// A chunky switch, 62 x 34: the knob with a star, gold when on.
void draw_switch(float x, float y, bool on, uint32_t cat) {
    sticker(RectF(x, y, x + 62, y + 34), 17, on ? rgb(cat) : rgb(0xece1c4), 2, 2.5f);
    float kx = on ? x + 45 : x + 18, ky = y + 17;
    circle(kx, ky, 12.5f, on ? rgb(0xfff8e6) : ColorF(1, 1, 1), 2.5f);
    auto& star = cached_path(L"star4", "M10 0L12.5 7.5L20 10L12.5 12.5L10 20L7.5 12.5L0 10L7.5 7.5Z");
    Matrix3x2F old;
    G.rt->GetTransform(&old);
    G.rt->SetTransform(Matrix3x2F::Scale(.6f, .6f) * Matrix3x2F::Translation(kx - 6, ky - 6) * old);
    G.rt->FillGeometry(star.Get(), brush(on ? rgb(0xc98f2a) : rgb(0xc8bfa8)));
    G.rt->SetTransform(old);
}

// A slider, 190 wide: the track (filled in the tab's color), a gold star
// thumb, and the value on a cream tag.
const float SLIDER_W = 190, THUMB = 28;
void draw_slider(float x, float y, float k, const std::wstring& val, uint32_t cat) {
    float ty = y + 7;
    G.rt->FillRoundedRectangle(rr(x + 2, ty + 2, SLIDER_W, 14, 7), brush(rgb(INK)));
    G.rt->FillRoundedRectangle(rr(x, ty, SLIDER_W, 14, 7), brush(ColorF(1, 1, 1)));
    float tx = x + THUMB / 2 + k * (SLIDER_W - THUMB);
    G.rt->PushAxisAlignedClip(RectF(x, ty, tx, ty + 14), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    G.rt->FillRoundedRectangle(rr(x, ty, SLIDER_W, 14, 7), brush(rgb(cat)));
    G.rt->PopAxisAlignedClip();
    G.rt->DrawRoundedRectangle(rr(x + 1.25f, ty + 1.25f, SLIDER_W - 2.5f, 11.5f, 5.75f), brush(rgb(INK)), 2.5f);
    draw_star(tx, y + 14, THUMB, rgb(GOLD), rgb(INK), 2.4f);
    float vx = x + SLIDER_W + 7;
    Rot rot(2, vx + 29, y + 14);
    G.rt->FillRoundedRectangle(rr(vx, y + 2, 58, 24, 8), brush(rgb(PAGE_TEXT)));
    text_c(val, fmt(L"Fredoka", 600, 15), vx, y + 14, 58, rgb(INK), CENTER);
}

// A controller icon (pad_icons.cpp), h scene units high, centred on (cx,
// cy): made at the window's real pixel size, once per icon, size and render
// target. Returns its width in scene units; 0 if it isn't there (not read
// yet), and then nothing is drawn.
struct PadIconBmp {
    ComPtr<ID2D1Bitmap> bmp;
    void* rtFor = nullptr;
    int w = 0, h = 0;
};
std::map<std::string, PadIconBmp> g_padIconBmps;
float draw_pad_icon(const char* name, float cx, float cy, float h, float op = 1) {
    if (!name) return 0;
    float k = G.h / 720.f;
    int ph = std::max(1, (int)std::lround(h * k));
    std::string key = std::string(name) + "|" + std::to_string(ph);
    PadIconBmp& b = g_padIconBmps[key];
    if (b.rtFor != G.rt.Get()) b.bmp.Reset(), b.rtFor = G.rt.Get();
    if (!b.bmp) {
        int pw = pad_icons_width(name, ph);
        if (pw <= 0) return 0;
        std::vector<uint8_t> rgba((size_t)pw * ph * 4);
        if (!pad_icons_rgba(name, ph, rgba.data(), pw)) return 0;
        std::vector<uint32_t> px((size_t)pw * ph);
        for (size_t i = 0; i < px.size(); i++) {
            uint32_t r = rgba[i * 4], g = rgba[i * 4 + 1], bl = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
            px[i] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (bl * a / 255);
        }
        D2D1_BITMAP_PROPERTIES p = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(G.rt->CreateBitmap(D2D1::SizeU(pw, ph), px.data(), pw * 4, p, &b.bmp))) return 0;
        b.w = pw, b.h = ph;
    }
    float w = b.w / k, hh = b.h / k;
    G.rt->DrawBitmap(b.bmp.Get(), RectF(cx - w / 2, cy - hh / 2, cx + w / 2, cy + hh / 2), op,
                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return w;
}
// Its width, without drawing it.
float pad_icon_w(const char* name, float h) {
    float k = G.h / 720.f;
    int ph = std::max(1, (int)std::lround(h * k));
    int pw = name ? pad_icons_width(name, ph) : 0;
    return pw / k;
}

// The players' slots (Multiplayer): who each player is, from input_win32.c's
// pad_of (player 1 the keyboard and mouse, with controller 1 unless the
// keyboard plays alone; the controllers after).
const uint32_t kPlayerColors[4] = {0xe2457f, 0x4a7fe0, 0x5bc26a, 0xf6d44a};
std::wstring pad_kind(int type) {
    return type == PADSET_PS ? L"PlayStation controller" : type == PADSET_NS ? L"Switch controller" : L"Xbox controller";
}
void draw_players(const Placed& p, float dy, int row, bool focused) {
    bool alone = g_x[X_KEYBOARD_PLAYER] != 0;
    for (int n = 0; n < 4; n++) {
        D2D1_RECT_F b = shift(p.parts[n], 0, dy);
        std::wstring dev, st;
        bool joined = true;
        if (n == 0) {
            dev = L"Keyboard + mouse";
            int t0 = alone ? 0 : pad_sdl_type(0);
            st = t0 ? L"Joined, with the " + pad_kind(t0) + L" · leads in the park" : L"Joined · leads in the park";
        } else {
            int t = pad_sdl_type(alone ? n - 1 : n);
            joined = t != 0;
            dev = joined ? pad_kind(t) : L"Press any button to join";
            st = joined ? L"Joined" : L"Open slot";
        }
        bool hov = U.hoverRow == row && U.hoverPart == n && n < 2;
        float cy = (b.top + b.bottom) / 2;
        if (joined) sticker(b, 12, hov ? rgb(0xfff3cf) : rgb(0xfff8e6), 3, 2.5f, rgb(INK), false, rgb(LEATHER_DARK));
        else sticker(b, 12, ColorF(0, 0, 0, 0), 0, 2.5f, rgb(0xffe2b4, .6f), true);
        if (joined) circle(b.left + 30, cy, 20, rgb(kPlayerColors[n]), 2.5f);
        else circle(b.left + 30, cy, 20, ColorF(0, 0, 0, 0), 2.5f, rgb(0xffe2b4, .6f), true);
        std::wstring tag = L"P" + std::to_wstring(n + 1);
        if (joined) text_c(tag, fmt(L"Fredoka", 700, 16), b.left + 10, cy + 1, 40, ColorF(0, 0, 0, .3f), CENTER);
        text_c(tag, fmt(L"Fredoka", 700, 16), b.left + 10, cy, 40, joined ? ColorF(1, 1, 1) : rgb(PAGE_SOFT), CENTER);
        float tx = b.left + 58, tw = b.right - tx - 10;
        auto l1 = layout(dev, fmt(L"Nunito", 800, 14), tw);
        auto l2 = layout(st, fmt(L"Nunito", 700, 12), tw);
        float h1 = measure(l1.Get()).height, h2 = measure(l2.Get()).height, ty = cy - (h1 + 3 + h2) / 2;
        G.rt->DrawTextLayout(Point2F(tx, ty), l1.Get(), brush(joined ? rgb(INK) : rgb(PAGE_TEXT)));
        G.rt->DrawTextLayout(Point2F(tx, ty + h1 + 3), l2.Get(), brush(joined ? rgb(0x6b5a45) : rgb(PAGE_SOFT)));
        if (focused && U.focusSlot == n) draw_focus(b, 12);
    }
}

void draw_rows(float listTop, float t) {
    const Category& c = cur();
    uint32_t cat = c.gem;
    IDWriteTextFormat* fName = fmt(L"Nunito", 800, 16);
    IDWriteTextFormat* fDesc = fmt(L"Nunito", 600, 13);
    int heads = 0;
    for (size_t i = 0; i < c.rows.size(); i++) {
        const Row& r = c.rows[i];
        const Placed& p = g_placed[i];
        int headN = r.kind == HEAD ? heads++ : 0;
        float y0 = listTop + p.y - U.scroll;
        if (y0 > listTop + view_h() + 40 || y0 + p.h < listTop - 40) continue;
        float dy = listTop - U.scroll;  // list -> panel
        bool focused = U.focus == (int)i && U.sheet < 0;
        auto separator = [&] {
            G.rt->DrawLine(Point2F(ROW_X, y0 + p.h - 1), Point2F(ROW_X + ROW_W, y0 + p.h - 1), brush(line_color()), 2, G.dashed.Get());
        };
        if (r.kind == HEAD) {
            // Washi tape in the tab's color, a little tilted.
            IDWriteTextFormat* f = fmt(L"Fredoka", 600, 13);
            std::wstring up = upper(r.label);
            float tw = text_w(up, f, 13 * .12f);
            float sw = r.soon ? stamp(L"Coming soon", 0, 0, true, false) + 8 : 0;
            D2D1_RECT_F b = RectF(ROW_X - 6, y0 + 18, ROW_X - 6 + 16 + tw + sw + 16, y0 + 18 + 25);
            Rot rot(headN % 2 ? 1.2f : -1.5f, (b.left + b.right) / 2, (b.top + b.bottom) / 2);
            tape(b, rgb(cat, .95f), .22f, 7, kSecTape);
            text_c(up, f, b.left + 16, y0 + 18 + 12.5f, tw + 4, rgb(c.ink), LEFT, 13 * .12f);
            if (r.soon) stamp(L"Coming soon", b.left + 16 + tw + 8, y0 + 18 + 12.5f, true);
            continue;
        }
        if (r.kind == NOTE && r.label.empty()) {
            text(r.desc, fDesc, ROW_X, y0 + 2, ROW_W, rgb(PAGE_SOFT));
            continue;
        }
        if (r.kind >= FBTOP) {
            draw_fb_row((int)i, r, p, y0, dy, focused);
            continue;
        }
        if (r.kind == PLAYERS) {
            draw_players(p, dy, (int)i, focused);
            continue;
        }
        if (r.kind == BINDHEAD) {
            const wchar_t* heads5[5] = {L"ACTION", L"KEY 1", L"KEY 2", L"PAD 1", L"PAD 2"};
            float fr = (ROW_W - 24) / 5.3f;
            IDWriteTextFormat* f = fmt(L"Nunito", 800, 11.5f);
            text(heads5[0], f, ROW_X, y0 + 8, fr * 1.3f, rgb(PAGE_SOFT), LEFT, .9f);
            for (int s = 0; s < 4; s++) text(heads5[s + 1], f, ROW_X + fr * 1.3f + 6 + s * (fr + 6), y0 + 8, fr, rgb(PAGE_SOFT), CENTER, .9f);
            continue;
        }
        if (r.kind == BIND || r.kind == FIXEDBIND) {
            float fr = (ROW_W - 24) / 5.3f;
            text_fit(r.label, fmt(L"Nunito", 800, 14), ROW_X, y0 + 5 + 8, fr * 1.3f, rgb(PAGE_TEXT), LEFT);
            for (int s = 0; s < 4; s++) {
                D2D1_RECT_F b = shift(p.parts[s], 0, dy);
                std::wstring v;
                if (r.kind == FIXEDBIND) v = s == 0 ? L"F1" : s == 2 ? L"Back + Start" : L"";
                else v = slot_text(r.act, s);
                bool listen = U.listenRow == (int)i && U.listenSlot == s;
                bool hov = U.hoverRow == (int)i && U.hoverPart == s && r.kind == BIND;
                float bw = b.right - b.left;
                // Pad slots: the pad's own icons (the Button icons setting or
                // the connected pad's type), combinations as icon + icon.
                std::vector<const char*> icons;
                if (r.kind == FIXEDBIND && s == 2)
                    icons = {pad_icons_name(XPAD_BACK, pad_icons_pad_set()), pad_icons_name(XPAD_START, pad_icons_pad_set())};
                else if (s >= 2 && r.kind == BIND && g_settings.pad[r.act][s - 2])
                    icons = {pad_icons_name(g_settings.pad[r.act][s - 2], pad_icons_pad_set())};
                for (auto* n : icons)
                    if (pad_icon_w(n, 26) <= 0) icons.clear();  // not read yet: the name, as before
                Fade fade(r.kind == FIXEDBIND ? .7f : 1);
                if (listen) {
                    float blink = U.reduced ? 1 : .6f + .4f * (.5f + .5f * std::cos((float)(t - U.listenT) * 2 * PI));
                    G.rt->FillRoundedRectangle(rr(b.left, b.top + 3, bw, 34, 9), brush(rgb(INK)));
                    G.rt->FillRoundedRectangle(rr(b.left, b.top, bw, 34, 9), brush(rgb(cat)));
                    G.rt->FillRoundedRectangle(rr(b.left, b.top, bw, 34, 9), brush(ColorF(1, 1, 1, 1 - blink)));
                    G.rt->DrawRoundedRectangle(rr(b.left + 1.25f, b.top + 1.25f, bw - 2.5f, 31.5f, 7.75f), brush(rgb(INK)), 2.5f);
                    text_fit(s >= 2 ? L"Press a button…" : L"Press a key…", fmt(L"Nunito", 800, s >= 2 ? 10.5f : 12), b.left + 4,
                             b.top + (s >= 2 ? 9 : 8), bw - 8, rgb(c.ink), CENTER);
                } else if (v.empty() && icons.empty()) {
                    G.rt->DrawRoundedRectangle(rr(b.left + 1.25f, b.top + 1.25f, bw - 2.5f, 31.5f, 7.75f), brush(rgb(0xffe2b4, hov ? .8f : .5f)),
                                               2.5f, G.dashed.Get());
                    if (r.kind == BIND)
                        text_fit(L"+ Add", fmt(L"Nunito", 800, 12), b.left + 6, b.top + 8, bw - 12, rgb(0xffeccd, .75f), CENTER);
                } else {
                    // A keycap: white, ink outline, a hard shadow below.
                    G.rt->FillRoundedRectangle(rr(b.left, b.top + 3, bw, 34, 9), brush(rgb(INK)));
                    G.rt->FillRoundedRectangle(rr(b.left, b.top, bw, 34, 9), brush(hov ? rgb(0xfff3cf) : ColorF(1, 1, 1)));
                    G.rt->DrawRoundedRectangle(rr(b.left + 1.25f, b.top + 1.25f, bw - 2.5f, 31.5f, 7.75f), brush(rgb(INK)), 2.5f);
                    if (!icons.empty()) {
                        float ih = 26, plus = icons.size() > 1 ? 12 : 0, total = plus;
                        for (auto* n : icons) total += pad_icon_w(n, ih);
                        if (total > bw - 6) ih = ih * (bw - 6 - plus) / (total - plus), total = bw - 6;
                        float x = (b.left + b.right) / 2 - total / 2, cy = b.top + 17;
                        for (size_t k = 0; k < icons.size(); k++) {
                            if (k) {
                                text_c(L"+", fmt(L"Nunito", 800, 13), x, cy, plus, rgb(INK), CENTER);
                                x += plus;
                            }
                            float iw = pad_icon_w(icons[k], ih);
                            draw_pad_icon(icons[k], x + iw / 2, cy, ih);
                            x += iw;
                        }
                    } else {
                        text_fit(v, fmt(L"Nunito", 800, 12), b.left + 6, b.top + 8, bw - 12, rgb(INK), CENTER);
                    }
                }
                if (focused && U.focusSlot == s && r.kind == BIND) draw_focus(b, 9);
            }
            separator();
            continue;
        }
        // Label and description; the stamp at the right when it can't be changed.
        auto ln = layout(r.label, fName, p.labelW);
        G.rt->DrawTextLayout(Point2F(ROW_X, y0 + 13), ln.Get(), brush(rgb(PAGE_TEXT)));
        float nameH = measure(ln.Get()).height;
        if (!r.desc.empty())
            G.rt->DrawTextLayout(Point2F(ROW_X, y0 + 13 + nameH + 2), layout(r.desc, fDesc, p.labelW).Get(), brush(rgb(PAGE_SOFT)));
        if (r.soon) stamp(r.tag.empty() ? L"Coming soon" : r.tag, ROW_X + ROW_W - p.stampW, y0 + p.h / 2, false);
        if (r.kind == NOTE) {
            separator();
            continue;
        }
        float cy = p.ctrlY + dy;
        int v = r.get ? r.get() : 0;
        {
            Fade fade(r.soon ? .4f : 1);
            if (r.kind == TOGGLE) {
                draw_switch(p.ctrlX, cy, v != 0, cat);
            } else if (r.kind == SLIDER) {
                draw_slider(p.ctrlX, cy, clamp01((float)(v - r.min) / (r.max - r.min)), slider_text(r, v), cat);
            } else if (r.kind == TEXTBOX) {
                sticker(RectF(p.ctrlX, cy, p.ctrlX + 180, cy + 36), 10, ColorF(1, 1, 1), 2);
                text_c(L"Your nickname", fmt(L"Nunito", 800, 14), p.ctrlX + 12, cy + 18, 156, rgb(0x9a8c78));
            } else if (r.kind == CHOICE) {
                IDWriteTextFormat* pf = pill_font(r.mini);
                for (size_t o = 0; o < r.opts.size(); o++) {
                    D2D1_RECT_F b = shift(p.parts[o], 0, dy);
                    bool soonOpt = (r.soonOpts >> o & 1) != 0;
                    bool sel = (int)o == v && !soonOpt;
                    bool hov = U.hoverRow == (int)i && U.hoverPart == (int)o && !soonOpt && !r.soon;
                    float bcx = (b.left + b.right) / 2, bcy = (b.top + b.bottom) / 2;
                    if (soonOpt) {
                        // Not built yet: a dashed pill, "soon".
                        Fade f2(.55f);
                        sticker(b, 11, ColorF(1, 1, 1), 0, 2.5f, rgb(INK), true);
                        float tw = text_w(r.opts[o], pf), sw = soon_tag_w();
                        float x0 = bcx - (tw + sw) / 2;
                        text_c(r.opts[o], pf, x0, bcy, tw + 2, rgb(INK));
                        text_c(L"SOON", fmt(L"Nunito", 800, 10), x0 + tw + 6, bcy + 1, sw, rgb(0xc9346c));
                    } else if (sel) {
                        Rot rot(-2.5f, bcx, bcy);
                        sticker(shift(b, -1, -1), 11, rgb(cat), 3);
                        text_c(r.opts[o], pf, b.left - 1, bcy - 1, b.right - b.left, rgb(c.ink), CENTER);
                    } else {
                        sticker(b, 11, hov ? rgb(0xfff3cf) : ColorF(1, 1, 1), 2);
                        text_c(r.opts[o], pf, b.left, bcy, b.right - b.left, rgb(INK), CENTER);
                    }
                }
            }
        }
        if (focused) {
            if (r.kind == TOGGLE) draw_focus(RectF(p.ctrlX, cy, p.ctrlX + 62, cy + 34), 17);
            else if (r.kind == SLIDER) draw_focus(RectF(p.ctrlX - 2, cy - 2, p.ctrlX + p.ctrlW + 2, cy + 30), 8);
            else if (r.kind == TEXTBOX) draw_focus(RectF(p.ctrlX, cy, p.ctrlX + 180, cy + 36), 10);
            else if (r.kind == CHOICE && !p.parts.empty())
                draw_focus(shift(p.parts[std::max(0, std::min<int>(U.focusSlot, (int)p.parts.size() - 1))], 0, dy), 11);
        }
        if (!r.label.empty()) separator();
    }
}

// Where things are, in tab coordinates (for the mouse).
struct Hits {
    D2D1_RECT_F close{}, reset{}, fb{}, sheetClose{}, reveal{}, sheet{};
    D2D1_RECT_F tabs[NTABS]{};
    float tabX = 0, k = 1;  // window = (tab coordinates + (tabX, 10)) * k
} H;

void draw_sheet(float t);  // the hint card (below, with the friends list)
int friends_made();
int friends_total();

// The Friend Book's areas (index tabs in its view).
struct ZoneTab {
    const wchar_t* tab;
    uint32_t gem, ink;
};
const ZoneTab kZoneTabs[NTABS] = {{L"All", 0xf3cf6b, 0x4a2f08},  {L"Cove", 0x4a7fe0, 0xffffff},   {L"Arbor", 0x5bc26a, 0x10182f},
                                  {L"Crag", 0xf39a2b, 0x10182f}, {L"Tech", 0x3fb6c8, 0x10182f},   {L"Arcane", 0x8b5bd2, 0xffffff},
                                  {L"Wish Park", 0xe2457f, 0xffffff}};

// The gold corner piece (the mockup's, 46 x 46), drawn 60 x 60 at (x, y),
// mirrored by (fx, fy).
void corner(float x, float y, float fx, float fy) {
    auto& a = cached_path(L"corner0", "M4 4h36a2 2 0 0 1 0 4H12a4 4 0 0 0-4 4v28a2 2 0 0 1-4 0Z");
    auto& b = cached_path(L"corner1", "M8 8l14 0c-4 2-7 3-8 6-3 1-4 4-6 8Z");
    auto& c = cached_path(L"corner2", "M20.4 17a3.4 3.4 0 1 1-6.8 0a3.4 3.4 0 1 1 6.8 0Z");
    Matrix3x2F old;
    G.rt->GetTransform(&old);
    float s = 60 / 46.f;
    G.rt->SetTransform(Matrix3x2F::Scale(s * fx, s * fy) * Matrix3x2F::Translation(x + (fx < 0 ? 60 : 0), y + (fy < 0 ? 60 : 0)) * old);
    for (auto* g : {a.Get(), b.Get(), c.Get()}) {
        G.rt->FillGeometry(g, brush(g == b.Get() ? rgb(0xffe7a3) : rgb(GOLD)));
        G.rt->DrawGeometry(g, brush(rgb(0x7a5418)), g == a.Get() ? 2.f : 1.6f, G.round.Get());
    }
    G.rt->SetTransform(old);
}

// A paw print (the page's edge), 18 x 40 tile at (x, y).
void paw(float x, float y, D2D1_COLOR_F c) {
    G.rt->FillEllipse(D2D1::Ellipse(Point2F(x + 9, y + 14), 5, 4.2f), brush(c));
    const float toes[4][2] = {{3.6f, 8.6f}, {7.2f, 6}, {10.8f, 6}, {14.4f, 8.6f}};
    for (auto& q : toes) G.rt->FillEllipse(D2D1::Ellipse(Point2F(x + q[0], y + q[1]), 1.9f, 1.9f), brush(c));
}

// The wavy underline under the page's title, from x0 to x1 at y.
void wavy(float x0, float x1, float y, D2D1_COLOR_F c) {
    ComPtr<ID2D1PathGeometry> p;
    ComPtr<ID2D1GeometrySink> s;
    G.f->CreatePathGeometry(&p);
    p->Open(&s);
    s->BeginFigure(Point2F(x0, y), D2D1_FIGURE_BEGIN_HOLLOW);
    bool up = true;
    for (float x = x0; x < x1; x += 10, up = !up)
        s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Point2F(x + 5, y + (up ? -5.f : 5.f)), Point2F(x + 10, y)));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    G.rt->PushAxisAlignedClip(RectF(x0, y - 8, x1, y + 8), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    G.rt->DrawGeometry(p.Get(), brush(c), 3.2f, G.round.Get());
    G.rt->PopAxisAlignedClip();
}

void render(float t) {
    ID2D1RenderTarget* rt = G.rt.Get();
    float k = G.h / 720.f;
    float vw = G.w / k;  // the window's width in scene units
    float openK = U.reduced ? 1 : clamp01((float)(t - U.openT) / .45f);
    float slide = bezier(.2f, .8f, .2f, 1, openK);
    float fade = U.reduced ? 1 : clamp01((float)(t - U.openT) / .35f);
    const Category& c = cur();
    uint32_t cat = c.gem;
    rt->BeginDraw();
    rt->Clear(ColorF(0, 0, 0, 0));
    rt->SetTransform(Matrix3x2F::Scale(k, k));
    // Scrim, darker toward the notebook.
    rt->FillRectangle(RectF(0, 0, vw, 720),
                      grad(Point2F(0, 0), Point2F(vw, 0),
                           {{0, ColorF(10 / 255.f, 14 / 255.f, 32 / 255.f, .15f * fade)},
                            {.5f, ColorF(10 / 255.f, 14 / 255.f, 32 / 255.f, .45f * fade)},
                            {1, ColorF(10 / 255.f, 14 / 255.f, 32 / 255.f, .75f * fade)}})
                          .Get());
    // "Game paused" sticker, bottom left.
    {
        Fade f(fade);
        const std::wstring s = L"Game paused · changes apply live";
        IDWriteTextFormat* tf = fmt(L"Fredoka", 600, 17);
        float tw = text_w(s, tf);
        float w = 3 + 12 + 18 + 10 + tw + 18 + 3, h = 46, x = 32, y = 720 - 32 - h;
        Rot rot(-1.5f, x + w / 2, y + h / 2);
        sticker(RectF(x, y, x + w, y + h), 14, rgb(0xfff8e6), 4, 3);
        rt->FillRoundedRectangle(rr(x + 15 + 3, y + 14, 4, 18, 1.5f), brush(rgb(PINK)));
        rt->FillRoundedRectangle(rr(x + 15 + 11, y + 14, 4, 18, 1.5f), brush(rgb(PINK)));
        text_c(s, tf, x + 15 + 18 + 10, y + h / 2, tw + 4, rgb(INK));
    }
    // The notebook, sliding in from the right.
    float tabX = vw - TAB_W - 10 + 60 * (1 - slide);
    H.tabX = tabX;
    H.k = k;
    Matrix3x2F T = Matrix3x2F::Translation(tabX, 10) * Matrix3x2F::Scale(k, k);
    rt->SetTransform(T);
    bool openLayer = openK < 1;
    if (openLayer)
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                            bezier(.2f, .8f, .2f, 1, openK)),
                      nullptr);
    // The green cover: its shadow, the cover, a dashed cream stitch.
    {
        auto cover = rrect_path(COVER_X, 0, TAB_W - COVER_X, TAB_H, 24, 10, 10, 24);
        fill_geo(cover.Get(), rgb(0x060a1a, .35f), -14, 10);
        rt->FillGeometry(cover.Get(), grad(Point2F(COVER_X + 60, 0), Point2F(TAB_W - 60, TAB_H),
                                           {{0, rgb(0x3f9e45)}, {.55f, rgb(0x2b7f35)}, {1, rgb(0x226a2c)}})
                                          .Get());
        rt->FillGeometry(cover.Get(), rglow(COVER_X + 150, 84, 300, ColorF(1, 1, 1, .12f)).Get());
        auto inner = rrect_path(COVER_X + 3, 3, TAB_W - COVER_X - 6, TAB_H - 6, 21, 7, 7, 21);
        rt->DrawGeometry(inner.Get(), brush(ColorF(1, 1, 1, .12f)), 4);
        rt->DrawGeometry(cover.Get(), brush(rgb(0x16401c)), 3);
        auto stitch = rrect_path(COVER_X + 8, 8, TAB_W - COVER_X - 16, TAB_H - 16, 18, 6, 6, 18);
        rt->DrawGeometry(stitch.Get(), brush(rgb(0xffefbe, .55f)), 2, G.dashed.Get());
    }
    // The index tabs, under the page; the chosen one sticks out further.
    {
        bool zones = U.view != 0;
        float top = zones ? 84 : 92, th = zones ? 58 : 64, gap = zones ? 5 : 7;
        for (int i = 0; i < NTABS; i++) {
            uint32_t gem = zones ? kZoneTabs[i].gem : g_cats[i].gem, ink = zones ? kZoneTabs[i].ink : g_cats[i].ink;
            bool sel = zones ? i == U.zone : i == U.tab;
            bool hov = U.hoverPart == 100 + i && U.hoverRow < 0;
            float x = sel ? -14.f : hov ? -4.f : 0.f, y = top + i * (th + gap);
            auto g = rrect_path(x, y, 96, th, 16, 0, 0, 16);
            D2D1_COLOR_F col = rgb(gem);
            if (!sel && !hov) {  // a little duller (saturate .8, brightness .93)
                float l = .3f * col.r + .59f * col.g + .11f * col.b;
                col = ColorF((col.r * .8f + l * .2f) * .93f, (col.g * .8f + l * .2f) * .93f, (col.b * .8f + l * .2f) * .93f);
            }
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.Get()), nullptr);
            rt->FillRectangle(RectF(x, y, x + 96, y + th), brush(col));
            rt->FillRectangle(RectF(x, y, x + 96, y + 3), brush(ColorF(1, 1, 1, .3f)));
            rt->FillRectangle(RectF(x, y + th - 4, x + 96, y + th), brush(ColorF(0, 0, 0, .12f)));
            rt->PopLayer();
            rt->DrawGeometry(g.Get(), brush(rgb(INK)), 3);
            float cx = x + 6 + 33;
            if (zones) {
                D2D1_COLOR_F ic = rgb(ink);
                draw_icon(i == 0 ? L"heart" : L"pin", cx - 12, y + th / 2 - 22, 24, ic);
            } else {
                draw_icon(g_cats[i].id, cx - 12, y + th / 2 - 22, 24, rgb(ink));
            }
            text_c(zones ? kZoneTabs[i].tab : g_cats[i].name, fmt(L"Fredoka", 600, 11), x + 2, y + th / 2 + 13, 74, rgb(ink), CENTER);
            H.tabs[i] = RectF(x, y, x + 76, y + th);
            if (U.focus == FOCUS_TOP && U.focusSlot == i && U.sheet < 0) draw_focus(RectF(x + 2, y + 2, x + 72, y + th - 2), 14);
        }
    }
    // The page: brown leather, faint grain, a dashed stitched edge, paw prints.
    rt->SetTransform(Matrix3x2F::Translation(PANEL_X, PANEL_Y) * T);
    auto pageGeo = rrect_path(0, 0, PANEL_W, PANEL_H, 6, 8, 8, 6);
    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), pageGeo.Get()), nullptr);
    {
        rt->FillRectangle(RectF(0, 0, PANEL_W, PANEL_H), vgrad(0, PANEL_H, rgb(0x7a5434), rgb(0x6a4529)).Get());
        rt->FillRectangle(RectF(0, 0, PANEL_W, PANEL_H), rglow(PANEL_W * .3f, PANEL_H * .2f, 420, ColorF(1, 1, 1, .05f)).Get());
        // Grain: thin dark lines at 115 degrees, 7 apart.
        float run = PANEL_H * std::tan(25 * PI / 180);
        for (float x = -run; x < PANEL_W; x += 7)
            rt->DrawLine(Point2F(x, PANEL_H), Point2F(x + run, 0), brush(ColorF(0, 0, 0, .035f)), 2);
        rt->DrawRoundedRectangle(rr(8.25f, 8.25f, PANEL_W - 16.5f, PANEL_H - 16.5f, 5), brush(rgb(0xffe2b4, .55f)), 2.5f, G.dashed.Get());
        for (float y = 22; y + 40 <= PANEL_H - 70; y += 40) paw(9, y, rgb(0x432a17));
    }
    // Page head: the badge, the title with its wavy underline, the line under it.
    {
        float bx = 40 + 27, by = 66 + 38;
        {
            Rot rot(-8, bx, by);
            circle(bx + 3, by + 6, 34 + 4, ColorF(0, 0, 0, .25f));
            circle(bx, by, 34, rgb(LEATHER_DARK));
            circle(bx, by, 31, rgb(PAGE_TEXT));
            circle(bx, by, 27, rgb(cat), 3);
            if (U.view) {
                D2D1_COLOR_F f = rgb(0xc9346c);
                draw_icon(L"heart", bx - 12, by - 12, 24, rgb(0xc9346c), &f);
            } else {
                draw_icon(c.id, bx - 12, by - 12, 24, rgb(c.ink));
            }
        }
        IDWriteTextFormat* nf = fmt(L"Fredoka", 700, 32);
        float nx = bx + 27 + 16, nw = text_w(c.name, nf);
        float ny = by - 34;
        text(c.name, nf, nx, ny + 2, 460, ColorF(0, 0, 0, .25f));
        text(c.name, nf, nx, ny, 460, rgb(PAGE_TEXT));
        wavy(nx - 2, nx + nw + 6, ny + 43, rgb(cat));
        text_fit(c.desc, fmt(L"Nunito", 700, 13.5f), nx, ny + 52, PANEL_W - nx - 26, rgb(PAGE_SOFT), LEFT);
    }
    // Rows, clipped to the list area.
    rt->PushAxisAlignedClip(RectF(3, ROWS_TOP - 6, PANEL_W - 3, PANEL_H - ROWS_BOTTOM), D2D1_ANTIALIAS_MODE_ALIASED);
    draw_rows(ROWS_TOP, t);
    rt->PopAxisAlignedClip();
    // Scroll bar
    if (g_listH > view_h()) {
        float vh = view_h(), bh = std::max(30.f, vh * vh / g_listH), by = ROWS_TOP + (vh - bh) * (U.scroll / (g_listH - vh));
        rt->FillRoundedRectangle(rr(PANEL_W - 14, by + 2, 5, bh - 4, 2.5f), brush(rgb(cat, .85f)));
    }
    // Footer: Reset (or Back to Settings), the note.
    {
        float fy = PANEL_H - FOOT_H;
        rt->FillRectangle(RectF(0, fy, PANEL_W, PANEL_H), brush(rgb(0x55361f)));
        rt->FillRectangle(RectF(0, fy - 1.5f, PANEL_W, fy + 1.5f), brush(rgb(0x3b2414)));
        float bh = 38, by = fy + (FOOT_H - bh) / 2 + 1;
        bool hov = U.hoverBtn == 2;
        float rw = U.view ? stone_button(40, by, bh, L"Back to Settings", L"F2", hov) : stone_button(40, by, bh, L"Reset " + c.name, nullptr, hov);
        H.reset = RectF(PANEL_X + 40, PANEL_Y + by, PANEL_X + 40 + rw, PANEL_Y + by + bh);
        if (U.focus == FOCUS_RESET && U.sheet < 0) draw_focus(RectF(40, by, 40 + rw, by + bh), 12);
        text_c(U.view ? L"Read from your save · updates live" : L"Saved to settings.ini right away", fmt(L"Nunito", 800, 12.5f), 40 + rw + 12,
               fy + FOOT_H / 2 + 1, PANEL_W - 24 - (40 + rw + 12), rgb(PAGE_SOFT), RIGHT);
    }
    // The hint card, over the list.
    if (U.view && U.sheet >= 0) draw_sheet(t);
    rt->PopLayer();
    rt->DrawGeometry(pageGeo.Get(), brush(rgb(0x3b2414)), 3);
    // Gold corners, over the cover's and the page's edges.
    rt->SetTransform(T);
    corner(58, -6, 1, 1);
    corner(TAB_W + 6 - 60, -6, -1, 1);
    corner(58, TAB_H + 6 - 60, 1, -1);
    corner(TAB_W + 6 - 60, TAB_H + 6 - 60, -1, -1);
    // The top: the page's label on gold tape; the Friend Book ribbon and Close.
    {
        IDWriteTextFormat* f = fmt(L"Fredoka", 600, 13);
        std::wstring s = U.view ? L"FRIEND BOOK" : L"SETTINGS";
        float tw = text_w(s, f, 13 * .16f), x = 104, cy = 26 + 23;
        D2D1_RECT_F b = RectF(x, cy - 12, x + 14 + tw + 14, cy + 12);
        {
            Rot rot(-2, (b.left + b.right) / 2, cy);
            tape(b, rgb(GOLD), .35f, 6, {{.02f, 0}, {.98f, .04f}, {1, .5f}, {.97f, 1}, {.01f, .96f}, {0, .5f}});
            text_c(s, f, x + 14, cy, tw + 4, rgb(INK), LEFT, 13 * .16f);
        }
        // Close
        float right = TAB_W - 34, bh = 40, by = cy - bh / 2;
        float cw = stone_button(0, 0, bh, L"Close", L"F1", false, false);
        stone_button(right - cw, by, bh, L"Close", L"F1", U.hoverBtn == 1);
        H.close = RectF(right - cw, by, right, by + bh);
        if (U.focus == FOCUS_TOP && U.focusSlot == CLOSE_SLOT && U.sheet < 0) draw_focus(H.close, 12);
        // The ribbon: pink, a notch at its end (white with "Settings" in the book).
        std::wstring label = U.view ? L"Settings" : L"Friend Book";
        std::wstring count = !U.view && friends_total() > 0 ? std::to_wstring(friends_made()) + L"/" + std::to_wstring(friends_total()) : L"";
        IDWriteTextFormat* lf = fmt(L"Fredoka", 600, 15);
        IDWriteTextFormat* cf = fmt(L"Fredoka", 600, 12.5f);
        float lw = text_w(label, lf), cwid = count.empty() ? 0 : text_w(count, cf) + 14;
        float rh = 42, rw = 3 + 10 + 20 + 7 + lw + (cwid ? 7 + cwid : 0) + 26 + 3;
        float rx = right - cw - 10 - rw, ry = cy - rh / 2;
        bool hov = U.hoverBtn == 3;
        if (hov) rx -= 1, ry -= 1;
        {
            Rot rot(-2, rx + rw / 2, cy);
            D2D1_POINT_2F pts[5] = {{rx, ry}, {rx + rw, ry}, {rx + rw * .92f, ry + rh / 2}, {rx + rw, ry + rh}, {rx, ry + rh}};
            auto g = poly_path(pts, 5);
            fill_geo(g.Get(), rgb(INK), 3, 3);
            rt->FillGeometry(g.Get(), brush(U.view ? ColorF(1, 1, 1) : rgb(PINK)));
            rt->DrawGeometry(g.Get(), brush(rgb(INK)), 3);
            D2D1_COLOR_F hf = U.view ? rgb(PINK) : ColorF(1, 1, 1);
            draw_icon(L"heart", rx + 3 + 10, cy - 10, 20, rgb(0x4a2f08), &hf);
            D2D1_COLOR_F tc = U.view ? rgb(INK) : ColorF(1, 1, 1);
            text_c(label, lf, rx + 3 + 10 + 20 + 7, cy, lw + 4, tc);
            if (cwid) {
                float px = rx + 3 + 10 + 20 + 7 + lw + 7;
                rt->FillRoundedRectangle(rr(px, cy - 10, cwid, 20, 10), brush(ColorF(1, 1, 1, .25f)));
                text_c(count, cf, px, cy, cwid, tc, CENTER);
            }
        }
        H.fb = RectF(rx, ry, rx + rw, ry + rh);
        if (U.focus == FOCUS_TOP && U.focusSlot == FB_SLOT && U.sheet < 0) draw_focus(H.fb, 8);
    }
    if (openLayer) rt->PopLayer();
    rt->EndDraw();
}

bool ensure_target(int w, int h) {
    if (!G.ok) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, G.f.GetAddressOf()))) return false;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&G.wic)))) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&G.wic)))) return false;
        }
        if (!load_fonts()) return false;
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                     D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        G.f->CreateStrokeStyle(sp, nullptr, 0, &G.round);
        // Dashes as the mockup's dashed borders: about 3 strokes long, 2 apart.
        const float dashes[2] = {2.6f, 1.8f};
        D2D1_STROKE_STYLE_PROPERTIES dp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                     D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_CUSTOM);
        G.f->CreateStrokeStyle(dp, dashes, 2, &G.dashed);
        build_rows();
        G.ok = true;
    }
    if (w == G.w && h == G.h && G.rt) return true;
    G.rt.Reset();
    G.bmp.Reset();
    G.icons.clear();
    if (FAILED(G.wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &G.bmp))) return false;
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(G.f->CreateWicBitmapRenderTarget(G.bmp.Get(), p, &G.rt))) return false;
    G.rt->CreateSolidColorBrush(ColorF(1, 1, 1), &G.solid);
    G.w = w, G.h = h;
    G.image.assign((size_t)w * h, 0);
    return true;
}

// ------------------------------------------------------------------------
// Actions

const Row* row_at(int i) {
    const Category& c = cur();
    return i >= 0 && i < (int)c.rows.size() ? &c.rows[i] : nullptr;
}

// Where the highlight starts on a line: the tab of this page, the chosen
// choice, the first key slot or card.
void enter_line(int focus) {
    int prevSlot = U.focusSlot;
    const Row* prev = row_at(U.focus);
    bool fromCards = prev && prev->kind == CARDS;
    U.focus = focus;
    if (focus == FOCUS_TOP) U.focusSlot = U.view ? U.zone : U.tab;
    else if (const Row* r = row_at(focus)) {
        if (r->kind == CHOICE) U.focusSlot = std::max(0, r->get ? r->get() : 0);
        else if (r->kind == CARDS && fromCards) U.focusSlot = std::min(prevSlot, (int)r->items.size() - 1);
        else if (r->kind == PLACES) {
            auto it = std::find(r->items.begin(), r->items.end(), U.place);
            U.focusSlot = it == r->items.end() ? 0 : (int)(it - r->items.begin());
        } else U.focusSlot = 0;
    } else U.focusSlot = 0;
}

void build_fb_rows();  // below, with the friends list

// The Friend Book's rows again (a filter, a place or the save changed),
// keeping the highlight where it was as far as it can.
void fb_rebuild() {
    build_fb_rows();
    g_fbDirty = false;
    if (!U.view) return;
    place_rows();
    clamp_scroll();
    if (U.focus >= (int)g_fb.rows.size()) U.focus = FOCUS_RESET;
    changed();
}

void set_tab(int t) {
    t = (t + NTABS) % NTABS;
    U.listenRow = U.listenSlot = -1;
    U.scroll = 0;
    if (U.view) {
        U.zone = t;
        U.place = -1;
        U.sheet = -1;
        fb_rebuild();
    } else {
        U.tab = t;
    }
    place_rows();
    enter_line(FOCUS_TOP);  // the highlight stays on the tabs (the new one)
    changed();
}

// The settings or the Friend Book.
void set_view(int v) {
    if (getenv("WONDERPARK_F1_SCRIPT")) fprintf(stderr, "[f1] %.2f view %d\n", now_s(), v);
    U.view = v;
    U.sheet = -1;
    U.zone = 0, U.place = -1, U.ffilter = 0;
    U.listenRow = U.listenSlot = -1;
    U.scroll = 0;
    if (v) fb_rebuild();
    place_rows();
    U.focus = FOCUS_TOP;
    U.focusSlot = FB_SLOT;
    changed();
}

void open_tab(bool on) {
    if (on == U.open) return;
    if (getenv("WONDERPARK_F1_SCRIPT")) fprintf(stderr, "[f1] %.2f %s\n", now_s(), on ? "open" : "close");
    U.open = on;
    U.listenRow = U.listenSlot = -1;
    U.dragRow = -1;
    if (on) {
        U.openT = now_s();
        U.reduced = reduced_motion();
        U.view = 0;
        U.sheet = -1;
        if (G.ok) place_rows();
        enter_line(FOCUS_TOP);
    }
    // The mouse cursor: free and shown while the tab is up (input_win32.c
    // stops capturing it), the game's again after.
    if (on && !g_wp_headless) input_release_mouse();
    changed();
}

void choose(const Row& r, int v) {
    if (r.soon || !r.set) return;
    if (r.kind == CHOICE) {
        if (v < 0 || v >= (int)r.opts.size() || (r.soonOpts >> v & 1)) return;
    }
    if (r.kind == SLIDER) {
        v = (int)std::lround((double)(v - r.min) / r.step) * r.step + r.min;
        v = std::max(r.min, std::min(r.max, v));
    }
    if (r.get && r.get() == v) return;
    r.set(v);
    changed();
}

// Left/Right on a row: the next choice (skipping ones not available), a step, a flip.
void step(const Row& r, int d) {
    if (r.soon) return;
    int v = r.get ? r.get() : 0;
    if (r.kind == TOGGLE) choose(r, !v);
    else if (r.kind == SLIDER) choose(r, v + d * r.step);
    else if (r.kind == CHOICE) {
        int n = (int)r.opts.size();
        for (int k = 1; k < n; k++) {
            int o = v + d * k;
            if (o < 0 || o >= n) break;
            if (!(r.soonOpts >> o & 1)) {
                choose(r, o);
                break;
            }
        }
    }
}

void reset_tab() {
    const Category& c = g_cats[U.tab];
    for (auto& r : c.rows)
        if (!r.soon && r.reset) r.reset();
    if (c.id == L"controls") {
        settings_default_controls();
        settings_save();
    }
    U.listenRow = U.listenSlot = -1;
    changed();
}

void start_listen(int row, int slot) {
    U.listenRow = row;
    U.listenSlot = slot;
    U.listenT = now_s();
    changed();
}

void bind_now(int vk, uint32_t pad) {
    const Row* r = row_at(U.listenRow);
    if (!r || r->kind != BIND) return;
    int s = U.listenSlot;
    if (s < 2 && vk) settings_bind(r->act, s, vk, 0);
    else if (s >= 2 && pad) settings_bind(r->act, s - 2, 0, pad);
    U.listenRow = U.listenSlot = -1;
    changed();
}
void clear_slot() {
    const Row* r = row_at(U.listenRow);
    if (r && r->kind == BIND) {
        if (U.listenSlot < 2) g_settings.key[r->act][U.listenSlot] = 0;
        else g_settings.pad[r->act][U.listenSlot - 2] = 0;
        settings_save();
    }
    U.listenRow = U.listenSlot = -1;
    changed();
}

void move_focus(int d) {
    auto f = focusables();
    if (f.empty()) return;
    auto it = std::find(f.begin(), f.end(), U.focus);
    int i = it == f.end() ? (d > 0 ? -1 : (int)f.size()) : (int)(it - f.begin());
    i = std::max(0, std::min((int)f.size() - 1, i + d));
    enter_line(f[i]);
    if (U.focus >= 0) scroll_to(U.focus);
    else if (U.focus == FOCUS_TOP) U.scroll = 0, clamp_scroll();
    else U.scroll = std::max(0.f, g_listH - view_h());
    changed();
}

// The hint card: open on a friend, closed; its "show more".
bool sheet_can_reveal();  // below, with the friends list
void open_sheet(int f) {
    U.sheet = f;
    U.revealed = false;
    changed();
}
void close_sheet() {
    U.sheet = -1;
    changed();
}

// A (controller), Enter or Space, a click: what's highlighted.
void activate() {
    if (U.sheet >= 0) {
        if (sheet_can_reveal() && !U.revealed) U.revealed = true, changed();
        else close_sheet();
        return;
    }
    if (U.focus == FOCUS_TOP) {
        if (U.focusSlot == CLOSE_SLOT) open_tab(false);
        else if (U.focusSlot == FB_SLOT) set_view(!U.view);
        else if (U.focusSlot != (U.view ? U.zone : U.tab)) set_tab(U.focusSlot);
        return;
    }
    if (U.focus == FOCUS_RESET) {
        if (U.view) set_view(0);
        else reset_tab();
        return;
    }
    const Row* r = row_at(U.focus);
    if (!r) return;
    if (r->kind == BIND) start_listen(U.focus, U.focusSlot);
    else if (r->kind == TOGGLE) step(*r, 1);
    else if (r->kind == CHOICE) choose(*r, U.focusSlot);
    else if (r->kind == PLAYERS) {
        if (U.focusSlot < 2) choose(*r, !r->get());
    } else if (r->kind == CARDS) {
        if (U.focusSlot >= 0 && U.focusSlot < (int)r->items.size()) open_sheet(r->items[U.focusSlot]);
    } else if (r->kind == PLACES) {
        if (U.focusSlot >= 0 && U.focusSlot < (int)r->items.size() && U.place != r->items[U.focusSlot]) {
            U.place = r->items[U.focusSlot];
            fb_rebuild();
        }
    }
}

// The stick, d-pad and arrow keys move the highlight: up and down between
// lines, left and right between a line's parts (on a slider they move its
// value, on a switch they set it).
enum Nav { N_UP, N_DOWN, N_LEFT, N_RIGHT, N_OK, N_BACK, N_PREV, N_NEXT, N_BOOK };
int parts_on_line(const Row* r) {
    if (!r) return 0;
    if (r->kind == CHOICE) return (int)r->opts.size();
    if (r->kind == BIND || r->kind == PLAYERS) return 4;
    if (r->kind == CARDS || r->kind == PLACES) return (int)r->items.size();
    return 0;
}
void nav(Nav n) {
    U.keyNav = true;
    if (U.sheet >= 0) {  // the hint card: A shows more (or closes), B closes
        if (n == N_OK) activate();
        else if (n == N_BACK) close_sheet();
        else if (n == N_BOOK) close_sheet(), set_view(0);
        return;
    }
    if (U.focus == -1 && n != N_BACK && n != N_PREV && n != N_NEXT && n != N_BOOK) {
        enter_line(FOCUS_TOP);
        changed();
        if (n != N_DOWN) return;
    }
    const Row* r = row_at(U.focus);
    switch (n) {
    case N_UP: move_focus(-1); break;
    case N_DOWN: move_focus(1); break;
    case N_LEFT:
    case N_RIGHT: {
        int d = n == N_RIGHT ? 1 : -1;
        if (U.focus == FOCUS_TOP) {
            U.focusSlot = std::max(0, std::min(CLOSE_SLOT, U.focusSlot + d));
            changed();
        } else if (int np = parts_on_line(r)) {
            U.focusSlot = std::max(0, std::min(np - 1, U.focusSlot + d));
            changed();
        } else if (r && r->kind == SLIDER) {
            step(*r, d);
        } else if (r && r->kind == TOGGLE && !r->soon) {
            choose(*r, d > 0 ? 1 : 0);
        }
        break;
    }
    case N_OK: activate(); break;
    case N_BACK: open_tab(false); break;
    case N_PREV: set_tab((U.view ? U.zone : U.tab) - 1); break;
    case N_NEXT: set_tab((U.view ? U.zone : U.tab) + 1); break;
    case N_BOOK: set_view(!U.view); break;
    }
    if (getenv("WONDERPARK_F1_SCRIPT"))  // tests: where the highlight is
        fprintf(stderr, "[f1] nav %d: view %d, tab %d, focus %d, part %d, window_mode %d\n", (int)n, U.view, U.tab, U.focus,
                U.focusSlot, g_x[X_WINDOW_MODE]);
}

// ---- Mouse: what's under a window point.
// what: 1 close, 2 reset (or Back to Settings), 3 the Friend Book ribbon,
// 4 a row's part, 5 the hint card's close, 6 its "show more", 7 an index tab,
// 8 inside the hint card.
struct Hit { int what = 0; int row = -1, part = -1; };
Hit hit_test(int px, int py) {
    Hit h;
    float x = px / H.k - H.tabX, y = py / H.k - 10;
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };
    if (in(H.close)) return {1};
    if (in(H.fb)) return {3};
    if (U.sheet >= 0) {
        if (in(H.sheetClose)) return {5};
        if (sheet_can_reveal() && !U.revealed && in(H.reveal)) return {6};
        if (in(H.sheet)) return {8};
    }
    if (in(H.reset)) return {2};
    for (int i = 0; i < NTABS; i++)
        if (in(H.tabs[i])) return {7, -1, i};
    float lx = x - PANEL_X, ly = y - PANEL_Y;
    if (lx < 0 || lx > PANEL_W || ly < ROWS_TOP || ly > PANEL_H - ROWS_BOTTOM) return h;
    float listY = ly - ROWS_TOP + U.scroll;
    const Category& c = cur();
    for (size_t i = 0; i < c.rows.size() && i < g_placed.size(); i++) {
        const Placed& p = g_placed[i];
        if (listY < p.y || listY > p.y + p.h) continue;
        const Row& r = c.rows[i];
        if (!p.parts.empty()) {
            for (size_t s = 0; s < p.parts.size(); s++) {
                const D2D1_RECT_F& b = p.parts[s];
                if (lx >= b.left && lx <= b.right && listY >= b.top && listY <= b.bottom) return {4, (int)i, (int)s};
            }
        } else if (r.kind == TOGGLE || r.kind == SLIDER || r.kind == TEXTBOX) {
            if (lx >= p.ctrlX - 8 && lx <= p.ctrlX + p.ctrlW + 8 && listY >= p.ctrlY - 8 && listY <= p.ctrlY + p.ctrlH + 8)
                return {4, (int)i, 0};
        }
        return h;
    }
    return h;
}

void slider_drag(int row, int px) {
    const Row* r = row_at(row);
    if (!r || r->kind != SLIDER) return;
    float x = px / H.k - H.tabX - PANEL_X - g_placed[row].ctrlX;
    float k = clamp01((x - THUMB / 2) / (SLIDER_W - THUMB));
    choose(*r, (int)std::lround(r->min + k * (r->max - r->min)));
}

// The shots for tests (WONDERPARK_F1_SHOT).
std::string g_shotPrefix;
int g_shotStep = -1;
double g_shotAt = 0;

// ========================================================================
// The game's own Options page: a "PC Settings" row (docs/settings/
// options-page-pc-settings.html, approach B). Below the game's three
// items the runtime draws a fourth in the game's own style, with the
// game's art and font read at run time from the player's extracted disc
// (game/files/Archive; never in the repo). Down from "Delete a Saved Game"
// moves to it; pressing it shows the seven PC settings in the same rows,
// with the line "In game, press F1 for every setting". The game sees none
// of the buttons while one of these rows has the focus.

// ---- Reading the game's files (LZ11, U8, TPL, BRFNT)

std::vector<uint8_t> lz11(const std::vector<uint8_t>& d) {
    if (d.size() < 4 || d[0] != 0x11) return d;
    size_t size = d[1] | d[2] << 8 | d[3] << 16, p = 4;
    if (!size && d.size() >= 8) size = d[4] | d[5] << 8 | d[6] << 16 | (size_t)d[7] << 24, p = 8;
    std::vector<uint8_t> out;
    out.reserve(size);
    while (out.size() < size && p < d.size()) {
        uint8_t flags = d[p++];
        for (int bit = 0; bit < 8 && out.size() < size && p < d.size(); bit++) {
            if (!(flags & (0x80 >> bit))) {
                out.push_back(d[p++]);
                continue;
            }
            size_t n, disp;
            int ind = d[p] >> 4;
            if (p + (ind == 0 ? 3 : ind == 1 ? 4 : 2) > d.size()) return out;  // (a reference ending the data is fine)
            if (ind == 0) {
                n = (((d[p] & 0xF) << 4) | (d[p + 1] >> 4)) + 0x11;
                disp = (((d[p + 1] & 0xF) << 8) | d[p + 2]) + 1;
                p += 3;
            } else if (ind == 1) {
                n = (((d[p] & 0xF) << 12) | (d[p + 1] << 4) | (d[p + 2] >> 4)) + 0x111;
                disp = (((d[p + 2] & 0xF) << 8) | d[p + 3]) + 1;
                p += 4;
            } else {
                n = ind + 1;
                disp = (((d[p] & 0xF) << 8) | d[p + 1]) + 1;
                p += 2;
            }
            if (disp > out.size()) return out;
            for (size_t k = 0; k < n; k++) out.push_back(out[out.size() - disp]);
        }
    }
    return out;
}
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

// The files of a U8 archive whose names end with any of `want`.
std::map<std::string, std::vector<uint8_t>> u8_pick(const std::vector<uint8_t>& a, std::vector<std::string> want) {
    std::map<std::string, std::vector<uint8_t>> out;
    if (a.size() < 0x20 || be32(&a[0]) != 0x55AA382D) return out;
    uint32_t root = be32(&a[4]);
    if (root + 12 > a.size()) return out;
    uint32_t n = be32(&a[root + 8]), strs = root + n * 12;
    for (uint32_t i = 1; i < n && root + i * 12 + 12 <= a.size(); i++) {
        const uint8_t* e = &a[root + i * 12];
        if (e[0] == 1) continue;  // a folder
        uint32_t no = be32(e) & 0xFFFFFF, off = be32(e + 4), size = be32(e + 8);
        if (strs + no >= a.size() || off + size > a.size()) continue;
        std::string name((const char*)&a[strs + no]);
        for (auto& w : want)
            if (name == w) out[w].assign(a.begin() + off, a.begin() + off + size);
    }
    return out;
}

extern "C" {
void gx_decode_texture(uint8_t* out, const uint8_t* src, int fmt, int width, int height, const uint8_t* tlut,
                       int tlut_fmt);
const char* dvd_root(void);
int hle_options_page_shown(void);
int hle_options_cursor(void);
}

struct Img {
    int w = 0, h = 0;
    std::vector<uint32_t> px;  // BGRA, premultiplied
    ComPtr<ID2D1Bitmap> bmp;   // made for the render target below
    void* rtFor = nullptr;
};

// RGBA (straight) to premultiplied BGRA.
void to_bgra(Img& img, const std::vector<uint8_t>& rgba) {
    img.px.resize((size_t)img.w * img.h);
    for (size_t i = 0; i < img.px.size(); i++) {
        uint32_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        img.px[i] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);
    }
}

// A TPL's first image (any GX format; CI formats with their palette).
bool tpl(const std::vector<uint8_t>& d, Img& img, std::vector<uint8_t>* rgbaOut = nullptr) {
    if (d.size() < 12) return false;
    uint32_t tab = be32(&d[8]);
    if (tab + 8 > d.size()) return false;
    uint32_t ih = be32(&d[tab]), ph = be32(&d[tab + 4]);
    if (ih + 12 > d.size()) return false;
    int h = be16(&d[ih]), w = be16(&d[ih + 2]), fmt = (int)be32(&d[ih + 4]);
    uint32_t off = be32(&d[ih + 8]);
    const uint8_t* tlut = nullptr;
    int tfmt = 0;
    if (ph && ph + 12 <= d.size()) {
        tfmt = (int)be32(&d[ph + 4]);
        tlut = &d[be32(&d[ph + 8])];
    }
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    gx_decode_texture(rgba.data(), &d[off], fmt, w, h, tlut, tfmt);
    img.w = w, img.h = h;
    to_bgra(img, rgba);
    if (rgbaOut) *rgbaOut = rgba;
    return true;
}

// The game's font (Standerd_US_EU.brfnt): I4 glyph sheets, widths, code map.
struct GameFont {
    int cellW = 0, cellH = 0, baseline = 0, cols = 0, rows = 0, sheetW = 0, sheetH = 0, alter = 0, lineFeed = 0;
    std::vector<Img> sheets;
    struct Width { int8_t left; uint8_t glyph; int8_t adv; };
    std::map<int, Width> widths;    // by glyph index
    std::map<uint32_t, int> cmap;  // code point -> glyph index
    bool ok = false;
};

bool brfnt(const std::vector<uint8_t>& f, GameFont& font) {
    if (f.size() < 16 || memcmp(f.data(), "RFNT", 4)) return false;
    size_t p = be16(&f[12]);
    while (p + 8 <= f.size()) {
        const uint8_t* s = &f[p];
        uint32_t size = be32(s + 4);
        if (!size || p + size > f.size()) break;
        if (!memcmp(s, "FINF", 4)) {
            font.lineFeed = (int8_t)s[9];
            font.alter = be16(s + 10);
        } else if (!memcmp(s, "TGLP", 4)) {
            font.cellW = s[8], font.cellH = s[9], font.baseline = s[10];
            uint32_t sheetSize = be32(s + 12);
            int n = be16(s + 16), fmt = be16(s + 18);
            font.cols = be16(s + 20), font.rows = be16(s + 22), font.sheetW = be16(s + 24), font.sheetH = be16(s + 26);
            uint32_t data = be32(s + 28);
            for (int i = 0; i < n && data + (i + 1) * (size_t)sheetSize <= f.size(); i++) {
                std::vector<uint8_t> rgba((size_t)font.sheetW * font.sheetH * 4);
                gx_decode_texture(rgba.data(), &f[data + i * (size_t)sheetSize], fmt, font.sheetW, font.sheetH, nullptr, 0);
                Img img;
                img.w = font.sheetW, img.h = font.sheetH;
                img.px.resize((size_t)img.w * img.h);
                for (size_t k = 0; k < img.px.size(); k++) {
                    uint32_t a = rgba[k * 4 + 3];
                    img.px[k] = a << 24 | a << 16 | a << 8 | a;  // white, alpha = the glyph
                }
                font.sheets.push_back(std::move(img));
            }
        } else if (!memcmp(s, "CWDH", 4)) {
            int first = be16(s + 8), last = be16(s + 10);
            for (int i = first; i <= last && p + 16 + (size_t)(i - first) * 3 + 3 <= f.size(); i++) {
                const uint8_t* e = s + 16 + (i - first) * 3;
                font.widths[i] = {(int8_t)e[0], e[1], (int8_t)e[2]};
            }
        } else if (!memcmp(s, "CMAP", 4)) {
            int begin = be16(s + 8), end = be16(s + 10), method = be16(s + 12);
            const uint8_t* m = s + 20;
            if (method == 0) {
                int base = be16(m);
                for (int c = begin; c <= end; c++) font.cmap[c] = base + (c - begin);
            } else if (method == 1) {
                for (int c = begin; c <= end; c++) {
                    int g = be16(m + (c - begin) * 2);
                    if (g != 0xFFFF) font.cmap[c] = g;
                }
            } else if (method == 2) {
                int n = be16(m);
                for (int i = 0; i < n; i++) font.cmap[be16(m + 2 + i * 4)] = be16(m + 4 + i * 4);
            }
        }
        p += size;
    }
    font.ok = !font.sheets.empty() && font.cellW && font.cols && font.rows;
    return font.ok;
}

// What the Options rows are drawn with, read once on a thread of its own
// (the disc folder is on a hard drive: never on the game thread).
struct OptionsArt {
    Img bgOrange, bgGreen, iconDelete, textDelete, iconPc;
    GameFont font;
};
OptionsArt* g_art;               // set once the thread is done
std::atomic<int> g_artState{0};  // 0 not started, 1 loading, 2 ready, 3 failed

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> d;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        d.resize((size_t)n);
        if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    }
    fclose(f);
    return d;
}

// The orange "selected" row as the game's green one: hue turned from
// orange to green, the same lightness (the game tints it by material).
void to_green(const Img& from, Img& to) {
    to.w = from.w, to.h = from.h;
    to.px.resize(from.px.size());
    for (size_t i = 0; i < from.px.size(); i++) {
        uint32_t c = from.px[i], a = c >> 24;
        if (!a) {
            to.px[i] = 0;
            continue;
        }
        // Premultiplied: back to straight colour first.
        float r = (c >> 16 & 255) * 255.f / a, g = (c >> 8 & 255) * 255.f / a, b = (c & 255) * 255.f / a;
        float nr, ng, nb;
        if (g > r * .82f && b < 110) {
            // The yellow glow of the selected row: the game's unselected
            // rows have none. It becomes the board's light wood, so it also
            // hides the glow of the game's own selected row under ours.
            nr = 0xEC, ng = 0xDF, nb = 0xCC;
        } else {
            // The orange body and its light band, by brightness, to the
            // game's green (sampled from its own unselected rows).
            float l = (r + g) / 2 + b * .25f;
            nr = l * .42f, ng = std::min(255.f, l * .98f + 12), nb = l * .30f + b * .2f;
        }
        auto pm = [a](float v) { return (uint32_t)std::min(255.f, std::max(0.f, v)) * a / 255; };
        to.px[i] = a << 24 | pm(nr) << 16 | pm(ng) << 8 | pm(nb);
    }
}

// A white-on-black label image as the game's unselected rows show it: the
// letters dark red-brown, their outline light (the menu's material swaps
// them; selected rows show it white on dark red).
void tint_unselected(Img& img) {
    for (auto& c : img.px) {
        uint32_t a = c >> 24;
        if (!a) continue;
        float l = (c >> 16 & 255) / (float)a;  // 1 = letter, 0 = outline
        float r = 0x5A * l + 0xFA * (1 - l), g = 0x1E * l + 0xF2 * (1 - l), b = 0x14 * l + 0xE6 * (1 - l);
        c = a << 24 | (uint32_t)(r * a / 255) << 16 | (uint32_t)(g * a / 255) << 8 | (uint32_t)(b * a / 255);
    }
}

void load_options_art() {
    int expected = 0;
    if (!g_artState.compare_exchange_strong(expected, 1)) return;
    std::thread([] {
        auto* art = new OptionsArt();
        std::string root = dvd_root() ? dvd_root() : "";
        auto menu = u8_pick(lz11(read_file(root + "/files/Archive/MenuStartMenu.dac")),
                            {"ListBg03.tpl", "ListIcon08.tpl", "ListText06.tpl"});
        auto msg = u8_pick(lz11(read_file(root + "/files/Archive/PrepareMessageRes.dac")), {"Standerd_US_EU.brfnt"});
        bool ok = tpl(menu["ListBg03.tpl"], art->bgOrange) && tpl(menu["ListIcon08.tpl"], art->iconDelete) &&
                  tpl(menu["ListText06.tpl"], art->textDelete) && brfnt(msg["Standerd_US_EU.brfnt"], art->font);
        if (ok) {
            to_green(art->bgOrange, art->bgGreen);
            tint_unselected(art->textDelete);
            // Our icon: the "!" icon's blue frame with its middle cleared
            // (a "PC" is written in it when drawn).
            art->iconPc = art->iconDelete;
            int w = art->iconPc.w, h = art->iconPc.h;
            for (int y = h / 4; y < h * 3 / 4; y++)
                for (int x = w / 4; x < w * 3 / 4; x++) art->iconPc.px[(size_t)y * w + x] = 0;
            g_art = art;
        } else {
            fprintf(stderr, "options: the game's menu art couldn't be read from %s/files/Archive\n", root.c_str());
            delete art;
        }
        g_artState = ok ? 2 : 3;
    }).detach();
}

// ---- State and input

enum OptState { OPT_GAME, OPT_ROW, OPT_SUB };
struct Opt {
    OptState st = OPT_GAME;
    int sub = 0, top = 0;  // the PC page's focused row and first row shown
    uint32_t prev = 0;
    uint32_t swallow = 0;  // buttons we used: kept from the game until let go
    bool shown = false;
} O;

struct PcRow {
    const wchar_t* label;
    std::vector<std::wstring> opts;
    unsigned soon;  // choices not available yet (bit per choice)
    std::function<int()> get;
    std::function<void(int)> set;
};
std::vector<PcRow> g_pcRows;

void build_pc_rows() {
    if (!g_pcRows.empty()) return;
    auto x = [](int which) {
        return std::make_pair(std::function<int()>([which] { return g_x[which]; }),
                              std::function<void(int)>([which](int v) { settings_set_x(which, v); }));
    };
    auto field = [](int* f, std::vector<int> values) {
        return std::make_pair(std::function<int()>([f, values] {
                                  for (size_t i = 0; i < values.size(); i++)
                                      if (values[i] == *f) return (int)i;
                                  return -1;
                              }),
                              std::function<void(int)>([f, values](int i) {
                                  *f = values[i];
                                  settings_save();
                              }));
    };
    auto add = [](const wchar_t* label, std::vector<std::wstring> opts, unsigned soon,
                  std::pair<std::function<int()>, std::function<void(int)>> gs) {
        g_pcRows.push_back({label, opts, soon, gs.first, gs.second});
    };
    add(L"Display mode", {L"Windowed", L"Borderless", L"Fullscreen"}, 4, x(X_WINDOW_MODE));
    add(L"Resolution", {L"1280×720", L"1600×900", L"1920×1080", L"2560×1440", L"Match screen"}, 0, x(X_RESOLUTION));
    add(L"Frame rate", {L"30 (original)", L"60", L"120", L"144", L"240"}, 0, field(&g_settings.frame_rate, {30, 60, 120, 144, 240}));
    add(L"Anti-aliasing", {L"Off", L"2×", L"4×", L"8×"}, 0, field(&g_settings.msaa, {1, 2, 4, 8}));
    add(L"Graphics quality", {L"Auto", L"Original", L"2×", L"3×", L"4×"}, 0, field(&g_settings.scale, {0, 1, 2, 3, 4}));
    add(L"Widescreen", {L"Off", L"On"}, 0, x(X_WIDESCREEN));
    add(L"V-Sync", {L"Off", L"On"}, 3, x(X_VSYNC));
    add(L"Button icons", {L"Auto", L"Xbox", L"PlayStation", L"Nintendo", L"Keyboard only"}, 0, x(X_BUTTON_ICONS));
}

void pc_step(PcRow& r, int d) {
    int n = (int)r.opts.size(), v = r.get();
    if (v < 0) v = d > 0 ? -1 : n;  // "Auto" internal resolution: start from an end
    for (int k = 1; k <= n; k++) {
        int o = v + d * k;
        if (o < 0 || o >= n) return;
        if (!(r.soon >> o & 1)) {
            r.set(o);
            changed();
            return;
        }
    }
}

// ---- Drawing

// Layout units (the game's 720x480 menu layout, centred) to window pixels,
// over the picture the game shows (gl_backend.c g_picture_rect): 877 units
// across in the game's 16:9 mode, 640 in 4:3, 480 down (render_options).
struct Map {
    float x0, y0, sx, sy;
    D2D1_RECT_F rect(float cx, float cy, float w, float h) const {
        return RectF(x0 + (cx - w / 2) * sx, y0 - (cy + h / 2) * sy, x0 + (cx + w / 2) * sx, y0 - (cy - h / 2) * sy);
    }
};

ID2D1Bitmap* bitmap(Img& img) {
    if (img.rtFor != G.rt.Get()) img.bmp.Reset(), img.rtFor = G.rt.Get();  // the window was resized
    if (!img.bmp && !img.px.empty()) {
        D2D1_BITMAP_PROPERTIES p = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        G.rt->CreateBitmap(D2D1::SizeU(img.w, img.h), img.px.data(), img.w * 4, p, &img.bmp);
    }
    return img.bmp.Get();
}

// ---- The Friend Book (F1's pink ribbon, F2, or Y on a controller; hle_friends.c)
//
// The Pokémon that can be befriended, in the PokéPark Pad's order, with
// their English names, icons and the Pad's own notes about them, read once
// on a thread of their own from the player's extracted disc
// (Archive/ResidentMessage.dac: Pokemon.msbt, labelled NAME_<NAME>;
// Flavor_Pokemon.msbt: FL_<NAME>_020 what it wants of you, _040 the step
// after (a few), _100 its note once a friend; PlaceName.msbt, the places'
// names. Archive/PokemonIcon.dac: Ic<character>.tpl), as
// MildRain479/PokePark-2-Wiicompiled's hle_pad_art.c (CC0) reads the names
// and icons. Nothing of it is in the repo.
//
// Friendship points are read live from the loaded save: 100 or more is a
// friend. A Pokémon with none yet is shown as not met (???): the save's own
// "met" flag hasn't been found, so this is our reading, not the game's.

extern "C" {
int hle_friends_pokemon(void);
int hle_friends_first(void);
int hle_friend_found(int p);
void hle_friend_label(int p, char* out, int size);
int hle_characters(void);
void hle_character_name(int c, char* out, int size);
int hle_friend_points(int p);
}

struct Friend {
    int pokemon = 0;
    std::wstring name;
    Img icon;
    std::wstring want, then, about;  // the Pad's notes: FL_<NAME>_020, _040, _100
};
std::vector<Friend>* g_friends;                    // set once the thread is done
std::map<std::string, std::wstring>* g_placeNames;  // PlaceName.msbt: "Fd0102" -> its name
std::atomic<int> g_friendsState{0};                // 0 not started, 1 loading, 2 ready, 3 failed
const int FRIEND_POINTS = 100;

// An MSBT's text for each label (LBL1's labels give TXT2's UTF-16 texts;
// tags, 0x0E group kind size data, left out).
std::map<std::string, std::wstring> msbt_texts(const std::vector<uint8_t>& m) {
    std::map<std::string, std::wstring> out;
    if (m.size() < 0x20 || memcmp(m.data(), "MsgStdBn", 8) || m[8] != 0xFE) return out;
    const uint8_t *lbl = nullptr, *txt = nullptr;
    uint32_t sections = be16(&m[0x0E]);
    size_t p = 0x20;
    for (uint32_t s = 0; s < sections && p + 16 <= m.size(); s++) {
        uint32_t len = be32(&m[p + 4]);
        if (p + 16 + len > m.size()) break;
        if (!memcmp(&m[p], "LBL1", 4)) lbl = &m[p + 16];
        if (!memcmp(&m[p], "TXT2", 4)) txt = &m[p + 16];
        p += 16 + ((len + 15) & ~15u);
    }
    if (!lbl || !txt) return out;
    uint32_t texts = be32(txt);
    for (uint32_t i = 0; i < be32(lbl); i++) {
        const uint8_t* e = lbl + be32(lbl + 8 + i * 8);
        for (uint32_t k = 0; k < be32(lbl + 4 + i * 8); k++) {
            uint32_t ln = e[0], idx = be32(e + 1 + ln);
            std::string label((const char*)e + 1, ln);
            e += 5 + ln;
            if (idx >= texts) continue;
            std::wstring s;
            const uint8_t* t = txt + be32(txt + 4 + idx * 4);
            for (int guard = 0; guard < 512; guard++) {
                uint32_t ch = be16(t);
                t += 2;
                if (!ch) break;
                if (ch == 0x0E) {
                    t += 4 + be16(t + 4);
                    continue;
                }
                s += ch >= 32 ? (wchar_t)ch : L' ';
            }
            // Tidy: no doubled or trailing spaces (line breaks and left-out tags).
            std::wstring tidy;
            for (wchar_t ch : s)
                if (ch != L' ' || (!tidy.empty() && tidy.back() != L' ')) tidy += ch;
            while (!tidy.empty() && tidy.back() == L' ') tidy.pop_back();
            out[label] = tidy;
        }
    }
    return out;
}

// Where each Pokémon lives: one letter per Pokémon in the Pad's order from
// Pokémon 4 (Piplup; 0-3 are the partners), a place in kPlaces. From the
// handoff's Friend Book roster (settings/ingame-f1-settings.html, the US
// game's areas and places), checked against the game's own name table
// (0x80435C10): the same order. Which Wish Park zone each of its Pokémon is
// in isn't known: they're shown under Wish Park only.
const char kWhere[] =
    "baaaaabaaabbbbbbbaaaabaaaaaabbbbbbbbbbbaaaabcccccccccccccccdccccccdddddddddddddddddddddeeeeeeeeeeeeeefeeeeeffffffffffffffffffg"
    "ggggggggggggggggggggiiihhhhhhhhhhhhhhhjjjjjjjjjjjjjjjjjkkkkkkkkkkkkk";
struct Place {
    const char* label;  // its name in PlaceName.msbt
    int zone;           // its area (an index tab: 1 Cove ... 6 Wish Park)
    uint32_t color;
};
const Place kPlaces[] = {
    {"Fd0101", 1, 0x7fd3e0},  // a: Seasong Beach
    {"Fd0102", 1, 0x79a5f0},  // b: Cove Town
    {"Fd0301", 2, 0x7ad68a},  // c: Windmill Way
    {"Fd0302", 2, 0xb6e07a},  // d: Verdant Court
    {"Fd0401", 3, 0xf7b45c},  // e: Rockridge Fort
    {"Fd0402", 3, 0xf28a6b},  // f: Colosseum
    {"Fd0501", 4, 0x7fd3e0},  // g: Train Depot
    {"Fd0502", 4, 0xa9b8d6},  // h: Unawarehouse
    {"Fd0503", 4, 0xb596f0},  // i: Scientorium
    {"Fd0303", 5, 0xb596f0},  // j: Lakeshore
    {"", 6, 0xf27aa5},        // k: Wish Park, its zone not known
};
const int NPLACES = (int)(sizeof(kPlaces) / sizeof(kPlaces[0]));
const char* const kZoneLabels[NTABS] = {nullptr, "Fd01", "Fd03", "Fd04", "Fd05", "Fd03b", "Fd06"};
const wchar_t* const kZoneFull[NTABS] = {L"Every friend", L"Cove Area", L"Arbor Area", L"Crag Area",
                                         L"Tech Area",    L"Arcane Area", L"Wish Park"};

int place_of(int pokemon) {
    int i = pokemon - 4;
    return i >= 0 && i < (int)sizeof(kWhere) - 1 ? kWhere[i] - 'a' : -1;
}
int zone_of(int pokemon) {
    int p = place_of(pokemon);
    return p < 0 ? 0 : kPlaces[p].zone;
}
std::wstring disc_text(const char* label, const wchar_t* fallback) {
    if (label && *label && g_friendsState == 2 && g_placeNames) {
        auto it = g_placeNames->find(label);
        if (it != g_placeNames->end() && !it->second.empty()) return it->second;
    }
    return fallback;
}
std::wstring place_name(int p) { return p < 0 || p >= NPLACES ? L"" : disc_text(kPlaces[p].label, L""); }
std::wstring zone_name(int z) { return z <= 0 ? std::wstring(kZoneFull[0]) : disc_text(kZoneLabels[z], kZoneFull[z]); }

void load_friends() {
    int expected = 0;
    if (!g_friendsState.compare_exchange_strong(expected, 1)) return;
    std::thread([] {
        std::string root = dvd_root() ? dvd_root() : "";
        auto icons = lz11(read_file(root + "/files/Archive/PokemonIcon.dac"));
        auto msgs = u8_pick(lz11(read_file(root + "/files/Archive/ResidentMessage.dac")),
                            {"Pokemon.msbt", "Flavor_Pokemon.msbt", "PlaceName.msbt"});
        auto names = msbt_texts(msgs["Pokemon.msbt"]);
        auto notes = msbt_texts(msgs["Flavor_Pokemon.msbt"]);
        auto* places = new std::map<std::string, std::wstring>(msbt_texts(msgs["PlaceName.msbt"]));
        // Every icon in the archive, by character name (Ic<name>.tpl).
        std::vector<std::string> want;
        std::vector<std::string> chars;
        for (int c = 0; c < hle_characters(); c++) {
            char buf[32];
            hle_character_name(c, buf, sizeof(buf));
            chars.push_back(buf);
            if (buf[0]) want.push_back(std::string("Ic") + buf + ".tpl");
        }
        auto files = u8_pick(icons, want);
        auto* list = new std::vector<Friend>();
        for (int p = hle_friends_first(); p < hle_friends_pokemon(); p++) {
            char label[40];
            hle_friend_label(p, label, sizeof(label));
            if (strncmp(label, "NAME_", 5) || !hle_friend_found(p)) continue;
            const char* inner = label + 5;
            // Its icon: the character of its name, or failing that the one
            // whose name starts the most like it (Onond for ONONDO).
            std::string best;
            size_t bestN = 4;
            for (auto& ch : chars) {
                size_t n = 0;
                while (n < ch.size() && inner[n] && tolower((unsigned char)ch[n]) == tolower((unsigned char)inner[n])) n++;
                if (n == ch.size() && !inner[n]) n = 99;
                if (n > bestN && files.count("Ic" + ch + ".tpl")) bestN = n, best = ch;
            }
            Friend f;
            f.pokemon = p;
            auto nm = names.find(label);
            f.name = nm != names.end() && !nm->second.empty() ? nm->second : widen(best.empty() ? inner : best.c_str());
            if (!best.empty()) tpl(files["Ic" + best + ".tpl"], f.icon);
            std::string fl = std::string("FL_") + inner + "_";
            f.want = notes[fl + "020"];
            if (f.want.empty()) f.want = notes[fl + "030"];
            f.then = notes[fl + "040"];
            f.about = notes[fl + "100"];
            list->push_back(std::move(f));
        }
        if (list->empty()) {
            fprintf(stderr, "settings: the friends list couldn't be read from %s/files/Archive\n", root.c_str());
            delete list;
            delete places;
            g_friendsState = 3;
            return;
        }
        g_placeNames = places;
        g_friends = list;
        g_friendsState = 2;
    }).detach();
}

bool save_loaded() { return g_friendsState == 2 && hle_friend_points(g_friends->front().pokemon) >= 0; }
// 0 not met (no points yet), 1 met, 2 a friend.
int fstate(const Friend& f) {
    int p = hle_friend_points(f.pokemon);
    return p >= FRIEND_POINTS ? 2 : p > 0 ? 1 : 0;
}
int friends_made() {
    int n = 0;
    if (!save_loaded()) return 0;
    for (auto& f : *g_friends) n += fstate(f) == 2;
    return n;
}
int friends_total() { return save_loaded() ? (int)g_friends->size() : 0; }

bool in_zone(const Friend& f) { return U.zone == 0 || zone_of(f.pokemon) == U.zone; }
// Friends / everyone in the area shown and a place (-1 all of it).
std::pair<int, int> count_in(int place) {
    int made = 0, all = 0;
    if (!save_loaded()) return {0, 0};
    for (auto& f : *g_friends)
        if (in_zone(f) && (place < 0 || place_of(f.pokemon) == place)) all++, made += fstate(f) == 2;
    return {made, all};
}
std::wstring place_chip_name(int place) {
    if (place < 0) return U.zone == 6 ? L"All zones" : L"All places";
    std::wstring n = place_name(place);
    return n.empty() ? L"?" : n;
}
std::wstring place_chip_count(int place) {
    auto c = count_in(place);
    return std::to_wstring(c.first) + L"/" + std::to_wstring(c.second);
}

// The page: the ring and counts, the filter, the places (an area with more
// than one), the cards three to a line, a tip.
void build_fb_rows() {
    Category& c = g_fb;
    c.id = L"friends";
    c.name = zone_name(U.zone);
    c.desc = U.zone == 0 ? L"Everyone you can befriend in the park" : L"Who lives here, place by place";
    c.gem = c.acc = kZoneTabs[U.zone].gem;
    c.ink = kZoneTabs[U.zone].ink;
    c.rows.clear();
    Row top;
    top.kind = FBTOP;
    top.min = top.max = top.step = 0;  // friends, everyone, met but not friends
    if (g_friendsState == 3) top.label = L"Couldn't read the disc", top.desc = L"The Friend Book reads names and pictures from your game files.";
    else if (g_friendsState != 2) top.label = L"Reading…";
    else if (!save_loaded()) top.label = L"Load a save first", top.desc = L"Your friends are read from the game you're playing.";
    std::vector<int> inZone, shown;
    if (top.label.empty()) {
        for (size_t i = 0; i < g_friends->size(); i++) {
            const Friend& f = (*g_friends)[i];
            if (!in_zone(f)) continue;
            int st = fstate(f);
            inZone.push_back((int)i);
            top.max++;
            if (st == 2) top.min++;
            else if (st == 1) top.step++;
            if (U.ffilter == 0 || (U.ffilter == 1) == (st == 2)) shown.push_back((int)i);
        }
    }
    c.rows.push_back(top);
    if (!top.label.empty()) return;
    Row filter;
    filter.kind = CHOICE;
    filter.mini = true;
    filter.opts = {L"All", L"Friends", L"Not yet"};
    filter.get = [] { return U.ffilter; };
    filter.set = [](int v) {
        U.ffilter = v;
        g_fbDirty = true;  // rebuilt at the next draw (this row is in the list being rebuilt)
    };
    filter.reset = [] {};
    c.rows.push_back(filter);
    auto cards = [&](const std::vector<int>& list) {
        if (list.empty()) {
            Row t;
            t.kind = TIP;
            t.desc = L"Nobody to show here with this filter.";
            c.rows.push_back(t);
        }
        for (size_t k = 0; k < list.size(); k += 3) {
            Row r;
            r.kind = CARDS;
            r.items.assign(list.begin() + k, list.begin() + std::min(list.size(), k + 3));
            c.rows.push_back(r);
        }
    };
    std::vector<int> places;
    for (int p = 0; p < NPLACES; p++)
        if (U.zone != 0 && kPlaces[p].zone == U.zone && kPlaces[p].label[0]) places.push_back(p);
    if (places.size() > 1) {
        Row chips;
        chips.kind = PLACES;
        chips.items.push_back(-1);
        chips.items.insert(chips.items.end(), places.begin(), places.end());
        c.rows.push_back(chips);
        for (int p : places) {
            if (U.place >= 0 && p != U.place) continue;
            Row h;
            h.kind = PLACEHEAD;
            h.act = p;
            h.color = kPlaces[p].color;
            c.rows.push_back(h);
            std::vector<int> here;
            for (int i : shown)
                if (place_of((*g_friends)[i].pokemon) == p) here.push_back(i);
            cards(here);
        }
    } else {
        cards(shown);
    }
    Row tip;
    tip.kind = TIP;
    tip.desc = L"Tip: pick any Pokémon for a hint on how to befriend it, in the game's own words. Pokémon you haven't met show as ???.";
    c.rows.push_back(tip);
}

// What the page shows, in a number: rebuilt when it changes (a friend made,
// a save loaded, the list read).
unsigned fb_signature() {
    unsigned s = (unsigned)g_friendsState * 31u + (save_loaded() ? 7u : 0u);
    if (save_loaded())
        for (auto& f : *g_friends) s = s * 3u + (unsigned)fstate(f);
    return s;
}

const uint32_t kCardColors[8] = {0xe2457f, 0xf39a2b, 0x5bc26a, 0x4a7fe0, 0x8b5bd2, 0x3fb6c8, 0xd9a52a, 0xc9346c};

// A Pokémon's portrait: its icon on a colored disc, or a dashed "?".
void portrait(Friend& f, int index, float cx, float cy, float r, bool unmet, D2D1_COLOR_F dash, D2D1_COLOR_F q) {
    if (unmet) {
        circle(cx, cy, r, ColorF(0, 0, 0, 0), r > 30 ? 3.f : 2.5f, dash, true);
        text_c(L"?", fmt(L"Fredoka", 700, r > 30 ? 36.f : 24.f), cx - r, cy, 2 * r, q, CENTER);
        return;
    }
    circle(cx, cy, r, rgb(kCardColors[index % 8]));
    G.rt->FillEllipse(D2D1::Ellipse(Point2F(cx, cy), r, r), rglow(cx - r * .3f, cy - r * .4f, r * 1.1f, ColorF(1, 1, 1, .4f)).Get());
    if (ID2D1Bitmap* b = bitmap(f.icon)) {
        float s = r * .92f;
        G.rt->DrawBitmap(b, RectF(cx - s, cy - s, cx + s, cy + s), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    circle(cx, cy, r, ColorF(0, 0, 0, 0), r > 30 ? 3.f : 2.5f);
}

void hearts(float cx, float cy, int pts) {
    float x = cx - (5 * 10 + 4 * 2) / 2.f;
    for (int i = 0; i < 5; i++, x += 12)
        circle(x + 5, cy, 5, pts >= (i + 1) * 20 ? rgb(PINK) : ColorF(1, 1, 1), 1.5f);
}

void draw_fb_row(int i, const Row& r, const Placed& p, float y0, float dy, bool focused) {
    const Category& c = cur();
    if (r.kind == FBTOP) {
        if (!r.label.empty()) {
            text(r.label, fmt(L"Fredoka", 600, 20), ROW_X, y0 + 24, ROW_W, rgb(PAGE_TEXT));
            if (!r.desc.empty()) text(r.desc, fmt(L"Nunito", 700, 13), ROW_X, y0 + 54, ROW_W, rgb(PAGE_SOFT));
            return;
        }
        int made = r.min, total = r.max, met = r.step, unmet = total - made - met;
        float pct = total ? (float)made / total : 0;
        float cx = ROW_X + 38, cy = y0 + 10 + 38, R = 38;
        circle(cx + 3, cy + 3, R, rgb(INK));
        circle(cx, cy, R, ColorF(1, 1, 1));
        if (pct >= 1) circle(cx, cy, R, rgb(c.gem));
        else if (pct > 0) {
            ComPtr<ID2D1PathGeometry> g;
            ComPtr<ID2D1GeometrySink> s;
            G.f->CreatePathGeometry(&g);
            g->Open(&s);
            float a = pct * 2 * PI;
            s->BeginFigure(Point2F(cx, cy), D2D1_FIGURE_BEGIN_FILLED);
            s->AddLine(Point2F(cx, cy - R));
            s->AddArc(D2D1::ArcSegment(Point2F(cx + R * std::sin(a), cy - R * std::cos(a)), D2D1::SizeF(R, R), 0,
                                       D2D1_SWEEP_DIRECTION_CLOCKWISE, pct > .5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
            G.rt->FillGeometry(g.Get(), brush(rgb(c.gem)));
        }
        circle(cx, cy, R, ColorF(0, 0, 0, 0), 3, rgb(LEATHER_DARK));
        circle(cx, cy, 26, rgb(0x6a4529), 2.5f, rgb(LEATHER_DARK));
        text_c(std::to_wstring((int)std::lround(pct * 100)) + L"%", fmt(L"Fredoka", 600, 16), cx - 26, cy - 5, 52, rgb(PAGE_TEXT), CENTER);
        text_c(L"friends", fmt(L"Fredoka", 600, 10), cx - 26, cy + 11, 52, rgb(PAGE_SOFT), CENTER);
        float sx = ROW_X + 76 + 18;
        text_c(std::to_wstring(made) + L" of " + std::to_wstring(total) + L" befriended", fmt(L"Fredoka", 600, 20), sx, cy - 11, ROW_W - 100,
               rgb(PAGE_TEXT));
        text_c(std::to_wstring(met) + L" met but not friends yet · " + std::to_wstring(unmet) + L" not met", fmt(L"Nunito", 700, 13), sx,
               cy + 15, ROW_W - 100, rgb(PAGE_SOFT));
        return;
    }
    if (r.kind == PLACES) {
        for (size_t k = 0; k < r.items.size(); k++) {
            int pid = r.items[k];
            D2D1_RECT_F b = shift(p.parts[k], 0, dy);
            bool sel = U.place == pid, hov = U.hoverRow == i && U.hoverPart == (int)k;
            uint32_t col = pid < 0 ? c.gem : kPlaces[pid].color;
            float bcy = (b.top + b.bottom) / 2;
            {
                Rot rot(sel ? -2.f : 0.f, (b.left + b.right) / 2, bcy);
                sticker(b, 11, sel ? rgb(col) : hov ? rgb(0xfff3cf) : ColorF(1, 1, 1), 2);
                std::wstring name = place_chip_name(pid), count = place_chip_count(pid);
                IDWriteTextFormat* nf = fmt(L"Fredoka", 600, 14);
                IDWriteTextFormat* cf = fmt(L"Nunito", 800, 11.5f);
                float nw = text_w(name, nf), cw = text_w(count, cf) + 14;
                text_c(name, nf, b.left + 2.5f + 11, bcy, nw + 4, rgb(INK));
                float px = b.left + 2.5f + 11 + nw + 6;
                G.rt->FillRoundedRectangle(rr(px, bcy - 10, cw, 20, 10), brush(sel ? ColorF(1, 1, 1, .6f) : rgb(0xf6ecd2)));
                text_c(count, cf, px, bcy, cw, rgb(INK), CENTER);
            }
            if (focused && U.focusSlot == (int)k) draw_focus(b, 11);
        }
        return;
    }
    if (r.kind == PLACEHEAD) {
        float cy = y0 + 18 + 13;
        {
            Rot rot(45, ROW_X + 8, cy);
            G.rt->FillRoundedRectangle(rr(ROW_X, cy - 8, 16, 16, 4), brush(rgb(r.color)));
            G.rt->DrawRoundedRectangle(rr(ROW_X + 1.25f, cy - 6.75f, 13.5f, 13.5f, 3), brush(rgb(LEATHER_DARK)), 2.5f);
        }
        text_c(place_name(r.act), fmt(L"Fredoka", 600, 18), ROW_X + 26, cy, 300, rgb(PAGE_TEXT));
        auto n = count_in(r.act);
        text_c(std::to_wstring(n.first) + L" of " + std::to_wstring(n.second) + L" friends", fmt(L"Nunito", 800, 12), ROW_X, cy, ROW_W,
               rgb(PAGE_SOFT), RIGHT);
        return;
    }
    if (r.kind == TIP) {
        D2D1_RECT_F b = RectF(ROW_X, y0 + 14, ROW_X + ROW_W, y0 + p.h - 10);
        Rot rot(-.6f, (b.left + b.right) / 2, (b.top + b.bottom) / 2);
        sticker(b, 4, rgb(0xfff1a8), 3);
        text(r.desc, fmt(L"Nunito", 700, 12.5f), b.left + 14, b.top + 11, ROW_W - 28, rgb(INK));
        return;
    }
    if (r.kind == CARDS && g_friendsState == 2) {
        const float tilt[3] = {-1.6f, .9f, -.5f};
        for (size_t k = 0; k < r.items.size(); k++) {
            int idx = r.items[k];
            if (idx < 0 || idx >= (int)g_friends->size()) continue;
            Friend& f = (*g_friends)[idx];
            int st = fstate(f);
            bool hi = (focused && U.focusSlot == (int)k) || (U.hoverRow == i && U.hoverPart == (int)k);
            D2D1_RECT_F b = shift(p.parts[k], 0, dy + (hi ? -3.f : 0.f));
            float cx = (b.left + b.right) / 2, pcy = b.top + 12 + 28;
            {
                Rot rot(hi ? 0 : tilt[k % 3], cx, (b.top + b.bottom) / 2);
                if (st == 0) {
                    D2D1_COLOR_F dash = rgb(0xffe2b4, .6f), soft = rgb(0xffeccd, .75f);
                    sticker(b, 6, ColorF(0, 0, 0, 0), 0, 2.5f, dash, true);
                    portrait(f, idx, cx, pcy, 28, true, dash, rgb(0xffeccd, .7f));
                    text_c(L"???", fmt(L"Nunito", 800, 13.5f), b.left, b.top + 82, CARD_W, soft, CENTER);
                    text_c(L"Not met yet", fmt(L"Nunito", 800, 11.5f), b.left, b.top + 104, CARD_W, soft, CENTER);
                } else {
                    sticker(b, 6, ColorF(1, 1, 1), 3);
                    {
                        Rot t2(-3, cx, b.top - 1);
                        G.rt->FillRectangle(RectF(cx - 22, b.top - 8, cx + 22, b.top + 6), brush(rgb(GOLD, .75f)));
                    }
                    portrait(f, idx, cx, pcy, 28, false, rgb(INK), rgb(INK));
                    text_fit(f.name, fmt(L"Nunito", 800, 13.5f), b.left + 6, b.top + 73, CARD_W - 12, rgb(INK), CENTER);
                    if (st == 2) {
                        IDWriteTextFormat* sf = fmt(L"Nunito", 800, 11.5f);
                        float tw = text_w(L"Friend", sf), x0 = cx - (14 + 4 + tw) / 2;
                        draw_star(x0 + 7, b.top + 104, 14, rgb(0xc9346c), rgb(0xc9346c), 0);
                        text_c(L"Friend", sf, x0 + 18, b.top + 104, tw + 4, rgb(0xc9346c));
                        // The gold star sticker.
                        float sx = b.right - 5, sy = b.top + 4;
                        Rot t3(12, sx, sy);
                        circle(sx, sy, 14, rgb(GOLD), 2.5f);
                        draw_star(sx, sy, 16, rgb(INK), rgb(INK), 0);
                    } else {
                        hearts(cx, b.top + 104, std::max(0, hle_friend_points(f.pokemon)));
                    }
                }
            }
            if (focused && U.focusSlot == (int)k) draw_focus(b, 6);
        }
    }
}

// ---- The hint card: a cream page over the list, a yellow sticky note.

struct SheetText {
    std::wstring h4, body, moreH, moreP;
    bool canReveal = false;
};
SheetText sheet_text(const Friend& f, int st) {
    SheetText s;
    int pl = place_of(f.pokemon), z = zone_of(f.pokemon);
    std::wstring where = place_name(pl);
    if (z > 0) where += (where.empty() ? L"" : L", ") + zone_name(z);
    if (where.empty()) where = L"the park";
    if (st == 0) {
        s.h4 = L"Where to look";
        s.body = L"A Pokémon you haven't met lives in " + where + L". Go say hello!";
        if (!f.want.empty()) s.canReveal = true, s.moreP = f.want;
    } else if (st == 2) {
        s.h4 = L"How you became friends";
        s.body = f.want.empty() ? L"You made friends with it in " + where + L"." : f.want;
        if (!f.then.empty()) s.body += L" " + f.then;
    } else {
        s.h4 = L"Hint";
        s.body = f.want.empty() ? L"Talk to it in " + where + L" and see what it wants." : f.want;
        if (!f.then.empty()) s.canReveal = true, s.moreH = L"After that", s.moreP = f.then;
    }
    return s;
}
bool sheet_can_reveal() {
    if (g_friendsState != 2 || U.sheet < 0 || U.sheet >= (int)g_friends->size()) return false;
    const Friend& f = (*g_friends)[U.sheet];
    return sheet_text(f, fstate(f)).canReveal;
}

void draw_sheet(float t) {
    (void)t;
    if (g_friendsState != 2 || U.sheet < 0 || U.sheet >= (int)g_friends->size()) return;
    Friend& f = (*g_friends)[U.sheet];
    int st = fstate(f), pts = std::max(0, hle_friend_points(f.pokemon));
    int z = zone_of(f.pokemon);
    SheetText tx = sheet_text(f, st);
    const D2D1_COLOR_F soft = rgb(0x6b5a45);
    float sx = 24, sy = 156, sw = PANEL_W - 24 - 12, sh = PANEL_H - sy + 4;
    H.sheet = RectF(PANEL_X + sx, PANEL_Y + sy - 12, PANEL_X + sx + sw, PANEL_Y + PANEL_H);
    // The page, its shadow above, a strip of tape.
    G.rt->FillRoundedRectangle(rr(sx, sy - 8, sw, sh, 8), brush(rgb(INK, .12f)));
    auto page = rrect_path(sx, sy, sw, sh, 8, 8, 0, 0);
    G.rt->FillGeometry(page.Get(), grad(Point2F(sx, 0), Point2F(sx + sw, 0), {{0, rgb(0xf3e6c6)}, {.08f, rgb(0xfff8e6)}, {1, rgb(0xfff8e6)}}).Get());
    G.rt->DrawGeometry(page.Get(), brush(rgb(0x3b2414)), 3);
    {
        float tx0 = sx + sw * .4f;
        Rot rot(-3, tx0 + 45, sy - 1);
        tape(RectF(tx0, sy - 12, tx0 + 90, sy + 10), rgb(kZoneTabs[z].gem, .9f), .3f, 6, {{0, 0}, {1, 0}, {1, 1}, {0, 1}});
    }
    // The top: portrait, name, where it lives; the close button.
    float pcx = sx + 20 + 42, pcy = sy + 20 + 42;
    if (st > 0) {
        circle(pcx, pcy, 42 + 8, rgb(INK));
        circle(pcx, pcy, 42 + 5, ColorF(1, 1, 1));
    }
    portrait(f, U.sheet, pcx, pcy, 42, st == 0, rgb(INK), rgb(0xa39678));
    float nx = sx + 20 + 84 + 18, nw = sw - (nx - sx) - 20 - 38 - 10;
    text_fit(st == 0 ? std::wstring(L"Someone new") : f.name, fmt(L"Fredoka", 700, 28), nx, sy + 22, nw, rgb(INK), LEFT);
    {
        float cy = sy + 22 + 36 + 7 + 13, x = nx;
        auto chip = [&](const std::wstring& s, const wchar_t* icon, bool gold) {
            IDWriteTextFormat* cf = fmt(L"Nunito", 800, 12);
            float w = 2 + 10 + (icon ? 14 + 5 : 0) + text_w(s, cf) + 10 + 2;
            if (x + w > sx + sw - 20) return;
            sticker(RectF(x, cy - 13, x + w, cy + 13), 8, gold ? rgb(GOLD) : ColorF(1, 1, 1), 0, 2);
            float ix = x + 12;
            if (icon) {
                if (!wcscmp(icon, L"star")) draw_star(ix + 7, cy, 14, rgb(INK), rgb(INK), 0);
                else draw_icon(icon, ix, cy - 7, 14, rgb(INK));
                ix += 19;
            }
            text_c(s, cf, ix, cy, w, rgb(INK));
            x += w + 6;
        };
        int pl = place_of(f.pokemon);
        std::wstring where = place_name(pl);
        if (z > 0) where += (where.empty() ? L"" : L", ") + zone_name(z);
        if (!where.empty()) chip(where, L"pin", false);
        if (st == 2) chip(L"Friend", L"star", true);
        else if (st == 0) chip(L"Not met yet", nullptr, false);
    }
    {
        float bx = sx + sw - 20 - 38, by = sy + 20;
        bool hov = U.hoverBtn == 5;
        sticker(RectF(bx, by, bx + 38, by + 38), 10, hov ? rgb(0xfff3cf) : ColorF(1, 1, 1), 2, 3);
        draw_icon(L"close", bx + 9, by + 9, 20, rgb(INK));
        H.sheetClose = RectF(PANEL_X + bx, PANEL_Y + by, PANEL_X + bx + 38, PANEL_Y + by + 38);
        if (U.keyNav && !(tx.canReveal && !U.revealed)) draw_focus(RectF(bx, by, bx + 38, by + 38), 10);
    }
    // The body: friendship, the sticky note, its note once a friend.
    float bx = sx + 20, bw = sw - 40, y = sy + 20 + 84 + 20;
    G.rt->PushAxisAlignedClip(RectF(sx + 3, y - 4, sx + sw - 3, PANEL_H - 3), D2D1_ANTIALIAS_MODE_ALIASED);
    if (st > 0) {
        G.rt->FillRoundedRectangle(rr(bx, y, bw, 16, 8), brush(ColorF(1, 1, 1)));
        float fw = bw * std::min(1.f, pts / 100.f);
        if (fw > 0) {
            G.rt->PushAxisAlignedClip(RectF(bx, y, bx + fw, y + 16), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            tape(RectF(bx, y, bx + bw, y + 16), rgb(PINK), .3f, 6, {{0, 0}, {1, 0}, {1, 1}, {0, 1}});
            G.rt->PopAxisAlignedClip();
        }
        G.rt->DrawRoundedRectangle(rr(bx + 1.25f, y + 1.25f, bw - 2.5f, 13.5f, 6.75f), brush(rgb(INK)), 2.5f);
        text(L"Friendship", fmt(L"Nunito", 800, 12), bx, y + 20, bw, soft);
        text(std::to_wstring(std::min(pts, 100)) + L" / 100", fmt(L"Nunito", 800, 12), bx, y + 20, bw, soft, RIGHT);
        y += 16 + 20 + 16 + 12;
    }
    {
        // The sticky note: a bulb, a pink title, the text; "show more" and what it shows.
        float colX = bx + 14 + 36 + 12, colW = bw - (14 + 36 + 12) - 16;
        IDWriteTextFormat* hf = fmt(L"Fredoka", 600, 13);
        IDWriteTextFormat* pf = fmt(L"Nunito", 700, 15);
        auto bodyL = layout(tx.body, pf, colW);
        float h = 14 + 17 + 4 + measure(bodyL.Get()).height;
        bool reveal = tx.canReveal && !U.revealed, more = tx.canReveal && U.revealed;
        ComPtr<IDWriteTextLayout> moreL;
        if (reveal) h += 12 + 36;
        if (more) {
            moreL = layout(tx.moreP, pf, colW);
            h += 10 + 2 + 10 + (tx.moreH.empty() ? 0 : 17 + 4) + measure(moreL.Get()).height;
        }
        h = std::max(h + 14, 14 + 36 + 14.f);
        D2D1_RECT_F nb = RectF(bx, y, bx + bw, y + h);
        {
            Rot rot(-.8f, (nb.left + nb.right) / 2, (nb.top + nb.bottom) / 2);
            sticker(nb, 3, rgb(0xfff1a8), 4);
            circle(bx + 14 + 18, y + 14 + 18, 18, ColorF(1, 1, 1), 2.5f);
            D2D1_COLOR_F bulb = rgb(GOLD);
            draw_icon(L"bulb", bx + 14 + 18 - 10, y + 14 + 18 - 10, 20, rgb(INK), &bulb);
            float ty = y + 14;
            text(upper(tx.h4), hf, colX, ty, colW, rgb(0xc9346c), LEFT, 1.3f);
            ty += 17 + 4;
            G.rt->DrawTextLayout(Point2F(colX, ty), bodyL.Get(), brush(rgb(INK)));
            ty += measure(bodyL.Get()).height;
            if (reveal) {
                std::wstring label = st == 0 ? L"Show a little more" : L"Show exact hint";
                IDWriteTextFormat* bf = fmt(L"Nunito", 800, 13);
                float w = text_w(label, bf) + 28 + 5;
                D2D1_RECT_F rb = RectF(colX, ty + 12, colX + w, ty + 12 + 36);
                sticker(rb, 10, U.hoverBtn == 6 ? rgb(0xfff3cf) : ColorF(1, 1, 1), 2);
                text_c(label, bf, rb.left, ty + 12 + 18, w, rgb(INK), CENTER);
                H.reveal = RectF(PANEL_X + rb.left, PANEL_Y + rb.top, PANEL_X + rb.right, PANEL_Y + rb.bottom);
                if (U.keyNav) draw_focus(rb, 10);
            }
            if (more) {
                ty += 10;
                G.rt->DrawLine(Point2F(colX, ty), Point2F(colX + colW, ty), brush(rgb(INK, .35f)), 2, G.dashed.Get());
                ty += 12;
                if (!tx.moreH.empty()) {
                    text(upper(tx.moreH), hf, colX, ty, colW, rgb(0xc9346c), LEFT, 1.3f);
                    ty += 17 + 4;
                }
                G.rt->DrawTextLayout(Point2F(colX, ty), moreL.Get(), brush(rgb(INK)));
            }
        }
        y += h + 16;
    }
    if (st == 2) {
        IDWriteTextFormat* df = fmt(L"Nunito", 800, 13);
        draw_star(bx + 7, y + 9, 14, rgb(0xc9346c), rgb(0xc9346c), 0);
        text(L"You're already friends. Nice work!", df, bx + 20, y, bw - 20, rgb(0xc9346c));
        y += 26;
        if (!f.about.empty()) text(f.about, fmt(L"Nunito", 700, 14), bx, y, bw, soft);
    }
    G.rt->PopAxisAlignedClip();
}

// Text in the game's font: white with the menu's dark red outline, `h`
// pixels for the font's cell height. Returns the width drawn.
float game_text(const std::wstring& s, float x, float y, float h, float opacity = 1, bool measureOnly = false,
                bool light = true) {
    GameFont& f = g_art->font;
    float k = h / f.cellH, w = 0;
    auto each = [&](auto fn) {
        float pen = 0;
        for (wchar_t ch : s) {
            auto it = f.cmap.find((uint32_t)ch);
            int gi = it != f.cmap.end() ? it->second : f.alter;
            auto wi = f.widths.find(gi);
            GameFont::Width gw = wi != f.widths.end() ? wi->second : GameFont::Width{0, (uint8_t)f.cellW, (int8_t)f.cellW};
            fn(gi, pen + gw.left * k, gw);
            pen += gw.adv * k;
        }
        return pen;
    };
    w = each([](int, float, GameFont::Width) {});
    if (measureOnly) return w;
    G.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    int per = f.cols * f.rows;
    auto draw = [&](float dx, float dy, D2D1_COLOR_F c) {
        each([&](int gi, float gx, GameFont::Width gw) {
            int sheet = gi / per, cell = gi % per;
            if (sheet >= (int)f.sheets.size()) return;
            float sxp = (float)(cell % f.cols) * (f.cellW + 1), syp = (float)(cell / f.cols) * (f.cellH + 1);
            D2D1_RECT_F src = RectF(sxp, syp, sxp + gw.glyph, syp + f.cellH);
            D2D1_RECT_F dst = RectF(x + gx + dx, y + dy, x + gx + dx + gw.glyph * k, y + dy + f.cellH * k);
            G.rt->FillOpacityMask(bitmap(f.sheets[sheet]), brush(c), D2D1_OPACITY_MASK_CONTENT_GRAPHICS, dst, src);
        });
    };
    // Light: white letters, dark red outline (selected rows, the board);
    // dark: the reverse, as the game's unselected rows.
    float o = std::max(1.f, h * .07f);
    D2D1_COLOR_F edge = light ? rgb(0x5a1616, opacity) : rgb(0xfaf2e6, opacity);
    D2D1_COLOR_F fill = light ? ColorF(1, 1, 1, opacity) : rgb(0x5a1e14, opacity);
    for (int i = 0; i < 8; i++) draw(o * std::cos(i * PI / 4), o * std::sin(i * PI / 4), edge);
    draw(0, 0, fill);
    G.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return w;
}

// The board's light wood, a little larger than a row: under our row where
// the game still draws its own selected row (its glow pulses wider than
// the texture we cover it with).
void board_wash(const Map& m, float cx, float cy) {
    for (int i = 0; i < 3; i++) {
        float gw = 352 + i * 10, gh = 62 + i * 8;
        D2D1_RECT_F r = m.rect(cx, cy, gw, gh);
        float rad = (r.bottom - r.top) / 2;
        G.rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush(rgb(0xECDFCC, i == 0 ? .95f : .35f)));
    }
}

// One row in the game's list style, at layout row position (cx, cy).
void list_row(const Map& m, float cx, float cy, bool selected, Img* icon, Img* textImg, const std::wstring& label,
              const std::wstring& value = L"", bool valueSoon = false, bool arrows = false) {
    OptionsArt& a = *g_art;
    G.rt->DrawBitmap(bitmap(selected ? a.bgOrange : a.bgGreen), m.rect(cx, cy, 340, 55));
    float textH = 28 * m.sy;
    if (icon) {
        G.rt->DrawBitmap(bitmap(*icon), m.rect(cx - 128, cy + 1, 44, 44));
        if (icon == &a.iconPc) {
            float tw = game_text(L"PC", 0, 0, 16 * m.sy, 1, true);
            game_text(L"PC", m.x0 + (cx - 128) * m.sx - tw / 2, m.y0 - (cy + 1) * m.sy - 8.5f * m.sy, 16 * m.sy);
        }
    }
    if (textImg) {
        G.rt->DrawBitmap(bitmap(*textImg), m.rect(cx + 26, cy - 1, 253, 28));
    } else {
        float lx = icon ? cx - 98 : cx - 150;
        game_text(label, m.x0 + lx * m.sx, m.y0 - (cy + 1) * m.sy - textH * .62f, textH * 1.05f, 1, false, selected);
    }
    if (!value.empty()) {
        // The value, centred in the right part, with < > when it can change.
        float vh = textH * .82f, vw = game_text(value, 0, 0, vh, 1, true);
        float vx = m.x0 + (cx + 92) * m.sx, vy = m.y0 - (cy + 1) * m.sy - vh * .58f;
        game_text(value, vx - vw / 2, vy, vh, valueSoon ? .55f : 1, false, selected);
        if (arrows) {
            float aw = game_text(L"<", 0, 0, vh, 1, true);
            game_text(L"<", vx - 76 * m.sx - aw / 2, vy, vh, 1, false, selected);
            game_text(L">", vx + 76 * m.sx - aw / 2, vy, vh, 1, false, selected);
        }
    }
}

// Our rows on screen, in window pixels (render_options), for the mouse.
struct OptHit { D2D1_RECT_F rect; int index; float valueX; };  // index -1: the PC Settings row
std::vector<OptHit> g_optHits;

void render_options(int w, int h) {
    g_optHits.clear();
    int px = g_picture_rect[0], py = g_picture_rect[1], pw = g_picture_rect[2], ph = g_picture_rect[3];
    if (pw <= 0 || ph <= 0) px = 0, py = 0, pw = w, ph = h;
    // The game's own layout width: its 2D projection is 833 units across in
    // 16:9 mode (608 in 4:3) and 456 down, for 480 layout units: 877 / 640.
    // In a window of another shape the layout keeps its shape (narrowed in
    // a wider one, cropped in a narrower one: gx_fifo.c "Widescreen").
    float across = hle_widescreen() ? 877.f * ((float)pw / ph) / (16.f / 9.f) : 640.f;
    Map m{px + pw / 2.f, py + ph / 2.f, pw / across, ph / 480.f};
    G.rt->BeginDraw();
    G.rt->Clear(ColorF(0, 0, 0, 0));
    G.rt->SetTransform(Matrix3x2F::Identity());
    OptionsArt& a = *g_art;
    // The game's rows: x 8, 4, 0 at y 50, -15, -80; ours follows on.
    const float rowX[4] = {8, 4, 0, -4}, rowY[4] = {50, -15, -80, -145};
    if (O.st != OPT_SUB) {
        if (O.st == OPT_ROW) {  // the game's last row, drawn again unselected
            board_wash(m, rowX[2], rowY[2]);
            list_row(m, rowX[2], rowY[2], false, &a.iconDelete, &a.textDelete, L"");
        }
        list_row(m, rowX[3], rowY[3], O.st == OPT_ROW, &a.iconPc, nullptr, L"PC Settings");
        g_optHits.push_back({m.rect(rowX[3], rowY[3], 340, 55), -1, 0});
    } else {
        build_pc_rows();
        for (int i = 0; i < 4 && O.top + i < (int)g_pcRows.size(); i++) {
            PcRow& r = g_pcRows[O.top + i];
            int v = r.get();
            if (i == 2) board_wash(m, rowX[i], rowY[i]);  // the game's selected row is under this one
            std::wstring value = v < 0 ? L"Auto" : r.opts[v];
            list_row(m, rowX[i], rowY[i], O.top + i == O.sub, nullptr, nullptr, r.label, value,
                     v >= 0 && (r.soon >> v & 1), O.top + i == O.sub);
            g_optHits.push_back({m.rect(rowX[i], rowY[i], 340, 55), O.top + i, m.x0 + (rowX[i] + 92) * m.sx});
        }
        // More rows above or below, and the line about F1.
        float lh = 20 * m.sy;
        if (O.top > 0) game_text(L"▲", m.x0 + 170 * m.sx, m.y0 - 82 * m.sy, lh);
        if (O.top + 4 < (int)g_pcRows.size()) game_text(L"▼", m.x0 + 170 * m.sx, m.y0 + 168 * m.sy, lh);
        std::wstring line = L"In game, press F1 for every setting";
        float lw = game_text(line, 0, 0, lh, 1, true);
        game_text(line, m.x0 - lw / 2, m.y0 + 178 * m.sy, lh);
    }
    G.rt->EndDraw();
}

}  // namespace

// The game's buttons each time it reads the Wii Remote (hle_wpad.c
// WPADRead): on the Options page, moving to and working the PC rows. While
// one of them has the focus the game gets no buttons (0 returned in *held).
extern "C" void options_rows_filter(uint32_t* held) {
    if (!hle_options_page_shown()) {
        if (O.st != OPT_GAME || O.shown) O.st = OPT_GAME, O.shown = false, changed();
        O.prev = *held;
        return;
    }
    load_options_art();
    if (!O.shown) O.shown = true, changed();
    uint32_t b = *held, down = b & ~O.prev;
    O.prev = b;
    // The game reads the remote more than once a frame: a press we used
    // stays hidden from it until it's let go.
    O.swallow &= b;
    if (O.st == OPT_GAME) {
        // Down from the game's last item: onto our row. Moving about our
        // rows is opt_nav's (the controller, keys and mouse, as they are,
        // not the Wii buttons they stand for).
        if ((down & HOST_PAD_SCREEN_DOWN) && hle_options_cursor() == 2 && g_artState == 2) {
            O.st = OPT_ROW;
            changed();
            O.swallow |= b;
        }
        *held &= ~O.swallow;
        return;
    }
    O.swallow |= b;  // everything pressed while ours has the focus
    *held = 0;
}

// Moving about our rows: up, down, left, right, select, back.
enum OptNav { ON_UP, ON_DOWN, ON_LEFT, ON_RIGHT, ON_OK, ON_BACK };
void opt_show(int i) {
    int n = (int)g_pcRows.size();
    O.sub = std::max(0, std::min(n - 1, i));
    if (O.sub < O.top) O.top = O.sub;
    if (O.sub >= O.top + 4) O.top = O.sub - 3;
}
void opt_nav(OptNav n) {
    if (O.st == OPT_ROW) {
        if (n == ON_UP || n == ON_BACK) O.st = OPT_GAME, O.swallow = ~0u;  // back to the game's own rows
        else if (n == ON_OK || n == ON_RIGHT) build_pc_rows(), O.st = OPT_SUB, O.sub = 0, O.top = 0;
    } else if (O.st == OPT_SUB) {
        if (n == ON_UP) opt_show(O.sub - 1);
        if (n == ON_DOWN) opt_show(O.sub + 1);
        if (n == ON_LEFT) pc_step(g_pcRows[O.sub], -1);
        if (n == ON_RIGHT || n == ON_OK) {
            // Right, or select: the next value (round to the first after the last).
            PcRow& r = g_pcRows[O.sub];
            int before = r.get();
            pc_step(r, 1);
            if (n == ON_OK && r.get() == before) {
                for (int o = 0; o < (int)r.opts.size(); o++)
                    if (!(r.soon >> o & 1)) {
                        if (o != before) r.set(o);
                        break;
                    }
            }
        }
        if (n == ON_BACK) O.st = OPT_ROW;
    }
    if (getenv("WONDERPARK_F1_SCRIPT"))  // tests: where the PC rows' highlight is
        fprintf(stderr, "[opt] nav %d: state %d, row %d, window_mode %d, scale %d\n", (int)n, (int)O.st, O.sub,
                g_x[X_WINDOW_MODE], g_settings.scale);
    changed();
}

// ------------------------------------------------------------------------
// The C interface (overlay_win32.c, gx_frames.c)

extern "C" int settings_ui_is_open(void) { return U.open; }
// The cursor free and shown (input_win32.c): the F1 tab, or the Options
// page with our row on it.
extern "C" int settings_ui_wants_cursor(void) { return U.open || (O.shown && g_artState == 2); }
extern "C" unsigned settings_ui_generation(void) { return U.gen; }

static void run_key_script();

extern "C" int settings_ui_wants_pause(void) {
    static int checked;
    if (!checked) {
        checked = 1;
        const char* e = getenv("WONDERPARK_F1_SHOT");
        const char* at = getenv("WONDERPARK_F1_SHOT_AT");
        if (e && *e) g_shotPrefix = e, g_shotStep = 0, g_shotAt = now_s() + (at && *at ? atof(at) : 1.0);
    }
    if (g_shotStep == 0 && now_s() >= g_shotAt) {
        open_tab(true);
        if (G.ok) set_tab(0);
        else U.tab = 0;
        g_shotStep = 1, g_shotAt = now_s() + .8;
    }
    run_key_script();
    if (g_wp_headless) return U.open;
    if (U.open) return 1;
    return g_x[X_PAUSE_BACKGROUND] && !win_has_focus();
}

// The drawn image to straight alpha in G.image, as the overlay is blended
// (gl_backend.c).
static void copy_out(int w, int h) {
    WICRect r = {0, 0, w, h};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(G.bmp->Lock(&r, WICBitmapLockRead, &lock))) return;
    UINT size = 0, stride = 0;
    BYTE* px = nullptr;
    lock->GetDataPointer(&size, &px);
    lock->GetStride(&stride);
    for (int y = 0; y < h; y++) {
        const uint32_t* src = (const uint32_t*)(px + (size_t)y * stride);
        uint32_t* dst = &G.image[(size_t)y * w];
        for (int x = 0; x < w; x++) {
            uint32_t c = src[x], a = c >> 24;
            if (a == 0 || a == 255) dst[x] = c;
            else {
                uint32_t rr2 = ((c >> 16 & 255) * 255 + a / 2) / a, gg = ((c >> 8 & 255) * 255 + a / 2) / a,
                         bb = ((c & 255) * 255 + a / 2) / a;
                dst[x] = a << 24 | std::min(255u, rr2) << 16 | std::min(255u, gg) << 8 | std::min(255u, bb);
            }
        }
    }
}

// The Options page's PC rows over the game's frame, when it shows them
// (overlay_win32.c menu_draw); NULL otherwise.
extern "C" const uint32_t* options_rows_draw(int w, int h) {
    static unsigned drawnGen = ~0u;
    static int dw, dh;
    if (!O.shown || g_artState != 2 || w <= 0 || h <= 0 || U.open) return nullptr;
    if (!ensure_target(w, h)) return nullptr;
    if (drawnGen != U.gen || dw != w || dh != h) {
        render_options(w, h);
        copy_out(w, h);
        drawnGen = U.gen;
        dw = w, dh = h;
    }
    return G.image.data();
}

extern "C" const uint32_t* settings_ui_draw(int w, int h) {
    static unsigned drawnGen = 0;
    static int dw, dh;
    if (!U.open || w <= 0 || h <= 0) return nullptr;
    if (!ensure_target(w, h)) return nullptr;
    // The friends list: read from the disc the first time; the Friend Book
    // built again when what it shows changes.
    load_friends();
    {
        static unsigned shown = ~0u;
        unsigned s = fb_signature();
        if (s != shown || g_fbDirty) shown = s, fb_rebuild();
    }
    // The pad icons: drawn again once read, and when the pad's type or the
    // setting changes; the player slots when a controller comes or goes.
    {
        static uint32_t iconsGen;
        uint32_t g = pad_icons_generation() ^ (uint32_t)pad_icons_pad_set() << 9 ^ pad_sdl_changes() << 13;
        if (g != iconsGen) iconsGen = g, U.gen++;
    }
    // Rows available only sometimes: greyed (soon) while they can't be used.
    if (!U.view)
        for (auto& r : g_cats[U.tab].rows)
            if (r.off) {
                bool o = r.off();
                if (o != r.soon) r.soon = o, U.gen++, place_rows();
            }
    // Test shots: each category in turn (its top, then scrolled to the end
    // when it doesn't fit), then the Friend Book (everyone, the Cove Area, a
    // hint card and its "show more"), then closed. Each step gets the tab
    // ready; the frame as shown (game and tab) is saved a moment later, by
    // the renderer at its next present.
    if (g_shotStep >= 1 && now_s() >= g_shotAt) {
        static std::string pending;
        if (!pending.empty()) {
            snprintf(g_dump_next, sizeof(g_dump_next), "%s-%s.bmp", g_shotPrefix.c_str(), pending.c_str());
            pending.clear();
            g_shotAt = now_s() + .3;
        } else {
            int s = g_shotStep++ - 1;
            if (s < 2 * NTABS) {
                int cat = s / 2;
                std::string id(g_cats[cat].id.begin(), g_cats[cat].id.end());
                if (s % 2 == 0) {
                    if (U.view) set_view(0);
                    set_tab(cat);
                    pending = std::to_string(cat + 1) + "-" + id;
                } else if (g_listH > view_h()) {
                    U.scroll = 1e6f;  // the same category, scrolled to the end
                    clamp_scroll();
                    changed();
                    pending = std::to_string(cat + 1) + "b-" + id;
                }
            } else if (s == 2 * NTABS) {
                if (g_friendsState == 1) g_shotStep--;  // still reading the disc: wait
                else set_view(1), pending = std::to_string(NTABS + 1) + "-friendbook";
            } else if (s == 2 * NTABS + 1) {
                set_tab(1);
                pending = std::to_string(NTABS + 2) + "-cove";
            } else if (s == 2 * NTABS + 2) {
                // A hint card: someone met but not a friend yet, else a friend, else anyone.
                int pick = -1, pickRank = -1;
                for (auto& r : g_fb.rows)
                    if (r.kind == CARDS)
                        for (int idx : r.items) {
                            const Friend& f = (*g_friends)[idx];
                            int st = fstate(f);
                            int rank = st == 1 ? (f.then.empty() ? 2 : 3) : st == 2 ? 1 : 0;
                            if (rank > pickRank) pick = idx, pickRank = rank;
                        }
                if (pick >= 0) open_sheet(pick), pending = std::to_string(NTABS + 3) + "-hint";
            } else if (s == 2 * NTABS + 3) {
                if (U.sheet >= 0 && sheet_can_reveal()) {
                    U.revealed = true;
                    changed();
                    pending = std::to_string(NTABS + 3) + "b-hint";
                }
            } else {
                open_tab(false);
                g_shotStep = -1;
            }
            g_shotAt = now_s() + (pending.empty() ? .1 : .8);
        }
    }
    if (g_placed.size() != cur().rows.size()) place_rows();
    double t = now_s();
    bool animating = !U.reduced && (t - U.openT < .5 || U.listenRow >= 0);
    if (drawnGen != U.gen || dw != w || dh != h || animating) {
        render((float)t);
        copy_out(w, h);
        if (drawnGen != U.gen || animating) U.gen += animating ? 1 : 0;
        drawnGen = U.gen;
        dw = w, dh = h;
    }
    return G.image.data();
}

extern "C" void settings_ui_controller(uint32_t b);
extern "C" int settings_ui_message(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp);

// WONDERPARK_F1_SCRIPT (tests): keys pressed at times, "<seconds>:<key>,...",
// seconds from the first frame shown; keys f1 esc up down left right enter q e
// backspace, or one letter or digit; "shot=<file.bmp>" saves the frame shown.
// Handled as the window's own key presses (works headless too).
static void run_key_script() {
    static std::vector<std::pair<double, std::string>> items;
    static size_t pos;
    static double t0 = -1;
    if (t0 < 0) {
        t0 = now_s();
        const char* e = getenv("WONDERPARK_F1_SCRIPT");
        std::string s = e ? e : "";
        size_t p = 0;
        while (p < s.size()) {
            size_t c = s.find(',', p);
            if (c == std::string::npos) c = s.size();
            std::string item = s.substr(p, c - p);
            size_t colon = item.find(':');
            // "f<n>:": at the game's n-th finished frame (stored negative).
            if (colon != std::string::npos)
                items.push_back({item[0] == 'f' ? -atof(item.c_str() + 1) : atof(item.c_str()), item.substr(colon + 1)});
            p = c + 1;
        }
    }
    auto due =[&](double at) { return at < 0 ? (double)gxb_game_frames() >= -at : now_s() - t0 >= at; };
    while (pos < items.size() && due(items[pos].first)) {
        const std::string& k = items[pos++].second;
        if (k.rfind("shot=", 0) == 0) {
            snprintf(g_dump_next, sizeof(g_dump_next), "%s", k.c_str() + 5);
            continue;
        }
        static const std::map<std::string, int> kKeys = {
            {"f1", VK_F1}, {"esc", VK_ESCAPE}, {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT},
            {"right", VK_RIGHT}, {"enter", VK_RETURN}, {"q", 'Q'}, {"e", 'E'}, {"backspace", VK_BACK}};
        auto it = kKeys.find(k);
        int vk = it != kKeys.end() ? it->second : k.size() == 1 ? toupper((unsigned char)k[0]) : 0;
        if (!vk) continue;
        bool was = g_wp_headless != 0;
        g_wp_headless = 0;  // the handler ignores a headless window's keys
        settings_ui_message(nullptr, WM_KEYDOWN, (uintptr_t)vk, 1);
        g_wp_headless = was;
    }
}

extern "C" void settings_ui_paused_tick(void) {
    run_key_script();
    // While paused the game doesn't poll input: the controller is read here.
    typedef DWORD(WINAPI * XInputGetState_t)(DWORD, void*);
    static XInputGetState_t get;
    static bool tried;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) get = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
    }
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st;
    uint32_t b = 0;
    if (get && get(0, &st) == 0) {
        b = st.buttons;
        if (st.lt > 128) b |= XPAD_LT;
        if (st.rt > 128) b |= XPAD_RT;
        if (st.ly > 16000) b |= XPAD_UP;
        if (st.ly < -16000) b |= XPAD_DOWN;
        if (st.lx > 16000) b |= XPAD_RIGHT;
        if (st.lx < -16000) b |= XPAD_LEFT;
    }
    settings_ui_controller(b);
}

extern "C" void settings_ui_controller(uint32_t b) {
    run_key_script();  // also each input poll: a busy game may not go idle
    uint32_t down = b & ~U.prevPad;
    U.prevPad = b;
    // Back + Start opens and closes it.
    if ((b & (XPAD_BACK | XPAD_START)) == (XPAD_BACK | XPAD_START) && (down & (XPAD_BACK | XPAD_START))) {
        open_tab(!U.open);
        return;
    }
    if (!U.open && O.shown && O.st != OPT_GAME) {
        // The Options page's PC rows: the stick or d-pad moves, A selects,
        // B goes back (with repeat while a direction is held).
        static uint32_t held;
        static double repeatAt;
        double t = now_s();
        uint32_t dirs = b & (XPAD_UP | XPAD_DOWN | XPAD_LEFT | XPAD_RIGHT), nd = down & dirs;
        if (nd || (dirs && dirs == held && t >= repeatAt)) {
            uint32_t d = nd ? nd : dirs;
            opt_nav(d & XPAD_UP ? ON_UP : d & XPAD_DOWN ? ON_DOWN : d & XPAD_LEFT ? ON_LEFT : ON_RIGHT);
            repeatAt = t + (nd ? .4 : .12);
        }
        held = dirs;
        if (down & XPAD_A) opt_nav(ON_OK);
        if (down & XPAD_B) opt_nav(ON_BACK);
        return;
    }
    if (!U.open || !G.ok) return;
    if (U.listenRow >= 0) {
        if (U.listenSlot >= 2 && down) {
            uint32_t bit = down & (0 - down);  // the lowest new button
            bind_now(0, bit);
        }
        return;
    }
    double t = now_s();
    uint32_t dirs = b & (XPAD_UP | XPAD_DOWN | XPAD_LEFT | XPAD_RIGHT);
    uint32_t newDir = down & dirs;
    bool repeat = dirs && dirs == U.padHeldDir && t >= U.padRepeatAt;
    if (newDir || repeat) {
        uint32_t d = newDir ? newDir : dirs;
        if (d & XPAD_UP) nav(N_UP);
        else if (d & XPAD_DOWN) nav(N_DOWN);
        else if (d & XPAD_LEFT) nav(N_LEFT);
        else if (d & XPAD_RIGHT) nav(N_RIGHT);
        U.padRepeatAt = t + (newDir ? .4 : .08);
    }
    U.padHeldDir = dirs;
    if (down & XPAD_A) nav(N_OK);
    if (down & XPAD_B) nav(N_BACK);
    if (down & XPAD_LB) nav(N_PREV);
    if (down & XPAD_RB) nav(N_NEXT);
    if (down & XPAD_Y) nav(N_BOOK);
}

extern "C" int settings_ui_message(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp) {
    (void)hwnd;
    if (g_wp_headless) return 0;
    bool keyDown = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (keyDown && wp == VK_F1) {
        if (!(lp & (1 << 30))) open_tab(!U.open);
        return 1;
    }
    if (!U.open && O.shown && g_artState == 2) {
        // The Options page's PC rows: the mouse highlights and clicks, the
        // keys move (arrows or WASD), Enter or Space select, Esc or
        // Backspace go back.
        if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MOUSEWHEEL) {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            const OptHit* over = nullptr;
            if (msg == WM_MOUSEWHEEL) {
                POINT pt = {x, y};
                ScreenToClient((HWND)win_handle(), &pt);
                x = pt.x, y = pt.y;
            }
            for (auto& h : g_optHits)
                if (x >= h.rect.left && x <= h.rect.right && y >= h.rect.top && y <= h.rect.bottom) over = &h;
            if (msg == WM_RBUTTONDOWN) {
                if (O.st != OPT_GAME) opt_nav(ON_BACK);
                return O.st != OPT_GAME;
            }
            if (msg == WM_MOUSEWHEEL) {
                if (O.st == OPT_SUB) opt_nav(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? ON_UP : ON_DOWN);
                return O.st == OPT_SUB;
            }
            if (!over) return 0;
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            if (over->index < 0) {  // the PC Settings row
                if (O.st == OPT_GAME) O.st = OPT_ROW, O.swallow = ~0u, changed();
                if (msg == WM_LBUTTONDOWN) opt_nav(ON_OK);
            } else if (O.st == OPT_SUB) {
                if (over->index != O.sub) opt_show(over->index), changed();
                // A click left of the value: the one before; else the next.
                if (msg == WM_LBUTTONDOWN) opt_nav((float)x < over->valueX - 40 && x > over->valueX - 200 ? ON_LEFT : ON_OK);
            }
            return 1;
        }
        if (O.st != OPT_GAME && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
            switch (wp) {
            case VK_UP: case 'W': opt_nav(ON_UP); break;
            case VK_DOWN: case 'S': opt_nav(ON_DOWN); break;
            case VK_LEFT: case 'A': opt_nav(ON_LEFT); break;
            case VK_RIGHT: case 'D': opt_nav(ON_RIGHT); break;
            case VK_RETURN: case VK_SPACE: opt_nav(ON_OK); break;
            case VK_ESCAPE: case VK_BACK: opt_nav(ON_BACK); break;
            default: break;
            }
            return 1;
        }
    }
    if (!U.open) return 0;
    if (!G.ok) return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR;
    // Rebinding: the next key or mouse button.
    if (U.listenRow >= 0) {
        if (keyDown) {
            int vk = (int)wp;
            if (vk == VK_ESCAPE) U.listenRow = U.listenSlot = -1, changed();
            else if (vk == VK_BACK) clear_slot();
            else if (U.listenSlot < 2) {
                if (vk == VK_SHIFT) vk = (MapVirtualKeyA((lp >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT) ? VK_RSHIFT : VK_LSHIFT;
                if (vk == VK_CONTROL) vk = (lp & (1 << 24)) ? VK_RCONTROL : VK_LCONTROL;
                if (vk == VK_MENU) vk = (lp & (1 << 24)) ? VK_RMENU : VK_LMENU;
                bind_now(vk, 0);
            }
            return 1;
        }
        if (U.listenSlot < 2 && (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN)) {
            int vk = msg == WM_LBUTTONDOWN ? VK_LBUTTON : msg == WM_RBUTTONDOWN ? VK_RBUTTON : msg == WM_MBUTTONDOWN ? VK_MBUTTON
                     : (HIWORD(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2);
            bind_now(vk, 0);
            return 1;
        }
    }
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        switch (wp) {
        case VK_ESCAPE:
            if (U.sheet >= 0) close_sheet();
            else open_tab(false);
            break;
        case VK_F2: nav(N_BOOK); break;
        case VK_UP: nav(N_UP); break;
        case VK_DOWN: nav(N_DOWN); break;
        case VK_LEFT: nav(N_LEFT); break;
        case VK_RIGHT: nav(N_RIGHT); break;
        case VK_RETURN:
        case VK_SPACE: nav(N_OK); break;
        case 'Q':
        case VK_PRIOR: nav(N_PREV); break;
        case 'E':
        case VK_NEXT: nav(N_NEXT); break;
        default: break;
        }
        return 1;
    case WM_KEYUP:
    case WM_CHAR:
    case WM_SYSKEYUP:
        return 1;
    case WM_MOUSEMOVE: {
        Hit h = hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (U.dragRow >= 0) slider_drag(U.dragRow, GET_X_LPARAM(lp));
        if (U.sheet >= 0 && h.what == 4) h = Hit();  // under the hint card
        int hr = h.what == 4 ? h.row : -1, hp = h.what == 4 ? h.part : h.what == 7 ? 100 + h.part : -1;
        int hb = h.what == 4 || h.what == 7 || h.what == 8 ? 0 : h.what;
        if (hr != U.hoverRow || hp != U.hoverPart || hb != U.hoverBtn) {
            U.hoverRow = hr, U.hoverPart = hp, U.hoverBtn = hb;
            changed();
        }
        // The highlight follows the mouse onto whatever it's over (the
        // same highlight the stick and keys move; A or a click selects).
        if (U.sheet < 0) {
            if (h.what == 1) U.focus = FOCUS_TOP, U.focusSlot = CLOSE_SLOT;
            else if (h.what == 3) U.focus = FOCUS_TOP, U.focusSlot = FB_SLOT;
            else if (h.what == 2) U.focus = FOCUS_RESET, U.focusSlot = 0;
            else if (h.what == 7) U.focus = FOCUS_TOP, U.focusSlot = h.part;
            else if (h.what == 4) U.focus = h.row, U.focusSlot = h.part;
        }
        SetCursor(LoadCursor(nullptr, h.what && h.what != 8 ? IDC_HAND : IDC_ARROW));
        return 1;
    }
    case WM_LBUTTONDOWN: {
        U.keyNav = false;
        Hit h = hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (U.sheet >= 0 && h.what == 4) h = Hit();
        if (h.what == 1) open_tab(false);
        else if (h.what == 2) U.view ? set_view(0) : reset_tab();
        else if (h.what == 3) set_view(!U.view);
        else if (h.what == 5) close_sheet();
        else if (h.what == 6) U.revealed = true;
        else if (h.what == 7) {
            if (U.sheet >= 0) close_sheet();
            set_tab(h.part);
        } else if (h.what == 4) {
            const Row* r = row_at(h.row);
            if (r->kind == CHOICE) choose(*r, h.part);
            else if (r->kind == TOGGLE) step(*r, 1);
            else if (r->kind == SLIDER && !r->soon) {
                U.dragRow = h.row;
                SetCapture((HWND)win_handle());
                slider_drag(h.row, GET_X_LPARAM(lp));
            } else if (r->kind == BIND) start_listen(h.row, h.part);
            else if (r->kind == PLAYERS || r->kind == CARDS || r->kind == PLACES) {
                U.focus = h.row, U.focusSlot = h.part;
                activate();
            }
        }
        changed();
        return 1;
    }
    case WM_LBUTTONUP:
        if (U.dragRow >= 0) ReleaseCapture();
        U.dragRow = -1;
        return 1;
    case WM_MOUSEWHEEL:
        U.scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 60;
        clamp_scroll();
        changed();
        return 1;
    case WM_RBUTTONDOWN:  // right click: back (closes the hint card, else the tab)
        if (U.sheet >= 0) close_sheet();
        else open_tab(false);
        return 1;
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_SETCURSOR:
        if (msg == WM_SETCURSOR && LOWORD(lp) != HTCLIENT) return 0;
        return 1;
    }
    return 0;
}
