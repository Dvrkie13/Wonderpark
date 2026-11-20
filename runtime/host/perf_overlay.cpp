// The performance overlay (F1 > Display > Performance overlay; F7 cycles
// it): a small piece of the PokéPark Pad in a corner of the screen: brown
// leather, a dashed cream stitch, four gold corner pieces (our own drawing,
// docs: the handoff kit's fps-overlay-mockup.html), cream text in the
// notebook's fonts (Fredoka, Nunito; SIL OFL, built into the program).
//
//   Compact:  a status dot, the frame rate, "FPS".
//   Detailed: the frame rate, the 1% and 0.1% lows (the last 10 s), a frame
//             time graph (the last 2 s, a point per frame shown, the target
//             as a dashed line), Wonderpark's own timings (Game: the game
//             thread's work per game frame; Graphics: turning its GX into
//             OpenGL; GPU: the graphics card's time per frame shown, from
//             timer queries) as bars against the frame's budget, and This
//             PC: CPU and GPU use with small graphs, the graphics memory and
//             RAM Wonderpark uses, with bars.
//
// Status colour: green at the target (95% of it or more), yellow a little
// under (80%), red below. The target is the Frame rate setting, or the
// screen's refresh rate if that's lower (no more is ever shown); 30 while a movie
// plays (movies always play at 30).
//
// Costs the game almost nothing: the renderer only notes each frame's time
// (perf_overlay_frame) and lays the finished picture over the window
// (gl_backend.c draw_fps, premultiplied, its texture reused); the picture is drawn on a thread of its own, below the game's priority, with
// software Direct2D, 8 times a second (Compact 4), at the UI's scale, so it
// stays crisp at 1440p and 4K. The PC's numbers come from Windows itself
// (GetSystemTimes; the "GPU Engine" and "GPU Process Memory" performance
// counters, as Task Manager shows them; the process's working set), with
// no driver and no admin rights: no temperatures or fans. A number Windows
// doesn't give is left out (its row hidden).
//
// Hidden while the F1 notebook is open.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite_3.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "host_settings.h"
#include "wp_stats.h"
double win_display_hz(void);
const char* wp_movie_playing(void); /* dvd.c */
int settings_ui_is_open(void);
extern int g_wp_headless;
}

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")

using Microsoft::WRL::ComPtr;
using D2D1::ColorF;
using D2D1::Point2F;
using D2D1::RectF;

namespace {

// The overlay's scale for a window win_h tall: 1 at 1350 lines (1440p 1.07,
// 1080p 0.8), at least 0.6. (The notebook's 720-line scale made it twice as
// big at 1440p: too big for something always on screen.)
double overlay_scale(int win_h) { return std::max(0.6, win_h / 1350.0); }

double now_s() {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

// ------------------------------------------------------------------------
// What the renderer tells it: each frame shown, and the window's size.

constexpr int HIST = 4096;  // frames: over 10 s at 240 fps is cut to what fits
struct Frame {
    double t, ms;
};
std::mutex gData;
Frame gFrames[HIST];
int gFrameAt, gFrameCount;
std::atomic<int> gWinW{0}, gWinH{0};

// ------------------------------------------------------------------------
// The finished picture: drawn into back, then swapped with front.

struct Picture {
    std::vector<uint32_t> px;  // straight-alpha BGRA, top row first
    int w = 0, h = 0;
    int panelX = 0, panelY = 0, panelW = 0, panelH = 0;  // the plaque inside it (corners and shadow stick out)
    int mode = 0;
};
std::mutex gPic;
Picture gFront, gBack;
std::atomic<double> gRenderMs{0};  // the render thread's time on it (gl_backend.c draw_fps), for the log
std::atomic<int> gRenderN{0};
unsigned gSerial;
std::atomic<bool> gStarted{false};

// ------------------------------------------------------------------------
// This PC's numbers (the worker, every half second).

struct PcStats {
    double cpu = -1, gpu = -1;              // percent; -1 not known
    double vramUsed = -1, vramTotal = -1;   // bytes
    double ramUsed = -1, ramTotal = -1;
    std::vector<float> cpuHist, gpuHist;    // the last 40 (20 s)
};

class PcReader {
public:
    void init() {
        // Graphics memory in all: the adapter with the most of its own.
        ComPtr<IDXGIFactory1> f;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
            ComPtr<IDXGIAdapter1> a;
            for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; i++, a.Reset()) {
                DXGI_ADAPTER_DESC1 d;
                if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                    vramTotal = std::max(vramTotal, (double)d.DedicatedVideoMemory);
            }
        }
        const char* nopdh = getenv("WONDERPARK_OVERLAY_NOPDH");  // tests: without the performance counters
        if (!(nopdh && *nopdh == '1') && PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
            wchar_t path[128];
            if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &gpuCounter) != ERROR_SUCCESS)
                gpuCounter = nullptr;
            swprintf(path, 128, L"\\GPU Process Memory(pid_%lu_*)\\Dedicated Usage", GetCurrentProcessId());
            if (PdhAddEnglishCounterW(query, path, 0, &vramCounter) != ERROR_SUCCESS) vramCounter = nullptr;
            PdhCollectQueryData(query);
        }
        MEMORYSTATUSEX ms = {sizeof(ms)};
        if (GlobalMemoryStatusEx(&ms)) ramTotal = (double)ms.ullTotalPhys;
        sys_times(&lastIdle, &lastAll);
    }

    void read(PcStats& s) {
        uint64_t idle, all;
        sys_times(&idle, &all);
        if (all > lastAll) s.cpu = 100.0 * (1.0 - (double)(idle - lastIdle) / (double)(all - lastAll));
        lastIdle = idle, lastAll = all;
        s.gpu = -1;
        s.vramUsed = -1;
        if (query && PdhCollectQueryData(query) == ERROR_SUCCESS) {
            // GPU: the 3D engines' use, summed per graphics card, the busiest card.
            std::vector<std::pair<std::wstring, double>> items;
            if (gpuCounter && array(gpuCounter, items)) {
                std::vector<std::pair<std::wstring, double>> perCard;
                for (auto& it : items) {
                    // "pid_123_luid_0x..._0x..._phys_0_eng_0_engtype_3D": the card is the luid.
                    size_t a = it.first.find(L"luid_"), b = a == std::wstring::npos ? a : it.first.find(L"_phys", a);
                    std::wstring card = a == std::wstring::npos ? L"" : it.first.substr(a, b - a);
                    auto c = std::find_if(perCard.begin(), perCard.end(), [&](auto& p) { return p.first == card; });
                    if (c == perCard.end()) perCard.push_back({card, it.second});
                    else c->second += it.second;
                }
                double best = 0;
                for (auto& c : perCard) best = std::max(best, c.second);
                s.gpu = std::min(100.0, best);
            }
            if (vramCounter && array(vramCounter, items)) {
                double sum = 0;
                for (auto& it : items) sum += it.second;
                if (sum > 0) s.vramUsed = sum;
            }
        }
        s.vramTotal = vramTotal;
        PROCESS_MEMORY_COUNTERS pmc = {sizeof(pmc)};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) s.ramUsed = (double)pmc.WorkingSetSize;
        s.ramTotal = ramTotal;
        auto add = [](std::vector<float>& h, double v) {
            h.push_back((float)std::max(0.0, v));
            if (h.size() > 40) h.erase(h.begin());
        };
        add(s.cpuHist, s.cpu);
        if (s.gpu >= 0) add(s.gpuHist, s.gpu);
    }

private:
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER gpuCounter = nullptr, vramCounter = nullptr;
    double vramTotal = -1, ramTotal = -1;
    uint64_t lastIdle = 0, lastAll = 0;

    static void sys_times(uint64_t* idle, uint64_t* all) {
        FILETIME i, k, u;
        GetSystemTimes(&i, &k, &u);
        auto v = [](FILETIME f) { return (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime; };
        *idle = v(i);
        *all = v(k) + v(u);  // kernel time includes idle
    }
    static bool array(PDH_HCOUNTER c, std::vector<std::pair<std::wstring, double>>& out) {
        out.clear();
        DWORD bytes = 0, n = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &n, nullptr) != (PDH_STATUS)PDH_MORE_DATA) return false;
        std::vector<BYTE> buf(bytes);
        auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &n, items) != ERROR_SUCCESS) return false;
        for (DWORD i = 0; i < n; i++)
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                out.push_back({items[i].szName, items[i].FmtValue.doubleValue});
        return true;
    }
};

// ------------------------------------------------------------------------
// Drawing (software Direct2D into a WIC bitmap, on the worker thread)

const uint32_t LEATHER = 0x7a5434, LEATHER2 = 0x6a4529, EDGE = 0x3b2414, CREAM = 0xfff3da, SOFT = 0xe9cfa5,
               GOLD = 0xf3cf6b, GOOD = 0x7ee08e, OK = 0xffd166, BAD = 0xff7a6b;

D2D1_COLOR_F rgb(uint32_t c, float a = 1) {
    return ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a);
}

class Painter {
public:
    bool init() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.GetAddressOf()))) return false;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                       reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
            return false;
        // The fonts built into the program (wonderpark.rc 101, 102).
        ComPtr<IDWriteInMemoryFontFileLoader> loader;
        dw->CreateInMemoryFontFileLoader(&loader);
        dw->RegisterFontFileLoader(loader.Get());
        ComPtr<IDWriteFontSetBuilder1> b;
        dw->CreateFontSetBuilder(&b);
        for (int id : {101, 102}) {
            HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
            if (!r) return false;
            HGLOBAL hg = LoadResource(nullptr, r);
            ComPtr<IDWriteFontFile> file;
            loader->CreateInMemoryFontFileReference(dw.Get(), LockResource(hg), SizeofResource(nullptr, r), nullptr, &file);
            b->AddFontFile(file.Get());
        }
        ComPtr<IDWriteFontSet> set;
        b->CreateFontSet(&set);
        if (FAILED(dw->CreateFontCollectionFromFontSet(set.Get(), &fonts))) return false;
        return true;
    }

    // Draws the plaque into p at scale s (1 = 720p at 100%).
    void paint(Picture& p, int mode, float s, float opacity, const PcStats& pc, const std::vector<Frame>& frames,
               double target) {
        sc = s;
        const float M = 3, SHX = 3, SHY = 4;  // room for the corners (they stick out 2) and the shadow
        float pw, ph;
        Detail d = compute(frames, target);
        if (mode == 1) {
            auto num = layout(std::to_wstring(d.fps), font(L"Fredoka", 700, 26), 200);
            DWRITE_TEXT_METRICS m;
            num->GetMetrics(&m);
            float nw = std::max(38.f, m.widthIncludingTrailingWhitespace / s);
            pw = 13 + 12 + 8 + nw + 8 + 30 + 16;
            ph = 46;
        } else {
            pw = 300;
            ph = detailed_height(pc);
        }
        int w = (int)std::ceil((pw + 2 * M + SHX) * s), h = (int)std::ceil((ph + 2 * M + SHY) * s);
        if (!target_for(w, h)) return;
        rt->BeginDraw();
        rt->Clear(ColorF(0, 0, 0, 0));
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(M * s, M * s));
        leather(pw, ph, mode == 2 ? 30.f : 22.f, 2.f);  // the corners lie over the dark edge
        D2D1_COLOR_F st = status(d.fps, target);
        if (mode == 1) compact(pw, ph, d, st);
        else detailed(pw, d, st, target, pc);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        if (FAILED(rt->EndDraw())) return;
        p.w = w, p.h = h, p.mode = mode;
        p.panelX = (int)std::lround(M * s), p.panelY = (int)std::lround(M * s);
        p.panelW = (int)std::lround(pw * s), p.panelH = (int)std::lround(ph * s);
        p.px.resize((size_t)w * h);
        read_out(p, opacity);
    }

private:
    ComPtr<ID2D1Factory> f;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteFactory5> dw;
    ComPtr<IDWriteFontCollection1> fonts;
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> br;
    ComPtr<ID2D1StrokeStyle> dashed, dashedFine, round;
    int bw = 0, bh = 0;
    float sc = 1;
    std::vector<std::pair<std::wstring, ComPtr<IDWriteTextFormat>>> formats;

    struct Detail {
        int fps = 0, low1 = 0, low01 = 0;
        double nowMs = 0;
        std::vector<double> graph;  // the last 2 s of frame times
    };

    static D2D1_COLOR_F status(int fps, double target) {
        return rgb(fps >= target * 0.95 ? GOOD : fps >= target * 0.8 ? OK : BAD);
    }

    static Detail compute(const std::vector<Frame>& fr, double target) {
        Detail d;
        (void)target;
        if (fr.empty()) return d;
        double tEnd = fr.back().t;
        int n1 = 0;
        double first1 = tEnd;
        std::vector<double> ten;
        for (auto& f : fr) {
            if (f.t > tEnd - 10) ten.push_back(f.ms);
            if (f.t > tEnd - 1) n1++, first1 = std::min(first1, f.t - f.ms / 1000.0);
            if (f.t > tEnd - 2) d.graph.push_back(f.ms);
        }
        double span = tEnd - first1;
        d.fps = span > 0.2 ? (int)std::lround(n1 / span) : 0;
        // The frame time now: the last half second's average.
        double sum = 0;
        int n = 0;
        for (auto it = fr.rbegin(); it != fr.rend() && it->t > tEnd - 0.5; ++it) sum += it->ms, n++;
        d.nowMs = n ? sum / n : 0;
        std::sort(ten.begin(), ten.end(), std::greater<double>());
        auto low = [&](double share) {
            size_t i = std::min(ten.size() - 1, (size_t)(ten.size() * share));
            return ten[i] > 0 ? (int)std::lround(1000.0 / ten[i]) : 0;
        };
        d.low1 = std::min(low(0.01), d.fps);
        d.low01 = std::min(low(0.001), d.low1);
        return d;
    }

    IDWriteTextFormat* font(const wchar_t* fam, int weight, float size) {
        wchar_t key[64];
        swprintf(key, 64, L"%s|%d|%g", fam, weight, size * sc);
        for (auto& e : formats)
            if (e.first == key) return e.second.Get();
        ComPtr<IDWriteTextFormat> t;
        dw->CreateTextFormat(fam, fonts.Get(), (DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             size * sc, L"en-us", &t);
        t->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        formats.push_back({key, t});
        if (formats.size() > 64) formats.erase(formats.begin());
        return t.Get();
    }
    // Layouts are made at the pixel size (crisp text) and drawn unscaled.
    ComPtr<IDWriteTextLayout> layout(const std::wstring& s, IDWriteTextFormat* tf, float w, float spacing = 0) {
        ComPtr<IDWriteTextLayout> l;
        dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, w * sc, 1000, &l);
        if (spacing != 0) {
            ComPtr<IDWriteTextLayout1> l1;
            if (SUCCEEDED(l.As(&l1))) l1->SetCharacterSpacing(0, spacing * sc, 0, {0, (UINT32)s.size()});
        }
        return l;
    }
    // Text at (x, y) in panel units; align 0 left, 1 right (x is the right edge).
    // Returns its width in panel units.
    float text(const std::wstring& s, IDWriteTextFormat* tf, float x, float y, D2D1_COLOR_F c, int align = 0,
               float spacing = 0, float* baseline = nullptr) {
        auto l = layout(s, tf, 600, spacing);
        DWRITE_TEXT_METRICS m;
        l->GetMetrics(&m);
        float w = m.widthIncludingTrailingWhitespace / sc;
        if (baseline) {
            DWRITE_LINE_METRICS lm;
            UINT32 cnt = 1;
            l->GetLineMetrics(&lm, 1, &cnt);
            y -= lm.baseline / sc;  // y given as the baseline
        }
        if (align == 1) x -= w;
        D2D1_MATRIX_3X2_F old;
        rt->GetTransform(&old);
        // Text drawn at whole pixels, unscaled (its layout is at pixel size).
        D2D1_POINT_2F px = Point2F(x * old._11 + y * old._21 + old._31, x * old._12 + y * old._22 + old._32);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        br->SetColor(c);
        rt->DrawTextLayout(Point2F(std::round(px.x), std::round(px.y)), l.Get(), br.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        rt->SetTransform(old);
        return w;
    }
    float baseline_of(IDWriteTextFormat* tf) {
        auto l = layout(L"0", tf, 100);
        DWRITE_LINE_METRICS lm;
        UINT32 cnt = 1;
        l->GetLineMetrics(&lm, 1, &cnt);
        return lm.baseline / sc;
    }

    ID2D1SolidColorBrush* brush(D2D1_COLOR_F c) {
        br->SetColor(c);
        return br.Get();
    }
    D2D1_ROUNDED_RECT rr(float x, float y, float w, float h, float r) { return D2D1::RoundedRect(RectF(x, y, x + w, y + h), r, r); }

    bool target_for(int w, int h) {
        if (w == bw && h == bh && rt) return true;
        rt.Reset(), bmp.Reset(), br.Reset();
        if (FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &bmp))) return false;
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(f->CreateWicBitmapRenderTarget(bmp.Get(), p, &rt))) return false;
        rt->CreateSolidColorBrush(ColorF(1, 1, 1), &br);
        if (!dashed) {
            const float d1[2] = {2.5f, 1.5f};  // in line widths: 5 on, 3 off at 2
            D2D1_STROKE_STYLE_PROPERTIES dp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                         D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_CUSTOM);
            f->CreateStrokeStyle(dp, d1, 2, &dashed);
            const float d2[2] = {3.3f, 3.3f};  // 4 on, 4 off at 1.2
            f->CreateStrokeStyle(dp, d2, 2, &dashedFine);
            D2D1_STROKE_STYLE_PROPERTIES rp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                         D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
            f->CreateStrokeStyle(rp, nullptr, 0, &round);
        }
        bw = w, bh = h;
        return true;
    }

    ComPtr<ID2D1PathGeometry> poly(const std::vector<D2D1_POINT_2F>& pts, bool closed) {
        ComPtr<ID2D1PathGeometry> g;
        ComPtr<ID2D1GeometrySink> k;
        f->CreatePathGeometry(&g);
        g->Open(&k);
        k->BeginFigure(pts[0], closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = 1; i < pts.size(); i++) k->AddLine(pts[i]);
        k->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        k->Close();
        return g;
    }

    // The plaque: shadow, leather with a faint diagonal grain, the dark edge,
    // a light inner line, the dashed stitch, the gold corners.
    void leather(float w, float h, float corner, float out) {
        rt->FillRoundedRectangle(rr(3, 4, w, h, 12), brush(rgb(0x140a04, .45f)));
        ComPtr<ID2D1LinearGradientBrush> lg;
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[2] = {{0, rgb(LEATHER)}, {1, rgb(LEATHER2)}};
        rt->CreateGradientStopCollection(gs, 2, &stops);
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(Point2F(0, 0), Point2F(0, h)), stops.Get(), &lg);
        rt->FillRoundedRectangle(rr(0, 0, w, h, 12), lg.Get());
        {
            // Grain and a soft light near the top left, inside the plaque only.
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            f->CreateRoundedRectangleGeometry(rr(0, 0, w, h, 12), &clip);
            ComPtr<ID2D1Layer> layer;
            rt->CreateLayer(&layer);
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
            float dx = h * std::tan(25.f * 3.14159265f / 180.f);  // lines 115 degrees from the vertical
            for (float x = -dx; x < w + dx; x += 7) rt->DrawLine(Point2F(x, 0), Point2F(x + dx, h), brush(ColorF(0, 0, 0, .035f)), 2);
            ComPtr<ID2D1RadialGradientBrush> rg;
            ComPtr<ID2D1GradientStopCollection> rs;
            D2D1_GRADIENT_STOP g2[2] = {{0, ColorF(1, 1, 1, .08f)}, {1, ColorF(1, 1, 1, 0)}};
            rt->CreateGradientStopCollection(g2, 2, &rs);
            rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(Point2F(w * .3f, h * .15f), Point2F(0, 0),
                                                                              std::max(w, h) * .55f, std::max(w, h) * .55f),
                                          rs.Get(), &rg);
            rt->FillRectangle(RectF(0, 0, w, h), rg.Get());
            rt->PopLayer();
        }
        rt->DrawRoundedRectangle(rr(4, 4, w - 8, h - 8, 9), brush(rgb(0xffecc8, .18f)), 2);   // the light line inside
        rt->DrawRoundedRectangle(rr(1.5f, 1.5f, w - 3, h - 3, 10.5f), brush(rgb(EDGE)), 3);  // the edge
        rt->DrawRoundedRectangle(rr(6, 6, w - 12, h - 12, 7), brush(rgb(0xffe2b4, .6f)), 2, dashed.Get());
        // Gold corners (30 x 30 units, drawn at corner size, out past the edge).
        float k = corner / 30;
        for (int c = 0; c < 4; c++) {
            bool right = c & 1, bottom = c & 2;
            float ox = right ? w + out : -out, oy = bottom ? h + out : -out;
            D2D1_MATRIX_3X2_F old;
            rt->GetTransform(&old);
            rt->SetTransform(D2D1::Matrix3x2F::Scale(right ? -k : k, bottom ? -k : k) * D2D1::Matrix3x2F::Translation(ox, oy) * old);
            auto p1 = poly({{2, 2}, {26, 2}, {20, 8}, {8, 8}, {8, 20}, {2, 26}}, true);
            auto p2 = poly({{5, 5}, {18, 5}, {15, 8}, {8, 8}, {8, 15}, {5, 18}}, true);
            auto p3 = poly({{8, 8}, {13, 8}, {8, 13}}, true);
            rt->FillGeometry(p1.Get(), brush(rgb(GOLD)));
            rt->DrawGeometry(p1.Get(), brush(rgb(0x7a5418)), 1.6f, round.Get());
            rt->FillGeometry(p2.Get(), brush(rgb(0xffe7a3)));
            rt->FillGeometry(p3.Get(), brush(rgb(0xc98f2a)));
            rt->SetTransform(old);
        }
    }

    void compact(float w, float h, const Detail& d, D2D1_COLOR_F st) {
        float cy = h / 2;
        rt->FillEllipse(D2D1::Ellipse(Point2F(13 + 6, cy), 6, 6), brush(st));
        rt->DrawEllipse(D2D1::Ellipse(Point2F(13 + 6, cy), 5, 5), brush(rgb(0x2a190d)), 2);
        IDWriteTextFormat* fn = font(L"Fredoka", 700, 26);
        IDWriteTextFormat* fl = font(L"Fredoka", 600, 13);
        float base = cy + 9.5f;  // the number's baseline: the digits sit in the middle
        float right = w - 16 - 30 - 8;
        // A soft dark line under the number (the mockup's text shadow, hard).
        text(std::to_wstring(d.fps), fn, right, base + 2, ColorF(0, 0, 0, .35f), 1, 0, &base);
        text(std::to_wstring(d.fps), fn, right, base, rgb(CREAM), 1, 0, &base);
        text(L"FPS", fl, right + 8, base, rgb(SOFT), 0, 13 * .08f, &base);
    }

    float detailed_height(const PcStats& pc) {
        int pcRows = 2 + (pc.gpu >= 0 ? 1 : 0) + (pc.vramUsed >= 0 ? 1 : 0);  // CPU and RAM always
        // top 14 + numbers 40 + graph 9+58+4 + two section heads 28 each + rows 21 each + 13
        return 14 + 40 + 71 + 28 + 3 * 21 + 28 + pcRows * 21 + 13 + 2;
    }

    void section(const wchar_t* name, float x, float y, float w) {
        IDWriteTextFormat* tf = font(L"Fredoka", 600, 11.5f);
        float base = y + 11;
        float tw = text(name, tf, x, base, rgb(GOLD), 0, 11.5f * .14f, &base);
        rt->DrawLine(Point2F(x + tw + 6, y + 6.5f), Point2F(x + w, y + 6.5f), brush(rgb(0xffe2b4, .3f)), 2, dashed.Get());
    }

    void bar(float x, float y, float w, double share, uint32_t color) {
        rt->FillRoundedRectangle(rr(x, y, w, 9, 4.5f), brush(rgb(0x1e1108, .5f)));
        float fw = (float)std::max(0.0, std::min(1.0, share)) * w;
        if (fw > 1) rt->FillRoundedRectangle(rr(x, y, std::max(fw, 9.f), 9, 4.5f), brush(rgb(color)));
        rt->DrawRoundedRectangle(rr(x + .75f, y + .75f, w - 1.5f, 7.5f, 3.75f), brush(rgb(0xffe2b4, .3f)), 1.5f);
    }

    void spark(float x, float y, float w, float h, const std::vector<float>& v, uint32_t color) {
        if (v.size() < 2) return;
        std::vector<D2D1_POINT_2F> pts;
        for (size_t i = 0; i < v.size(); i++)
            pts.push_back(Point2F(x + w * i / (v.size() - 1), y + h - std::min(v[i], 100.f) / 100.f * (h - 2) - 1));
        auto g = poly(pts, false);
        rt->DrawGeometry(g.Get(), brush(rgb(color)), 1.8f, round.Get());
    }

    // A row: the name, the value, then a bar or a small graph.
    void row(float x, float y, float w, const wchar_t* k, const std::wstring& v) {
        IDWriteTextFormat* fk = font(L"Nunito", 800, 13);
        IDWriteTextFormat* fv = font(L"Fredoka", 600, 14.5f);
        float base = y + 14;
        text(k, fk, x, base, rgb(SOFT), 0, 0, &base);
        text(v, fv, x + 82, base, rgb(CREAM), 0, 0, &base);
        (void)w;
    }

    void detailed(float w, const Detail& d, D2D1_COLOR_F st, double target, const PcStats& pc) {
        float x = 16, iw = w - 32, y = 14;
        // FPS, big, in the status colour; the lows on the right.
        IDWriteTextFormat* fBig = font(L"Fredoka", 700, 44);
        float base = y + 37;
        float nw = text(std::to_wstring(d.fps), fBig, x, base + 2, ColorF(0, 0, 0, .4f), 0, 0, &base);
        text(std::to_wstring(d.fps), fBig, x, base, st, 0, 0, &base);
        text(L"FPS", font(L"Fredoka", 600, 13), x + nw + 10, base - 3, rgb(SOFT), 0, 13 * .1f, &base);
        IDWriteTextFormat* fl = font(L"Nunito", 800, 11.5f);
        IDWriteTextFormat* fv = font(L"Fredoka", 600, 14);
        float rx = x + iw;
        float b1 = y + 14, b2 = y + 31;
        float vw = text(std::to_wstring(d.low1), fv, rx, b1, rgb(CREAM), 1, 0, &b1);
        text(L"1% low ", fl, rx - vw, b1, rgb(SOFT), 1, 0, &b1);
        vw = text(std::to_wstring(d.low01), fv, rx, b2, rgb(CREAM), 1, 0, &b2);
        text(L"0.1% low ", fl, rx - vw, b2, rgb(SOFT), 1, 0, &b2);
        y += 40 + 9;
        // The frame time graph: the last 2 s, one point per frame shown;
        // spikes stay inside it, below its labels.
        {
            const float H = 58, top = 18;
            rt->FillRoundedRectangle(rr(x, y, iw, H, 7), brush(rgb(0x1e1108, .45f)));
            double maxFt = std::max(50.0, 3000.0 / std::max(1.0, target));
            auto yOf = [&](double ms) { return (float)(y + H - 3 - std::min(ms, maxFt) / maxFt * (H - top - 5)); };
            float yt = yOf(1000.0 / std::max(1.0, target));
            rt->DrawLine(Point2F(x + 2, yt), Point2F(x + iw - 2, yt), brush(rgb(GOOD, .55f)), 1.2f, dashedFine.Get());
            if (d.graph.size() >= 2) {
                std::vector<D2D1_POINT_2F> pts;
                size_t n = d.graph.size();
                for (size_t i = 0; i < n; i++) pts.push_back(Point2F(x + 1 + (iw - 2) * i / (n - 1), yOf(d.graph[i])));
                auto g = poly(pts, false);
                rt->DrawGeometry(g.Get(), brush(rgb(0xffe7a3)), 2, round.Get());
            }
            rt->DrawRoundedRectangle(rr(x + .75f, y + .75f, iw - 1.5f, H - 1.5f, 6.25f), brush(rgb(0xffe2b4, .25f)), 1.5f);
            IDWriteTextFormat* fs = font(L"Nunito", 900, 10);
            float lb = y + 13;
            text(L"FRAME TIME", fs, x + 7, lb, rgb(SOFT), 0, 10 * .06f, &lb);
            wchar_t ms[32];
            swprintf(ms, 32, L"%.1f ms", d.nowMs);
            text(ms, font(L"Nunito", 900, 10.5f), x + iw - 7, lb, rgb(CREAM), 1, 0, &lb);
            y += H + 4;
        }
        // Wonderpark's own timings, per game frame, against the frame's budget.
        const WpStats& s = g_wp_stats;
        double frames = s.frames > 0 ? s.frames : 1, budget = 1000.0 / std::max(1.0, target);
        double game = s.cpu_ms / frames, gfx = s.gpu_ms / frames, gpu = s.gpu_present_ms;
        float barX = x + 82 + 66 + 8, barW = x + iw - barX;
        y += 10;
        section(L"WONDERPARK", x, y, iw);
        y += 18;
        auto msRow = [&](const wchar_t* k, double v, uint32_t c) {
            wchar_t t[32];
            if (v >= 0) swprintf(t, 32, L"%.1f ms", v);
            else wcscpy(t, L"–");
            row(x, y, iw, k, t);
            bar(barX, y + 6, barW, v >= 0 ? v / budget : 0, c);
            y += 21;
        };
        msRow(L"Game", game, 0xf7b45c);
        msRow(L"Graphics", gfx, 0xb596f0);
        msRow(L"GPU", gpu, 0x7fd3e0);
        y += 10;
        section(L"THIS PC", x, y, iw);
        y += 18;
        auto pct = [](double v) {
            wchar_t t[16];
            swprintf(t, 16, L"%.0f%%", v);
            return std::wstring(t);
        };
        auto gb = [](double v) {
            wchar_t t[16];
            swprintf(t, 16, L"%.1f GB", v / (1024.0 * 1024 * 1024));
            return std::wstring(t);
        };
        row(x, y, iw, L"CPU", pc.cpu >= 0 ? pct(pc.cpu) : L"–");
        spark(barX, y + 1, barW, 18, pc.cpuHist, 0xf7b45c);
        y += 21;
        if (pc.gpu >= 0) {
            row(x, y, iw, L"GPU", pct(pc.gpu));
            spark(barX, y + 1, barW, 18, pc.gpuHist, 0x7fd3e0);
            y += 21;
        }
        if (pc.vramUsed >= 0) {
            row(x, y, iw, L"VRAM", gb(pc.vramUsed));
            bar(barX, y + 6, barW, pc.vramTotal > 0 ? pc.vramUsed / pc.vramTotal : 0, 0xb596f0);
            y += 21;
        }
        row(x, y, iw, L"RAM", pc.ramUsed >= 0 ? gb(pc.ramUsed) : L"–");
        bar(barX, y + 6, barW, pc.ramTotal > 0 ? pc.ramUsed / pc.ramTotal : 0, GOOD);
    }

    // Premultiplied out of WIC into straight alpha, times the Opacity setting.
    void read_out(Picture& p, float opacity) {
        WICRect wr = {0, 0, p.w, p.h};
        ComPtr<IWICBitmapLock> lock;
        if (FAILED(bmp->Lock(&wr, WICBitmapLockRead, &lock))) return;
        UINT size = 0, stride = 0;
        BYTE* px = nullptr;
        lock->GetDataPointer(&size, &px);
        lock->GetStride(&stride);
        // Kept premultiplied (the renderer blends it that way); the Opacity
        // setting scales all four channels.
        unsigned op = (unsigned)std::lround(opacity * 256);
        for (int y = 0; y < p.h; y++) {
            const uint32_t* src = (const uint32_t*)(px + (size_t)y * stride);
            uint32_t* dst = &p.px[(size_t)y * p.w];
            if (op >= 256) {
                std::memcpy(dst, src, (size_t)p.w * 4);
                continue;
            }
            for (int x = 0; x < p.w; x++) {
                uint32_t c = src[x];
                dst[x] = ((c >> 24) * op >> 8) << 24 | ((c >> 16 & 255) * op >> 8) << 16 | ((c >> 8 & 255) * op >> 8) << 8 |
                         ((c & 255) * op >> 8);
            }
        }
    }
};

double target_fps() {
    // Movies always play at 30 (hle_frame.c): that's on target too.
    if (wp_movie_playing()) return 30;
    double hz = win_display_hz(), t = g_settings.frame_rate > 0 ? g_settings.frame_rate : 30;
    if (hz >= 30 && hz + 0.5 < t) t = std::floor(hz + 0.5);
    return t;
}

void worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Painter painter;
    if (!painter.init()) {
        std::fprintf(stderr, "overlay: no Direct2D; the performance overlay is off\n");
        return;
    }
    PcReader reader;
    reader.init();
    PcStats pc;
    double nextPc = 0;
    std::vector<Frame> frames;
    // Below the game's threads: it never takes a core from them.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const char* lg = getenv("WONDERPARK_LOG");
    bool log = lg && std::strstr(lg, "overlay");
    double logAt = now_s() + 5, paintMs = 0, pcMs = 0;
    int paints = 0, pcs = 0;
    for (;;) {
        int mode = g_x[X_SHOW_FPS], wh = gWinH.load();
        if (!mode || wh <= 0 || settings_ui_is_open()) {
            Sleep(100);
            continue;
        }
        double t = now_s();
        if (mode == 2 && t >= nextPc) {
            reader.read(pc);
            nextPc = t + 0.5;
            pcMs += (now_s() - t) * 1000, pcs++;
        }
        double tp = now_s();
        {
            std::lock_guard<std::mutex> lk(gData);
            frames.clear();
            int n = gFrameCount, at = gFrameAt;
            for (int i = n; i > 0; i--) frames.push_back(gFrames[(at - i + HIST) % HIST]);
        }
        // Smaller than the notebook's scale: 1 at 1350 lines (1440p: 1.07), at least 0.6.
        float s = overlay_scale(wh) * g_x[X_OVERLAY_SIZE] / 100.f;
        painter.paint(gBack, mode, s, g_x[X_OVERLAY_OPACITY] / 100.f, pc, frames, target_fps());
        {
            std::lock_guard<std::mutex> lk(gPic);
            std::swap(gFront, gBack);
            gSerial++;
        }
        paintMs += (now_s() - tp) * 1000, paints++;
        if (log && now_s() >= logAt) {
            // WONDERPARK_LOG=overlay: what it costs, every 5 s.
            double r = gRenderMs.exchange(0);
            int rn = gRenderN.exchange(0);
            std::fprintf(stderr, "[overlay] 5 s: %d pictures, %.2f ms each; PC numbers %.2f ms each; render thread %.3f ms a frame\n",
                         paints, paints ? paintMs / paints : 0, pcs ? pcMs / pcs : 0, rn ? r / rn : 0);
            paintMs = pcMs = 0, paints = pcs = 0, logAt += 5;
        }
        double wait = (mode == 2 ? 0.125 : 0.25) - (now_s() - t);
        if (wait > 0) Sleep((DWORD)(wait * 1000));
    }
}

}  // namespace

// Each frame shown (gx_frames.c report): its time since the one before.
extern "C" void perf_overlay_frame(double ms) {
    if (!g_x[X_SHOW_FPS]) return;
    std::lock_guard<std::mutex> lk(gData);
    gFrames[gFrameAt] = {now_s(), ms};
    gFrameAt = (gFrameAt + 1) % HIST;
    if (gFrameCount < HIST) gFrameCount++;
}

// The newest picture for a window w x h, locked until perf_overlay_unlock:
// *x, *y its top-left corner in the window (from the top), NULL if nothing
// is to be shown. *serial changes when the picture does.
extern "C" const uint32_t* perf_overlay_lock(int win_w, int win_h, int* bw, int* bh, int* x, int* y, unsigned* serial) {
    if (!g_x[X_SHOW_FPS] || win_w <= 0 || win_h <= 0 || g_wp_headless) return nullptr;
    gWinW = win_w, gWinH = win_h;
    if (!gStarted.exchange(true)) std::thread(worker).detach();
    if (settings_ui_is_open()) return nullptr;
    gPic.lock();
    const Picture& p = gFront;
    if (p.px.empty() || p.mode != g_x[X_SHOW_FPS] || p.w > win_w || p.h > win_h) {
        gPic.unlock();
        return nullptr;
    }
    // The plaque 14 units from the edges (at the overlay's scale); its corners and shadow
    // stick out into the margin.
    int m = (int)std::lround(14 * overlay_scale(win_h)), c = g_x[X_OVERLAY_CORNER];
    *x = c & 1 ? win_w - m - p.panelX - p.panelW : m - p.panelX;
    *y = c & 2 ? win_h - m - p.panelY - p.panelH : m - p.panelY;
    *x = std::max(0, std::min(*x, win_w - p.w));
    *y = std::max(0, std::min(*y, win_h - p.h));
    *bw = p.w, *bh = p.h;
    *serial = gSerial;
    return p.px.data();
}

extern "C" void perf_overlay_unlock(void) { gPic.unlock(); }

extern "C" void perf_overlay_render_time(double ms) {
    double v = gRenderMs.load();
    while (!gRenderMs.compare_exchange_weak(v, v + ms)) {}
    gRenderN++;
}

// F7 (the overlay's key) during play: Off, Compact, Detailed, Off.
extern "C" void perf_overlay_cycle(void) { settings_set_x(X_SHOW_FPS, (g_x[X_SHOW_FPS] + 1) % 3); }
