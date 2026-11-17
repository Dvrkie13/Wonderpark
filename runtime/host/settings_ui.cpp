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
// Settings whose feature isn't built yet are shown greyed, "Coming soon";
// so are choices that aren't (a pill dimmed).
//
// Controls: mouse (click, drag sliders, wheel scrolls); keyboard Up/Down
// move, Left/Right change, Enter/Space press, Q/E (or PageUp/PageDown)
// switch category; controller d-pad / left stick, A, B (close), LB/RB.
// Rebinding: press a key cap, then the key, mouse button or controller
// button; Esc cancels, Backspace clears.
//
// WONDERPARK_F1_SHOT=<prefix> (tests): opens the tab WONDERPARK_F1_SHOT_AT
// seconds after the game first shows a frame (default 1) and saves
// <prefix>-<n>-<category>.png for each category (the tab's image alone), then
// closes it.

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
char g_dump_next[512];  // a frame for gl_backend.c to save as shown (test shots)
extern int g_picture_rect[4];  // gl_backend.c: where the game's picture is in the window
int win_has_focus(void);
int hle_widescreen(void);  // nand.c: the game runs in its 16:9 mode
extern int g_wp_headless;
void input_release_mouse(void);          /* input_win32.c */
unsigned gxb_game_frames(void);          /* gx_frames.c */
int hle_game_text_speed(void);           /* hle_options.c: 0 Normal, 1 Fast, -1 no save loaded */
void hle_set_game_text_speed(int fast);
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

enum Kind { HEAD, CHOICE, TOGGLE, SLIDER, BINDHEAD, BIND, FIXEDBIND };

struct Row {
    Kind kind = HEAD;
    std::wstring label, desc;
    std::vector<std::wstring> opts;
    unsigned soonOpts = 0;  // choices not available yet (bit per option)
    int min = 0, max = 100, step = 5;
    std::wstring unit;
    bool sign = false;
    bool soon = false;      // the whole row: "Coming soon"
    int act = -1;           // BIND: the Wii Remote button (ACT_*)
    std::function<int()> get;
    std::function<void(int)> set;
    std::function<void()> reset;
    // Available only sometimes (the game's own settings need a loaded save):
    // while off() is true the row is greyed with tag instead of "Coming soon".
    std::function<bool()> off;
    std::wstring tag;
};

struct Category {
    std::wstring id, name, desc;
    uint32_t gem, acc, ink;
    std::vector<Row> rows;
};

std::vector<Category> g_cats;

Row head(const wchar_t* t) {
    Row r;
    r.kind = HEAD;
    r.label = t;
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
        Row fr = vchoice(L"Frame rate", L"60 draws every step of the game; movies stay at 30.", {L"30 (original)", L"60", L"120"},
                         {30, 60, 120}, &g_settings.frame_rate, 30);
        fr.soonOpts = 4;  // 120: frame interpolation, not built yet
        c.rows.push_back(fr);
        c.rows.push_back(xrow(TOGGLE, X_VSYNC, L"V-Sync", L"Stops the picture from tearing in fullscreen. A window never tears."));
        Row rd;
        rd.kind = CHOICE;
        rd.label = L"Renderer";
        rd.desc = L"OpenGL draws for now; Vulkan isn't built yet.";
        rd.opts = {L"Vulkan", L"OpenGL"};
        rd.soonOpts = 1;
        rd.get = [] { return 1; };
        rd.set = [](int) {};
        rd.reset = [] {};
        c.rows.push_back(rd);
        Row fov;
        fov.kind = SLIDER;
        fov.label = L"Field of view";
        fov.desc = L"In widescreen. Wider views can show things popping in at the sides.";
        fov.min = 80, fov.max = 130, fov.step = 5, fov.unit = L"%";
        fov.get = [] { return g_settings.fov; };
        fov.set = [](int v) {
            g_settings.fov = v;
            settings_save();
        };
        fov.reset = [] {
            g_settings.fov = 100;
            settings_save();
        };
        fov.off = [] { return !g_x[X_WIDESCREEN]; };
        fov.tag = L"Widescreen only";
        c.rows.push_back(fov);
        c.rows.push_back(xslider(X_BRIGHTNESS, L"Brightness", nullptr, 50, 150, 5, L"%", false));
        g_cats.push_back(c);
    }
    // Graphics
    {
        Category c{L"graphics", L"Graphics", L"How sharp and shiny the park looks", 0x8b5bd2, 0xb596f0, 0xffffff, {}};
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
        c.rows.push_back(xrow(TOGGLE, X_PER_PIXEL_LIGHTING, L"Per-pixel lighting", L"Smoother lighting on Pokémon and the park.", {}, true));
        c.rows.push_back(xrow(TOGGLE, X_HDR, L"HDR", nullptr, {}, true));
        c.rows.push_back(xrow(CHOICE, X_COLORS, L"Colors", nullptr, {L"Original", L"Vivid"}));
        c.rows.push_back(head(L"Performance"));
        c.rows.push_back(xrow(CHOICE, X_SHADER_MODE, L"Shader compilation",
                              L"Smooth never stutters. Precompile prepares every saved shader before the game starts.",
                              {L"Smooth", L"Fast", L"Precompile"}, true));
        c.rows.push_back(xrow(TOGGLE, X_FAST_LOADING, L"Fast loading",
                              L"Reads the game's files into RAM at start (about 280 MB), so areas load without waiting for the drive. From the next start."));
        g_cats.push_back(c);
    }
    // Controls
    {
        Category c{L"controls", L"Controls", L"Keyboard, mouse and controller", 0xf6d44a, 0xfbe27a, 0x10182f, {}};
        c.rows.push_back(head(L"Camera and aiming"));
        Row cam;
        cam.kind = SLIDER;
        cam.label = L"Camera speed";
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
        c.rows.push_back(xrow(TOGGLE, X_INVERT_X, L"Invert left / right", nullptr, {}, true));
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
        c.rows.push_back(xrow(CHOICE, X_RIGHT_STICK, L"Right stick", L"Auto switches to the pointer when the game asks for one.",
                              {L"Camera", L"Pointer", L"Auto"}, true));
        c.rows.push_back(xrow(TOGGLE, X_AIM_ASSIST, L"Aim assist", L"Controller aiming leans toward targets (Cake Contraption).", {}, true));
        c.rows.push_back(vchoice(L"Rumble", nullptr, {L"Off", L"Light", L"Normal", L"Strong"}, {0, 1, 2, 3},
                                 &g_settings.rumble, 2, [] { input_rumble_preview(); }));
        c.rows.push_back(head(L"Buttons"));
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
        c.rows.push_back(xslider(X_STEER_SENSITIVITY, L"Steering sensitivity", nullptr, 25, 200, 5, L"%", true));
        c.rows.push_back(xslider(X_AIM_SPEED, L"Aim speed", nullptr, 25, 200, 5, L"%", true));
        c.rows.push_back(xslider(X_RHYTHM_OFFSET, L"Dance timing offset", L"Shift if your presses feel early or late.", -100, 100,
                                 5, L" ms", true, true));
        g_cats.push_back(c);
    }
    // Audio
    {
        Category c{L"audio", L"Audio", L"Music, sound effects and voices", 0x5bc26a, 0x7ad68a, 0x10182f, {}};
        c.rows.push_back(xslider(X_VOLUME_MASTER, L"Master volume", nullptr, 0, 100, 5, L"%", false));
        c.rows.push_back(xslider(X_VOLUME_MUSIC, L"Music", L"Music and jingles.", 0, 100, 5, L"%", false));
        c.rows.push_back(xslider(X_VOLUME_SFX, L"Sound effects", nullptr, 0, 100, 5, L"%", false));
        c.rows.push_back(xslider(X_VOLUME_VOICES, L"Pokémon voices", nullptr, 0, 100, 5, L"%", false));
        c.rows.push_back(xrow(CHOICE, X_VOICE_LANGUAGE, L"Voice language", L"Japanese needs your own Japanese disc.",
                              {L"English", L"Japanese"}, true));
        c.rows.push_back(xrow(TOGGLE, X_MUTE_BACKGROUND, L"Mute in the background", L"Goes quiet when you switch to another window."));
        c.rows.push_back(xrow(CHOICE, X_AUDIO_LATENCY, L"Audio delay", L"Raise it if the sound crackles.", {L"Low", L"Normal", L"Safe"}));
        g_cats.push_back(c);
    }
    // Gameplay
    {
        Category c{L"gameplay", L"Gameplay", L"Tune the pace and the challenge", 0xf39a2b, 0xf7b45c, 0x10182f, {}};
        c.rows.push_back(head(L"Balance"));
        auto soon = [](const wchar_t* label, const wchar_t* desc, int mn, int mx, int step, int* field) {
            Row r;
            r.kind = SLIDER;
            r.label = label;
            r.desc = desc ? desc : L"";
            r.min = mn, r.max = mx, r.step = step, r.unit = L"%", r.soon = true;
            r.get = [field] { return field ? *field : 100; };
            r.set = [](int) {};
            r.reset = [] {};
            return r;
        };
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
        c.rows.push_back(soon(L"Berries collected", nullptr, 50, 500, 25, &g_settings.berries));
        c.rows.push_back(soon(L"Shop prices", nullptr, 0, 200, 10, &g_settings.prices));
        c.rows.push_back(soon(L"Damage you deal", nullptr, 25, 400, 25, &g_settings.damage_dealt));
        c.rows.push_back(soon(L"Damage you take", nullptr, 0, 200, 10, &g_settings.damage_taken));
        c.rows.push_back(soon(L"Chase speed", L"How fast Pokémon run in chase games.", 50, 150, 5, &g_settings.chase_speed));
        c.rows.push_back(head(L"Comfort"));
        c.rows.push_back(xrow(TOGGLE, X_PAUSE_BACKGROUND, L"Pause in the background", L"Pauses when the window loses focus."));
        c.rows.push_back(xrow(TOGGLE, X_SKIP_LOGOS, L"Skip startup logos", nullptr, {}, true));
        g_cats.push_back(c);
    }
    // Silly
    {
        Category c{L"silly", L"Silly", L"Just for fun", 0xe2457f, 0xf27aa5, 0xffffff, {}};
        c.rows.push_back(xrow(CHOICE, X_POKEMON_SIZE, L"Pokémon size", nullptr, {L"Mini", L"Normal", L"Giant"}, true));
        c.rows.push_back(xslider(X_VOICE_PITCH, L"Voice pitch", L"From deep and grumbly to tiny and squeaky.", -50, 50, 5, L"%", true, true));
        c.rows.push_back(xrow(TOGGLE, X_MIRROR_WORLD, L"Mirror world", L"Flips the whole park left to right. Left and right swap too."));
        c.rows.push_back(xrow(TOGGLE, X_VICTORY_CONFETTI, L"Victory confetti", L"A burst of confetti when you win an attraction.", {}, true));
        c.rows.push_back(xrow(CHOICE, X_STEREO_3D, L"3D glasses", L"For red-cyan glasses or a 3D TV.", {L"Off", L"Red-cyan", L"Side by side"}, true));
        g_cats.push_back(c);
    }
}

// ------------------------------------------------------------------------
// State

struct UI {
    bool open = false;
    double openT = 0;
    int tab = 0;
    float scroll = 0;            // the panel's rows, scrolled by
    int focus = -1;              // focused item (keyboard/controller), -1 none
    int focusSlot = 0;           // BIND: which of the four slots
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
    bool closeBtnHover = false, resetHover = false;
} U;

void changed() { U.gen++; }

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

ComPtr<IDWriteTextLayout> layout(const std::wstring& s, IDWriteTextFormat* f, float w, float h = 1000,
                                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, float spacing = 0) {
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
          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, float spacing = 0) {
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
        s->BeginFigure(Point2F(x, y), D2D1_FIGURE_BEGIN_HOLLOW);
        open = true;
    };
    while (*q) {
        while (*q == ' ' || *q == ',') q++;
        if (!*q) break;
        if (isalpha((unsigned char)*q)) cmd = *q++;
        bool rel = islower((unsigned char)cmd) != 0;
        float a, b, c, d2, e, f, g;
        switch (toupper(cmd)) {
        case 'M':
            if (!num(a) || !num(b)) return p;
            if (rel) a += cx, b += cy;
            begin(a, b);
            cx = sx = a, cy = sy = b;
            cmd = rel ? 'l' : 'L';
            break;
        case 'L':
            if (!num(a) || !num(b)) return p;
            if (rel) a += cx, b += cy;
            s->AddLine(Point2F(a, b));
            cx = a, cy = b;
            break;
        case 'H':
            if (!num(a)) return p;
            cx = rel ? cx + a : a;
            s->AddLine(Point2F(cx, cy));
            break;
        case 'V':
            if (!num(a)) return p;
            cy = rel ? cy + a : a;
            s->AddLine(Point2F(cx, cy));
            break;
        case 'C':
            if (!num(a) || !num(b) || !num(c) || !num(d2) || !num(e) || !num(f)) return p;
            if (rel) a += cx, b += cy, c += cx, d2 += cy, e += cx, f += cy;
            s->AddBezier(D2D1::BezierSegment(Point2F(a, b), Point2F(c, d2), Point2F(e, f)));
            cx = e, cy = f;
            break;
        case 'A':
            if (!num(a) || !num(b) || !num(c) || !flag(d2) || !flag(e) || !num(f) || !num(g)) return p;
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
    if (open) s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    return p;
}

// The category icons (the mockup's, 24x24, stroke 2, round).
void draw_icon(const std::wstring& id, float x, float y, float size, D2D1_COLOR_F c) {
    static const std::map<std::wstring, std::vector<const char*>> paths = {
        {L"display", {"M5 4h14a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2Z", "M8 20h8M12 16v4"}},
        {L"graphics", {"M12 3l2.2 5.8L20 11l-5.8 2.2L12 19l-2.2-5.8L4 11l5.8-2.2Z"}},
        {L"controls", {"M6 8h12a4 4 0 0 1 3.8 5.3l-1 3a2 2 0 0 1-3.5.6L15.5 14.5h-7l-1.8 2.4a2 2 0 0 1-3.5-.6l-1-3A4 4 0 0 1 6 8Z",
                       "M8 10v3M6.5 11.5h3M15.5 11h.01M17.5 12.5h.01"}},
        {L"audio", {"M4 10v4h4l5 4V6L8 10Z", "M16.5 9a4 4 0 0 1 0 6M19 6.5a7.5 7.5 0 0 1 0 11"}},
        {L"gameplay", {"M4 7h10M18 7h2M4 17h4M12 17h8", "M18 7a2 2 0 1 1-4 0a2 2 0 1 1 4 0Z", "M12 17a2 2 0 1 1-4 0a2 2 0 1 1 4 0Z"}},
        {L"silly", {"M21 12a9 9 0 1 1-18 0a9 9 0 1 1 18 0Z", "M8.5 14.5a4.5 4.5 0 0 0 7 0M9 9.5h.01M15 9.5h.01"}},
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
        G.rt->DrawGeometry(g.Get(), brush(c), 2, G.round.Get());
    }
    G.rt->SetTransform(old);
}

ComPtr<ID2D1LinearGradientBrush> vgrad(float y0, float y1, D2D1_COLOR_F a, D2D1_COLOR_F b) {
    D2D1_GRADIENT_STOP st[2] = {{0, a}, {1, b}};
    ComPtr<ID2D1GradientStopCollection> sc;
    G.rt->CreateGradientStopCollection(st, 2, &sc);
    ComPtr<ID2D1LinearGradientBrush> l;
    G.rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(Point2F(0, y0), Point2F(0, y1)), sc.Get(), &l);
    return l;
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

// A stone button (Close, Reset): returns its width.
float stone_button(float x, float y, float h, const std::wstring& label, bool key, bool hover) {
    IDWriteTextFormat* f = fmt(L"Fredoka", 700, 16);
    float tw = measure(layout(label, f, 400).Get()).widthIncludingTrailingWhitespace;
    float w = (key ? 6 + 36 + 8 : 14) + tw + 14 + 4;
    G.rt->FillRoundedRectangle(rr(x, y + 4, w, h, 12), brush(rgb(0x4a5677)));
    G.rt->FillRoundedRectangle(rr(x, y, w, h, 12), vgrad(y, y + h, rgb(0xc3cde0), rgb(0x9aa7c0)).Get());
    G.rt->FillRectangle(RectF(x + 10, y + 2, x + w - 10, y + 4), brush(ColorF(1, 1, 1, .45f)));
    if (hover) G.rt->FillRoundedRectangle(rr(x, y, w, h, 12), brush(ColorF(1, 1, 1, .1f)));
    G.rt->DrawRoundedRectangle(rr(x + 1, y + 1, w - 2, h - 2, 11), brush(rgb(0x5d6a8a)), 2);
    float tx = x + 14;
    if (key) {
        G.rt->FillRoundedRectangle(rr(x + 6, y + (h - 28) / 2, 36, 28, 8), brush(rgb(0x1b2440)));
        text(L"F1", fmt(L"Nunito", 700, 14), x + 6, y + (h - 28) / 2 + 4, 36, rgb(0xffe7a3), DWRITE_TEXT_ALIGNMENT_CENTER);
        tx = x + 6 + 36 + 8;
    }
    text(label, f, tx, y + (h - 21) / 2, 400, rgb(0x1b2440));
    return w;
}

// ---- Layout of the rows (panel coordinates: x from 24, width 444)

const float PANEL_X = 14, PANEL_Y = 168, PANEL_W = 492, PANEL_H = 538;
const float ROWS_TOP = 82, ROWS_BOTTOM = 66, ROW_X = 24, ROW_W = 444;

struct Placed {
    float y = 0, h = 0;       // in the scrolled list
    float ctrlX = 0, ctrlY = 0, ctrlW = 0, ctrlH = 0;  // the control's box (list coordinates)
    std::vector<D2D1_RECT_F> parts;  // pills / slots, list coordinates
    float labelW = ROW_W;
    bool wrap = false;
};
std::vector<Placed> g_placed;
float g_listH = 0;

float pill_w(const std::wstring& s) {
    return measure(layout(s, fmt(L"Nunito", 800, 13.5f), 400).Get()).widthIncludingTrailingWhitespace + 28 + 4;
}
float label_h(const Row& r, float w) {
    float h = measure(layout(r.label, fmt(L"Nunito", 800, 16), w).Get()).height;
    if (r.soon) h = std::max(h, 22.f);
    if (!r.desc.empty()) h += 2 + measure(layout(r.desc, fmt(L"Nunito", 600, 13), w).Get()).height;
    return h;
}

void place_rows() {
    const Category& c = g_cats[U.tab];
    g_placed.assign(c.rows.size(), Placed());
    float y = 4;
    for (size_t i = 0; i < c.rows.size(); i++) {
        const Row& r = c.rows[i];
        Placed& p = g_placed[i];
        p.y = y;
        if (r.kind == HEAD) {
            p.h = 18 + 18 + 6;
        } else if (r.kind == BINDHEAD) {
            p.h = 6 + 16 + 4;
        } else if (r.kind == BIND || r.kind == FIXEDBIND) {
            float fr = (ROW_W - 24) / 5.3f;
            p.h = 5 + 34 + 5;
            for (int s = 0; s < 4; s++) {
                float x = ROW_X + fr * 1.3f + 6 + s * (fr + 6);
                p.parts.push_back(RectF(x, y + 5, x + fr, y + 5 + 34));
            }
        } else {
            // Control size
            float cw = 0, ch = 0;
            if (r.kind == TOGGLE) cw = 64, ch = 36;
            else if (r.kind == SLIDER) cw = 200 + 16 + 58, ch = 24;
            else {
                for (auto& o : r.opts) cw += pill_w(o) + 6;
                cw -= 6;
                ch = 38;
            }
            bool wrap = 200 + 16 + cw > ROW_W;
            p.wrap = wrap;
            p.labelW = wrap ? ROW_W : ROW_W - 16 - cw;
            float lh = label_h(r, p.labelW);
            float top = y + 14;
            if (r.kind == CHOICE && wrap) {
                // Pills on lines of their own under the label, from the left
                // (as the mockup's flex-wrap puts them).
                std::vector<std::vector<int>> lines(1);
                float lw = 0;
                for (size_t o = 0; o < r.opts.size(); o++) {
                    float pw = pill_w(r.opts[o]);
                    if (lw > 0 && lw + 6 + pw > ROW_W) lines.push_back({}), lw = 0;
                    lines.back().push_back((int)o);
                    lw += (lw > 0 ? 6 : 0) + pw;
                }
                float py = top + lh + 10;
                p.parts.assign(r.opts.size(), RectF());
                for (auto& line : lines) {
                    // One line: from the left; several (the pills fill the
                    // row): each right-aligned, as flex-end in the mockup.
                    float total = -6;
                    for (int o : line) total += pill_w(r.opts[o]) + 6;
                    float px = lines.size() > 1 ? ROW_X + ROW_W - total : ROW_X;
                    for (int o : line) {
                        float pw = pill_w(r.opts[o]);
                        p.parts[o] = RectF(px, py, px + pw, py + 38);
                        px += pw + 6;
                    }
                    py += 38 + 6;
                }
                p.ctrlY = top + lh + 10;
                p.ctrlH = py - 6 - p.ctrlY;
                p.ctrlX = ROW_X, p.ctrlW = ROW_W;
                p.h = (py - 6 + 14) - y;
            } else {
                float rowH = std::max(lh, ch);
                p.ctrlX = wrap ? ROW_X : ROW_X + ROW_W - cw, p.ctrlW = cw, p.ctrlH = ch;
                p.ctrlY = wrap ? top + lh + 10 : top + (rowH - ch) / 2;
                if (r.kind == CHOICE) {
                    float px = p.ctrlX;
                    for (auto& o : r.opts) {
                        float pw = pill_w(o);
                        p.parts.push_back(RectF(px, p.ctrlY, px + pw, p.ctrlY + 38));
                        px += pw + 6;
                    }
                }
                p.h = (wrap ? lh + 10 + ch : rowH) + 28;
            }
        }
        y += p.h;
    }
    g_listH = y + 16;
}

// What can be highlighted, top to bottom: the top line (FOCUS_TOP: the six
// gems, then Close), the rows, and the Reset button (FOCUS_RESET). Within
// a line, U.focusSlot says which part (gem, choice, key slot).
const int FOCUS_TOP = -3, FOCUS_RESET = -2, CLOSE_SLOT = 6;
std::vector<int> focusables() {
    std::vector<int> f;
    f.push_back(FOCUS_TOP);
    const Category& c = g_cats[U.tab];
    for (size_t i = 0; i < c.rows.size(); i++)
        if (c.rows[i].kind != HEAD && c.rows[i].kind != BINDHEAD) f.push_back((int)i);
    f.push_back(-2);
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

void draw_switch(float x, float y, bool on, uint32_t acc) {
    G.rt->FillRoundedRectangle(rr(x, y, 64, 36, 18), brush(on ? rgb(acc) : rgb(0x3a466b)));
    G.rt->DrawRoundedRectangle(rr(x + 1, y + 1, 62, 34, 17), brush(on ? rgb(0xfff1c2) : rgb(0x5d6a8a)), 2);
    float kx = x + (on ? 31 : 3) + 2, ky = y + 3 + 2;
    G.rt->FillEllipse(D2D1::Ellipse(Point2F(kx + 13, ky + 15), 13, 13), brush(rgb(0x10182f, .4f)));
    G.rt->FillEllipse(D2D1::Ellipse(Point2F(kx + 13, ky + 13), 13, 13), brush(on ? rgb(0xf3cf6b) : rgb(0xb6c1d6)));
    // The star on the knob
    static ComPtr<ID2D1PathGeometry> star;
    if (!star) star = svg_path("M10 0L12.5 7.5L20 10L12.5 12.5L10 20L7.5 12.5L0 10L7.5 7.5Z");
    Matrix3x2F old;
    G.rt->GetTransform(&old);
    G.rt->SetTransform(Matrix3x2F::Scale(.6f, .6f) * Matrix3x2F::Translation(kx + 7, ky + 7) * old);
    G.rt->FillGeometry(star.Get(), brush(on ? rgb(0xa8741d) : rgb(0x7f8daa)));
    G.rt->SetTransform(old);
}

void draw_focus(D2D1_RECT_F r, float radius) {
    G.rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(r.left - 4, r.top - 4, r.right + 4, r.bottom + 4), radius + 4, radius + 4),
                               brush(rgb(0x3fd0d8)), 3);
}

void draw_rows(float listTop, float t) {
    const Category& c = g_cats[U.tab];
    uint32_t acc = c.acc;
    IDWriteTextFormat* fName = fmt(L"Nunito", 800, 16);
    IDWriteTextFormat* fDesc = fmt(L"Nunito", 600, 13);
    for (size_t i = 0; i < c.rows.size(); i++) {
        const Row& r = c.rows[i];
        const Placed& p = g_placed[i];
        float y0 = listTop + p.y - U.scroll;
        if (y0 > listTop + view_h() + 40 || y0 + p.h < listTop - 40) continue;
        float dy = listTop - U.scroll;  // list -> panel
        bool focused = U.focus == (int)i;  // highlighted by the mouse, stick, d-pad or keys
        if (r.kind == HEAD) {
            std::wstring up = r.label;
            for (auto& ch : up) ch = towupper(ch);
            text(up, fmt(L"Fredoka", 600, 14), ROW_X, y0 + 18, ROW_W, rgb(acc), DWRITE_TEXT_ALIGNMENT_LEADING, 14 * .12f);
            continue;
        }
        if (r.kind == BINDHEAD) {
            const wchar_t* heads[5] = {L"ACTION", L"KEY 1", L"KEY 2", L"PAD 1", L"PAD 2"};
            float fr = (ROW_W - 24) / 5.3f;
            text(heads[0], fmt(L"Nunito", 800, 11.5f), ROW_X, y0 + 6, fr * 1.3f, rgb(0xc6d0e6), DWRITE_TEXT_ALIGNMENT_LEADING, .9f);
            for (int s = 0; s < 4; s++)
                text(heads[s + 1], fmt(L"Nunito", 800, 11.5f), ROW_X + fr * 1.3f + 6 + s * (fr + 6), y0 + 6, fr, rgb(0xc6d0e6),
                     DWRITE_TEXT_ALIGNMENT_CENTER, .9f);
            continue;
        }
        if (r.kind == BIND || r.kind == FIXEDBIND) {
            float fr = (ROW_W - 24) / 5.3f;
            text_fit(r.label, fmt(L"Nunito", 800, 14), ROW_X, y0 + 5 + 8, fr * 1.3f, rgb(0xf4efe1), DWRITE_TEXT_ALIGNMENT_LEADING);
            for (int s = 0; s < 4; s++) {
                D2D1_RECT_F b = p.parts[s];
                b.top += dy, b.bottom += dy;
                std::wstring v;
                if (r.kind == FIXEDBIND) v = s == 0 ? L"F1" : s == 2 ? L"Back + Start" : L"";
                else v = slot_text(r.act, s);
                bool listen = U.listenRow == (int)i && U.listenSlot == s;
                bool hov = U.hoverRow == (int)i && U.hoverPart == s && r.kind == BIND;
                float bw = b.right - b.left;
                if (listen) {
                    float blink = U.reduced ? 1 : .6f + .4f * (.5f + .5f * std::cos((float)(t - U.listenT) * 2 * PI));
                    G.rt->FillRoundedRectangle(rr(b.left - 6, b.top - 6, bw + 12, 46, 14), rglow((b.left + b.right) / 2, b.top + 17, bw * .7f, rgb(acc, .6f)).Get());
                    G.rt->FillRoundedRectangle(rr(b.left, b.top, bw, 34, 10), brush(rgb(acc, blink)));
                    G.rt->DrawRoundedRectangle(rr(b.left + 1, b.top + 1, bw - 2, 32, 9), brush(rgb(0xfff1c2)), 2);
                    text_fit(L"Press…", fmt(L"Nunito", 800, 12), b.left + 6, b.top + 8, bw - 12, rgb(0x1b2440), DWRITE_TEXT_ALIGNMENT_CENTER);
                } else if (v.empty()) {
                    G.rt->DrawRoundedRectangle(rr(b.left + 1, b.top + 1, bw - 2, 32, 9), brush(rgb(0xd6deef, hov ? .7f : .45f)), 2,
                                               G.dashed.Get());
                    if (r.kind == BIND)
                        text_fit(L"+ Add", fmt(L"Nunito", 800, 12), b.left + 6, b.top + 8, bw - 12, rgb(0xaab6cf), DWRITE_TEXT_ALIGNMENT_CENTER);
                } else {
                    float op = r.kind == FIXEDBIND ? .7f : 1;
                    G.rt->FillRoundedRectangle(rr(b.left, b.top + 3, bw, 34, 10), brush(rgb(0x4a5677, op)));
                    G.rt->FillRoundedRectangle(rr(b.left, b.top, bw, 34, 10), vgrad(b.top, b.top + 34, rgb(0xc3cde0, op), rgb(0x9aa7c0, op)).Get());
                    if (hov) G.rt->FillRoundedRectangle(rr(b.left, b.top, bw, 34, 10), brush(ColorF(1, 1, 1, .12f)));
                    G.rt->DrawRoundedRectangle(rr(b.left + 1, b.top + 1, bw - 2, 32, 9), brush(rgb(0x5d6a8a, op)), 2);
                    text_fit(v, fmt(L"Nunito", 800, 12), b.left + 6, b.top + 8, bw - 12, rgb(0x1b2440, op), DWRITE_TEXT_ALIGNMENT_CENTER);
                }
                if (focused && U.focusSlot == s && r.kind == BIND) draw_focus(b, 10);
            }
            G.rt->DrawLine(Point2F(ROW_X, y0 + p.h), Point2F(ROW_X + ROW_W, y0 + p.h), brush(rgb(0xf3cf6b, .1f)), 1);
            continue;
        }
        // Label (and "Coming soon")
        float op = r.soon ? .45f : 1;
        auto ln = layout(r.label, fName, p.labelW);
        G.rt->DrawTextLayout(Point2F(ROW_X, y0 + 14), ln.Get(), brush(rgb(0xf4efe1, op)));
        float nameH = measure(ln.Get()).height;
        if (r.soon) {
            float nw = measure(ln.Get()).widthIncludingTrailingWhitespace;
            auto tl = layout(r.tag.empty() ? L"Coming soon" : r.tag, fmt(L"Nunito", 800, 11), 200);
            float tw = measure(tl.Get()).widthIncludingTrailingWhitespace + 16;
            float tx = ROW_X + nw + 8;
            if (tx + tw > ROW_X + p.labelW) tx = ROW_X + p.labelW - tw;
            G.rt->FillRoundedRectangle(rr(tx, y0 + 15, tw, 20, 10), brush(rgb(0xffe7a3, .16f)));
            G.rt->DrawRoundedRectangle(rr(tx + .5f, y0 + 15.5f, tw - 1, 19, 9.5f), brush(rgb(0xffe7a3, .55f)), 1);
            G.rt->DrawTextLayout(Point2F(tx + 8, y0 + 17), tl.Get(), brush(rgb(0xffe7a3)));
            nameH = std::max(nameH, 22.f);
        }
        if (!r.desc.empty())
            G.rt->DrawTextLayout(Point2F(ROW_X, y0 + 14 + nameH + 2), layout(r.desc, fDesc, p.labelW).Get(), brush(rgb(0xc6d0e6, op)));
        float cy = p.ctrlY + dy;
        int v = r.get ? r.get() : 0;
        if (r.kind == TOGGLE) {
            if (r.soon) G.rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                              D2D1::IdentityMatrix(), op), nullptr);
            draw_switch(p.ctrlX, cy, v != 0, acc);
            if (r.soon) G.rt->PopLayer();
            if (focused) draw_focus(RectF(p.ctrlX, cy, p.ctrlX + 64, cy + 36), 18);
        } else if (r.kind == SLIDER) {
            float tx = p.ctrlX, tw = 200, my = cy + 12;
            float k = clamp01((float)(v - r.min) / (r.max - r.min));
            D2D1_COLOR_F a = rgb(acc, op);
            G.rt->FillRoundedRectangle(rr(tx, my - 3, tw, 6, 3), brush(rgb(0x3a466b, op)));
            G.rt->FillRoundedRectangle(rr(tx, my - 3, tw * k, 6, 3), brush(a));
            G.rt->FillEllipse(D2D1::Ellipse(Point2F(tx + tw * k, my), 10, 10), brush(a));
            G.rt->DrawEllipse(D2D1::Ellipse(Point2F(tx + tw * k, my), 10, 10), brush(rgb(0xfff1c2, op)), 2);
            text(slider_text(r, v), fmt(L"Fredoka", 600, 17), tx + tw + 16, cy, 58, rgb(0xffe7a3, op), DWRITE_TEXT_ALIGNMENT_TRAILING);
            if (focused) draw_focus(RectF(tx - 6, cy - 4, tx + tw + 16 + 58, cy + 28), 8);
        } else if (r.kind == CHOICE) {
            for (size_t o = 0; o < r.opts.size(); o++) {
                D2D1_RECT_F b = p.parts[o];
                b.top += dy, b.bottom += dy;
                bool sel = (int)o == v;
                bool soonOpt = r.soon || (r.soonOpts >> o & 1);
                float po = soonOpt ? .4f : 1;
                bool hov = U.hoverRow == (int)i && U.hoverPart == (int)o && !soonOpt;
                float bw = b.right - b.left;
                if (sel) {
                    G.rt->FillRoundedRectangle(rr(b.left - 8, b.top - 8, bw + 16, 54, 27), rglow((b.left + b.right) / 2, b.top + 19, bw * .75f, rgb(acc, .5f * po)).Get());
                    G.rt->FillRoundedRectangle(rr(b.left, b.top, bw, 38, 19), brush(rgb(acc, po)));
                    G.rt->DrawRoundedRectangle(rr(b.left + 1, b.top + 1, bw - 2, 36, 18), brush(rgb(0xfff1c2, po)), 2);
                } else {
                    G.rt->FillRoundedRectangle(rr(b.left, b.top, bw, 38, 19), brush(rgb(hov ? 0x2c3d70 : 0x24335f, po)));
                    G.rt->DrawRoundedRectangle(rr(b.left + 1, b.top + 1, bw - 2, 36, 18), brush(rgb(0xf3cf6b, .4f * po)), 2);
                }
                text(r.opts[o], fmt(L"Nunito", 800, 13.5f), b.left, b.top + 10, bw, sel ? rgb(c.ink == 0xffffff ? 0x10182f : c.ink, po) : rgb(0xe6ebf5, po),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            if (focused && !p.parts.empty()) {
                D2D1_RECT_F b = p.parts[std::max(0, std::min<int>(U.focusSlot, (int)p.parts.size() - 1))];
                b.top += dy, b.bottom += dy;
                draw_focus(b, 19);
            }
        }
        G.rt->DrawLine(Point2F(ROW_X, y0 + p.h), Point2F(ROW_X + ROW_W, y0 + p.h), brush(rgb(0xf3cf6b, .16f)), 1);
    }
}

// Where things are, in tab coordinates (for the mouse).
struct Hits {
    D2D1_RECT_F close{}, reset{};
    D2D1_RECT_F gems[6]{};
    float tabX = 0, k = 1;  // window = (tab coordinates + tabX) * k
} H;

void render(float t) {
    ID2D1RenderTarget* rt = G.rt.Get();
    float k = G.h / 720.f;
    float vw = G.w / k;  // the window's width in scene units
    float openK = U.reduced ? 1 : clamp01((float)(t - U.openT) / .45f);
    float slide = bezier(.2f, .8f, .2f, 1, openK);
    float fade = U.reduced ? 1 : clamp01((float)(t - U.openT) / .35f);
    rt->BeginDraw();
    rt->Clear(ColorF(0, 0, 0, 0));
    rt->SetTransform(Matrix3x2F::Scale(k, k));
    // Scrim, darker toward the tab.
    {
        D2D1_GRADIENT_STOP st[3] = {{0, ColorF(10 / 255.f, 14 / 255.f, 32 / 255.f, .25f * fade)},
                                    {.55f, ColorF(10 / 255.f, 14 / 255.f, 32 / 255.f, .55f * fade)},
                                    {1, ColorF(10 / 255.f, 14 / 255.f, 32 / 255.f, .8f * fade)}};
        ComPtr<ID2D1GradientStopCollection> sc;
        rt->CreateGradientStopCollection(st, 3, &sc);
        ComPtr<ID2D1LinearGradientBrush> b;
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(Point2F(0, 0), Point2F(vw, 0)), sc.Get(), &b);
        rt->FillRectangle(RectF(0, 0, vw, 720), b.Get());
    }
    // "Game paused" chip, bottom left.
    {
        const std::wstring s = L"Game paused · changes apply live";
        IDWriteTextFormat* f = fmt(L"Fredoka", 600, 17);
        float tw = measure(layout(s, f, 800).Get()).widthIncludingTrailingWhitespace;
        float w = 16 + 18 + 10 + tw + 16, h = 40, x = 32, y = 720 - 32 - h;
        rt->FillRoundedRectangle(rr(x, y, w, h, 12), brush(rgb(0x121b3a, .85f * fade)));
        rt->DrawRoundedRectangle(rr(x + 1, y + 1, w - 2, h - 2, 11), brush(rgb(0xf3cf6b, fade)), 2);
        rt->FillRoundedRectangle(rr(x + 16 + 4, y + 11, 4, 18, 1.5f), brush(rgb(0xffe7a3, fade)));
        rt->FillRoundedRectangle(rr(x + 16 + 11, y + 11, 4, 18, 1.5f), brush(rgb(0xffe7a3, fade)));
        text(s, f, x + 16 + 18 + 10, y + 9, 800, rgb(0xffe7a3, fade));
    }
    // The tab: 520 wide at the right edge, sliding in.
    float tabX = vw - 520 + 60 * (1 - slide);
    H.tabX = tabX;
    H.k = k;
    Matrix3x2F T = Matrix3x2F::Translation(tabX, 0) * Matrix3x2F::Scale(k, k);
    rt->SetTransform(T);
    bool layer = openK < 1;
    if (layer) rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                   D2D1::IdentityMatrix(), bezier(.2f, .8f, .2f, 1, openK)), nullptr);
    {
        // Shadow to the left, the stone body with its gold edge.
        rt->FillRectangle(RectF(-40, 0, 0, 720), [&] {
            D2D1_GRADIENT_STOP st[2] = {{0, ColorF(6 / 255.f, 10 / 255.f, 26 / 255.f, 0)}, {1, ColorF(6 / 255.f, 10 / 255.f, 26 / 255.f, .45f)}};
            ComPtr<ID2D1GradientStopCollection> sc;
            rt->CreateGradientStopCollection(st, 2, &sc);
            ComPtr<ID2D1LinearGradientBrush> b;
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(Point2F(-40, 0), Point2F(0, 0)), sc.Get(), &b);
            return b;
        }().Get());
        D2D1_ROUNDED_RECT body = rr(0, 0, 560, 720, 26);  // extends past the right edge
        rt->FillRoundedRectangle(rr(-3, -3, 566, 726, 29), brush(rgb(0xf3cf6b)));
        rt->FillRoundedRectangle(body, vgrad(0, 720, rgb(0xb6c1d6), rgb(0x8f9cb7)).Get());
        rt->FillRectangle(RectF(20, 0, 540, 4), brush(ColorF(1, 1, 1, .4f)));
        rt->FillRectangle(RectF(0, 26, 6, 694), brush(ColorF(1, 1, 1, .18f)));
        // Header
        text(L"Settings", fmt(L"Fredoka", 700, 32), 24, 18 + 2 + 2, 300, ColorF(1, 1, 1, .5f));
        text(L"Settings", fmt(L"Fredoka", 700, 32), 24, 18 + 2, 300, rgb(0x1b2440));
        {
            IDWriteTextFormat* f = fmt(L"Fredoka", 700, 16);
            float tw = measure(layout(L"Close", f, 400).Get()).widthIncludingTrailingWhitespace;
            float w = 6 + 36 + 8 + tw + 14 + 4;
            float x = 520 - 20 - w, y = 18 + 2;
            stone_button(x, y, 44, L"Close", true, U.closeBtnHover);
            H.close = RectF(x, y, x + w, y + 44);
            if (U.focus == FOCUS_TOP && U.focusSlot == CLOSE_SLOT) draw_focus(H.close, 12);
        }
        // Gem tabs
        for (int i = 0; i < 6; i++) {
            const Category& c = g_cats[i];
            float gx = 24 + i * (472 - 70) / 5.f, gy = 74;
            bool sel = i == U.tab;
            float op = sel ? 1 : .5f, lift = sel ? -2.f : 0;
            float cx = gx + 35, cy = gy + 27 + lift;
            if (sel) rt->FillEllipse(D2D1::Ellipse(Point2F(cx, cy), 40, 40), rglow(cx, cy, 40, rgb(c.gem, .55f)).Get());
            ComPtr<ID2D1PathGeometry> hex;
            {
                ComPtr<ID2D1GeometrySink> s;
                G.f->CreatePathGeometry(&hex);
                hex->Open(&s);
                float x0 = cx - 27, y0 = cy - 27;
                s->BeginFigure(Point2F(x0 + 27, y0), D2D1_FIGURE_BEGIN_FILLED);
                D2D1_POINT_2F pts[5] = {{x0 + 54, y0 + 15.1f}, {x0 + 54, y0 + 38.9f}, {x0 + 27, y0 + 54}, {x0, y0 + 38.9f}, {x0, y0 + 15.1f}};
                s->AddLines(pts, 5);
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->Close();
            }
            rt->FillGeometry(hex.Get(), brush(rgb(c.gem, op)));
            draw_icon(c.id, cx - 12, cy - 12, 24, rgb(c.ink, op));
            text(c.name, fmt(L"Nunito", 800, 12), gx - 10, gy + 54 + 4 + lift, 90, rgb(0x1b2440, op), DWRITE_TEXT_ALIGNMENT_CENTER);
            H.gems[i] = RectF(gx, gy, gx + 70, gy + 74);
            if (U.focus == FOCUS_TOP && U.focusSlot == i) draw_focus(RectF(gx + 2, gy - 2, gx + 68, gy + 74), 14);
        }
        // The panel
        const Category& c = g_cats[U.tab];
        rt->SetTransform(Matrix3x2F::Translation(PANEL_X, PANEL_Y) * T);
        rt->FillRoundedRectangle(rr(0, 0, PANEL_W, PANEL_H, 16), vgrad(0, PANEL_H, rgb(0x1f2b4f), rgb(0x121b3a)).Get());
        rt->DrawRoundedRectangle(rr(5, 5, PANEL_W - 10, PANEL_H - 10, 12), brush(rgb(0xf3cf6b, .35f)), 1);
        rt->DrawRoundedRectangle(rr(1, 1, PANEL_W - 2, PANEL_H - 2, 15), brush(rgb(0xf3cf6b)), 2);
        // Head: badge, name, description.
        rt->FillEllipse(D2D1::Ellipse(Point2F(24 + 23, 40), 34, 34), rglow(47, 40, 34, rgb(c.acc, .6f)).Get());
        rt->FillEllipse(D2D1::Ellipse(Point2F(47, 40), 26, 26), brush(rgb(0xf3cf6b)));
        rt->FillEllipse(D2D1::Ellipse(Point2F(47, 40), 23, 23), brush(rgb(c.acc)));
        draw_icon(c.id, 47 - 12, 40 - 12, 24, rgb(0x10182f));
        text(c.name, fmt(L"Fredoka", 700, 26), 24 + 46 + 14, 40 - 26, 360, rgb(0xffe7a3));
        text(c.desc, fmt(L"Nunito", 700, 13), 24 + 46 + 14, 40 + 6, 360, rgb(0xc6d0e6));
        rt->DrawLine(Point2F(2, 80), Point2F(PANEL_W - 2, 80), brush(rgb(0xf3cf6b, .35f)), 2);
        // Rows, clipped to the list area.
        rt->PushAxisAlignedClip(RectF(2, ROWS_TOP, PANEL_W - 2, PANEL_H - ROWS_BOTTOM), D2D1_ANTIALIAS_MODE_ALIASED);
        draw_rows(ROWS_TOP, t);
        rt->PopAxisAlignedClip();
        // Scroll bar
        if (g_listH > view_h()) {
            float vh = view_h(), bh = std::max(30.f, vh * vh / g_listH), by = ROWS_TOP + (vh - bh) * (U.scroll / (g_listH - vh));
            rt->FillRoundedRectangle(rr(PANEL_W - 9, by + 2, 4, bh - 4, 2), brush(rgb(0xf3cf6b, .5f)));
        }
        // Footer: Reset, note.
        float fy = PANEL_H - ROWS_BOTTOM;
        rt->FillRectangle(RectF(2, fy, PANEL_W - 2, PANEL_H - 14), brush(rgb(0x121b3a, .6f)));
        rt->FillRoundedRectangle(rr(2, PANEL_H - 30, PANEL_W - 4, 28, 14), brush(rgb(0x121b3a, .6f)));
        rt->DrawLine(Point2F(2, fy), Point2F(PANEL_W - 2, fy), brush(rgb(0xf3cf6b, .35f)), 2);
        float rw = stone_button(24, fy + 12, 40, L"Reset " + c.name, false, U.resetHover);
        H.reset = RectF(PANEL_X + 24, PANEL_Y + fy + 12, PANEL_X + 24 + rw, PANEL_Y + fy + 52);
        if (U.focus == FOCUS_RESET) draw_focus(RectF(24, fy + 12, 24 + rw, fy + 52), 12);
        text(L"Saved to settings.ini right away", fmt(L"Nunito", 700, 12.5f), 24, fy + 24, PANEL_W - 48, rgb(0xc6d0e6),
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (layer) rt->PopLayer();
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
        D2D1_STROKE_STYLE_PROPERTIES dp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                     D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_DASH);
        G.f->CreateStrokeStyle(dp, nullptr, 0, &G.dashed);
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
    const Category& c = g_cats[U.tab];
    return i >= 0 && i < (int)c.rows.size() ? &c.rows[i] : nullptr;
}

// Where the highlight starts on a line: the gem of this category, the
// chosen choice, the first key slot.
void enter_line(int focus) {
    U.focus = focus;
    if (focus == FOCUS_TOP) U.focusSlot = U.tab;
    else if (const Row* r = row_at(focus)) U.focusSlot = r->kind == CHOICE ? std::max(0, r->get ? r->get() : 0) : 0;
    else U.focusSlot = 0;
}

void set_tab(int t) {
    bool onTop = U.focus == FOCUS_TOP;
    U.tab = (t + 6) % 6;
    U.scroll = 0;
    U.listenRow = U.listenSlot = -1;
    place_rows();
    enter_line(FOCUS_TOP);  // the highlight stays on the gems (the new one)
    (void)onTop;
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

// A (controller), Enter or Space, a click: what's highlighted.
void activate() {
    if (U.focus == FOCUS_TOP) {
        if (U.focusSlot >= CLOSE_SLOT) open_tab(false);
        else if (U.focusSlot != U.tab) set_tab(U.focusSlot);
        return;
    }
    if (U.focus == FOCUS_RESET) {
        reset_tab();
        return;
    }
    const Row* r = row_at(U.focus);
    if (!r) return;
    if (r->kind == BIND) start_listen(U.focus, U.focusSlot);
    else if (r->kind == TOGGLE) step(*r, 1);
    else if (r->kind == CHOICE) choose(*r, U.focusSlot);
}

// The stick, d-pad and arrow keys move the highlight: up and down between
// lines, left and right between a line's parts (on a slider they move its
// value, on a switch they set it).
enum Nav { N_UP, N_DOWN, N_LEFT, N_RIGHT, N_OK, N_BACK, N_PREV, N_NEXT };
void nav(Nav n) {
    U.keyNav = true;
    if (U.focus == -1 && n != N_BACK && n != N_PREV && n != N_NEXT) {
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
        } else if (r && r->kind == CHOICE) {
            int n2 = (int)r->opts.size();
            U.focusSlot = std::max(0, std::min(n2 - 1, U.focusSlot + d));
            changed();
        } else if (r && r->kind == BIND) {
            U.focusSlot = std::max(0, std::min(3, U.focusSlot + d));
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
    case N_PREV: set_tab(U.tab - 1); break;
    case N_NEXT: set_tab(U.tab + 1); break;
    }
    if (getenv("WONDERPARK_F1_SCRIPT"))  // tests: where the highlight is
        fprintf(stderr, "[f1] nav %d: tab %d, focus %d, part %d, window_mode %d\n", (int)n, U.tab, U.focus,
                U.focusSlot, g_x[X_WINDOW_MODE]);
}

// ---- Mouse: what's under a window point.
struct Hit { int what = 0; int row = -1, part = -1; };  // 1 close, 2 reset, 3 gem, 4 row part
Hit hit_test(int px, int py) {
    Hit h;
    float x = px / H.k - H.tabX, y = py / H.k;
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };
    if (in(H.close)) return {1};
    if (in(H.reset)) return {2};
    for (int i = 0; i < 6; i++)
        if (in(H.gems[i])) return {3, -1, i};
    float lx = x - PANEL_X, ly = y - PANEL_Y;
    if (lx < 0 || lx > PANEL_W || ly < ROWS_TOP || ly > PANEL_H - ROWS_BOTTOM) return h;
    float listY = ly - ROWS_TOP + U.scroll;
    const Category& c = g_cats[U.tab];
    for (size_t i = 0; i < c.rows.size(); i++) {
        const Placed& p = g_placed[i];
        if (listY < p.y || listY > p.y + p.h) continue;
        const Row& r = c.rows[i];
        if (r.kind == BIND || r.kind == CHOICE) {
            for (size_t s = 0; s < p.parts.size(); s++) {
                const D2D1_RECT_F& b = p.parts[s];
                if (lx >= b.left && lx <= b.right && listY >= b.top && listY <= b.bottom) return {4, (int)i, (int)s};
            }
        } else if (r.kind == TOGGLE || r.kind == SLIDER) {
            if (lx >= p.ctrlX - 8 && lx <= p.ctrlX + (r.kind == SLIDER ? 210 : p.ctrlW) + 8 && listY >= p.ctrlY - 8 &&
                listY <= p.ctrlY + p.ctrlH + 8)
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
    float k = clamp01(x / 200);
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
            if (p + 3 >= d.size()) return out;
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
    add(L"Frame rate", {L"30 (original)", L"60"}, 0, field(&g_settings.frame_rate, {30, 60}));
    add(L"Anti-aliasing", {L"Off", L"2×", L"4×", L"8×"}, 0, field(&g_settings.msaa, {1, 2, 4, 8}));
    add(L"Graphics quality", {L"Auto", L"Original", L"2×", L"3×", L"4×"}, 0, field(&g_settings.scale, {0, 1, 2, 3, 4}));
    add(L"Widescreen", {L"Off", L"On"}, 0, x(X_WIDESCREEN));
    add(L"V-Sync", {L"Off", L"On"}, 3, x(X_VSYNC));
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
    // Rows available only sometimes: greyed (soon) while they can't be used.
    for (auto& r : g_cats[U.tab].rows)
        if (r.off) {
            bool o = r.off();
            if (o != r.soon) r.soon = o, U.gen++;
        }
    if (g_placed.size() != g_cats[U.tab].rows.size()) place_rows();
    double t = now_s();
    bool animating = !U.reduced && (t - U.openT < .5 || U.listenRow >= 0);
    if (drawnGen != U.gen || dw != w || dh != h || animating) {
        render((float)t);
        copy_out(w, h);
        if (drawnGen != U.gen || animating) U.gen += animating ? 1 : 0;
        drawnGen = U.gen;
        dw = w, dh = h;
    }
    // Test shots: each category in turn (top, then scrolled to the end when
    // it doesn't fit), then closed. Steps: 2 per category.
    if (g_shotStep >= 1 && now_s() >= g_shotAt) {
        int cat = (g_shotStep - 1) / 2, bottom = (g_shotStep - 1) % 2;
        if (cat < 6) {
            if (bottom && g_listH <= view_h()) {
                g_shotStep++;  // nothing more to see
            } else {
                // The frame as shown (game and tab), saved by the renderer at
                // its next present.
                std::string id(g_cats[U.tab].id.begin(), g_cats[U.tab].id.end());
                snprintf(g_dump_next, sizeof(g_dump_next), "%s-%d%s-%s.bmp", g_shotPrefix.c_str(), cat + 1,
                         bottom ? "b" : "", id.c_str());
                g_shotStep++;
            }
            g_shotAt = now_s() + .3;
            if ((g_shotStep - 1) % 2 == 1 && (g_shotStep - 1) / 2 == cat) {
                U.scroll = 1e6f;  // next: the same category, scrolled to the end
                clamp_scroll();
            } else if (cat < 5) {
                set_tab(cat + 1);
            }
            changed();
        } else {
            open_tab(false);
            g_shotStep = -1;
        }
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
        case VK_ESCAPE: open_tab(false); break;
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
        int hr = h.what == 4 ? h.row : -1, hp = h.what == 4 ? h.part : h.what == 3 ? 100 + h.part : -1;
        bool ch = h.what == 1, rh = h.what == 2;
        if (hr != U.hoverRow || hp != U.hoverPart || ch != U.closeBtnHover || rh != U.resetHover) {
            U.hoverRow = hr, U.hoverPart = hp, U.closeBtnHover = ch, U.resetHover = rh;
            changed();
        }
        // The highlight follows the mouse onto whatever it's over (the
        // same highlight the stick and keys move; A or a click selects).
        if (h.what == 1) U.focus = FOCUS_TOP, U.focusSlot = CLOSE_SLOT;
        else if (h.what == 2) U.focus = FOCUS_RESET, U.focusSlot = 0;
        else if (h.what == 3) U.focus = FOCUS_TOP, U.focusSlot = h.part;
        else if (h.what == 4) U.focus = h.row, U.focusSlot = h.part;
        SetCursor(LoadCursor(nullptr, h.what ? IDC_HAND : IDC_ARROW));
        return 1;
    }
    case WM_LBUTTONDOWN: {
        U.keyNav = false;
        Hit h = hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h.what == 1) open_tab(false);
        else if (h.what == 2) reset_tab();
        else if (h.what == 3) set_tab(h.part);
        else if (h.what == 4) {
            const Row* r = row_at(h.row);
            if (r->kind == CHOICE) choose(*r, h.part);
            else if (r->kind == TOGGLE) step(*r, 1);
            else if (r->kind == SLIDER && !r->soon) {
                U.dragRow = h.row;
                SetCapture((HWND)win_handle());
                slider_drag(h.row, GET_X_LPARAM(lp));
            } else if (r->kind == BIND) start_listen(h.row, h.part);
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
    case WM_RBUTTONDOWN:  // right click: back (closes the tab)
        if (msg == WM_RBUTTONDOWN) open_tab(false);
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
