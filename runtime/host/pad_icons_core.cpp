// Controller button icons, the part shared by the game and the launcher: the
// icons themselves, the pad's type and the buttons' names. (pad_icons.cpp
// adds the game's side: the setting, key caps and the in-game prompts.)
//
// The icons are Flat Gamepad Icons by RobTheFiveNine (CC BY-SA 4.0,
// assets/ui/pad-icons/LICENSE.txt), plus our home icon in the same style,
// built into the program as resources (pad_icons.rc, PADICON_<NAME>_<DPI>)
// and decoded once, at start, on a thread of their own (WIC). The pack's
// *_super icons (the platforms' logos) are never included.
//
// The pad's type: we read pads through XInput, which doesn't say what a
// pad is, so the type comes from the game controllers Windows lists (HID),
// by maker: Sony is PlayStation, Nintendo (and the Switch pads of Hori,
// PowerA and PDP) Nintendo, anything else Xbox, which most PC pads copy. A
// PlayStation or Switch pad reaches XInput only through a mapper (Steam
// Input, DS4Windows), which shows it as an Xbox pad: its own listing still
// says what it is, and that wins. Looked at again, on the same thread, when
// a pad connects or goes away (pad_icons_hotplug).
//
// Buttons go by position: XInput's A is the bottom face button, which a
// PlayStation pad prints as a cross and a Nintendo pad as B; so a press of
// the bottom button always shows the bottom button's own letter.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pad_icons.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace {

// The XInput buttons (host_settings.h XPAD_*; the launcher doesn't include it).
enum : uint32_t {
    B_UP = 0x0001, B_DOWN = 0x0002, B_LEFT = 0x0004, B_RIGHT = 0x0008, B_START = 0x0010, B_BACK = 0x0020,
    B_LTHUMB = 0x0040, B_RTHUMB = 0x0080, B_LB = 0x0100, B_RB = 0x0200, B_A = 0x1000, B_B = 0x2000, B_X = 0x4000,
    B_Y = 0x8000, B_LT = 0x10000, B_RT = 0x20000,
};

// ------------------------------------------------------------------------
// The icons

struct Variant {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // straight
};
struct Icon {
    std::vector<Variant> sizes;  // by height, smallest first
};

std::map<std::string, Icon>* g_icons;  // set once read (then never changed)
std::atomic<int> g_ready{0};
std::atomic<int> g_detected{PADSET_XB};
std::atomic<uint32_t> g_found{0};  // bumped each time the type is looked at
char g_foundText[96] = "not looked yet";
HANDLE g_wake;

bool decode_png(IWICImagingFactory* wic, const void* data, DWORD size, Variant& v) {
    ComPtr<IWICStream> s;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateStream(&s)) || FAILED(s->InitializeFromMemory((BYTE*)data, size)) ||
        FAILED(wic->CreateDecoderFromStream(s.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT w, h;
    conv->GetSize(&w, &h);
    v.w = (int)w, v.h = (int)h;
    v.rgba.resize((size_t)w * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)v.rgba.size(), v.rgba.data()));
}

BOOL CALLBACK collect_name(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR list) {
    if (!IS_INTRESOURCE(name) && !wcsncmp(name, L"PADICON_", 8))
        reinterpret_cast<std::vector<std::wstring>*>(list)->push_back(name);
    return TRUE;
}

void read_icons() {
    ComPtr<IWICImagingFactory> wic;
    auto* icons = new std::map<std::string, Icon>();
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        std::vector<std::wstring> names;
        EnumResourceNamesW(nullptr, (LPCWSTR)RT_RCDATA, collect_name, (LONG_PTR)&names);
        for (auto& n : names) {
            // PADICON_XB_A_300 -> "xb_a"
            size_t us = n.rfind(L'_');
            if (us == std::wstring::npos || us <= 8) continue;
            std::string name;
            for (size_t i = 8; i < us; i++) name += (char)towlower(n[i]);
            if (name.size() > 6 && !name.compare(name.size() - 6, 6, "_super")) continue;  // logos: never
            HRSRC r = FindResourceW(nullptr, n.c_str(), (LPCWSTR)RT_RCDATA);
            HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
            Variant v;
            if (!g || !decode_png(wic.Get(), LockResource(g), SizeofResource(nullptr, r), v)) continue;
            auto& sizes = (*icons)[name].sizes;
            sizes.push_back(std::move(v));
            std::sort(sizes.begin(), sizes.end(), [](const Variant& a, const Variant& b) { return a.h < b.h; });
        }
    }
    g_icons = icons;
    g_ready.store(1, std::memory_order_release);
}

// ------------------------------------------------------------------------
// The pad's type

// Switch pads by other makers (vendor, product): Hori, PowerA, PDP.
const struct { uint16_t vid, pid; } kSwitchThirdParty[] = {
    {0x0F0D, 0x0092}, {0x0F0D, 0x00C1}, {0x0F0D, 0x00F6}, {0x0F0D, 0x00DC}, {0x20D6, 0xA711}, {0x20D6, 0xA712},
    {0x20D6, 0xA713}, {0x20D6, 0xA714}, {0x20D6, 0xA715}, {0x20D6, 0xA716}, {0x20D6, 0xA717}, {0x20D6, 0xA718},
    {0x0E6F, 0x0180}, {0x0E6F, 0x0181}, {0x0E6F, 0x0184}, {0x0E6F, 0x0185}, {0x0E6F, 0x0186}, {0x0E6F, 0x0187},
    {0x0E6F, 0x0188},
};
// Sony's and Nintendo's controllers (their other devices aren't pads):
// DualShock 3 and 4, DualSense (and Edge); Joy-Cons, the Pro Controller,
// the charging grip, the Online controllers.
const uint16_t kSony[] = {0x0268, 0x05C4, 0x09CC, 0x0BA0, 0x0CE6, 0x0DF2, 0x0E5F};
const uint16_t kNintendo[] = {0x2006, 0x2007, 0x2009, 0x200E, 0x2017, 0x2019, 0x201A, 0x201E,
                              0x2066, 0x2067, 0x2069, 0x2073};

// The vendor and product in a device path: USB "vid_054c&pid_09cc",
// Bluetooth "vid&0002054c_pid&09cc" (the first 4 of the 8 digits say
// where the vendor number is from).
bool ids_of(std::wstring p, uint16_t* vid, uint16_t* pid) {
    for (auto& ch : p) ch = towlower(ch);
    size_t v = p.find(L"vid_"), q = p.find(L"pid_");
    if (v == std::wstring::npos || q == std::wstring::npos) {
        v = p.find(L"vid&");
        q = p.find(L"pid&");
        if (v == std::wstring::npos || q == std::wstring::npos) return false;
        v += 4;
    }
    if (v + 8 > p.size() || q + 8 > p.size()) return false;
    *vid = (uint16_t)wcstoul(p.substr(v + 4, 4).c_str(), nullptr, 16);
    *pid = (uint16_t)wcstoul(p.substr(q + 4, 4).c_str(), nullptr, 16);
    return true;
}

void detect() {
    static const GUID kHid = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
    HDEVINFO set = SetupDiGetClassDevsW(&kHid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    int type = PADSET_XB;
    uint16_t foundVid = 0, foundPid = 0;
    if (set != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA d = {sizeof(d)};
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &kHid, i, &d); i++) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &d, nullptr, 0, &need, nullptr);
            if (!need) continue;
            std::vector<uint8_t> buf(need);
            auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &d, det, need, nullptr, nullptr)) continue;
            uint16_t vid, pid;
            if (!ids_of(det->DevicePath, &vid, &pid)) continue;
            int t = -1;
            if (vid == 0x054C && std::find(std::begin(kSony), std::end(kSony), pid) != std::end(kSony)) t = PADSET_PS;
            if (vid == 0x057E && std::find(std::begin(kNintendo), std::end(kNintendo), pid) != std::end(kNintendo))
                t = PADSET_NS;
            for (auto& s : kSwitchThirdParty)
                if (s.vid == vid && s.pid == pid) t = PADSET_NS;
            if (t < 0) continue;
            // PlayStation before Nintendo if both are there (the setting decides otherwise).
            if (type == PADSET_XB || t == PADSET_PS) type = t, foundVid = vid, foundPid = pid;
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    if (type == PADSET_PS || type == PADSET_NS)
        snprintf(g_foundText, sizeof(g_foundText), "%s (%04X:%04X)", type == PADSET_PS ? "PlayStation" : "Nintendo",
                 foundVid, foundPid);
    else
        snprintf(g_foundText, sizeof(g_foundText), "Xbox (no PlayStation or Nintendo pad listed)");
    g_detected = type;
    g_found++;
    fprintf(stderr, "pad icons: %s\n", g_foundText);
}

DWORD WINAPI worker(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    read_icons();
    for (;;) {
        detect();
        WaitForSingleObject(g_wake, INFINITE);
        Sleep(500);  // a pad's HID side is listed a moment after it connects
    }
}

// ------------------------------------------------------------------------
// Names

struct Names {
    uint32_t b;
    const char *xb, *ps, *ns, *xbLabel, *psLabel, *nsLabel;
};
const Names kNames[] = {
    {B_A, "xb_a", "ps_x", "switch_b", "A", "Cross", "B"},
    {B_B, "xb_b", "ps_circle", "switch_a", "B", "Circle", "A"},
    {B_X, "xb_x", "ps_square", "switch_y", "X", "Square", "Y"},
    {B_Y, "xb_y", "ps_triangle", "switch_x", "Y", "Triangle", "X"},
    {B_LB, "xb_lb", "ps_l1", "switch_l", "LB", "L1", "L"},
    {B_RB, "xb_rb", "ps_r1", "switch_r", "RB", "R1", "R"},
    {B_LT, "xb_lt", "ps_l2", "switch_zl", "LT", "L2", "ZL"},
    {B_RT, "xb_rt", "ps_r2", "switch_zr", "RT", "R2", "ZR"},
    {B_START, "xb_start", "generic_start", "switch_plus", "Start", "Options", "+"},
    {B_BACK, "xb_select", "generic_select", "switch_minus", "Back", "Share", "-"},
    {B_LTHUMB, "generic_l3", "generic_l3", "generic_l3", "LS", "L3", "LS"},
    {B_RTHUMB, "generic_r3", "generic_r3", "generic_r3", "RS", "R3", "RS"},
    {B_UP, "generic_dpad_up", "generic_dpad_up", "generic_dpad_up", "Up", "Up", "Up"},
    {B_DOWN, "generic_dpad_down", "generic_dpad_down", "generic_dpad_down", "Down", "Down", "Down"},
    {B_LEFT, "generic_dpad_left", "generic_dpad_left", "generic_dpad_left", "Left", "Left", "Left"},
    {B_RIGHT, "generic_dpad_right", "generic_dpad_right", "generic_dpad_right", "Right", "Right", "Right"},
};

const Names* names_of(uint32_t b) {
    for (auto& n : kNames)
        if (n.b == b) return &n;
    return nullptr;
}

const Icon* icon_of(const char* name) {
    if (!name || !g_ready.load(std::memory_order_acquire)) return nullptr;
    auto it = g_icons->find(name);
    return it == g_icons->end() || it->second.sizes.empty() ? nullptr : &it->second;
}

}  // namespace

extern "C" {

void pad_icons_core_init(void) {
    static bool started;
    if (started) return;
    started = true;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)) CloseHandle(t);
}

void pad_icons_hotplug(void) {
    if (g_wake) SetEvent(g_wake);
}

int pad_icons_detected(void) { return g_detected; }

const char* pad_icons_name(uint32_t xpad, int set) {
    const Names* n = names_of(xpad);
    if (!n) return nullptr;
    return set == PADSET_PS ? n->ps : set == PADSET_NS ? n->ns : n->xb;
}

const char* pad_icons_label(uint32_t xpad, int set) {
    const Names* n = names_of(xpad);
    if (!n) return "";
    return set == PADSET_PS ? n->psLabel : set == PADSET_NS ? n->nsLabel : n->xbLabel;
}

int pad_icons_width(const char* name, int h) {
    const Icon* ic = icon_of(name);
    if (!ic || h <= 0) return 0;
    const Variant& v = ic->sizes.back();
    return std::max(1, (int)((long long)v.w * h / v.h));
}

int pad_icons_rgba(const char* name, int h, uint8_t* out, int maxw) {
    const Icon* ic = icon_of(name);
    if (!ic || h <= 0) return 0;
    // The nearest size at or above h (the largest if none is).
    const Variant* src = &ic->sizes.back();
    for (auto& v : ic->sizes)
        if (v.h >= h) {
            src = &v;
            break;
        }
    int w = pad_icons_width(name, h);
    if (w > maxw) return 0;
    // Each pixel the average of the source's under it (weighted by alpha,
    // so clear pixels don't darken the edge).
    for (int y = 0; y < h; y++) {
        float sy0 = (float)y * src->h / h, sy1 = (float)(y + 1) * src->h / h;
        for (int x = 0; x < w; x++) {
            float sx0 = (float)x * src->w / w, sx1 = (float)(x + 1) * src->w / w;
            float acc[4] = {0, 0, 0, 0}, area = 0;
            for (int sy = (int)sy0; sy < src->h && sy < sy1; sy++) {
                float fy = std::min(sy1, (float)sy + 1) - std::max(sy0, (float)sy);
                for (int sx = (int)sx0; sx < src->w && sx < sx1; sx++) {
                    float f = fy * (std::min(sx1, (float)sx + 1) - std::max(sx0, (float)sx));
                    const uint8_t* p = &src->rgba[((size_t)sy * src->w + sx) * 4];
                    float a = p[3] / 255.f * f;
                    acc[0] += p[0] * a, acc[1] += p[1] * a, acc[2] += p[2] * a, acc[3] += a;
                    area += f;
                }
            }
            uint8_t* o = &out[((size_t)y * w + x) * 4];
            if (acc[3] <= 0 || area <= 0) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            o[0] = (uint8_t)std::min(255.f, acc[0] / acc[3] + .5f);
            o[1] = (uint8_t)std::min(255.f, acc[1] / acc[3] + .5f);
            o[2] = (uint8_t)std::min(255.f, acc[2] / acc[3] + .5f);
            o[3] = (uint8_t)std::min(255.f, acc[3] / area * 255 + .5f);
        }
    }
    return w;
}

uint32_t pad_icons_core_generation(void) {
    return g_found.load() * 40503u ^ (uint32_t)g_ready.load(std::memory_order_acquire) << 7 ^ (uint32_t)g_detected << 9;
}

const char* pad_icons_found(void) { return g_foundText; }

}  // extern "C"
