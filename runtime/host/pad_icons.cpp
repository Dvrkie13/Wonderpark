// Controller button icons, the game's side: which set the player sees, key
// caps, and the game's button prompts (hle_prompts.c). The icons, the pad's
// type and the buttons' names are in pad_icons_core.cpp (shared with the
// launcher).
//
// Which set: the Button icons setting (Controls; [input] button_icons,
// WONDERPARK_BUTTON_ICONS): Auto shows the connected pad's own type; Xbox,
// PlayStation or Nintendo when the type is found wrong; Keyboard only never
// shows a pad's buttons in the game's prompts.
//
// The game's prompts show the device used last: a pad's button or its stick
// past half switches them to the pad's icons, a key or mouse button back to
// key caps (drawn here in code, no art).

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl/client.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
#include "host_input.h"
#include "host_settings.h"
#include "pad_icons.h"


}

using Microsoft::WRL::ComPtr;

namespace {

// ------------------------------------------------------------------------
// Key caps: a rounded key with the key's name, as coverage. Direct2D in
// software, only on the game thread, when the prompts are redrawn.

struct Caps {
    ComPtr<ID2D1Factory> f;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteFactory> dw;
    bool tried = false, ok = false;
} C;

bool caps_init() {
    if (C.tried) return C.ok;
    C.tried = true;
    C.ok = SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, C.f.GetAddressOf())) &&
           SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&C.wic))) &&
           SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(C.dw.GetAddressOf())));
    return C.ok;
}

int key_cap(int vk, uint8_t* cov, int w, int h) {
    char name[32];
    settings_key_name(vk, name, sizeof(name));
    if (!name[0] || !caps_init()) return 0;
    wchar_t text[32];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, text, 32);
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> white;
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(C.wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return 0;
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(C.f->CreateWicBitmapRenderTarget(bmp.Get(), p, &rt))) return 0;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    float line = std::max(1.5f, h / 12.f);
    // The letters as large as fit across (Space, Enter, Shift are long).
    size_t len = wcslen(text);
    float size = std::min(h * .62f, (w - 2 * line) * .92f / (len * .58f));
    C.dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                           size, L"en-us", &fmt);
    if (!fmt) return 0;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    D2D1_RECT_F r = D2D1::RectF(line / 2, line / 2, w - line / 2, h - line / 2);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(r, h / 5.f, h / 5.f), white.Get(), line);
    rt->DrawText(text, (UINT32)len, fmt.Get(), D2D1::RectF(0, 0, (float)w, (float)h * .96f), white.Get());
    if (FAILED(rt->EndDraw())) return 0;
    std::vector<uint8_t> px((size_t)w * h * 4);
    bmp->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data());
    for (int i = 0; i < w * h; i++) cov[i] = px[i * 4 + 3];
    return 1;
}

// ------------------------------------------------------------------------
// The game's prompts (hle_prompts.c)

const uint32_t kActionBit[ACT_COUNT] = {
    HOST_PAD_SCREEN_UP, HOST_PAD_SCREEN_DOWN, HOST_PAD_SCREEN_LEFT, HOST_PAD_SCREEN_RIGHT, HOST_PAD_2, HOST_PAD_1,
    HOST_PAD_A, HOST_PAD_B, HOST_PAD_PLUS, HOST_PAD_MINUS, HOST_PAD_HOME, HOST_PAD_SHAKE, HOST_PAD_SHAKE2,
};

// A pad icon as coverage for the game's one-colour font: the icon's light
// parts (its rim and its letter) as ink, its dark face clear; fitted into
// w x h, centred.
int pad_cover(const char* name, uint8_t* cov, int w, int h) {
    int iw = pad_icons_width(name, h), ih = h;
    if (iw <= 0) return 0;
    if (iw > w) ih = std::max(1, h * w / iw), iw = pad_icons_width(name, ih);
    std::vector<uint8_t> rgba((size_t)iw * ih * 4);
    if (!pad_icons_rgba(name, ih, rgba.data(), iw)) return 0;
    memset(cov, 0, (size_t)w * h);
    int ox = (w - iw) / 2, oy = (h - ih) / 2;
    for (int y = 0; y < ih; y++)
        for (int x = 0; x < iw; x++) {
            const uint8_t* p = &rgba[((size_t)y * iw + x) * 4];
            float lum = (.299f * p[0] + .587f * p[1] + .114f * p[2]) / 255.f;
            float ink = std::min(1.f, std::max(0.f, (lum - .40f) / .25f));
            int X = ox + x, Y = oy + y;
            if (X >= 0 && Y >= 0 && X < w && Y < h) cov[Y * w + X] = (uint8_t)(ink * p[3]);
        }
    return 1;
}

int prompt_icon(uint32_t button, uint8_t* cov, int w, int h) {
    int a, set = pad_icons_prompt_set();
    for (a = 0; a < ACT_COUNT && kActionBit[a] != button; a++) {}
    if (a == ACT_COUNT) return 0;
    if (set == PADSET_KEYS) {
        // The d-pad would be four keys, too small to read: the game's cross stays.
        if (button == HOST_PAD_SCREEN_UP) return 0;
        for (int k = 0; k < BIND_SLOTS; k++)
            if (g_settings.key[a][k]) return key_cap(g_settings.key[a][k], cov, w, h);
        return 0;
    }
    if (button == HOST_PAD_SCREEN_UP) return pad_cover("generic_left_stick", cov, w, h);  // the stick always moves
    for (int k = 0; k < BIND_SLOTS; k++)
        if (g_settings.pad[a][k]) return pad_cover(pad_icons_name(g_settings.pad[a][k], set), cov, w, h);
    return 0;
}

uint32_t prompt_generation(void) { return pad_icons_generation(); }

}  // namespace

extern "C" {

void pad_icons_init(void) {
    pad_icons_core_init();
    g_host_prompt_icon = prompt_icon;
    g_host_prompt_generation = prompt_generation;
}

// (input_win32.c calls this at start.)
void prompts_init(void) { pad_icons_init(); }

int pad_icons_pad_set(void) {
    switch (g_x[X_BUTTON_ICONS]) {
    case 1: return PADSET_XB;
    case 2: return PADSET_PS;
    case 3: return PADSET_NS;
    default: return pad_icons_detected();
    }
}

int pad_icons_prompt_set(void) {
    if (g_x[X_BUTTON_ICONS] == 4 || !input_controller_last()) return PADSET_KEYS;
    return pad_icons_pad_set();
}

uint32_t pad_icons_generation(void) {
    uint32_t g = settings_generation() * 2654435761u ^ (uint32_t)pad_icons_prompt_set() << 3 ^ pad_icons_core_generation();
    // The low bit: a pad's icons are shown (hle_prompts.c draws those and key
    // caps a little differently).
    return (g & ~1u) | (pad_icons_prompt_set() != PADSET_KEYS);
}

}  // extern "C"
