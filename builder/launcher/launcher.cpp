// Wonderpark launcher: the one-time builder's window (docs/launcher-mockup.html).
//
// A native Win32 window drawn with Direct2D and DirectWrite (both part of
// Windows: no runtime to install, GPU-drawn, a small standalone .exe).
// Everything is drawn in code; the only files built in are the Fredoka and
// Nunito fonts (SIL OFL). The scene is laid out at 1280x720 and scaled to
// fit the window.
//
// Flow, with the one main button: Place ISO -> Verifying... -> Build ->
// percent while building -> Play. The build is builder/pipeline.py, run by
// Python on a worker thread (the window never waits on it or on the disk);
// its output goes to build/logs/build-<time>.log. Each finished stage
// relights one star point (top, right, bottom, left). Esc or closing the
// window during a build offers to stop it. On later runs the button goes
// straight to Play when the build is current, or Rebuild when it's out of
// date (pipeline.py --status).
//
// Command line (development and tests):
//   --root <dir>          the Wonderpark folder (else found above the exe, or WONDERPARK_ROOT)
//   --test-iso <file>     use this file instead of the file picker
//   --test-script <list>  timed actions, "<seconds>:<action>,...": logo, press, drop (the --test-iso), cancel,
//                         shot=<file.png>, quit (seconds from start; "+N" after the previous)
//   --reduced-motion      as Windows' "Animation effects" off
// WONDERPARK_PYTHON picks the Python to run the builder with.

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <d2d1_3.h>
#include <d2d1_3helper.h>
#include <d3d11.h>
#include <dwrite_3.h>
#include <dxgi1_2.h>
#include <wincodec.h>

#include "mods.h"
#include "saves.h"
#include "../../runtime/host/pad_icons.h"  // controller icons (pad_icons_core.cpp, shared with the game)
#include "../../runtime/host/pad_sdl.h"    // controllers through SDL3 (pad_sdl.c, shared with the game)

#include <algorithm>
#include <atomic>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using D2D1::ColorF;
using D2D1::Matrix3x2F;
using D2D1::Point2F;
using D2D1::RectF;

// ------------------------------------------------------------------------
// Small helpers

static const float W = 1280, H = 720;  // the scene's own size
static const float PI = 3.14159265358979f;

static D2D1_COLOR_F rgb(uint32_t c, float a = 1) {
    return ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a);
}
static D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

// CSS timing functions: cubic-bezier(x1, y1, x2, y2), solved for x.
static float bezier(float x1, float y1, float x2, float y2, float x) {
    x = clamp01(x);
    float t = x;
    for (int i = 0; i < 8; i++) {
        float cx = 3 * x1 * t * (1 - t) * (1 - t) + 3 * x2 * t * t * (1 - t) + t * t * t - x;
        float dx = 3 * x1 * (1 - t) * (1 - t) + 6 * (x2 - x1) * t * (1 - t) + 3 * (1 - x2) * t * t;
        if (std::fabs(cx) < 1e-5f) break;
        if (std::fabs(dx) < 1e-6f) break;
        t = clamp01(t - cx / dx);
    }
    return 3 * y1 * t * (1 - t) * (1 - t) + 3 * y2 * t * t * (1 - t) + t * t * t;
}
static float ease_in_out(float x) { return bezier(.42f, 0, .58f, 1, x); }
static float ease_in(float x) { return bezier(.42f, 0, 1, 1, x); }
static float ease_out(float x) { return bezier(0, 0, .58f, 1, x); }
static float ease_pop(float x) { return bezier(.2f, .8f, .2f, 1, x); }  // the mockup's cubic-bezier(.2,.8,.2,1)
// A 0 -> 1 -> 0 wave over period p, eased in and out each half (CSS "0%,100% {a} 50% {b}").
static float wave(double t, float p) {
    float ph = (float)std::fmod(t, (double)p) / p;
    if (ph < 0) ph += 1;
    return ph < .5f ? ease_in_out(ph * 2) : ease_in_out((1 - ph) * 2);
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring file_name(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}
static bool file_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static double now_s() {
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER t;
    if (!f.QuadPart) {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
    }
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
}

// ------------------------------------------------------------------------
// State shared with the worker thread

enum State { S_CHECKING, S_NOISO, S_VERIFYING, S_READY, S_BUILDING, S_DONE };

struct Shared {
    std::mutex m;
    State state = S_CHECKING;
    std::wstring image;         // the disc image chosen (or built from last)
    std::wstring error;         // the red line under the status, "" for none
    bool error_has_log = false; // ... with "Show log" after it
    std::wstring warn;          // a cream line ("Build stopped.", "Wonderpark was updated...")
    bool rebuild = false;       // READY means Rebuild (from game/, no image needed)
    int stage = 0;              // 1-4 while building
    int lit = 0;                // star points relit
    float progress = 0;         // 0-100
    std::wstring stage_name;
    std::wstring exe;           // build/game/Wonderpark.exe when built
    std::wstring log_path;
    bool built_now = false;     // the build finished in this session (confetti)
};
static Shared g;
static HWND g_wnd;
static std::wstring g_root, g_python, g_python_args;
static HANDLE g_job;             // the build's processes, ended together
static std::atomic<bool> g_cancel{false};
static std::thread g_worker;

static void wake() {
    if (g_wnd) PostMessageW(g_wnd, WM_APP, 0, 0);
}

static const wchar_t* STAGE_NAMES[4] = {L"Verifying disc", L"Extracting main.dol and files",
                                        L"Recompiling PowerPC to C++", L"Compiling Wonderpark.exe"};

// ------------------------------------------------------------------------
// Finding things

static std::wstring find_root(const wchar_t* given) {
    auto ok = [](const std::wstring& d) { return file_exists(d + L"\\builder\\pipeline.py"); };
    if (given && *given) return given;
    wchar_t buf[MAX_PATH * 2];
    if (GetEnvironmentVariableW(L"WONDERPARK_ROOT", buf, MAX_PATH * 2) && ok(buf)) return buf;
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring d = buf;
    for (int i = 0; i < 6; i++) {
        size_t p = d.find_last_of(L"\\/");
        if (p == std::wstring::npos) break;
        d = d.substr(0, p);
        if (ok(d)) return d;
    }
    return L"";
}

// A real python.exe (not the Microsoft Store's placeholder), or the py launcher.
static void find_python() {
    wchar_t buf[MAX_PATH * 2];
    if (GetEnvironmentVariableW(L"WONDERPARK_PYTHON", buf, MAX_PATH * 2) && file_exists(buf)) {
        g_python = buf;
        return;
    }
    wchar_t path[32768];
    if (!GetEnvironmentVariableW(L"PATH", path, 32768)) path[0] = 0;
    std::wstring p = path;
    size_t s = 0;
    while (s <= p.size()) {
        size_t e = p.find(L';', s);
        if (e == std::wstring::npos) e = p.size();
        std::wstring dir = p.substr(s, e - s);
        s = e + 1;
        if (dir.empty()) continue;
        std::wstring low = dir;
        for (auto& c : low) c = towlower(c);
        if (low.find(L"windowsapps") != std::wstring::npos) continue;
        if (dir.back() != L'\\') dir += L'\\';
        if (file_exists(dir + L"python.exe")) {
            g_python = dir + L"python.exe";
            return;
        }
    }
    if (SearchPathW(nullptr, L"py.exe", nullptr, MAX_PATH * 2, buf, nullptr)) {
        g_python = buf;
        g_python_args = L"-3 ";
    }
}

// ------------------------------------------------------------------------
// Running the builder (worker thread)

// Runs python <args> in the Wonderpark folder; each output line goes to
// on_line (and the log file, if given). Returns the exit code, or -1.
template <class F>
static int run_python(const std::wstring& args, FILE* log, F on_line) {
    if (g_python.empty()) return -1;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + g_python + L"\" " + g_python_args + L"-u " + args;
    // UTF-8 output; the environment block is this process's plus these.
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr,
                             g_root.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::string pending;
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) {
        pending.append(buf, n);
        size_t e;
        while ((e = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, e);
            pending.erase(0, e + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (log) {
                fputs(line.c_str(), log);
                fputc('\n', log);
            }
            on_line(line);
        }
    }
    if (!pending.empty()) {
        if (log) fputs(pending.c_str(), log);
        on_line(pending);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static void join_worker() {
    if (g_worker.joinable()) g_worker.join();
}

// At start: is there a current build?
static void check_status() {
    join_worker();
    g_worker = std::thread([] {
        std::string status;
        std::wstring image;
        int rc = run_python(L"builder\\pipeline.py --status", nullptr, [&](const std::string& l) {
            if (l.rfind("@status ", 0) == 0) status = l.substr(8);
            if (l.rfind("@image ", 0) == 0) image = widen(l.substr(7));
        });
        std::lock_guard<std::mutex> lk(g.m);
        g.image = image;
        if (g_python.empty() || rc < 0) {
            g.state = S_NOISO;
            g.error = L"Python 3 wasn't found. The README says what to install.";
        } else if (status == "ok") {
            g.state = S_DONE;
            g.lit = 4;
            g.progress = 100;
            g.exe = g_root + L"\\build\\game\\Wonderpark.exe";
        } else if (status == "outdated") {
            g.state = S_READY;
            g.rebuild = true;
            g.warn = L"Wonderpark has been updated. Rebuild to play.";
        } else {
            g.state = S_NOISO;
        }
        wake();
    });
}

// The disc header's game ID and revision (as tools/extract_disc.py reads it).
static std::wstring verify_disc(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"That file couldn't be opened.";
    unsigned char h[0x220] = {};
    DWORD n = 0;
    ReadFile(f, h, sizeof(h), &n, nullptr);
    CloseHandle(f);
    if (n < 0x20) return L"That file isn't a Wii disc image.";
    const unsigned char* id = h;
    int rev = h[7];
    if (!memcmp(h, "WBFS", 4)) id = h + 0x200, rev = h[0x207];
    else if (!memcmp(h, "RVZ", 3) || !memcmp(h, "WIA", 3)) id = h + 0x58, rev = h[0x5F];
    char sid[7] = {};
    for (int i = 0; i < 6; i++) {
        if (id[i] < 0x20 || id[i] > 0x7E) return L"That file isn't a Wii disc image.";
        sid[i] = (char)id[i];
    }
    std::wstring wid = widen(sid);
    if (wid == L"S2LJ01") return L"That's the Japanese PokéPark 2 disc. Wonderpark needs the US one (S2LE01).";
    if (wid == L"S2LP01") return L"That's the European PokéPark 2 disc. Wonderpark needs the US one (S2LE01).";
    if (wid != L"S2LE01") return L"That disc is " + wid + L", not S2LE01. Use the US PokéPark 2 disc.";
    if (rev != 0)
        return L"This disc revision (" + std::to_wstring(rev) + L") hasn't been tested with Wonderpark yet.";
    return L"";
}

// Whether the image is the Japanese disc (S2LJ01): its voices are copied for
// the Voice language setting instead (tools/extract_voices.py).
static bool japanese_disc(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    unsigned char h[0x220] = {};
    DWORD n = 0;
    ReadFile(f, h, sizeof(h), &n, nullptr);
    CloseHandle(f);
    if (n < 0x220) return false;
    const unsigned char* id = h;
    if (!memcmp(h, "WBFS", 4)) id = h + 0x200;
    else if (!memcmp(h, "RVZ", 3) || !memcmp(h, "WIA", 3)) id = h + 0x58;
    return !memcmp(id, "S2LJ01", 6);
}

static void start_verify(const std::wstring& path) {
    join_worker();
    State prevState;
    std::wstring prevImage;
    bool prevRebuild;
    {
        std::lock_guard<std::mutex> lk(g.m);
        prevState = g.state, prevImage = g.image, prevRebuild = g.rebuild;
        g.state = S_VERIFYING;
        g.image = path;
        g.error.clear();
        g.warn.clear();
        g.rebuild = false;
    }
    g_worker = std::thread([path, prevState, prevImage, prevRebuild] {
        if (japanese_disc(path)) {
            // The Japanese disc: its Pokémon voices copied for Audio > Voice
            // language (only the voice banks are kept; a few minutes, as the
            // disc is read whole). The launcher stays as it was meanwhile.
            {
                std::lock_guard<std::mutex> lk(g.m);
                g.state = prevState, g.image = prevImage, g.rebuild = prevRebuild;
                g.warn = L"Copying the Pokémon voices from your Japanese disc…";
                wake();
            }
            std::wstring logdir = g_root + L"\\build\\logs";
            SHCreateDirectoryExW(nullptr, logdir.c_str(), nullptr);
            std::wstring log_path = logdir + L"\\voices-ja.log";
            FILE* log = _wfopen(log_path.c_str(), L"wb");
            int rc = run_python(L"tools\\extract_voices.py \"" + path + L"\"", log, [](const std::string&) {});
            if (log) fclose(log);
            std::lock_guard<std::mutex> lk(g.m);
            if (rc == 0) {
                g.warn = L"Japanese voices ready: choose Japanese in Settings (F1) › Audio › Voice language.";
            } else {
                g.warn.clear();
                g.error = L"The Japanese voices couldn't be copied (build\\logs\\voices-ja.log says why).";
                g.log_path = log_path;
                g.error_has_log = true;
            }
            wake();
            return;
        }
        double t0 = now_s();
        std::wstring err = verify_disc(path);
        double left = 1.0 - (now_s() - t0);  // long enough to read "Verifying..."
        if (left > 0) Sleep((DWORD)(left * 1000));
        std::lock_guard<std::mutex> lk(g.m);
        g.state = err.empty() ? S_READY : S_NOISO;
        g.error = err;
        g.error_has_log = false;
        wake();
    });
}

static void start_build() {
    join_worker();
    std::wstring args, image;
    bool rebuild;
    {
        std::lock_guard<std::mutex> lk(g.m);
        rebuild = g.rebuild;
        image = g.image;
        g.state = S_BUILDING;
        g.stage = 1;
        g.lit = 0;
        g.progress = 0;
        g.stage_name = STAGE_NAMES[0];
        g.error.clear();
        g.warn.clear();
        g.error_has_log = false;
    }
    g_cancel = false;
    args = rebuild ? L"builder\\pipeline.py --rebuild" : L"builder\\pipeline.py \"" + image + L"\"";
    g_worker = std::thread([args, rebuild] {
        std::wstring logdir = g_root + L"\\build\\logs";
        SHCreateDirectoryExW(nullptr, logdir.c_str(), nullptr);
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t name[64];
        swprintf(name, 64, L"\\build-%04d%02d%02d-%02d%02d%02d.log", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
        std::wstring log_path = logdir + name;
        FILE* log = _wfopen(log_path.c_str(), L"wb");
        {
            std::lock_guard<std::mutex> lk(g.m);
            g.log_path = log_path;
        }
        std::wstring done, error;
        int rc = run_python(args, log, [&](const std::string& l) {
            std::lock_guard<std::mutex> lk(g.m);
            if (l.rfind("@stage ", 0) == 0) {
                int s = atoi(l.c_str() + 7);
                if (s >= 1 && s <= 4) {
                    g.stage = s;
                    g.lit = s - 1;
                    g.stage_name = STAGE_NAMES[s - 1];
                }
            } else if (l.rfind("@progress ", 0) == 0) {
                g.progress = (float)atof(l.c_str() + 10);
            } else if (l.rfind("@done ", 0) == 0) {
                done = widen(l.substr(6));
            } else if (l.rfind("@error ", 0) == 0) {
                error = widen(l.substr(7));
            } else {
                return;
            }
            wake();
        });
        if (log) fclose(log);
        std::lock_guard<std::mutex> lk(g.m);
        if (g_cancel) {
            g.state = S_READY;
            g.warn = L"Build stopped. Press Build to start again.";
            g.lit = 0;
        } else if (rc == 0 && !done.empty()) {
            g.state = S_DONE;
            g.lit = 4;
            g.progress = 100;
            g.exe = done;
            g.built_now = true;
            g.rebuild = false;
        } else {
            g.state = S_READY;
            g.lit = 0;
            if (rc < 0 && error.empty()) error = L"The builder couldn't start (Python 3 is needed).";
            g.error = error.empty() ? L"The build failed." : error;
            g.error_has_log = true;
        }
        wake();
    });
}

static void cancel_build() {
    g_cancel = true;
    if (g_job) TerminateJobObject(g_job, 1);
}

// ------------------------------------------------------------------------
// Graphics

struct Gfx {
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Factory3> f;
    ComPtr<ID2D1Device2> dev;
    ComPtr<ID2D1DeviceContext2> dc;
    ComPtr<ID2D1Bitmap1> back;
    ComPtr<IDWriteFactory5> dw;
    ComPtr<IDWriteFontCollection1> fonts;
    ComPtr<IDWriteFontFace> fredoka7, nunito8;
    ComPtr<IDWriteTextFormat> tfStatus, tfProgress, tfLegal, tfHint, tfButton, tfToast;
    // The corner hint and the Mods page.
    ComPtr<IDWriteTextFormat> tfPill, tfTab, tfBadge, tfMTitle, tfMSub, tfDrop, tfDropSmall, tfMProg, tfMLabel,
        tfModName, tfModSum, tfMFoot, tfMErr, tfTip;
    // Extras: the page tabs, small buttons, the "Coming soon" stamp.
    ComPtr<IDWriteTextFormat> tfSeg, tfBtn, tfStamp;
    ComPtr<ID2D1PathGeometry> puzzle, upload, card;  // card: 422 x 61, uneven corners
    ComPtr<ID2D1StrokeStyle> dash;                    // the drop zone's dashed border
    // Built once (scene units)
    ComPtr<ID2D1PathGeometry> star, gems[8], wedges[12], ring, rays[20];
    ComPtr<ID2D1GeometryGroup> starClip;
    ComPtr<ID2D1Geometry> wonderT, parkT, recompT, wonderparkM, unofficialM;
    ComPtr<ID2D1PathGeometry> sparkle4, ringBack, ringFront, bandA, bandB, hintStar, shootLine, checkMark;
    ComPtr<ID2D1StrokeStyle> round;
    // Rendered once per size (pixels)
    ComPtr<ID2D1Bitmap1> sky, twA, twB, beam, glow8, glow18;
    float k = 1, ox = 0, oy = 0;  // scene -> pixels
    UINT pw = 0, ph = 0;
    // Reused every frame (made once): one solid brush whose colour is set
    // per draw, gradients by name, text layouts by text and box.
    ComPtr<ID2D1SolidColorBrush> solid, gold, goldHi, link;
    std::map<std::string, ComPtr<ID2D1Brush>> grads;
    std::map<std::wstring, ComPtr<IDWriteTextLayout>> layouts;
    ComPtr<ID2D1PathGeometry> titleSpark;
    // Shapes flattened once per window size (fills, and strokes by width).
    std::map<std::pair<void*, float>, ComPtr<ID2D1GeometryRealization>> real;
};
static Gfx G;

static Matrix3x2F root_m() { return Matrix3x2F::Scale(G.k, G.k) * Matrix3x2F::Translation(G.ox, G.oy); }

// A controller icon (pad_icons_core.cpp), h scene units high, its top-left
// at (x, y): made at the window's pixel size, once per icon and size.
// Returns its width in scene units (0 if not read yet); draws it if draw.
static bool g_padOn;  // a pad is connected (poll_controller)
static float pad_icon(const char* name, float x, float y, float h, float op, bool draw) {
    static std::map<std::string, ComPtr<ID2D1Bitmap>> cache;
    static void* cacheFor;
    if (!name || !G.dc) return 0;
    if (cacheFor != G.dc.Get()) cache.clear(), cacheFor = G.dc.Get();
    int ph = std::max(1, (int)std::lround(h * G.k));
    int pw = pad_icons_width(name, ph);
    if (pw <= 0) return 0;
    if (!draw) return pw / G.k;
    auto& b = cache[std::string(name) + "|" + std::to_string(ph)];
    if (!b) {
        std::vector<uint8_t> rgba((size_t)pw * ph * 4);
        if (!pad_icons_rgba(name, ph, rgba.data(), pw)) return 0;
        std::vector<uint32_t> px((size_t)pw * ph);
        for (size_t i = 0; i < px.size(); i++) {
            uint32_t r = rgba[i * 4], g = rgba[i * 4 + 1], bl = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
            px[i] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (bl * a / 255);
        }
        D2D1_BITMAP_PROPERTIES p = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(G.dc->CreateBitmap(D2D1::SizeU(pw, ph), px.data(), pw * 4, p, &b))) return 0;
    }
    G.dc->DrawBitmap(b.Get(), RectF(x, y, x + pw / G.k, y + ph / G.k), op);
    return pw / G.k;
}

// The shared solid brush, set to colour c: use it in the very next draw call
// (two at once need their own brushes).
static ComPtr<ID2D1SolidColorBrush> brush(D2D1_COLOR_F c) {
    if (!G.solid) G.dc->CreateSolidColorBrush(c, &G.solid);
    else G.solid->SetColor(c);
    return G.solid;
}
// A brush made once and kept under name (gradients; set opacity or a
// transform on it per use).
template <class F>
static ID2D1Brush* cached(const std::string& name, F make) {
    auto it = G.grads.find(name);
    if (it != G.grads.end()) return it->second.Get();
    ComPtr<ID2D1Brush> b;
    make().As(&b);
    G.grads[name] = b;
    return b.Get();
}
static ComPtr<ID2D1GradientStopCollection> stops(std::initializer_list<D2D1_GRADIENT_STOP> s) {
    ComPtr<ID2D1GradientStopCollection> c;
    G.dc->CreateGradientStopCollection(s.begin(), (UINT32)s.size(), &c);
    return c;
}
static ComPtr<ID2D1RadialGradientBrush> radial(D2D1_POINT_2F c, float rx, float ry,
                                               std::initializer_list<D2D1_GRADIENT_STOP> s) {
    ComPtr<ID2D1RadialGradientBrush> b;
    G.dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, Point2F(), rx, ry), stops(s).Get(), &b);
    return b;
}
static ComPtr<ID2D1LinearGradientBrush> linear(D2D1_POINT_2F a, D2D1_POINT_2F b,
                                               std::initializer_list<D2D1_GRADIENT_STOP> s) {
    ComPtr<ID2D1LinearGradientBrush> l;
    G.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), stops(s).Get(), &l);
    return l;
}

// A path from points (closed polygon).
static ComPtr<ID2D1PathGeometry> polygon(std::initializer_list<D2D1_POINT_2F> pts) {
    ComPtr<ID2D1PathGeometry> p;
    ComPtr<ID2D1GeometrySink> s;
    G.f->CreatePathGeometry(&p);
    p->Open(&s);
    auto it = pts.begin();
    s->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
    for (++it; it != pts.end(); ++it) s->AddLine(*it);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    return p;
}

// The 4-point sparkle of the mockup ("M10 0L12.5 7.5L20 10..." in a 20 box).
static ComPtr<ID2D1PathGeometry> sparkle_path() {
    return polygon({{10, 0}, {12.5f, 7.5f}, {20, 10}, {12.5f, 12.5f}, {10, 20}, {7.5f, 12.5f}, {0, 10}, {7.5f, 7.5f}});
}

// ---- fonts

static ComPtr<IDWriteInMemoryFontFileLoader> g_memLoader;

static bool load_fonts() {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                   reinterpret_cast<IUnknown**>(G.dw.GetAddressOf()))))
        return false;
    G.dw->CreateInMemoryFontFileLoader(&g_memLoader);
    G.dw->RegisterFontFileLoader(g_memLoader.Get());
    ComPtr<IDWriteFontSetBuilder1> b;
    G.dw->CreateFontSetBuilder(&b);
    ComPtr<IDWriteFontFile> files[2];
    int fi = 0;
    for (int id : {101, 102}) {
        HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
        if (!r) return false;
        HGLOBAL h = LoadResource(nullptr, r);
        ComPtr<IDWriteFontFile> file;
        g_memLoader->CreateInMemoryFontFileReference(G.dw.Get(), LockResource(h), SizeofResource(nullptr, r),
                                                     nullptr, &file);
        b->AddFontFile(file.Get());
        files[fi++] = file;
    }
    // Outlines for the lettering: the variable fonts at a set weight (a face
    // from the collection gives the default instance's outlines).
    ComPtr<IDWriteFactory6> f6;
    if (SUCCEEDED(G.dw.As(&f6))) {
        auto weighted = [&](IDWriteFontFile* file, float wght, ComPtr<IDWriteFontFace>& out) {
            ComPtr<IDWriteFontResource> res;
            ComPtr<IDWriteFontFace5> face;
            DWRITE_FONT_AXIS_VALUE ax = {DWRITE_FONT_AXIS_TAG_WEIGHT, wght};
            if (SUCCEEDED(f6->CreateFontResource(file, 0, &res)) &&
                SUCCEEDED(res->CreateFontFace(DWRITE_FONT_SIMULATIONS_NONE, &ax, 1, &face)))
                face.As(&out);
        };
        weighted(files[0].Get(), 700, G.fredoka7);
        weighted(files[1].Get(), 800, G.nunito8);
    }
    ComPtr<IDWriteFontSet> set;
    b->CreateFontSet(&set);
    G.dw->CreateFontCollectionFromFontSet(set.Get(), &G.fonts);
    auto face = [](const wchar_t* fam, DWRITE_FONT_WEIGHT wt, ComPtr<IDWriteFontFace>& out) {
        UINT32 i;
        BOOL exists;
        G.fonts->FindFamilyName(fam, &i, &exists);
        if (!exists) return;
        ComPtr<IDWriteFontFamily> f;
        G.fonts->GetFontFamily(i, &f);
        ComPtr<IDWriteFont> font;
        f->GetFirstMatchingFont(wt, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font);
        font->CreateFontFace(&out);
    };
    if (!G.fredoka7) face(L"Fredoka", DWRITE_FONT_WEIGHT_BOLD, G.fredoka7);
    if (!G.nunito8) face(L"Nunito", DWRITE_FONT_WEIGHT_EXTRA_BOLD, G.nunito8);
    auto fmt = [](const wchar_t* fam, DWRITE_FONT_WEIGHT wt, float size, ComPtr<IDWriteTextFormat>& out) {
        G.dw->CreateTextFormat(fam, G.fonts.Get(), wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                               L"en-us", &out);
    };
    fmt(L"Nunito", DWRITE_FONT_WEIGHT_BOLD, 14, G.tfStatus);
    fmt(L"Nunito", DWRITE_FONT_WEIGHT_BOLD, 13, G.tfProgress);
    fmt(L"Nunito", DWRITE_FONT_WEIGHT_SEMI_BOLD, 12, G.tfLegal);
    fmt(L"Fredoka", DWRITE_FONT_WEIGHT_BOLD, 19, G.tfHint);
    fmt(L"Fredoka", DWRITE_FONT_WEIGHT_BOLD, 21, G.tfButton);
    fmt(L"Fredoka", DWRITE_FONT_WEIGHT_SEMI_BOLD, 18, G.tfToast);
    G.tfLegal->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    G.tfLegal->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 18, 13.5f);
    fmt(L"Fredoka", DWRITE_FONT_WEIGHT_SEMI_BOLD, 17, G.tfPill);
    fmt(L"Fredoka", DWRITE_FONT_WEIGHT_BOLD, 17, G.tfTab);
    fmt(L"Fredoka", DWRITE_FONT_WEIGHT_BOLD, 13, G.tfBadge);
    fmt(L"Fredoka", DWRITE_FONT_WEIGHT_BOLD, 32, G.tfMTitle);
    fmt(L"Nunito", DWRITE_FONT_WEIGHT_BOLD, 13.5f, G.tfMSub);
    fmt(L"Fredoka", DWRITE_FONT_WEIGHT_SEMI_BOLD, 19, G.tfDrop);
    fmt(L"Nunito", DWRITE_FONT_WEIGHT_BOLD, 12.5f, G.tfDropSmall);
    fmt(L"Nunito", DWRITE_FONT_WEIGHT_BOLD, 13, G.tfMProg);
    fmt(L"Nunito", DWRITE_FONT_WEIGHT_EXTRA_BOLD, 11.5f, G.tfMLabel);
    fmt(L"Fredoka", DWRITE_FONT_WEIGHT_SEMI_BOLD, 18, G.tfModName);
    fmt(L"Nunito", DWRITE_FONT_WEIGHT_BOLD, 12, G.tfModSum);
    fmt(L"Nunito", DWRITE_FONT_WEIGHT_BOLD, 12, G.tfMFoot);
    fmt(L"Nunito", DWRITE_FONT_WEIGHT_BOLD, 13.5f, G.tfMErr);
    fmt(L"Nunito", DWRITE_FONT_WEIGHT_BOLD, 12.5f, G.tfTip);
    fmt(L"Fredoka", DWRITE_FONT_WEIGHT_SEMI_BOLD, 16, G.tfSeg);
    fmt(L"Fredoka", DWRITE_FONT_WEIGHT_SEMI_BOLD, 14, G.tfBtn);
    fmt(L"Fredoka", DWRITE_FONT_WEIGHT_BOLD, 11, G.tfStamp);
    G.tfMSub->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 13.5f * 1.45f, 13.5f * 1.08f);
    // Card text: one line, cut off with "…".
    for (IDWriteTextFormat* f : {G.tfModName.Get(), G.tfModSum.Get(), G.tfMProg.Get()}) {
        if (!f) continue;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteInlineObject> ell;
        G.dw->CreateEllipsisTrimmingSign(f, &ell);
        DWRITE_TRIMMING tr = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&tr, ell.Get());
    }
    return G.fredoka7 && G.nunito8;
}

// Passes a glyph outline on, filled by the winding rule: variable fonts'
// letters are overlapping shapes, which the even-odd rule would leave holes in.
struct WindingSink : ID2D1SimplifiedGeometrySink {
    ID2D1GeometrySink* s;
    explicit WindingSink(ID2D1GeometrySink* sink) : s(sink) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** p) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(ID2D1SimplifiedGeometrySink)) {
            *p = this;
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) override { s->SetFillMode(D2D1_FILL_MODE_WINDING); }
    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT f) override { s->SetSegmentFlags(f); }
    void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN b) override { s->BeginFigure(p, b); }
    void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* p, UINT32 n) override { s->AddLines(p, n); }
    void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* b, UINT32 n) override { s->AddBeziers(b, n); }
    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END e) override { s->EndFigure(e); }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
};

// Text as one geometry, glyph by glyph: on a straight baseline centred at
// (cx, y), or along a circle (centre cx, cy, radius r) over its top
// (baseline on the circle) or, bottom = true, under its bottom with the
// glyphs hanging from it. spacing is CSS letter-spacing.
// Collects a laid-out line's glyphs (font face, index, advance with kerning).
struct GlyphOut { ComPtr<IDWriteFontFace> face; UINT16 index; float advance, em; };
struct GlyphCollector : IDWriteTextRenderer {
    std::vector<GlyphOut> glyphs;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** p) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteTextRenderer) || id == __uuidof(IDWritePixelSnapping)) {
            *p = this;
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* d) override { *d = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        *m = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* p) override { *p = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT, FLOAT, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        for (UINT32 i = 0; i < run->glyphCount; i++)
            glyphs.push_back({run->fontFace, run->glyphIndices[i], run->glyphAdvances[i], run->fontEmSize});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override {
        return S_OK;
    }
};

static ComPtr<ID2D1Geometry> text_geometry(const wchar_t* family, DWRITE_FONT_WEIGHT weight, float em,
                                           const wchar_t* text, float spacing, float cx, float y, bool arc = false,
                                           float cy = 0, float r = 0, bool bottom = false) {
    // Laid out by DirectWrite (kerning, the variable font's weight), then
    // each glyph's outline placed by hand.
    ComPtr<IDWriteTextFormat> fmt;
    G.dw->CreateTextFormat(family, G.fonts.Get(), weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, em,
                           L"en-us", &fmt);
    UINT32 n = (UINT32)wcslen(text);
    ComPtr<IDWriteTextLayout> lay;
    G.dw->CreateTextLayout(text, n, fmt.Get(), 4000, 1000, &lay);
    ComPtr<IDWriteTextLayout1> lay1;
    if (spacing != 0 && SUCCEEDED(lay.As(&lay1))) lay1->SetCharacterSpacing(0, spacing, 0, {0, n});
    GlyphCollector col;
    lay->Draw(nullptr, &col, 0, 0);
    if (col.glyphs.empty()) return nullptr;
    DWRITE_FONT_METRICS fm;
    col.glyphs[0].face->GetMetrics(&fm);
    float total = 0;
    for (auto& gl : col.glyphs) total += gl.advance;
    float hang = bottom ? fm.capHeight * em / fm.designUnitsPerEm : 0;
    std::vector<ID2D1Geometry*> parts;
    std::vector<ComPtr<ID2D1Geometry>> keep;
    float x = -total / 2;
    for (auto& gl : col.glyphs) {
        ComPtr<ID2D1PathGeometry> pg;
        ComPtr<ID2D1GeometrySink> s;
        G.f->CreatePathGeometry(&pg);
        pg->Open(&s);
        s->SetFillMode(D2D1_FILL_MODE_WINDING);
        WindingSink ws(s.Get());
        float a0 = 0;
        gl.face->GetGlyphRunOutline(gl.em, &gl.index, &a0, nullptr, 1, FALSE, FALSE, &ws);
        s->Close();
        std::vector<float> adv(1, gl.advance);
        const size_t i = 0;
        float mid = x + (adv[i] - spacing) / 2;  // the glyph's centre along the line
        Matrix3x2F m;
        if (!arc) {
            m = Matrix3x2F::Translation(cx + x, y);
        } else if (!bottom) {
            float a = mid / r;  // radians clockwise from the top
            m = Matrix3x2F::Translation(-(adv[i] - spacing) / 2, 0) * Matrix3x2F::Rotation(a * 180 / PI) *
                Matrix3x2F::Translation(cx + r * std::sin(a), cy - r * std::cos(a));
        } else {
            float b = -mid / r;  // radians from the bottom, left side positive
            m = Matrix3x2F::Translation(-(adv[i] - spacing) / 2, hang) * Matrix3x2F::Rotation(b * 180 / PI) *
                Matrix3x2F::Translation(cx - r * std::sin(b), cy + r * std::cos(b));
        }
        ComPtr<ID2D1TransformedGeometry> tg;
        G.f->CreateTransformedGeometry(pg.Get(), m, &tg);
        keep.push_back(tg);
        parts.push_back(tg.Get());
        x += adv[i];
    }
    ComPtr<ID2D1GeometryGroup> grp;
    G.f->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, parts.data(), (UINT32)parts.size(), &grp);
    // One clean outline (no overlapping contours), so strokes follow only the letters' edges.
    ComPtr<ID2D1PathGeometry> out;
    ComPtr<ID2D1GeometrySink> s;
    G.f->CreatePathGeometry(&out);
    out->Open(&s);
    grp->Outline(nullptr, s.Get());
    s->Close();
    return out;
}

static ComPtr<ID2D1PathGeometry> arc_path(D2D1_POINT_2F from, D2D1_POINT_2F to, float rx, float ry, float rot) {
    ComPtr<ID2D1PathGeometry> p;
    ComPtr<ID2D1GeometrySink> s;
    G.f->CreatePathGeometry(&p);
    p->Open(&s);
    s->BeginFigure(from, D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddArc(D2D1::ArcSegment(to, D2D1::SizeF(rx, ry), rot, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    return p;
}
static ComPtr<ID2D1PathGeometry> curve_path(D2D1_POINT_2F a, D2D1_POINT_2F c1, D2D1_POINT_2F c2, D2D1_POINT_2F b) {
    ComPtr<ID2D1PathGeometry> p;
    ComPtr<ID2D1GeometrySink> s;
    G.f->CreatePathGeometry(&p);
    p->Open(&s);
    s->BeginFigure(a, D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddBezier(D2D1::BezierSegment(c1, c2, b));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    return p;
}

// A rectangle with its own radius at each corner (top left, top right,
// bottom right, bottom left): the mod cards' "slightly uneven" corners.
static ComPtr<ID2D1PathGeometry> uneven(D2D1_RECT_F r, float tl, float tr, float br, float bl) {
    ComPtr<ID2D1PathGeometry> p;
    ComPtr<ID2D1GeometrySink> s;
    G.f->CreatePathGeometry(&p);
    p->Open(&s);
    auto arc = [&](float px, float py, float rad) {
        s->AddArc(D2D1::ArcSegment(Point2F(px, py), D2D1::SizeF(rad, rad), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                   D2D1_ARC_SIZE_SMALL));
    };
    s->BeginFigure(Point2F(r.left + tl, r.top), D2D1_FIGURE_BEGIN_FILLED);
    s->AddLine(Point2F(r.right - tr, r.top));
    arc(r.right, r.top + tr, tr);
    s->AddLine(Point2F(r.right, r.bottom - br));
    arc(r.right - br, r.bottom, br);
    s->AddLine(Point2F(r.left + bl, r.bottom));
    arc(r.left, r.bottom - bl, bl);
    s->AddLine(Point2F(r.left, r.top + tl));
    arc(r.left + tl, r.top, tl);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    return p;
}

static void build_geometry() {
    G.star = polygon({{300, 8}, {380, 220}, {592, 300}, {380, 380}, {300, 592}, {220, 380}, {8, 300}, {220, 220}});
    const D2D1_POINT_2F tri[8][3] = {
        {{300, 40}, {300, 212}, {238, 212}}, {{300, 40}, {362, 212}, {300, 212}},
        {{560, 300}, {388, 300}, {388, 238}}, {{560, 300}, {388, 362}, {388, 300}},
        {{300, 560}, {238, 388}, {300, 388}}, {{300, 560}, {300, 388}, {362, 388}},
        {{40, 300}, {212, 238}, {212, 300}}, {{40, 300}, {212, 300}, {212, 362}}};
    for (int i = 0; i < 8; i++) G.gems[i] = polygon({tri[i][0], tri[i][1], tri[i][2]});
    for (int i = 0; i < 12; i++) {
        float a0 = (-90 + 30.f * i) * PI / 180, a1 = (-60 + 30.f * i) * PI / 180;
        ComPtr<ID2D1GeometrySink> s;
        G.f->CreatePathGeometry(&G.wedges[i]);
        G.wedges[i]->Open(&s);
        s->BeginFigure(Point2F(300 + 140 * std::cos(a0), 300 + 140 * std::sin(a0)), D2D1_FIGURE_BEGIN_FILLED);
        s->AddArc(D2D1::ArcSegment(Point2F(300 + 140 * std::cos(a1), 300 + 140 * std::sin(a1)), D2D1::SizeF(140, 140),
                                   0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->AddLine(Point2F(300 + 112 * std::cos(a1), 300 + 112 * std::sin(a1)));
        s->AddArc(D2D1::ArcSegment(Point2F(300 + 112 * std::cos(a0), 300 + 112 * std::sin(a0)), D2D1::SizeF(112, 112),
                                   0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
    }
    ComPtr<ID2D1EllipseGeometry> c150;
    G.f->CreateEllipseGeometry(D2D1::Ellipse(Point2F(300, 300), 150, 150), &c150);
    ID2D1Geometry* sc[2] = {G.star.Get(), c150.Get()};
    G.f->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, sc, 2, &G.starClip);

    // The disc's lettering, in stage units (the 224 box sits at 188, 188).
    const float o = 188;
    const auto B = DWRITE_FONT_WEIGHT_BOLD, XB = DWRITE_FONT_WEIGHT_EXTRA_BOLD;
    G.wonderT = text_geometry(L"Fredoka", B, 42, L"Wonder", 0, o + 112, 0, true, o + 256, 160);
    G.parkT = text_geometry(L"Fredoka", B, 50, L"park", 0, o + 112, o + 150);
    G.recompT = text_geometry(L"Nunito", XB, 10.5f, L"UNOFFICIAL RECOMP", 2.4f, o + 112, o + 184);
    G.wonderparkM = text_geometry(L"Fredoka", B, 30, L"Wonderpark", 0, o + 112, 0, true, o + 116, 82);
    G.unofficialM = text_geometry(L"Nunito", XB, 10, L"UNOFFICIAL RECOMP", 2.4f, o + 112, 0, true, o + 112, 82, true);

    // Light rays: 4 degrees lit in every 18 (repeating-conic-gradient), from the centre.
    for (int i = 0; i < 20; i++) {
        float a0 = (i * 18 - 90) * PI / 180, a1 = (i * 18 + 4 - 90) * PI / 180;
        G.rays[i] = polygon({{640, 360},
                             {640 + 640 * std::cos(a0), 360 + 640 * std::sin(a0)},
                             {640 + 640 * std::cos(a1), 360 + 640 * std::sin(a1)}});
    }
    G.sparkle4 = sparkle_path();
    G.ringBack = arc_path(Point2F(18, 74), Point2F(162, 40), 72, 22, -14);
    G.ringFront = arc_path(Point2F(162, 40), Point2F(18, 74), 72, 22, -14);
    G.bandA = curve_path(Point2F(30, 70), Point2F(40, 62), Point2F(60, 56), Point2F(90, 52));
    G.bandB = curve_path(Point2F(14, 34), Point2F(24, 30), Point2F(40, 30), Point2F(56, 36));
    G.hintStar = polygon({{12, 2}, {14.6f, 9.4f}, {22, 12}, {14.6f, 14.6f}, {12, 22}, {9.4f, 14.6f}, {2, 12}, {9.4f, 9.4f}});
    {
        ComPtr<ID2D1GeometrySink> s;
        G.f->CreatePathGeometry(&G.checkMark);
        G.checkMark->Open(&s);
        s->BeginFigure(Point2F(5, 12.5f), D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddLine(Point2F(9.5f, 17));
        s->AddLine(Point2F(19, 7.5f));
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
    }
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                 D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    G.f->CreateStrokeStyle(sp, nullptr, 0, &G.round);
    {
        const float dashes[2] = {3, 3};  // in stroke widths: 6 px on, 6 off at 2 px
        D2D1_STROKE_STYLE_PROPERTIES dp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                     D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10,
                                                                     D2D1_DASH_STYLE_CUSTOM, 0);
        G.f->CreateStrokeStyle(dp, dashes, 2, &G.dash);
    }
    G.card = uneven(RectF(0, 0, 422, 61), 13, 16, 12, 15);

    // The Mods tab's puzzle piece (the mockup's 24-box SVG path, stroked).
    {
        G.f->CreatePathGeometry(&G.puzzle);
        ComPtr<ID2D1GeometrySink> s;
        G.puzzle->Open(&s);
        auto arc = [&](float x, float y) {
            s->AddArc(D2D1::ArcSegment(Point2F(x, y), D2D1::SizeF(2, 2), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                       D2D1_ARC_SIZE_LARGE));
        };
        auto to = [&](float x, float y) { s->AddLine(Point2F(x, y)); };
        s->BeginFigure(Point2F(10, 3), D2D1_FIGURE_BEGIN_HOLLOW);
        to(14, 3), to(14, 6), arc(18, 6), to(18, 3), to(21, 3), to(21, 10), to(18, 10), arc(18, 14), to(21, 14), to(21, 21),
            to(14, 21), to(14, 18), arc(10, 18), to(10, 21), to(3, 21), to(3, 14), to(6, 14), arc(6, 10), to(3, 10), to(3, 3);
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
    }
    // The drop zone's upload arrow ("M12 15V3M7 8l5-5 5 5" and the tray).
    {
        G.f->CreatePathGeometry(&G.upload);
        ComPtr<ID2D1GeometrySink> s;
        G.upload->Open(&s);
        s->BeginFigure(Point2F(12, 15), D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddLine(Point2F(12, 3));
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->BeginFigure(Point2F(7, 8), D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddLine(Point2F(12, 3));
        s->AddLine(Point2F(17, 8));
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->BeginFigure(Point2F(4, 15), D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddLine(Point2F(4, 19));
        s->AddArc(D2D1::ArcSegment(Point2F(6, 21), D2D1::SizeF(2, 2), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                   D2D1_ARC_SIZE_SMALL));
        s->AddLine(Point2F(18, 21));
        s->AddArc(D2D1::ArcSegment(Point2F(20, 19), D2D1::SizeF(2, 2), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                   D2D1_ARC_SIZE_SMALL));
        s->AddLine(Point2F(20, 15));
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
    }
}

// ---- layers rendered once per window size

static ComPtr<ID2D1Bitmap1> make_target(UINT w, UINT h) {
    ComPtr<ID2D1Bitmap1> b;
    D2D1_BITMAP_PROPERTIES1 p = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    G.dc->CreateBitmap(D2D1::SizeU(std::max(1u, w), std::max(1u, h)), nullptr, 0, p, &b);
    return b;
}

// Renders draw() into a new w x h bitmap; with blur, blurs it (sigma, in pixels).
template <class F>
static ComPtr<ID2D1Bitmap1> offscreen(UINT w, UINT h, F draw, float blur = 0) {
    ComPtr<ID2D1Bitmap1> b = make_target(w, h);
    ComPtr<ID2D1Image> old;
    G.dc->GetTarget(&old);
    G.dc->SetTarget(b.Get());
    G.dc->BeginDraw();
    G.dc->Clear(ColorF(0, 0, 0, 0));
    draw();
    G.dc->EndDraw();
    if (blur > 0) {
        ComPtr<ID2D1Bitmap1> out = make_target(w, h);
        ComPtr<ID2D1Effect> fx;
        G.dc->CreateEffect(CLSID_D2D1GaussianBlur, &fx);
        fx->SetInput(0, b.Get());
        fx->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, blur);
        fx->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
        G.dc->SetTarget(out.Get());
        G.dc->BeginDraw();
        G.dc->Clear(ColorF(0, 0, 0, 0));
        G.dc->SetTransform(Matrix3x2F::Identity());
        G.dc->DrawImage(fx.Get());
        G.dc->EndDraw();
        b = out;
    }
    G.dc->SetTarget(old.Get());
    return b;
}

// Bitmaps covering the scene from -40 to +40 past each edge (the star layers' inset:-40px).
static const float PAD = 40;
static void dots(float tw, float th, float ox, float oy, D2D1_COLOR_F c, float core, float edge, float halo = 0,
                 D2D1_COLOR_F hc = {}) {
    auto b = brush(c);
    auto hb = brush(hc);
    for (float y = -PAD + oy + th / 2; y < H + PAD; y += th)
        for (float x = -PAD + ox + tw / 2; x < W + PAD; x += tw) {
            if (halo > 0) {
                auto rg = radial(Point2F(x + PAD, y + PAD), halo, halo,
                                 {{0, hc}, {(edge) / halo, hc}, {1, ColorF(hc.r, hc.g, hc.b, 0)}});
                G.dc->FillEllipse(D2D1::Ellipse(Point2F(x + PAD, y + PAD), halo, halo), rg.Get());
            }
            auto rg = radial(Point2F(x + PAD, y + PAD), edge, edge,
                             {{0, c}, {core / edge, c}, {1, ColorF(c.r, c.g, c.b, 0)}});
            G.dc->FillEllipse(D2D1::Ellipse(Point2F(x + PAD, y + PAD), edge, edge), rg.Get());
        }
    (void)b;
    (void)hb;
}

static void build_layers() {
    UINT lw = (UINT)std::ceil((W + 2 * PAD) * G.k), lh = (UINT)std::ceil((H + 2 * PAD) * G.k);
    auto scaled = [](float extra = 0) { G.dc->SetTransform(Matrix3x2F::Scale(G.k, G.k)); };
    G.sky = offscreen(lw, lh, [&] {
        scaled();
        D2D1_RECT_F all = RectF(0, 0, W + 2 * PAD, H + 2 * PAD);
        auto at = [](float x, float y) { return Point2F(x + PAD, y + PAD); };
        G.dc->FillRectangle(all, radial(at(640, 360), 640 * 1.4142f, 360 * 1.4142f,
                                        {{0, rgb(0x26387a)}, {.45f, rgb(0x16224f)}, {1, rgb(0x0a1029)}}).Get());
        G.dc->FillRectangle(all, radial(at(1024, 532.8f), 380, 380 * 532.8f / 1024,
                                        {{0, rgb(0x3fd0d8, .18f)}, {1, rgb(0x3fd0d8, 0)}}).Get());
        G.dc->FillRectangle(all, radial(at(281.6f, 201.6f), 420, 420 * 518.4f / 998.4f,
                                        {{0, rgb(0x8b5bd2, .3f)}, {1, rgb(0x8b5bd2, 0)}}).Get());
        G.dc->FillRectangle(all, radial(at(640, 374.4f), 330, 330,
                                        {{0, rgb(0xfff6dc, .22f)}, {1, rgb(0xfff6dc, 0)}}).Get());
        dots(61, 67, 0, 0, ColorF(1, 1, 1, .85f), 1, 1.6f);
        dots(97, 89, 31, 47, rgb(0xcdf6f8, .7f), 1, 1.6f);
        dots(173, 151, 83, 19, rgb(0xfff4cf, .9f), 1.4f, 2);
    });
    G.twA = offscreen(lw, lh, [&] {
        scaled();
        dots(211, 193, 40, 70, rgb(0xffffff), 1.6f, 2.4f, 4, ColorF(1, 1, 1, .35f));
        dots(257, 229, 150, 20, rgb(0xcdf6f8), 1.4f, 2.2f, 3.6f, rgb(0xcdf6f8, .3f));
    });
    G.twB = offscreen(lw, lh, [&] {
        scaled();
        dots(233, 211, 120, 150, rgb(0xfff4cf), 1.6f, 2.4f, 4, rgb(0xfff4cf, .35f));
        dots(181, 241, 10, 110, rgb(0xffffff), 1.2f, 2, 3.4f, ColorF(1, 1, 1, .3f));
    });
    // The beam onto the emblem: 140 x 360, blurred 8px. Drawn from x 546, y -24.
    G.beam = offscreen((UINT)std::ceil(188 * G.k), (UINT)std::ceil(408 * G.k), [&] {
        scaled();
        G.dc->FillRectangle(RectF(24, 24, 164, 384),
                            linear(Point2F(0, 24), Point2F(0, 384),
                                   {{0, rgb(0xdcfaff, 0)}, {.6f, rgb(0xdcfaff, .32f)}, {1, rgb(0xfffae6, .55f)}}).Get());
    }, 8 * G.k);
    // The emblem's glow (drop-shadow of the star and circle), as masks: drawn
    // for the stage box (600) plus 100 around, at half scale.
    auto glow = [&](float sigma) {
        return offscreen(400, 400, [&] {
            G.dc->SetTransform(Matrix3x2F::Translation(100, 100) * Matrix3x2F::Scale(.5f, .5f));
            G.dc->FillGeometry(G.starClip.Get(), brush(ColorF(1, 1, 1)).Get());
        }, sigma * .5f);
    };
    G.glow8 = glow(8);
    G.glow18 = glow(18);
}

static bool create_device() {
    D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                              D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 4,
                                   D3D11_SDK_VERSION, &G.d3d, nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 4,
                               D3D11_SDK_VERSION, &G.d3d, nullptr, nullptr);
    if (FAILED(hr)) return false;
    ComPtr<IDXGIDevice> dxgi;
    G.d3d.As(&dxgi);
    if (!G.f) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), nullptr, &G.f);
    G.f->CreateDevice(dxgi.Get(), &G.dev);
    G.dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &G.dc);
    ComPtr<IDXGIAdapter> ad;
    dxgi->GetAdapter(&ad);
    ComPtr<IDXGIFactory2> fac;
    ad->GetParent(IID_PPV_ARGS(&fac));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(fac->CreateSwapChainForHwnd(G.d3d.Get(), g_wnd, &sd, nullptr, nullptr, &G.swap))) return false;
    fac->MakeWindowAssociation(g_wnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

static void resize_targets() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    UINT w = std::max<LONG>(1, rc.right), h = std::max<LONG>(1, rc.bottom);
    if (w == G.pw && h == G.ph && G.back) return;
    G.dc->SetTarget(nullptr);
    G.back.Reset();
    G.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    ComPtr<IDXGISurface> surf;
    G.swap->GetBuffer(0, IID_PPV_ARGS(&surf));
    D2D1_BITMAP_PROPERTIES1 p = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    G.dc->CreateBitmapFromDxgiSurface(surf.Get(), &p, &G.back);
    G.dc->SetTarget(G.back.Get());
    G.dc->SetDpi(96, 96);
    G.pw = w;
    G.ph = h;
    G.k = std::min(w / W, h / H);
    G.ox = (w - W * G.k) / 2;
    G.oy = (h - H * G.k) / 2;
    G.real.clear();  // flattened for the old size
    build_layers();
}

// ------------------------------------------------------------------------
// The scene

struct View {
    bool menu = false;         // the logo was clicked
    double menu_t = -100;      // when
    double start = 0;
    bool reduced = false;      // reduced motion
    float mx = 0, my = 0;      // parallax, -1..1
    int hover = 0;             // 1 logo, 2 hint, 3 main button, 4 show log
    bool pressed = false;      // mouse down on the hovered control
    double done_t = -100;      // build finished (beam flash, confetti)
    double toast_t = -100;     // "Launching Wonderpark..."
    float gem_mix[4] = {0, 0, 0, 0};  // 1 = coloured, per point (animated)
    double last_frame = 0, last_render = -1;
    double fast_until = 3;     // 60 fps until then (the intro, then after input)
    D2D1_RECT_F button = {}, log_link = {};
    // The Mods page
    bool mods = false;         // open (or opening)
    float modsK = 0;           // 0 closed .. 1 open, linear over .8 s (eased where drawn)
    double mods_t = -100;      // when it last opened (the contents' fade-ups, the sweep)
    int focus = -1;            // keyboard/controller focus: 0 drop zone, 1 close, 2+ the switches
    float scroll = 0;          // the list's scroll, scene px
    std::wstring fresh;        // the mod just installed: at the top, glowing, popped in
    double fresh_t = -100;
    D2D1_RECT_F tab = {}, close = {}, drop = {}, list = {};
    std::vector<D2D1_RECT_F> switches, summaries;
    std::vector<bool> cut;     // a card's summary is cut off (full text on hover)
    int tip = -1;              // the card whose summary tooltip shows
    float tip_x = 0, tip_y = 0;
    bool drag_over = false;    // a file is being dragged over the window
    bool inst_handled = true;  // the last install's result has been taken in
    double inst_done_t = -100; // when it finished (the bar shows "Done" for .25 s)
    float list_h = 0, content_h = 0;  // the list's visible and full height (scrolling)
    bool keys = false;         // last input was keyboard/controller: focus rings show
    // Extras: which page (0 Mods, 1 Saves, 2 HD Pack) and Mods' sub-page
    // (0 Installed, 1 Get mods). Everything clickable on the panel, in
    // order, for the mouse and the keyboard/controller focus (fcode: the
    // focused one's code, 0 none).
    int page = 0, modsSub = 0;
    std::vector<std::pair<D2D1_RECT_F, int>> items;
    int fcode = 0;
    // The Saves page: what was read last, and the last action's result.
    std::vector<SaveFile> saves;
    std::vector<SaveBackup> backups;
    bool game_running = false;
    double saves_t = -100;     // when they were read (again every 2 s while it shows)
    std::wstring saves_msg;
    bool saves_err = false;
};
static View V;
static std::vector<ModInfo> g_mods;  // installed, in mods.ini's order
static void fast(double secs) { V.fast_until = std::max(V.fast_until, now_s() + secs); }

// Seconds since the menu opened; huge when there's no animation.
static float since(double t0, double t) { return V.reduced ? 1e6f : (float)(t - t0); }

// Fixed shapes are drawn from realizations (flattened once, at a tolerance
// fine enough for the emblem's largest zoom) instead of being flattened
// again every frame, which was most of the launcher's CPU time.
static ID2D1GeometryRealization* realization(ID2D1Geometry* g, float stroke, ID2D1StrokeStyle* style) {
    auto key = std::make_pair((void*)g, stroke);
    auto it = G.real.find(key);
    if (it != G.real.end()) return it->second.Get();
    float tol = D2D1::ComputeFlatteningTolerance(Matrix3x2F::Identity(), 96, 96, 2.0f * G.k);
    ComPtr<ID2D1GeometryRealization> r;
    if (stroke > 0) G.dc->CreateStrokedGeometryRealization(g, tol, stroke, style, &r);
    else G.dc->CreateFilledGeometryRealization(g, tol, &r);
    G.real[key] = r;
    return r.Get();
}
static void fillG(ID2D1Geometry* g, ID2D1Brush* b) { G.dc->DrawGeometryRealization(realization(g, 0, nullptr), b); }
static void strokeG(ID2D1Geometry* g, ID2D1Brush* b, float w, ID2D1StrokeStyle* s = nullptr) {
    G.dc->DrawGeometryRealization(realization(g, w, s), b);
}

static void fill_ellipse(float x, float y, float r, ID2D1Brush* b) {
    G.dc->FillEllipse(D2D1::Ellipse(Point2F(x, y), r, r), b);
}

static void draw_sparkle(const Matrix3x2F& m, float x, float y, float size, D2D1_COLOR_F c, float opacity, float scale) {
    float s = size / 20 * scale;
    G.dc->SetTransform(Matrix3x2F::Translation(-10, -10) * Matrix3x2F::Scale(s, s) *
                       Matrix3x2F::Translation(x + size / 2, y + size / 2) * m);
    c.a *= opacity;
    fillG(G.sparkle4.Get(), brush(c).Get());
}

// A text layout, made once per text, format, box and alignment.
static IDWriteTextLayout* layout(const std::wstring& s, IDWriteTextFormat* f, float w, float h,
                                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    wchar_t k[96];
    swprintf(k, 96, L"|%p|%g|%g|%d", (void*)f, w, h, (int)align);
    std::wstring key = s + k;
    auto it = G.layouts.find(key);
    if (it != G.layouts.end()) return it->second.Get();
    if (G.layouts.size() > 400) G.layouts.clear();
    ComPtr<IDWriteTextLayout> l;
    G.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, h, &l);
    l->SetTextAlignment(align);
    G.layouts[key] = l;
    return l.Get();
}
static void draw_text(const std::wstring& s, IDWriteTextFormat* f, float x, float y, float w, float h,
                      D2D1_COLOR_F c, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    G.dc->DrawTextLayout(Point2F(x, y), layout(s, f, w, h, align), brush(c).Get());
}
static float text_height(const std::wstring& s, IDWriteTextFormat* f, float w, float* width = nullptr) {
    DWRITE_TEXT_METRICS m;
    layout(s, f, w, 1000)->GetMetrics(&m);
    if (width) *width = m.widthIncludingTrailingWhitespace;
    return m.height;
}

// ------------------------------------------------------------------------
// The Mods page: the MODS tab on the right edge and the panel that slides in
// (the mockup's .mods-tab and .mods). Rects for the mouse are in scene units.

static std::map<std::wstring, float> g_knob;  // each switch's knob, 0 off .. 1 on (.2 s)

// A text layout with letter spacing (the tab's MODS, the INSTALLED label).
static ComPtr<IDWriteTextLayout> spaced(const std::wstring& s, IDWriteTextFormat* f, float spacing) {
    ComPtr<IDWriteTextLayout> l;
    G.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 1000, 100, &l);
    ComPtr<IDWriteTextLayout1> l1;
    if (l && SUCCEEDED(l.As(&l1))) l1->SetCharacterSpacing(0, spacing, 0, {0, (UINT32)s.size()});
    return l;
}

static void focus_ring(D2D1_RECT_F r, float rad) {  // outline: 3px teal, 3px out
    if (!V.keys) return;  // only while moving with the keyboard or a controller
    G.dc->DrawRoundedRectangle(
        D2D1::RoundedRect(RectF(r.left - 4.5f, r.top - 4.5f, r.right + 4.5f, r.bottom + 4.5f), rad + 4.5f, rad + 4.5f),
        brush(rgb(0x3fd0d8)).Get(), 3);
}

// ---- Extras: helpers for the pages (scene units, the panel's transform set)

static void ui_reg(D2D1_RECT_F r, int code) { V.items.push_back({r, code}); }
static void ui_ring(D2D1_RECT_F r, float rad, int code) {
    if (V.fcode == code) focus_ring(r, rad);
}
// A pill button 34 high at (x, by): solid (teal) or outlined; returns its width.
static float ui_button(float x, float by, const std::wstring& label, int code, bool solid, bool enabled = true) {
    ID2D1DeviceContext2* dc = G.dc.Get();
    float tw = 0;
    text_height(label, G.tfBtn.Get(), 1000, &tw);
    float w = tw + 30, h = 34;
    D2D1_RECT_F b = RectF(x, by, x + w, by + h);
    bool hov = enabled && V.hover == code;
    float op = enabled ? 1.f : .45f;
    if (solid) dc->FillRoundedRectangle(D2D1::RoundedRect(b, 17, 17), brush(hov ? rgb(0x7ae4ea, op) : rgb(0x3fd0d8, op)).Get());
    else dc->FillRoundedRectangle(D2D1::RoundedRect(b, 17, 17), brush(hov ? rgb(0x24335f, op) : rgb(0x121b3a, .6f * op)).Get());
    dc->DrawRoundedRectangle(D2D1::RoundedRect(RectF(x + 1, by + 1, x + w - 1, by + h - 1), 16, 16),
                             brush(solid ? rgb(0xe8fdff, .8f * op) : rgb(0xf3cf6b, .6f * op)).Get(), 1.5f);
    float th = text_height(label, G.tfBtn.Get(), w);
    draw_text(label, G.tfBtn.Get(), x, by + h / 2 - th / 2, w, th, solid ? rgb(0x10223a, op) : rgb(0xffe7a3, op),
              DWRITE_TEXT_ALIGNMENT_CENTER);
    if (enabled) {
        ui_reg(b, code);
        ui_ring(b, 17, code);
    }
    return w;
}
static float ui_button_w(const std::wstring& label) {
    float tw = 0;
    text_height(label, G.tfBtn.Get(), 1000, &tw);
    return tw + 30;
}
// The red "Coming soon" rubber stamp, tilted, at (x, cy); returns its width.
static float ui_stamp(float x, float cy, const std::wstring& s) {
    ID2D1DeviceContext2* dc = G.dc.Get();
    ComPtr<IDWriteTextLayout> l = spaced(s, G.tfStamp.Get(), 1.1f);
    DWRITE_TEXT_METRICS m;
    l->GetMetrics(&m);
    float w = m.widthIncludingTrailingWhitespace + 18, h = 22;
    Matrix3x2F old;
    dc->GetTransform(&old);
    dc->SetTransform(Matrix3x2F::Rotation(-6, Point2F(x + w / 2, cy)) * old);
    dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(x, cy - h / 2, x + w, cy + h / 2), 6, 6), brush(rgb(0x3a160e, .35f)).Get());
    dc->DrawRoundedRectangle(D2D1::RoundedRect(RectF(x + 1.25f, cy - h / 2 + 1.25f, x + w - 1.25f, cy + h / 2 - 1.25f), 5, 5),
                             brush(rgb(0xff8a7a)).Get(), 2.5f);
    dc->DrawTextLayout(Point2F(x + 9, cy - m.height / 2), l.Get(), brush(rgb(0xff8a7a)).Get());
    dc->SetTransform(old);
    return w;
}
// A small capitals label (YOUR SAVE FILES); returns its height.
static float ui_label(float x, float ly, const std::wstring& s) {
    ComPtr<IDWriteTextLayout> l = spaced(s, G.tfMLabel.Get(), 11.5f * .12f);
    DWRITE_TEXT_METRICS m;
    l->GetMetrics(&m);
    G.dc->DrawTextLayout(Point2F(x, ly), l.Get(), brush(rgb(0xc6d0e6)).Get());
    return m.height;
}
// A switch, 52 x 30 (as the mods' switches), not animated.
static void ui_switch(D2D1_RECT_F r, bool on, int code, bool enabled = true) {
    ID2D1DeviceContext2* dc = G.dc.Get();
    float kv = on ? 1.f : 0.f, op = enabled ? 1.f : .45f;
    D2D1_ROUNDED_RECT tr = D2D1::RoundedRect(r, 15, 15);
    dc->FillRoundedRectangle(tr, brush(mix(rgb(0x3a466b, op), rgb(0x3fd0d8, op), kv)).Get());
    dc->DrawRoundedRectangle(D2D1::RoundedRect(RectF(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), 14, 14),
                             brush(mix(rgb(0x5d6a8a, op), rgb(0xe8fdff, op), kv)).Get(), 2);
    float kx = r.left + 4 + 11 + 22 * kv, ky = r.top + 4 + 11;
    fill_ellipse(kx, ky + 2, 11, brush(rgb(0x10182f, .4f)).Get());
    fill_ellipse(kx, ky, 11, brush(mix(rgb(0xb6c1d6), rgb(0xf3cf6b), kv)).Get());
    if (enabled) {
        ui_reg(r, code);
        ui_ring(r, 15, code);
    }
}

// The Get mods list's address (launcher.ini, [mods] index_url): empty until
// the list exists.
static std::wstring mods_index_url() {
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"mods", L"index_url", L"", buf, 1024, (mods_data_dir() + L"\\launcher.ini").c_str());
    return buf;
}

static std::wstring play_time_text(int s) {
    wchar_t b[32];
    if (s >= 3600) swprintf(b, 32, L"%dh %02dm", s / 3600, s / 60 % 60);
    else swprintf(b, 32, L"%dm", s / 60);
    return b;
}
static std::wstring thousands(int n) {
    std::wstring s = std::to_wstring(n), out;
    for (size_t i = 0; i < s.size(); i++) {
        if (i && (s.size() - i) % 3 == 0) out += L',';
        out += s[i];
    }
    return out;
}
// "today, 9:14 PM", "yesterday, 9:14 PM", "Nov 2" or "Nov 2, 2025".
static std::wstring when_text(uint64_t ft) {
    if (!ft) return L"";
    FILETIME f = {(DWORD)ft, (DWORD)(ft >> 32)}, lf;
    SYSTEMTIME s, now;
    FileTimeToLocalFileTime(&f, &lf);
    FileTimeToSystemTime(&lf, &s);
    GetLocalTime(&now);
    static const wchar_t* mon[12] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    wchar_t b[64], clock[24];
    swprintf(clock, 24, L"%d:%02d %s", s.wHour % 12 ? s.wHour % 12 : 12, s.wMinute, s.wHour < 12 ? L"AM" : L"PM");
    auto day = [](const SYSTEMTIME& x) {
        FILETIME y;
        SYSTEMTIME d = x;
        d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0;
        SystemTimeToFileTime(&d, &y);
        return ((uint64_t)y.dwHighDateTime << 32 | y.dwLowDateTime) / 864000000000ull;
    };
    uint64_t dd = day(now) - day(s);
    if (dd == 0) swprintf(b, 64, L"today, %s", clock);
    else if (dd == 1) swprintf(b, 64, L"yesterday, %s", clock);
    else if (s.wYear == now.wYear) swprintf(b, 64, L"%s %d", mon[(s.wMonth + 11) % 12], s.wDay);
    else swprintf(b, 64, L"%s %d, %d", mon[(s.wMonth + 11) % 12], s.wDay, s.wYear);
    return b;
}

static void refresh_saves() {
    V.saves = saves_read(g_root);
    V.backups = saves_backups();
    V.game_running = saves_game_running();
}

// ---- Extras > Saves: the game's two save files, import and export, backups.
// Codes: 300 + 2 * file back up, 301 + 2 * file export, 320 import, 321 open
// the folder, 322 the "Back up before every Play" switch, 330 + n restore
// backup n.
static void render_saves_page(double t, Matrix3x2F M, float L, float Rr, float y, float footTop, float px, float pw) {
    ID2D1DeviceContext2* dc = G.dc.Get();
    float CW = Rr - L;
    if (V.saves_t < 0) refresh_saves(), V.saves_t = t;
    if (t - V.saves_t > 2) {  // whether the game started or stopped
        V.saves_t = t;
        if (saves_game_running() != V.game_running) refresh_saves();
    }
    bool locked = V.game_running;
    // The status line.
    {
        D2D1_ROUNDED_RECT r = D2D1::RoundedRect(RectF(L, y, Rr, y + 38), 12, 12);
        uint32_t c = locked ? 0xf39a2b : 0x5bc26a;
        dc->FillRoundedRectangle(r, brush(rgb(c, .14f)).Get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + .5f, y + .5f, Rr - .5f, y + 37.5f), 12, 12), brush(rgb(c, .55f)).Get(), 1);
        fill_ellipse(L + 18, y + 19, 5, brush(rgb(c)).Get());
        std::wstring s = locked ? L"The game is running: your saves are locked until you close it."
                                : L"The game is closed, so your saves are safe to change.";
        float th = text_height(s, G.tfMFoot.Get(), CW - 40);
        draw_text(s, G.tfMFoot.Get(), L + 32, y + 19 - th / 2, CW - 40, th, locked ? rgb(0xffd8a8) : rgb(0xbff0c6));
        y += 38 + 12;
    }
    // The rest scrolls.
    D2D1_RECT_F region = RectF(px + 2, y, px + pw - 2, footTop);
    V.list = region;
    V.list_h = footTop - y;
    dc->PushAxisAlignedClip(region, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float cy = y + 6 - V.scroll;
    auto visible = [&](D2D1_RECT_F r) { return r.top >= region.top - 1 && r.bottom <= region.bottom + 1; };
    auto button = [&](float x, float by, const std::wstring& s, int code, bool solid, bool enabled) {
        D2D1_RECT_F r = RectF(x, by, x + ui_button_w(s), by + 34);
        return ui_button(x, by, s, code, solid, enabled && visible(r));
    };
    cy += ui_label(L, cy, L"YOUR SAVE FILES") + 10;
    const float cardH = 100;
    for (auto& f : V.saves) {
        D2D1_RECT_F card = RectF(L, cy, Rr, cy + cardH);
        if (f.exists) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(L, cy + 3, Rr, cy + cardH + 3), 16, 16), brush(rgb(0x080c1e, .45f)).Get());
            dc->FillRoundedRectangle(D2D1::RoundedRect(card, 16, 16),
                                     linear(Point2F(0, cy), Point2F(0, cy + cardH), {{0, rgb(0x2c3d70, .7f)}, {1, rgb(0x1e2b52, .75f)}}).Get());
            dc->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + .75f, cy + .75f, Rr - .75f, cy + cardH - .75f), 15, 15),
                                     brush(rgb(0xf3cf6b, .32f)).Get(), 1.5f);
        } else {
            dc->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + 1, cy + 1, Rr - 1, cy + cardH - 1), 15, 15), brush(rgb(0xf3cf6b, .35f)).Get(), 1.5f,
                                     G.dash.Get());
        }
        // The file's number on a gold tile.
        D2D1_RECT_F nb = RectF(L + 16, cy + cardH / 2 - 23, L + 16 + 46, cy + cardH / 2 + 23);
        dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(nb.left, nb.top + 3, nb.right, nb.bottom + 3), 12, 12), brush(rgb(0xa8741d, f.exists ? 1.f : .5f)).Get());
        dc->FillRoundedRectangle(D2D1::RoundedRect(nb, 12, 12),
                                 linear(Point2F(0, nb.top), Point2F(0, nb.bottom), {{0, rgb(0xffe7a3, f.exists ? 1.f : .5f)}, {1, rgb(0xf3cf6b, f.exists ? 1.f : .5f)}}).Get());
        std::wstring num = std::to_wstring(f.slot + 1);
        float nh = text_height(num, G.tfDrop.Get(), 46);
        draw_text(num, G.tfDrop.Get(), nb.left, (nb.top + nb.bottom) / 2 - nh / 2, 46, nh, rgb(0x1b2440), DWRITE_TEXT_ALIGNMENT_CENTER);
        float tx = nb.right + 14, bw = std::max(ui_button_w(L"Back up"), ui_button_w(L"Export")), tw = Rr - 14 - bw - 10 - tx;
        if (f.exists) {
            std::wstring title = L"File " + num + (f.place.empty() ? L"" : L" · " + f.place);
            std::wstring stats;
            if (f.friends >= 0) stats += std::to_wstring(f.friends) + (f.friends == 1 ? L" friend · " : L" friends · ");
            stats += thousands(f.berries) + L" berries · " + play_time_text(f.play_seconds);
            std::wstring last = L"Last played " + when_text(f.last_played);
            float h1 = text_height(title, G.tfModName.Get(), tw), h2 = text_height(stats, G.tfModSum.Get(), tw), h3 = text_height(last, G.tfModSum.Get(), tw);
            float ty = cy + cardH / 2 - (h1 + 3 + h2 + 2 + h3) / 2;
            draw_text(title, G.tfModName.Get(), tx, ty, tw, h1, rgb(0xffe7a3));
            draw_text(stats, G.tfModSum.Get(), tx, ty + h1 + 3, tw, h2, rgb(0xe6ebf5));
            draw_text(last, G.tfModSum.Get(), tx, ty + h1 + 3 + h2 + 2, tw, h3, rgb(0xb9c4dc));
            float bx = Rr - 14 - bw;
            button(bx, cy + cardH / 2 - 34 - 4, L"Back up", 300 + 2 * f.slot, false, !locked);
            button(bx, cy + cardH / 2 + 4, L"Export", 301 + 2 * f.slot, false, true);
        } else {
            float h1 = text_height(L"Empty", G.tfModName.Get(), tw), h2 = text_height(L"No save in this file yet.", G.tfModSum.Get(), tw);
            float ty = cy + cardH / 2 - (h1 + 3 + h2) / 2;
            draw_text(L"Empty", G.tfModName.Get(), tx, ty, tw, h1, rgb(0xffe7a3, .7f));
            draw_text(L"No save in this file yet.", G.tfModSum.Get(), tx, ty + h1 + 3, tw, h2, rgb(0xb9c4dc, .8f));
        }
        cy += cardH + 10;
    }
    cy += 4;
    {
        float w = button(L, cy, L"Import a save…", 320, false, !locked);
        button(L + w + 10, cy, L"Open saves folder", 321, false, true);
        cy += 34 + 8;
    }
    if (!V.saves_msg.empty()) {
        float h = text_height(V.saves_msg, G.tfMFoot.Get(), CW);
        draw_text(V.saves_msg, G.tfMFoot.Get(), L, cy, CW, h, V.saves_err ? rgb(0xffb4b4) : rgb(0xbff0c6));
        cy += h + 6;
    }
    {
        std::wstring s = L"Import: a Wii's data.bin (from an SD card), a .zip, or any file in Dolphin's save folder. Your saves are backed up first.";
        float h = text_height(s, G.tfMFoot.Get(), CW);
        draw_text(s, G.tfMFoot.Get(), L, cy, CW, h, rgb(0x8f9cbd));
        cy += h + 18;
    }
    cy += ui_label(L, cy, L"BACKUPS") + 10;
    {
        float h1 = text_height(L"Back up before every Play", G.tfBtn.Get(), CW - 70);
        std::wstring sub = L"Keeps the last 10 automatic backups.";
        float h2 = text_height(sub, G.tfModSum.Get(), CW - 70);
        draw_text(L"Back up before every Play", G.tfBtn.Get(), L, cy, CW - 70, h1, rgb(0xffe7a3));
        draw_text(sub, G.tfModSum.Get(), L, cy + h1 + 2, CW - 70, h2, rgb(0xb9c4dc));
        D2D1_RECT_F sw = RectF(Rr - 52, cy + (h1 + 2 + h2) / 2 - 15, Rr, cy + (h1 + 2 + h2) / 2 + 15);
        ui_switch(sw, saves_auto_backup(), 322, visible(sw));
        cy += std::max(h1 + 2 + h2, 30.f) + 14;
    }
    if (V.backups.empty()) {
        draw_text(L"No backups yet.", G.tfStatus.Get(), L, cy, CW, 30, rgb(0xaab6cf));
        cy += 30;
    }
    for (size_t i = 0; i < V.backups.size() && i < 30; i++) {
        const SaveBackup& b = V.backups[i];
        float rh = 48;
        dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(L, cy, Rr, cy + rh), 12, 12), brush(rgb(0x121b3a, .55f)).Get());
        std::wstring when = when_text(b.when);
        if (!when.empty()) when[0] = towupper(when[0]);
        float h1 = text_height(b.label, G.tfBtn.Get(), CW - 120), h2 = text_height(when, G.tfModSum.Get(), CW - 120);
        float ty = cy + rh / 2 - (h1 + 1 + h2) / 2;
        draw_text(b.label, G.tfBtn.Get(), L + 14, ty, CW - 120, h1, rgb(0xe6ebf5));
        draw_text(when, G.tfModSum.Get(), L + 14, ty + h1 + 1, CW - 120, h2, rgb(0xb9c4dc));
        float bw = ui_button_w(L"Restore");
        button(Rr - 8 - bw, cy + rh / 2 - 17, L"Restore", 330 + (int)i, false, !locked);
        cy += rh + 8;
    }
    float content = cy + V.scroll - y + 10;
    V.content_h = content;
    dc->PopAxisAlignedClip();
    V.scroll = std::max(0.f, std::min(V.scroll, content - V.list_h));
    if (content > V.list_h + 1) {
        float th = std::max(30.f, V.list_h * V.list_h / content);
        float sy = y + (V.list_h - th) * (V.scroll / (content - V.list_h));
        dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(px + pw - 12, sy + 4, px + pw - 7, sy + th - 4), 2.5f, 2.5f),
                                 brush(rgb(0xf3cf6b, .5f)).Get());
    }
    (void)M;
}

// ---- Extras > HD Pack: sharper textures made on this PC from the player's
// own disc (not built yet: shown with its steps and "Coming soon").
static void render_hd_page(double t, Matrix3x2F M, float L, float Rr, float y, float footTop, float px, float pw) {
    ID2D1DeviceContext2* dc = G.dc.Get();
    float CW = Rr - L;
    (void)t, (void)M, (void)footTop, (void)px, (void)pw;
    std::wstring s = L"Make sharper textures from your own disc, upscaled on this PC with an AI upscaler. Everything stays on this PC: "
                     L"the pack is made from the game's art, so never share it.";
    float h = text_height(s, G.tfMSub.Get(), CW);
    draw_text(s, G.tfMSub.Get(), L, y, CW, h, rgb(0xc6d0e6));
    y += h + 16;
    const wchar_t* steps[3][2] = {{L"Pull textures from your disc", L"Read from your own game files"},
                                  {L"Upscale them on this PC", L"AI upscaler on your graphics card (Vulkan)"},
                                  {L"Turn on as a texture pack", L"Added to Mods automatically, with a switch"}};
    for (int i = 0; i < 3; i++) {
        D2D1_ROUNDED_RECT r = D2D1::RoundedRect(RectF(L, y, Rr, y + 58), 14, 14);
        dc->FillRoundedRectangle(r, brush(rgb(0x121b3a, .55f)).Get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + .5f, y + .5f, Rr - .5f, y + 57.5f), 14, 14), brush(rgb(0xf3cf6b, .3f)).Get(), 1);
        G.dc->DrawEllipse(D2D1::Ellipse(Point2F(L + 32, y + 29), 15, 15), brush(rgb(0x8f9cbd)).Get(), 2);
        std::wstring n = std::to_wstring(i + 1);
        float nh = text_height(n, G.tfBtn.Get(), 30);
        draw_text(n, G.tfBtn.Get(), L + 17, y + 29 - nh / 2, 30, nh, rgb(0xc6d0e6), DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(steps[i][0], G.tfBtn.Get(), L + 60, y + 11, CW - 70, 20, rgb(0xffe7a3, .7f));
        draw_text(steps[i][1], G.tfModSum.Get(), L + 60, y + 32, CW - 70, 20, rgb(0xb9c4dc, .7f));
        y += 58 + 8;
    }
    y += 16;
    ui_stamp(L + 4, y + 12, L"COMING SOON");
    draw_text(L"Not built yet.", G.tfMSub.Get(), L + 140, y + 3, CW - 140, 30, rgb(0xc6d0e6));
}

static void render_mods(double t, float /*mk*/) {
    ID2D1DeviceContext2* dc = G.dc.Get();
    Matrix3x2F R = root_m();
    float lin = V.reduced ? (V.mods ? 1.f : 0.f) : V.modsK;  // 0..1, linear over .8 s
    float tsec = lin * .8f;
    float open_s = V.reduced ? 1e6f : (float)(t - V.mods_t);  // since it last opened
    size_t n = g_mods.size();
    static double lastT = t;
    float dt = (float)std::min(.1, std::max(0.0, t - lastT));
    lastT = t;
    dc->SetTransform(R);

    // ---- The tab (slides off to the right as the page opens: 130%, .45 s).
    {
        float tk = V.reduced ? lin : ease_pop(clamp01(tsec / .45f));
        ComPtr<IDWriteTextLayout> word = spaced(L"EXTRAS", G.tfTab.Get(), 17 * .14f);
        DWRITE_TEXT_METRICS m;
        word->GetMetrics(&m);
        float len = m.widthIncludingTrailingWhitespace, lh = m.height;
        bool hov = V.hover == 5;
        float w = 1 + 14 + 24 + (hov ? 20 : 12);
        float h = 1 + 18 + 24 + 10 + len + (n ? 10 + 24 : 0) + 18 + 1;
        float x = W - w + w * 1.3f * tk, y = H / 2 - h / 2;
        if (tk < 1) {
            for (int i = 5; i >= 1; i--) {  // teal glow (0 0 22px; 32px hovered)
                float g = i * (hov ? 6.4f : 4.4f);
                dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(x - g, y - g, W + 40, y + h + g), 18 + g, 18 + g),
                                         brush(rgb(0x3fd0d8, hov ? .07f : .04f)).Get());
            }
            ID2D1Brush* bg = cached("modsTab", [] {
                return linear(Point2F(0, 0), Point2F(0, 1), {{0, rgb(0x26387a, .8f)}, {1, rgb(0x121b3a, .85f)}});
            });
            bg->SetTransform(Matrix3x2F::Scale(1, h) * Matrix3x2F::Translation(0, y));
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(RectF(x, y, W + 40, y + h), 18, 18);
            dc->FillRoundedRectangle(rr, bg);
            D2D1_ROUNDED_RECT bd = D2D1::RoundedRect(RectF(x + .5f, y + .5f, W + 40, y + h - .5f), 17.5f, 17.5f);
            dc->DrawRoundedRectangle(bd, brush(rgb(0xf3cf6b, .55f)).Get(), 1);
            float cx = x + 1 + 14 + 12;
            dc->SetTransform(Matrix3x2F::Translation(cx - 12, y + 1 + 18) * R);
            strokeG(G.puzzle.Get(), brush(rgb(0xffe7a3)).Get(), 2, G.round.Get());
            // MODS, read bottom to top (vertical-rl turned 180 degrees).
            float ty = y + 1 + 18 + 24 + 10;
            dc->SetTransform(Matrix3x2F::Rotation(-90) * Matrix3x2F::Translation(cx - lh / 2, ty + len) * R);
            dc->DrawTextLayout(Point2F(0, 0), word.Get(), brush(rgb(0xffe7a3)).Get());
            dc->SetTransform(R);
            if (n) {
                std::wstring c = std::to_wstring(n);
                float cw = 0, ch = text_height(c, G.tfBadge.Get(), 1000, &cw);
                float bw = std::max(24.f, cw + 12), by = ty + len + 10;
                dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(cx - bw / 2, by, cx + bw / 2, by + 24), 12, 12),
                                         brush(rgb(0x3fd0d8)).Get());
                draw_text(c, G.tfBadge.Get(), cx - bw / 2, by + 12 - ch / 2, bw, ch, rgb(0x10223a),
                          DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            if (V.focus == -2 && !V.mods) focus_ring(RectF(x, y, W - 6, y + h), 18);
        }
        V.tab = tk < .5f ? RectF(x, y, W, y + h) : RectF();
    }

    // ---- The panel: 470 wide, 24 from the top, right and bottom. Slides in
    // over .6 s, fades in over .4 s.
    float pk = V.reduced ? lin : ease_pop(clamp01(tsec / .6f));
    float po = V.reduced ? lin : clamp01(tsec / .4f);
    V.close = V.drop = V.list = RectF();
    V.switches.assign(n, RectF());
    V.summaries.assign(n, RectF());
    V.cut.assign(n, false);
    if (po <= 0) return;
    float px = 786 + (470 + 48) * (1 - pk), top = 24, bot = H - 24, pw = 470;
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                         Matrix3x2F::Identity(), po), nullptr);
    D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(RectF(px, top, px + pw, bot), 24, 24);
    auto grow = [](D2D1_ROUNDED_RECT r, float g, float dy = 0) {
        return D2D1::RoundedRect(RectF(r.rect.left - g, r.rect.top - g + dy, r.rect.right + g, r.rect.bottom + g + dy),
                                 r.radiusX + g, r.radiusY + g);
    };
    for (int i = 6; i >= 1; i--)  // drop shadow (0 24px 60px) and the purple glow (44px)
        dc->FillRoundedRectangle(grow(panel, i * 7.f, 24 * i / 6.f), brush(rgb(0x040818, .07f)).Get());
    for (int i = 5; i >= 1; i--) dc->FillRoundedRectangle(grow(panel, i * 8.f), brush(rgb(0x8b5bd2, .028f)).Get());
    dc->DrawRoundedRectangle(grow(panel, 2), brush(rgb(0x121b3a, .6f)).Get(), 4);  // the 4px ring
    ID2D1Brush* bg = cached("modsPanel", [] {
        return linear(Point2F(0, 0), Point2F(0, 1), {{0, rgb(0x1f2b4f, .95f)}, {1, rgb(0x0e1530, .97f)}});
    });
    bg->SetTransform(Matrix3x2F::Scale(1, bot - top) * Matrix3x2F::Translation(0, top));
    dc->FillRoundedRectangle(panel, bg);
    // The light sweep across it as it opens (.25 s in, 1.1 s).
    if (V.mods && !V.reduced && open_s > .25f && open_s < 1.35f) {
        float k = ease_out((open_s - .25f) / 1.1f);
        float cx = px + pw / 2 + pw * (-1.2f + 2.4f * k), cy = (top + bot) / 2;
        float dx = .966f, dy = .259f, half = 314;  // 105deg, the gradient line's half length
        ComPtr<ID2D1LinearGradientBrush> sw = linear(Point2F(cx - dx * half, cy - dy * half), Point2F(cx + dx * half, cy + dy * half),
                                                     {{.35f, ColorF(1, 1, 1, 0)}, {.5f, ColorF(1, 1, 1, .14f)}, {.65f, ColorF(1, 1, 1, 0)}});
        dc->FillRoundedRectangle(panel, sw.Get());
    }
    dc->DrawRoundedRectangle(grow(panel, -1), brush(rgb(0xf3cf6b)).Get(), 2);

    // The contents fade up one group after another (.2, .3, .4, .5 s).
    auto group = [&](float delay) {
        float k = V.mods && !V.reduced ? ease_pop(clamp01((open_s - delay) / .5f)) : 1.f;
        dc->SetTransform(Matrix3x2F::Translation(0, 14 * (1 - k)) * R);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             Matrix3x2F::Identity(), k), nullptr);
        return Matrix3x2F::Translation(0, 14 * (1 - k)) * R;
    };
    float L = px + 24, Rr = px + pw - 24, CW = Rr - L;

    // Everything clickable, gathered as it's drawn (the mouse and the focus).
    V.items.clear();

    // Head: the title and the close button.
    Matrix3x2F M = group(.2f);
    float y = top + 2 + 22;
    {
        float th = text_height(L"Extras", G.tfMTitle.Get(), 300);
        draw_text(L"Extras", G.tfMTitle.Get(), L, y + 22 - th / 2 + 3, 300, th, rgb(0x6b4410));
        draw_text(L"Extras", G.tfMTitle.Get(), L, y + 22 - th / 2, 300, th, rgb(0xffe7a3));
        D2D1_ELLIPSE c = D2D1::Ellipse(Point2F(Rr - 22, y + 22), 22, 22);
        dc->FillEllipse(c, brush(V.hover == 6 ? rgb(0x24335f) : rgb(0x121b3a, .6f)).Get());
        dc->DrawEllipse(D2D1::Ellipse(c.point, 21, 21), brush(rgb(0xf3cf6b, .6f)).Get(), 2);
        float a = 5;
        dc->DrawLine(Point2F(c.point.x - a, c.point.y - a), Point2F(c.point.x + a, c.point.y + a), brush(rgb(0xffe7a3)).Get(), 2.2f, G.round.Get());
        dc->DrawLine(Point2F(c.point.x + a, c.point.y - a), Point2F(c.point.x - a, c.point.y + a), brush(rgb(0xffe7a3)).Get(), 2.2f, G.round.Get());
        V.close = RectF(Rr - 44, y, Rr, y + 44);
        ui_reg(V.close, 6);
        ui_ring(V.close, 22, 6);
    }
    dc->PopLayer();
    y += 44 + 16;

    // The page tabs: Mods, Saves, HD Pack (the chosen one gold).
    M = group(.25f);
    {
        const wchar_t* names[3] = {L"Mods", L"Saves", L"HD Pack"};
        D2D1_ROUNDED_RECT box = D2D1::RoundedRect(RectF(L, y, Rr, y + 48), 14, 14);
        dc->FillRoundedRectangle(box, brush(rgb(0x121b3a, .7f)).Get());
        dc->DrawRoundedRectangle(grow(box, -.5f), brush(rgb(0xf3cf6b, .3f)).Get(), 1);
        float bw = (CW - 8) / 3;
        for (int i = 0; i < 3; i++) {
            D2D1_RECT_F b = RectF(L + 4 + i * bw, y + 4, L + 4 + (i + 1) * bw, y + 44);
            bool sel = V.page == i, hov = V.hover == 200 + i;
            if (sel) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(b.left, b.top + 3, b.right, b.bottom + 3), 11, 11), brush(rgb(0xa8741d)).Get());
                dc->FillRoundedRectangle(D2D1::RoundedRect(b, 11, 11),
                                         linear(Point2F(0, b.top), Point2F(0, b.bottom), {{0, rgb(0xffe7a3)}, {1, rgb(0xf3cf6b)}}).Get());
            } else if (hov) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(b, 11, 11), brush(rgb(0x24335f)).Get());
            }
            float th = text_height(names[i], G.tfSeg.Get(), bw);
            draw_text(names[i], G.tfSeg.Get(), b.left, (b.top + b.bottom) / 2 - th / 2, bw, th, sel ? rgb(0x1b2440) : rgb(0xe6ebf5),
                      DWRITE_TEXT_ALIGNMENT_CENTER);
            ui_reg(b, 200 + i);
            ui_ring(b, 11, 200 + i);
        }
    }
    dc->PopLayer();
    y += 48 + 14;

    // The footer (measured first: the page fills the space above it).
    std::wstring foot = V.page == 0 ? L"Mods are stored in %APPDATA%\\Wonderpark\\mods. Only install mods you trust."
                      : V.page == 1 ? L"Saves live in %APPDATA%\\Wonderpark\\nand (the game's own folders). Backups in %APPDATA%\\Wonderpark\\backups\\saves."
                                    : L"The HD pack is made on this PC, saved with your build on this drive, and never shared.";
    float fh = text_height(foot, G.tfMFoot.Get(), CW);
    float footTop = bot - 2 - (12 + fh + 18);

    if (V.page == 0) {
        // Installed / Get mods.
        M = group(.3f);
        {
            float x = L;
            for (int i = 0; i < 2; i++) {
                const wchar_t* s = i ? L"Get mods" : L"Installed";
                float tw = 0;
                text_height(s, G.tfBtn.Get(), 1000, &tw);
                D2D1_RECT_F b = RectF(x, y, x + tw + 28, y + 32);
                bool sel = V.modsSub == i, hov = V.hover == 210 + i;
                dc->FillRoundedRectangle(D2D1::RoundedRect(b, 16, 16), brush(sel ? rgb(0x3fd0d8) : hov ? rgb(0x24335f) : rgb(0x121b3a, .6f)).Get());
                dc->DrawRoundedRectangle(D2D1::RoundedRect(RectF(b.left + 1, b.top + 1, b.right - 1, b.bottom - 1), 15, 15),
                                         brush(sel ? rgb(0xe8fdff, .8f) : rgb(0xf3cf6b, .5f)).Get(), 1.5f);
                float th = text_height(s, G.tfBtn.Get(), 1000);
                draw_text(s, G.tfBtn.Get(), b.left, y + 16 - th / 2, tw + 28, th, sel ? rgb(0x10223a) : rgb(0xe6ebf5), DWRITE_TEXT_ALIGNMENT_CENTER);
                ui_reg(b, 210 + i);
                ui_ring(b, 16, 210 + i);
                x = b.right + 8;
            }
        }
        dc->PopLayer();
        y += 32 + 14;
    }

    if (V.page == 0 && V.modsSub == 0) {
        std::wstring sub = L"Add mods from your own .zip files. Changes apply the next time you press Play.";
        M = group(.3f);
        draw_text(sub, G.tfMSub.Get(), L, y, CW, 100, rgb(0xc6d0e6));
        dc->PopLayer();
        y += text_height(sub, G.tfMSub.Get(), CW) + 12;

        // The drop zone, the progress and any error.
        std::wstring ifile, istage, ierr;
        float iprog = 0;
        bool irun;
        {
            std::lock_guard<std::mutex> lk(g_install.m);
            irun = g_install.running;
            ifile = g_install.file, istage = g_install.stage, ierr = g_install.error, iprog = g_install.progress;
        }
        bool showProg = irun || (ierr.empty() && t - V.inst_done_t < .35);
        M = group(.3f);
        {
            D2D1_RECT_F r = RectF(L, y, Rr, y + 76);
            bool lit = !irun && (V.hover == 7 || V.drag_over);
            if (irun) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                            Matrix3x2F::Identity(), .5f), nullptr);
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, 16, 16), brush(rgb(0x3fd0d8, lit ? .14f : .06f)).Get());
            dc->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + 1, y + 1, Rr - 1, y + 75), 15, 15),
                                     brush(lit ? rgb(0x3fd0d8) : rgb(0xf3cf6b, .6f)).Get(), 2, G.dash.Get());
            dc->SetTransform(Matrix3x2F::Scale(1.25f, 1.25f) * Matrix3x2F::Translation(L + 2 + 18, y + 38 - 15) * M);
            strokeG(G.upload.Get(), brush(rgb(0x3fd0d8)).Get(), 2, G.round.Get());
            dc->SetTransform(M);
            float tx = L + 2 + 18 + 30 + 14;
            float h1 = text_height(L"Install from .zip", G.tfDrop.Get(), Rr - tx);
            std::wstring hintLine = L"Click to choose a file, or drop a .zip here";
            float h2 = text_height(hintLine, G.tfDropSmall.Get(), Rr - tx - 18);
            float ty = y + 38 - (h1 + h2) / 2;
            draw_text(L"Install from .zip", G.tfDrop.Get(), tx, ty, Rr - tx, h1, rgb(0xffe7a3));
            draw_text(hintLine, G.tfDropSmall.Get(), tx, ty + h1, Rr - tx - 18, h2, rgb(0xc6d0e6));
            if (irun) dc->PopLayer();
            V.drop = r;
            if (!irun) ui_reg(r, 7);
            ui_ring(r, 16, 7);
            y += 76;
            if (showProg) {
                y += 12;
                float rh = text_height(L"Ag", G.tfMProg.Get(), 1000);
                float bh = 1 + 14 + rh + 8 + 14 + 8 + rh + 14 + 1;
                D2D1_ROUNDED_RECT box = D2D1::RoundedRect(RectF(L, y, Rr, y + bh), 14, 14);
                dc->FillRoundedRectangle(box, brush(rgb(0x121b3a, .7f)).Get());
                dc->DrawRoundedRectangle(grow(box, -.5f), brush(rgb(0xf3cf6b, .3f)).Get(), 1);
                float ix = L + 17, iw = CW - 34, iy = y + 15;
                float pct = irun ? iprog : 100;
                std::wstring ps = std::to_wstring((int)(pct + .5f)) + L"%";
                draw_text(ps, G.tfMProg.Get(), ix, iy, iw, rh, rgb(0xe6ebf5), DWRITE_TEXT_ALIGNMENT_TRAILING);
                draw_text(ifile, G.tfMProg.Get(), ix, iy, iw - 60, rh, rgb(0xe6ebf5));
                float by = iy + rh + 8;
                D2D1_ROUNDED_RECT bar = D2D1::RoundedRect(RectF(ix + 1, by + 1, ix + iw - 1, by + 13), 6, 6);
                dc->FillRoundedRectangle(bar, brush(rgb(0x121b3a)).Get());
                float fw = (iw - 4) * clamp01(pct / 100);
                if (fw > 1)
                    dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(ix + 2, by + 2, ix + 2 + fw, by + 12), 5, 5),
                                             brush(rgb(0x3fd0d8)).Get());
                dc->DrawRoundedRectangle(bar, brush(rgb(0xf3cf6b)).Get(), 2);
                draw_text(irun ? istage : L"Done", G.tfMProg.Get(), ix, by + 14 + 8, iw, rh, rgb(0xe6ebf5));
                y += bh;
            }
            if (!irun && !ierr.empty()) {
                y += 12;
                float eh = text_height(ierr, G.tfMErr.Get(), CW);
                draw_text(ierr, G.tfMErr.Get(), L, y, CW, eh, rgb(0xffb4b4));
                y += eh;
            }
        }
        dc->PopLayer();

        // The list of installed mods (scrolls).
        M = group(.4f);
        {
            D2D1_RECT_F lr = RectF(px + 2, y, px + pw - 2, footTop);
            V.list = lr;
            ComPtr<IDWriteTextLayout> lbl = spaced(L"INSTALLED", G.tfMLabel.Get(), 11.5f * .12f);
            DWRITE_TEXT_METRICS lm;
            lbl->GetMetrics(&lm);
            float lblH = lm.height;
            const float cardH = 61, gap = 10;
            float content = 14 + 4 + lblH + gap + (n ? n * (cardH + gap) - gap : 20) + 10;
            V.list_h = footTop - y;
            V.content_h = content;
            V.scroll = std::max(0.f, std::min(V.scroll, content - V.list_h));
            dc->PushAxisAlignedClip(lr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            float cy = y + 14 - V.scroll;
            dc->DrawTextLayout(Point2F(L, cy + 4), lbl.Get(), brush(rgb(0xc6d0e6)).Get());
            cy += 4 + lblH + gap;
            if (!n) draw_text(L"No mods yet.", G.tfStatus.Get(), L, cy + 6, CW, 30, rgb(0xaab6cf));
            for (size_t i = 0; i < n; i++, cy += cardH + gap) {
                const ModInfo& md = g_mods[i];
                float want = md.enabled ? 1.f : 0.f;
                auto it = g_knob.find(md.id);
                float kv = it == g_knob.end() || V.reduced ? want : it->second;
                kv = kv < want ? std::min(want, kv + dt / .2f) : std::max(want, kv - dt / .2f);
                g_knob[md.id] = kv;
                if (kv != want) fast(.3);
                if (cy > footTop || cy + cardH < y) continue;
                bool fresh = !V.fresh.empty() && md.id == V.fresh;
                float fk = fresh && !V.reduced ? ease_pop(clamp01(since(V.fresh_t, t) / .5f)) : 1.f;
                float sc = .88f + .12f * fk;
                Matrix3x2F C = Matrix3x2F::Translation(L, cy) * Matrix3x2F::Scale(sc, sc, Point2F(L + CW / 2, cy + cardH / 2)) * M;
                if (fk < 1)
                    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                         Matrix3x2F::Identity(), fk), nullptr);
                bool hov = V.hover == 100 + (int)i || V.tip == (int)i;
                if (fresh)
                    for (int g = 4; g >= 1; g--) {
                        dc->SetTransform(Matrix3x2F::Scale(1 + g * 4.5f / 211, 1 + g * 4.5f / 30.5f, Point2F(211, 30.5f)) * C);
                        fillG(G.card.Get(), brush(rgb(0xf3cf6b, .06f)).Get());
                    }
                dc->SetTransform(Matrix3x2F::Translation(0, 3) * C);
                fillG(G.card.Get(), brush(rgb(0x080c1e, .45f)).Get());
                dc->SetTransform(C);
                ID2D1Brush* cb = cached("modCard", [] {
                    return linear(Point2F(0, 0), Point2F(0, 61), {{0, rgb(0x2c3d70, .7f)}, {1, rgb(0x1e2b52, .75f)}});
                });
                fillG(G.card.Get(), cb);
                dc->DrawLine(Point2F(14, 1.5f), Point2F(422 - 16, 1.5f), brush(ColorF(1, 1, 1, .08f)).Get(), 1);
                strokeG(G.card.Get(), brush(fresh ? rgb(0xf3cf6b) : rgb(0xf3cf6b, hov ? .6f : .32f)).Get(), 1.5f);
                float tx = 1.5f + 16, tw = 422 - 3 - 16 - 12 - 52 - 12;
                float nh = text_height(L"Ag", G.tfModName.Get(), 1000), sh = text_height(L"Ag", G.tfModSum.Get(), 1000);
                float ty = cardH / 2 - (nh + 3 + sh) / 2;
                float a = md.enabled ? 1.f : .55f;
                draw_text(md.name, G.tfModName.Get(), tx, ty, tw, nh, rgb(0xffe7a3, a));
                draw_text(md.summary, G.tfModSum.Get(), tx, ty + nh + 3, tw, sh, rgb(0xb9c4dc, a));
                float full = 0;
                text_height(md.summary, G.tfModSum.Get(), 100000, &full);
                V.cut[i] = full > tw + .5f;
                V.summaries[i] = RectF(L + tx, cy + ty + nh + 3, L + tx + tw, cy + ty + nh + 3 + sh);
                float sx = 422 - 1.5f - 12 - 52, sy = cardH / 2 - 15;
                D2D1_ROUNDED_RECT tr = D2D1::RoundedRect(RectF(sx, sy, sx + 52, sy + 30), 15, 15);
                dc->FillRoundedRectangle(tr, brush(mix(rgb(0x3a466b), rgb(0x3fd0d8), kv)).Get());
                dc->DrawRoundedRectangle(grow(tr, -1), brush(mix(rgb(0x5d6a8a), rgb(0xe8fdff), kv)).Get(), 2);
                float kx = sx + 4 + 11 + 22 * kv, ky = sy + 4 + 11;
                fill_ellipse(kx, ky + 2, 11, brush(rgb(0x10182f, .4f)).Get());
                fill_ellipse(kx, ky, 11, brush(mix(rgb(0xb6c1d6), rgb(0xf3cf6b), kv)).Get());
                draw_sparkle(C, kx - 5.5f, ky - 5.5f, 11, mix(rgb(0x7f8daa), rgb(0xa8741d), kv), 1, 1);
                dc->SetTransform(C);
                if (V.fcode == 100 + (int)i) focus_ring(RectF(sx, sy, sx + 52, sy + 30), 15);
                V.switches[i] = RectF(L + sx, cy + sy, L + sx + 52, cy + sy + 30);
                if (cy + sy >= y && cy + sy + 30 <= footTop) ui_reg(V.switches[i], 100 + (int)i);
                if (fk < 1) dc->PopLayer();
            }
            dc->PopAxisAlignedClip();
            if (content > V.list_h + 1) {
                float th = std::max(30.f, V.list_h * V.list_h / content);
                float ty = y + (V.list_h - th) * (V.scroll / (content - V.list_h));
                dc->SetTransform(M);
                dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(px + pw - 12, ty + 4, px + pw - 7, ty + th - 4), 2.5f, 2.5f),
                                         brush(rgb(0xf3cf6b, .5f)).Get());
            }
        }
        dc->PopLayer();
    } else if (V.page == 0) {
        // Get mods: a curated list of mods made and hosted by their authors
        // (wonderpark-mods-index). Not online yet.
        M = group(.3f);
        std::wstring s = L"From the Wonderpark mod list. Mods are made and hosted by their own authors; Wonderpark only lists them, "
                         L"and never lists mods that contain game files.";
        float h = text_height(s, G.tfMSub.Get(), CW);
        draw_text(s, G.tfMSub.Get(), L, y, CW, h, rgb(0xc6d0e6));
        y += h + 22;
        D2D1_ROUNDED_RECT box = D2D1::RoundedRect(RectF(L, y, Rr, y + 120), 16, 16);
        dc->FillRoundedRectangle(box, brush(rgb(0x121b3a, .55f)).Get());
        dc->DrawRoundedRectangle(grow(box, -1), brush(rgb(0xf3cf6b, .4f)).Get(), 2, G.dash.Get());
        draw_text(L"The mod list isn't online yet", G.tfModName.Get(), L + 20, y + 20, CW - 40, 30, rgb(0xffe7a3));
        ui_stamp(L + 20, y + 68, L"COMING SOON");
        std::wstring url = mods_index_url();
        std::wstring us = url.empty() ? L"List address: not set (launcher.ini, [mods] index_url)" : L"List address: " + url;
        draw_text(us, G.tfModSum.Get(), L + 20, y + 92, CW - 40, 20, rgb(0x8f9cbd));
        dc->PopLayer();
    } else {
        M = group(.3f);
        if (V.page == 1) render_saves_page(t, M, L, Rr, y, footTop, px, pw);
        else render_hd_page(t, M, L, Rr, y, footTop, px, pw);
        dc->PopLayer();
    }

    M = group(.5f);
    dc->DrawLine(Point2F(px + 2, footTop + .5f), Point2F(px + pw - 2, footTop + .5f), brush(rgb(0xf3cf6b, .2f)).Get(), 1);
    draw_text(foot, G.tfMFoot.Get(), L, footTop + 12, CW, fh, rgb(0x8f9cbd));
    dc->PopLayer();
    // The tooltip with a cut-off summary's full text.
    if (V.tip >= 0 && V.tip < (int)n && V.cut[V.tip]) {
        dc->SetTransform(R);
        const std::wstring& s = g_mods[V.tip].summary;
        float ww = 0, hh = text_height(s, G.tfTip.Get(), 300, &ww);
        float bw = std::min(300.f, ww) + 20, bh = hh + 14;
        float bx = std::min(V.tip_x, px + pw - 12 - bw), by = V.tip_y + 20;
        if (by + bh > H - 8) by = V.tip_y - 10 - bh;
        D2D1_ROUNDED_RECT tb = D2D1::RoundedRect(RectF(bx, by, bx + bw, by + bh), 10, 10);
        dc->FillRoundedRectangle(tb, brush(rgb(0x121b3a, .97f)).Get());
        dc->DrawRoundedRectangle(grow(tb, -.5f), brush(rgb(0xf3cf6b, .6f)).Get(), 1);
        draw_text(s, G.tfTip.Get(), bx + 10, by + 7, 300, hh, rgb(0xe6ebf5));
    }
    dc->PopLayer();
    dc->SetTransform(R);
}

static void render(double t, const Shared& s) {
    ID2D1DeviceContext2* dc = G.dc.Get();
    Matrix3x2F R = root_m();
    bool building = s.state == S_BUILDING, done = s.state == S_DONE;
    float tm = since(V.menu_t, t);          // since the menu opened
    float ti = since(V.start, t);           // since the window opened
    float menuK = V.menu ? clamp01(tm / 1.4f) : 0;  // the emblem's zoom transition
    float menuE = ease_pop(menuK);
    auto fade = [&](float dur, float delay = 0) { return V.menu ? clamp01((tm - delay) / dur) : 0.f; };
    // The Mods page: 0 closed .. 1 open (the emblem's shift, .8 s eased).
    float mk = V.reduced ? (V.mods ? 1.f : 0.f) : ease_pop(V.modsK);

    dc->SetTransform(Matrix3x2F::Identity());
    dc->Clear(rgb(0x060a1c));
    dc->PushAxisAlignedClip(RectF(G.ox, G.oy, G.ox + W * G.k, G.oy + H * G.k), D2D1_ANTIALIAS_MODE_ALIASED);
    dc->SetTransform(R);
    dc->FillRectangle(RectF(0, 0, W, H), brush(rgb(0x0a1029)).Get());

    // Sky and stars (parallax 8px), scaled 1.05 about the centre.
    {
        Matrix3x2F m = Matrix3x2F::Scale(1.05f, 1.05f, Point2F(640, 360)) *
                       Matrix3x2F::Translation(-8 * V.mx, -6 * V.my) * R;
        dc->SetTransform(m);
        dc->DrawBitmap(G.sky.Get(), RectF(-PAD, -PAD, W + PAD, H + PAD));
    }
    // Twinkling stars, planets, shooting star (parallax 14px).
    Matrix3x2F D = Matrix3x2F::Translation(-14 * V.mx, -10 * V.my) * R;
    {
        dc->SetTransform(D);
        float per = building ? 1.2f : 3.6f;
        float a = V.reduced ? 1 : .25f + .75f * wave(t, per);
        float b = V.reduced ? 1 : .25f + .75f * wave(t + 2.2, per);
        dc->DrawBitmap(G.twA.Get(), RectF(-PAD, -PAD, W + PAD, H + PAD), a);
        dc->DrawBitmap(G.twB.Get(), RectF(-PAD, -PAD, W + PAD, H + PAD), b);
        auto bob = [&](float period, float delay) { return V.reduced ? 0.f : -8 * wave(t - delay, period); };
        // Orange ringed planet
        dc->SetTransform(Matrix3x2F::Translation(1010, 70 + bob(9, 0)) * D);
        strokeG(G.ringBack.Get(), brush(rgb(0xffe7a3, .55f)).Get(), 5, G.round.Get());
        fill_ellipse(90, 60, 36, cached("planetA", [] {
                         return radial(Point2F(79.2f, 45.6f), 54, 54,
                                       {{0, rgb(0xffe2a0)}, {.5f, rgb(0xf39a2b)}, {1, rgb(0x7a3d0e)}});
                     }));
        strokeG(G.bandA.Get(), brush(rgb(0x7a3d0e, .35f)).Get(), 4, G.round.Get());
        strokeG(G.ringFront.Get(), brush(rgb(0xffe7a3)).Get(), 5, G.round.Get());
        // Teal planet and its moon
        dc->SetTransform(Matrix3x2F::Translation(118, 392 + bob(11, -3)) * D);
        fill_ellipse(34, 40, 26, cached("planetB", [] {
                         return radial(Point2F(26.2f, 29.6f), 39, 39,
                                       {{0, rgb(0xd9fdff)}, {.5f, rgb(0x3fd0d8)}, {1, rgb(0x14505e)}});
                     }));
        strokeG(G.bandB.Get(), brush(ColorF(1, 1, 1, .25f)).Get(), 3, G.round.Get());
        fill_ellipse(76, 14, 8, cached("moon", [] {
                         return radial(Point2F(73.6f, 10.8f), 12, 12,
                                       {{0, rgb(0xe3d2ff)}, {.6f, rgb(0x8b5bd2)}, {1, rgb(0x3b2370)}});
                     }));
        // Tiny pink planet
        dc->SetTransform(Matrix3x2F::Translation(456, 70 + bob(13, -6)) * D);
        fill_ellipse(16, 16, 14, cached("planetC", [] {
                         return radial(Point2F(11.8f, 10.4f), 21, 21,
                                       {{0, rgb(0xffc4da)}, {.55f, rgb(0xe2457f)}, {1, rgb(0x6e1838)}});
                     }));
        // Shooting star, every 9 s
        if (!V.reduced) {
            float ph = (float)std::fmod(t, 9.0) / 9;
            if (ph > .86f) {
                float k2 = ease_in((ph - .86f) / .14f);
                float op = ph < .88f ? ease_in((ph - .86f) / .02f) : 1 - ease_in((ph - .88f) / .12f);
                dc->SetTransform(Matrix3x2F::Rotation(21, Point2F(740, 25)) *
                                 Matrix3x2F::Translation(420 * k2, 160 * k2) * D);
                ID2D1Brush* sb = cached("shoot", [] {
                    return linear(Point2F(600, 0), Point2F(740, 0), {{0, ColorF(1, 1, 1, 0)}, {1, ColorF(1, 1, 1, .95f)}});
                });
                sb->SetOpacity(op);
                dc->FillRectangle(RectF(600, 24, 740, 26), sb);
            }
        }
    }
    // Light rays (title 1, menu .45 over 1 s), turning once in 90 s.
    {
        float op = 1 - .55f * fade(1);
        op = op + (.2f - op) * mk;  // .2 with the Mods page open
        float rot = V.reduced ? 0 : (float)std::fmod(t / 90 * 360, 360.0);
        dc->SetTransform(Matrix3x2F::Rotation(rot, Point2F(640, 360)) * R);
        ID2D1Brush* rb = cached("rays", [] {
            return radial(Point2F(640, 360), 615, 615,
                          {{0, rgb(0xfff4d6, .18f)}, {.277f, rgb(0xfff4d6, .18f)}, {1, rgb(0xfff4d6, 0)}});
        });
        rb->SetOpacity(op);
        for (auto& ray : G.rays) fillG(ray.Get(), rb);
    }
    // The beam: title 1, menu .55, pulsing while building, flashing when done.
    {
        float op = 1 - .45f * fade(1);
        float flash = 0;
        if (building) op = V.reduced ? 1 : .7f + .3f * wave(t, 1.1f);
        if (done) {
            op = 1;
            float f = since(V.done_t, t);
            if (f < 1.6f) flash = f < .4f ? ease_out(f / .4f) : 1 - ease_out((f - .4f) / 1.2f);
        }
        op *= 1 - mk;  // gone with the Mods page open
        flash *= 1 - mk;
        dc->SetTransform(R);
        D2D1_RECT_F r = RectF(546, -24, 734, 384);
        dc->DrawBitmap(G.beam.Get(), r, op);
        if (flash > 0) {
            dc->DrawBitmap(G.beam.Get(), r, flash);
            dc->DrawBitmap(G.beam.Get(), r, flash * .4f);
        }
    }
    // Rising motes and twinkling sparkles (parallax 26px).
    Matrix3x2F A = Matrix3x2F::Translation(-26 * V.mx, -18 * V.my) * R;
    {
        struct Mote { float x, y; uint32_t c, glow; float dur, delay; };
        static const Mote motes[] = {{160, 520, 0xffe7a3, 0xf3cf6b, 6, 0},     {300, 640, 0xcdf6f8, 0x3fd0d8, 7.5f, -2},
                                     {520, 690, 0xffe7a3, 0xf3cf6b, 8, -5},    {760, 680, 0xcdf6f8, 0x3fd0d8, 6.5f, -3.5f},
                                     {940, 600, 0xffe7a3, 0xf3cf6b, 7, -1},    {1100, 500, 0xcdf6f8, 0x3fd0d8, 8.5f, -6},
                                     {230, 330, 0xcdf6f8, 0x3fd0d8, 9, -4},    {1040, 300, 0xffe7a3, 0xf3cf6b, 7.8f, -2.6f},
                                     {610, 330, 0xfff4cf, 0xffffff, 5.5f, -1.8f}, {680, 300, 0xfff4cf, 0xffffff, 6.2f, -4.4f}};
        for (const Mote& m : motes) {
            float y = 0, sc = 1, op = 1;
            if (!V.reduced) {
                float ph = (float)std::fmod(t - m.delay, (double)m.dur) / m.dur;
                float e = ease_in(ph);
                y = -260 * e;
                sc = .6f + .4f * e;
                op = ph < .15f ? ease_in(ph / .15f) : 1 - ease_in((ph - .15f) / .85f);
            }
            dc->SetTransform(A);
            float cx = m.x + 3, cy = m.y + 3 + y;
            ID2D1Brush* gb = cached("mote" + std::to_string(m.glow), [&] {
                return radial(Point2F(0, 0), 1, 1,
                              {{0, rgb(m.glow, .8f)}, {.25f, rgb(m.glow, .5f)}, {1, rgb(m.glow, 0)}});
            });
            gb->SetTransform(Matrix3x2F::Scale(13 * sc, 13 * sc) * Matrix3x2F::Translation(cx, cy));
            gb->SetOpacity(op);
            fill_ellipse(cx, cy, 13 * sc, gb);
            fill_ellipse(cx, cy, 3 * sc, brush(rgb(m.c, op)).Get());
        }
        struct Spark { float x, y, s, d; uint32_t c; };
        static const Spark sparks[] = {{180, 90, 18, 0, 0xfff4cf},   {930, 200, 22, .8f, 0xfff4cf},
                                       {1210, 330, 16, 1.6f, 0xcdf6f8}, {260, 560, 18, 2.2f, 0xcdf6f8},
                                       {1060, 610, 20, 1.1f, 0xfff4cf}, {560, 36, 16, .4f, 0xfff4cf}};
        for (const Spark& sp : sparks) {
            float w = V.reduced || t < sp.d ? 1 : wave(t - sp.d, 2.8f);
            float op = V.reduced || t < sp.d ? 1 : .15f + .85f * w, sc = V.reduced || t < sp.d ? 1 : .6f + .4f * w;
            draw_sparkle(A, sp.x, sp.y, sp.s, rgb(sp.c), op, sc);
        }
    }

    // ---- The emblem stage (600 x 600 at 340, 60), rising into place over 1.4 s.
    float introK = V.reduced ? 1 : clamp01(ti / 1.4f);
    float introE = ease_pop(introK);
    float introOp = V.reduced ? 1 : ease_pop(clamp01(introK / .6f));
    float introS = .7f + .3f * introE;
    // With the Mods page open the stage slides left and shrinks (the mockup's
    // .shift: translateX(-250px) scale(.82) about the middle).
    Matrix3x2F SH = Matrix3x2F::Scale(1 - .18f * mk, 1 - .18f * mk, Point2F(640, 360)) *
                    Matrix3x2F::Translation(-250 * mk, 0);
    Matrix3x2F S = Matrix3x2F::Scale(introS, introS, Point2F(300, 300)) *
                   Matrix3x2F::Translation(340, 60 + 50 * (1 - introE)) * SH * R;
    // Orbiting sparkles (title only).
    {
        float op = (1 - fade(.6f)) * introOp;
        if (op > 0) {
            float rot = V.reduced ? 0 : (float)std::fmod(t / 26 * 360, 360.0);
            Matrix3x2F O = Matrix3x2F::Rotation(rot, Point2F(300, 300)) * S;
            draw_sparkle(O, 290, 4, 20, rgb(0xfff4cf), op, 1);
            draw_sparkle(O, 540, 436, 16, rgb(0xcdf6f8), op, 1);
            draw_sparkle(O, 44, 434, 18, rgb(0xffe7a3), op, 1);
        }
    }
    // The emblem: .84 on the title, 1.75 in the menu; pulsing on the title.
    float escale = .84f + (1.75f - .84f) * menuE;
    float pulse = (!V.menu && !V.reduced) ? wave(t, 3.6f) : 0;
    float pscale = 1 + .045f * pulse;
    Matrix3x2F E = Matrix3x2F::Scale(escale * pscale, escale * pscale, Point2F(300, 300)) * S;
    // Glow behind (drop-shadow): gold pulsing on the title, teal in the menu.
    {
        dc->SetTransform(Matrix3x2F::Translation(-100, -100) * E);
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        D2D1_RECT_F gr = RectF(0, 0, 800, 800);
        if (!V.menu || menuK < 1) {
            float k0 = V.menu ? 1 - menuK : 1;
            if (!V.reduced) {
                dc->FillOpacityMask(G.glow8.Get(), brush(rgb(0xffe28c, .35f * (1 - pulse) * k0 * introOp)).Get(), &gr);
                dc->FillOpacityMask(G.glow18.Get(), brush(rgb(0xffe28c, .75f * pulse * k0 * introOp)).Get(), &gr);
            }
        }
        if (V.menu) dc->FillOpacityMask(G.glow8.Get(), brush(rgb(0x3fd0d8, .45f * menuK)).Get(), &gr);
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
    if (introOp < 1) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                          Matrix3x2F::Identity(), introOp), nullptr);
    dc->SetTransform(E);
    if (!G.gold) {
        G.dc->CreateSolidColorBrush(rgb(0xc98f2a), &G.gold);
        G.dc->CreateSolidColorBrush(rgb(0xf3cf6b), &G.goldHi);
        G.dc->CreateSolidColorBrush(rgb(0x3fd0d8), &G.link);
    }
    ID2D1SolidColorBrush *gold = G.gold.Get(), *goldHi = G.goldHi.Get();
    fillG(G.star.Get(), gold);
    strokeG(G.star.Get(), goldHi, 6, G.round.Get());
    // Gems: coloured on the title; stone in the menu until their stage is done.
    {
        static const uint32_t lit[8] = {0xf27aa5, 0xc9346c, 0xfbe27a, 0xe3ad26, 0x7ad68a, 0x3f9e55, 0x79a5f0, 0x3d68c8};
        for (int i = 0; i < 8; i++) {
            float k2 = V.gem_mix[i / 2];
            D2D1_COLOR_F c = mix(rgb(i % 2 ? 0x7f8daa : 0xa7b3ca), rgb(lit[i]), k2);
            fillG(G.gems[i].Get(), brush(c).Get());
            strokeG(G.gems[i].Get(), goldHi, 2, G.round.Get());
        }
    }
    fill_ellipse(300, 300, 150, gold);
    dc->DrawEllipse(D2D1::Ellipse(Point2F(300, 300), 150, 150), goldHi, 6);
    {
        static const uint32_t wc[6] = {0xe2457f, 0xf39a2b, 0xf6d44a, 0x5bc26a, 0x4a7fe0, 0x8b5bd2};
        for (int i = 0; i < 12; i++) {
            fillG(G.wedges[i].Get(), brush(rgb(wc[i % 6])).Get());
            strokeG(G.wedges[i].Get(), goldHi, 3, G.round.Get());
        }
    }
    fill_ellipse(300, 300, 112, cached("disc", [] {
                     return radial(Point2F(300, 282.1f), 134.4f, 134.4f, {{0, rgb(0x2c3d70)}, {1, rgb(0x121b3a)}});
                 }));
    dc->DrawEllipse(D2D1::Ellipse(Point2F(300, 300), 112, 112), goldHi, 4);
    dc->DrawEllipse(D2D1::Ellipse(Point2F(300, 300), 103, 103), brush(rgb(0xf3cf6b, .45f)).Get(), 1.5f);
    // The glint sweeping across the gold every 7 s.
    if (!V.reduced) {
        float ph = (float)std::fmod(t, 7.0) / 7;
        if (ph > .6f) {
            float x = -760 + 1520 * ease_in_out((ph - .6f) / .4f);
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), G.starClip.Get()), nullptr);
            dc->SetTransform(Matrix3x2F::Rotation(25, Point2F(300, 300)) * Matrix3x2F::Translation(x, 0) * E);
            dc->FillRectangle(RectF(240, -200, 360, 800), cached("glint", [] {
                                  return linear(Point2F(240, 0), Point2F(360, 0),
                                                {{0, ColorF(1, 1, 1, 0)}, {.5f, ColorF(1, 1, 1, .5f)}, {1, ColorF(1, 1, 1, 0)}});
                              }));
            dc->PopLayer();
            dc->SetTransform(E);
        }
    }
    // Brighter on hover (title).
    if (!V.menu && V.hover == 1) fillG(G.starClip.Get(), brush(ColorF(1, 1, 1, .1f)).Get());

    // Lettering: the title's, then the menu's.
    auto lettering = [&](ID2D1Geometry* g, float sw, float drop, float op) {
        D2D1_RECT_F b;
        g->GetBounds(nullptr, &b);
        dc->SetTransform(Matrix3x2F::Translation(0, drop) * E);
        strokeG(g, brush(rgb(0x6b4410, op)).Get(), sw, G.round.Get());
        fillG(g, brush(rgb(0x6b4410, op)).Get());
        dc->SetTransform(E);
        strokeG(g, brush(rgb(0x1b2440, op)).Get(), sw, G.round.Get());
        fillG(g, brush(rgb(0x1b2440, op)).Get());
        char key[40];
        snprintf(key, sizeof(key), "gold%p", (void*)g);
        ID2D1Brush* gb = cached(key, [&] {
            return linear(Point2F(0, b.top), Point2F(0, b.bottom),
                          {{0, rgb(0xfff7d6)}, {.5f, rgb(0xf7d164)}, {1, rgb(0xd48f26)}});
        });
        gb->SetOpacity(op);
        fillG(g, gb);
        strokeG(g, brush(rgb(0xfff4cc, op)).Get(), sw * .13f);
    };
    {
        float op = 1 - (V.menu ? clamp01(tm / .5f) : 0);
        if (op > 0) {
            lettering(G.wonderT.Get(), 9, 4, op);
            lettering(G.parkT.Get(), 9, 4, op);
            dc->SetTransform(E);
            fillG(G.recompT.Get(), brush(rgb(0xcfdaf2, op)).Get());
            dc->SetTransform(Matrix3x2F::Translation(188, 188) * E);
            if (!G.titleSpark)
                G.titleSpark = polygon({{112, 38}, {114, 44}, {120, 46}, {114, 48}, {112, 54}, {110, 48}, {104, 46}, {110, 44}});
            fillG(G.titleSpark.Get(), brush(rgb(0xfff4cf, op)).Get());
        }
    }
    V.button = {};
    if (V.menu) {
        float pk = clamp01((tm - .8f) / .7f);
        float pop = V.reduced ? 1 : ease_pop(pk);
        float op = V.reduced ? 1 : pop;
        if (op > 0) {
            float ps = .88f + .12f * pop;
            Matrix3x2F P = Matrix3x2F::Scale(ps, ps, Point2F(300, 300)) * E;
            dc->SetTransform(P);
            const bool layer = op < 1;  // a layer only while fading in (layers cost)
            if (layer)
                dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     Matrix3x2F::Identity(), op), nullptr);
            lettering(G.wonderparkM.Get(), 7, 3, 1);
            dc->SetTransform(P);
            fillG(G.unofficialM.Get(), brush(rgb(0xcfdaf2)).Get());
            // The main button
            std::wstring label;
            switch (s.state) {
                case S_CHECKING: label = L"…"; break;
                case S_NOISO: label = L"Place ISO"; break;
                case S_VERIFYING: label = L"Verifying…"; break;
                case S_READY: label = s.rebuild ? L"Rebuild" : L"Build"; break;
                case S_BUILDING: label = std::to_wstring((int)s.progress) + L"%"; break;
                case S_DONE: label = L"Play"; break;
            }
            float tw = 0;
            text_height(label, G.tfButton.Get(), 1000, &tw);
            float bw = std::max(128.f, tw + 54), bh = 46;
            bool busy = s.state == S_VERIFYING || s.state == S_BUILDING || s.state == S_CHECKING;
            bool play = s.state == S_DONE;
            bool hov = V.hover == 3 && !busy;
            float press = hov && V.pressed ? 2 : 0;
            float bx = 300 - bw / 2, by = 300 - bh / 2 + press;
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(RectF(bx, by, bx + bw, by + bh), bh / 2, bh / 2);
            dc->SetTransform(P);
            if (!busy && !play)  // the dark ring around it
                dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(bx - 4, by - 4, bx + bw + 4, by + bh + 4), bh / 2 + 4,
                                                           bh / 2 + 4), brush(rgb(0x1b2440, .6f)).Get());
            if (play) {  // teal glow
                ID2D1Brush* gl = cached("playglow", [] {
                    return radial(Point2F(0, 0), 1, 1,
                                  {{0, rgb(0x3fd0d8, .6f)}, {.6f, rgb(0x3fd0d8, .3f)}, {1, rgb(0x3fd0d8, 0)}});
                });
                gl->SetTransform(Matrix3x2F::Scale(bw / 2 + 24, bh / 2 + 24) * Matrix3x2F::Translation(300, 300 + press));
                dc->FillEllipse(D2D1::Ellipse(Point2F(300, 300 + press), bw / 2 + 24, bh / 2 + 24), gl);
            }
            uint32_t bg = busy ? 0x24335f : play ? (hov ? 0x7ae4ea : 0x3fd0d8) : (hov ? 0xffd877 : 0xf2c14e);
            uint32_t sh = busy ? 0x121b3a : play ? 0x1f8c93 : 0xa8741d;
            uint32_t fg = busy ? 0xffe7a3 : play ? 0x10223a : 0x1b2440;
            D2D1_ROUNDED_RECT shr = rr;
            shr.rect.top += 4 - press;
            shr.rect.bottom += 4 - press;
            dc->FillRoundedRectangle(shr, brush(rgb(sh)).Get());
            dc->FillRoundedRectangle(rr, brush(rgb(bg)).Get());
            D2D1_ROUNDED_RECT br = rr;
            br.rect.left += 1.5f, br.rect.top += 1.5f, br.rect.right -= 1.5f, br.rect.bottom -= 1.5f;
            br.radiusX -= 1.5f, br.radiusY -= 1.5f;
            dc->DrawRoundedRectangle(br, brush(rgb(0xfff1c2)).Get(), 3);
            float th = text_height(label, G.tfButton.Get(), 1000);
            draw_text(label, G.tfButton.Get(), bx, by + (bh - th) / 2 - 1, bw, bh, rgb(fg), DWRITE_TEXT_ALIGNMENT_CENTER);
            if (layer) dc->PopLayer();
            // The button's box in window pixels, for the mouse.
            D2D1_POINT_2F a = P.TransformPoint(Point2F(bx, by)), b2 = P.TransformPoint(Point2F(bx + bw, by + bh));
            V.button = RectF(a.x, a.y, b2.x, b2.y);
        }
        // The big sparkle at the circle's top right.
        float so = V.reduced ? 1 : clamp01((tm - 1.2f) / .6f);
        if (so > 0) {
            float w2 = V.reduced ? 1 : wave(t, 2.2f);
            draw_sparkle(E, 398, 172, 30, ColorF(1, 1, 1), so * (V.reduced ? 1 : .15f + .85f * w2),
                         V.reduced ? 1 : .6f + .4f * w2);
        }
    }
    // Dimmed with the Mods page open (brightness .65).
    if (mk > 0) {
        dc->SetTransform(E);
        fillG(G.starClip.Get(), brush(rgb(0x060a1c, .35f * mk)).Get());
    }
    if (introOp < 1) dc->PopLayer();

    // ---- The "Click the logo" hint: a glass pill in the top-left corner, a
    // label only (not clickable). Glows gently; fades with the menu and the
    // Mods page.
    {
        float op = (1 - fade(.5f)) * (1 - mk);
        if (op > 0) {
            if (!V.menu && !V.reduced) op *= .78f + .22f * wave(t, 3);
            dc->SetTransform(R);
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 Matrix3x2F::Identity(), op), nullptr);
            float tw = 0, th = text_height(L"Click the logo", G.tfPill.Get(), 1000, &tw);
            float x = 32, y = 28, h = 1 + 9 + th + 9 + 1, w = 1 + 12 + 18 + 10 + tw + 18 + 1, cy = y + h / 2;
            for (int i = 5; i >= 1; i--) {  // the purple glow (box-shadow 0 0 22px)
                float g = i * 4.4f;
                dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(x - g, y - g, x + w + g, y + h + g), h / 2 + g, h / 2 + g),
                                         brush(rgb(0x8b5bd2, .055f)).Get());
            }
            D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(RectF(x, y, x + w, y + h), h / 2, h / 2);
            dc->FillRoundedRectangle(pill, brush(rgb(0x121b3a, .55f)).Get());
            D2D1_ROUNDED_RECT in = D2D1::RoundedRect(RectF(x + 2, y + 2, x + w - 2, y + h - 2), h / 2 - 2, h / 2 - 2);
            dc->DrawRoundedRectangle(in, brush(ColorF(1, 1, 1, .05f)).Get(), 3);
            D2D1_ROUNDED_RECT bd = D2D1::RoundedRect(RectF(x + .5f, y + .5f, x + w - .5f, y + h - .5f), h / 2 - .5f, h / 2 - .5f);
            dc->DrawRoundedRectangle(bd, brush(rgb(0xf3cf6b, .4f)).Get(), 1);
            float w2 = V.reduced ? 1 : wave(t, 2.2f);
            draw_sparkle(R, x + 13, cy - 9, 18, rgb(0xfff4cf), V.reduced ? 1 : .15f + .85f * w2, V.reduced ? 1 : .6f + .4f * w2);
            dc->SetTransform(R);
            draw_text(L"Click the logo", G.tfPill.Get(), x + 13 + 18 + 10, cy - th / 2, tw + 4, th, rgb(0xffe7a3));
            dc->PopLayer();
        }
    }

    // ---- The controller's buttons, top right, while a pad is connected: its
    // own icons (pad_icons_core.cpp: Xbox, PlayStation or Nintendo, from the
    // pad's maker). Fades with the Mods page, like the logo hint.
    {
        float op = (1 - mk) * (1 - fade(.5f));
        if (g_padOn && op > 0) {
            int set = pad_sdl_type(0) ? pad_sdl_type(0) : pad_icons_detected();  // the pad's own type from SDL
            struct Item { uint32_t b; const wchar_t* label; } items[] = {{0x1000, L"Choose"}, {0x8000, L"Mods"}};
            float ih = 30, gap = 8, sep = 22, x = 0, total = 0;
            std::vector<float> iw;
            for (auto& it : items) {
                float tw = 0;
                text_height(it.label, G.tfPill.Get(), 1000, &tw);
                float w = pad_icon(pad_icons_name(it.b, set), 0, 0, ih, 0, false);
                iw.push_back(w);
                total += w + gap + tw + sep;
            }
            total -= sep;
            if (iw[0] > 0) {
                dc->SetTransform(R);
                x = W - 32 - total;
                float cy = 28 + 21;
                for (size_t i = 0; i < 2; i++) {
                    pad_icon(pad_icons_name(items[i].b, set), x, cy - ih / 2, ih, op, true);
                    x += iw[i] + gap;
                    float tw = 0, th = text_height(items[i].label, G.tfPill.Get(), 1000, &tw);
                    draw_text(items[i].label, G.tfPill.Get(), x, cy - th / 2, tw + 4, th, rgb(0xffe7a3, op));
                    x += tw + sep;
                }
            }
        }
    }

    // Status and the legal line fade out with the Mods page open.
    bool uiLayer = mk > 0;
    if (uiLayer) {
        dc->SetTransform(R);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             Matrix3x2F::Identity(), 1 - mk), nullptr);
    }
    // ---- Status, bottom left (40, bottom 36, 340 wide): lines stacked upwards.
    {
        dc->SetTransform(R);
        struct Line { std::wstring text; uint32_t color; int kind; };  // kind 0 text, 1 ok (tick), 2 progress
        std::vector<Line> lines;
        switch (s.state) {
            case S_CHECKING: break;
            case S_NOISO:
                lines.push_back({L"Place your own US disc as an .iso (S2LE01)", 0xd6deef, 0});
                break;
            case S_VERIFYING: lines.push_back({L"Verifying " + file_name(s.image) + L"…", 0xffe7a3, 0}); break;
            case S_READY:
            case S_BUILDING:
            case S_DONE:
                if (!s.warn.empty()) lines.push_back({s.warn, 0xffe7a3, 0});
                if (!s.image.empty() && !(s.state == S_READY && s.rebuild))
                    lines.push_back({file_name(s.image) + L" · disc verified (S2LE01)", 0xa8f0d0, 1});
                if (s.state != S_READY) lines.push_back({L"", 0, 2});
                break;
        }
        if (!s.error.empty()) {
            auto at = s.state == S_NOISO ? lines.end() : lines.begin();
            lines.insert(at, {s.error, 0xffb4b4, 3});
        }
        float hsum = 0;
        std::vector<float> hs;
        for (auto& l : lines) {
            float h = l.kind == 2 ? 18 + 6 + 14 : text_height(l.text, G.tfStatus.Get(), l.kind == 1 ? 314 : 340);
            if (l.kind == 3 && s.error_has_log) h = text_height(l.text + L"  Show log", G.tfStatus.Get(), 340);
            hs.push_back(h);
            hsum += h;
        }
        hsum += 12 * (lines.empty() ? 0 : lines.size() - 1);
        float y = H - 36 - hsum;
        V.log_link = {};
        for (size_t i = 0; i < lines.size(); i++) {
            auto& l = lines[i];
            if (l.kind == 0) draw_text(l.text, G.tfStatus.Get(), 40, y, 340, 200, rgb(l.color));
            if (l.kind == 1) {
                dc->SetTransform(Matrix3x2F::Scale(.75f, .75f) * Matrix3x2F::Translation(40, y + hs[i] / 2 - 9) * R);
                strokeG(G.checkMark.Get(), brush(rgb(0xa8f0d0)).Get(), 2.6f, G.round.Get());
                dc->SetTransform(R);
                draw_text(l.text, G.tfStatus.Get(), 66, y, 314, 200, rgb(l.color));
            }
            if (l.kind == 3) {
                ComPtr<IDWriteTextLayout> tl;
                std::wstring t2 = s.error_has_log ? l.text + L"  Show log" : l.text;
                G.dw->CreateTextLayout(t2.c_str(), (UINT32)t2.size(), G.tfStatus.Get(), 340, 200, &tl);
                if (s.error_has_log) {
                    DWRITE_TEXT_RANGE rg = {(UINT32)l.text.size() + 2, 8};
                    tl->SetUnderline(TRUE, rg);
                    G.link->SetColor(rgb(V.hover == 4 ? 0xcdf6f8 : 0x3fd0d8));
                    tl->SetDrawingEffect(G.link.Get(), rg);
                    DWRITE_HIT_TEST_METRICS hm[4];
                    UINT32 cnt = 0;
                    tl->HitTestTextRange(rg.startPosition, rg.length, 40, y, hm, 4, &cnt);
                    if (cnt) {
                        D2D1_POINT_2F a = R.TransformPoint(Point2F(hm[0].left, hm[0].top));
                        D2D1_POINT_2F b = R.TransformPoint(Point2F(hm[0].left + hm[0].width, hm[0].top + hm[0].height));
                        V.log_link = RectF(a.x, a.y, b.x, b.y);
                    }
                }
                dc->DrawTextLayout(Point2F(40, y), tl.Get(), brush(rgb(l.color)).Get());
            }
            if (l.kind == 2) {
                std::wstring name = s.state == S_DONE ? (s.built_now ? L"Build complete" : L"Ready to play") : s.stage_name;
                draw_text(name, G.tfProgress.Get(), 40, y, 300, 30, rgb(0xe6ebf5));
                draw_text(std::to_wstring((int)s.progress) + L"%", G.tfProgress.Get(), 40, y, 340, 30, rgb(0xe6ebf5),
                          DWRITE_TEXT_ALIGNMENT_TRAILING);
                float by = y + 24;
                D2D1_ROUNDED_RECT bar = D2D1::RoundedRect(RectF(41, by + 1, 379, by + 13), 6, 6);
                dc->FillRoundedRectangle(bar, brush(rgb(0x121b3a)).Get());
                float fw = 336 * clamp01(s.progress / 100);
                if (fw > 1)
                    dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(42, by + 2, 42 + fw, by + 12), 5, 5),
                                             brush(rgb(0x3fd0d8)).Get());
                dc->DrawRoundedRectangle(bar, brush(rgb(0xf3cf6b)).Get(), 2);
            }
            y += hs[i] + 12;
        }
    }
    // Legal line, bottom right.
    {
        dc->SetTransform(R);
        std::wstring legal =
            L"Wonderpark is an unofficial, non-commercial fan project. Not affiliated with or endorsed by Nintendo, "
            L"The Pokémon Company, Creatures Inc. or Game Freak. Contains no game files; you need your own disc.";
        float h = text_height(legal, G.tfLegal.Get(), 460);
        draw_text(legal, G.tfLegal.Get(), W - 40 - 460, H - 40 - h, 460, 100, rgb(0xd0d8ea), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (uiLayer) dc->PopLayer();
    render_mods(t, mk);
    // Confetti for 4.5 s after the build finishes.
    {
        float f = since(V.done_t, t);
        if (!V.reduced && f >= 0 && f < 4.5f) {
            static const uint32_t cc[8] = {0xe2457f, 0xf39a2b, 0xf6d44a, 0x5bc26a, 0x4a7fe0, 0x8b5bd2, 0x3fd0d8, 0xffe7a3};
            for (int i = 0; i < 48; i++) {
                float dur = 2.4f + ((i * 7) % 10) * .18f, delay = ((i * 13) % 20) * .05f;
                float p = clamp01((f - delay) / dur);
                if (f < delay || p >= 1) continue;
                float x = (float)((i * 263 + 37) % 1270), y = -60 + 880 * p;
                dc->SetTransform(Matrix3x2F::Rotation(600 * p, Point2F(x + 5, 7)) * Matrix3x2F::Translation(0, y) * R);
                dc->FillRoundedRectangle(D2D1::RoundedRect(RectF(x, 0, x + 10, 14), 2, 2), brush(rgb(cc[i % 8])).Get());
            }
        }
    }
    // Toast.
    {
        float f = since(V.toast_t, t);
        if (f >= 0 && f < 2.6f) {
            float k2 = V.reduced ? 1 : ease_out(clamp01(f / .4f));
            dc->SetTransform(R);
            float tw = 0;
            float th = text_height(L"Launching Wonderpark…", G.tfToast.Get(), 1000, &tw);
            float bw = tw + 44 + 4, bh = th + 20 + 4, bx = 640 - bw / 2, by = 28 - 12 * (1 - k2);
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 Matrix3x2F::Identity(), k2), nullptr);
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(RectF(bx, by, bx + bw, by + bh), 14, 14);
            dc->FillRoundedRectangle(rr, brush(rgb(0x121b3a)).Get());
            dc->DrawRoundedRectangle(rr, brush(rgb(0x3fd0d8)).Get(), 2);
            draw_text(L"Launching Wonderpark…", G.tfToast.Get(), bx, by + 12, bw, th, rgb(0xe8fbfc), DWRITE_TEXT_ALIGNMENT_CENTER);
            dc->PopLayer();
        }
    }
    dc->SetTransform(Matrix3x2F::Identity());
    dc->PopAxisAlignedClip();
}

// ------------------------------------------------------------------------
// Screenshots (tests): the next frame rendered into a bitmap and saved as PNG.

static bool save_png(ID2D1Bitmap1* src, const std::wstring& path) {
    D2D1_SIZE_U sz = src->GetPixelSize();
    ComPtr<ID2D1Bitmap1> cpu;
    D2D1_BITMAP_PROPERTIES1 p = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(G.dc->CreateBitmap(sz, nullptr, 0, p, &cpu))) return false;
    cpu->CopyFromBitmap(nullptr, src, nullptr);
    D2D1_MAPPED_RECT m;
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &m))) return false;
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    bool ok = wic && SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr));
    if (ok) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        fr->SetSize(sz.width, sz.height);
        fr->SetPixelFormat(&fmt);
        // Opaque picture: alpha to 255.
        std::vector<BYTE> row(sz.width * 4 * sz.height);
        for (UINT y = 0; y < sz.height; y++) {
            memcpy(&row[y * sz.width * 4], m.bits + y * m.pitch, sz.width * 4);
            for (UINT x = 0; x < sz.width; x++) row[(y * sz.width + x) * 4 + 3] = 255;
        }
        ok = SUCCEEDED(fr->WritePixels(sz.height, sz.width * 4, (UINT)row.size(), row.data())) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    cpu->Unmap();
    return ok;
}

// ------------------------------------------------------------------------
// Actions

static void open_menu() {
    if (V.menu) return;
    V.menu = true;
    V.menu_t = now_s();
    fast(2.5);
}

// ---- The Mods page

static void open_mods() {
    if (V.mods) return;
    g_mods = mods_list();
    V.mods = true;
    V.mods_t = now_s();
    V.focus = 0;
    V.fcode = V.page == 0 && V.modsSub == 0 ? 7 : 200 + V.page;  // the drop zone, or the page's tab
    V.saves_t = -100;  // the Saves page reads them again
    V.scroll = 0;
    V.tip = -1;
    fast(2);
}

static void close_mods() {
    if (!V.mods) return;
    V.mods = false;
    V.fresh.clear();
    V.focus = -1;
    V.tip = -1;
    fast(1.2);
}

static void toggle_mod(size_t i) {
    if (i >= g_mods.size()) return;
    g_mods[i].enabled = !g_mods[i].enabled;
    mods_save(g_mods);
    fast(.5);
}

static void start_install(const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lk(g_install.m);
        if (g_install.running) return;
    }
    V.inst_handled = false;
    mods_install_start(path, wake);
    fast(1);
}

static std::wstring g_test_zip;  // --test-zip: the file picker answers with this

static void pick_zip() {
    {
        std::lock_guard<std::mutex> lk(g_install.m);
        if (g_install.running) return;
    }
    if (!g_test_zip.empty()) {
        start_install(g_test_zip);
        return;
    }
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC types[] = {{L"Wonderpark mod (*.zip)", L"*.zip"}, {L"All files", L"*.*"}};
    d->SetFileTypes(2, types);
    d->SetTitle(L"Install a mod from a .zip");
    if (FAILED(d->Show(g_wnd))) return;
    ComPtr<IShellItem> it;
    PWSTR path = nullptr;
    if (SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        start_install(path);
        CoTaskMemFree(path);
    }
}

// Each frame: the page's open amount, and an install's result.
static void update_mods(double t, float dt) {
    float want = V.mods ? 1.f : 0.f;
    if (V.modsK != want) {
        V.modsK = want > V.modsK ? std::min(1.f, V.modsK + dt / .8f) : std::max(0.f, V.modsK - dt / .8f);
        fast(.2);
    }
    bool fin;
    std::wstring id;
    {
        std::lock_guard<std::mutex> lk(g_install.m);
        fin = g_install.finished;
        id = g_install.new_id;
        if (g_install.running) fast(.2);
    }
    if (fin && !V.inst_handled) {
        V.inst_handled = true;
        V.inst_done_t = t;
        if (!id.empty()) {
            g_mods = mods_list();
            V.fresh = id;
            V.fresh_t = t + .25;  // pops in as the bar goes
            V.scroll = 0;
            g_knob.erase(id);
        }
        fast(1.5);
    }
}

// Keyboard and controller focus on the panel: through everything clickable,
// in the order it's drawn (V.items, the codes click() takes). Past the last
// one in view, the page scrolls on.
static void click(int h);
static void move_focus(int d) {
    int n = (int)V.items.size(), i = -1;
    for (int k = 0; k < n; k++)
        if (V.items[k].second == V.fcode) i = k;
    int j = i < 0 ? (d > 0 ? 0 : n - 1) : i + d;
    if (j >= 0 && j < n) V.fcode = V.items[j].second;
    else if (d > 0 && V.content_h > V.list_h + V.scroll) V.scroll += 60;
    else if (d < 0 && V.scroll > 0) V.scroll -= 60;
    if (j >= 0 && j < n) {  // keep it in view
        D2D1_RECT_F r = V.items[j].first;
        if (r.top >= V.list.top && r.bottom > V.list.bottom - 8) V.scroll += r.bottom - (V.list.bottom - 8);
        if (r.bottom <= V.list.bottom && r.top < V.list.top + 8 && r.top >= V.list.top - 200) V.scroll -= std::max(0.f, (V.list.top + 8) - r.top);
    }
    fast(1);
}

static void activate_focus() {
    if (V.fcode) click(V.fcode);
}

// ---- Extras > Saves: the buttons.

static std::wstring g_test_save;  // --test-save: the file pickers answer with this

static std::wstring pick_save_file(bool save, const std::wstring& name) {
    if (!g_test_save.empty()) return g_test_save;
    ComPtr<IFileDialog> d;
    if (save) {
        ComPtr<IFileSaveDialog> s;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s)))) return L"";
        d = s;
        COMDLG_FILTERSPEC types[] = {{L"Save (.zip, Dolphin's layout)", L"*.zip"}};
        d->SetFileTypes(1, types);
        d->SetDefaultExtension(L"zip");
        d->SetFileName(name.c_str());
        d->SetTitle(L"Export a save");
    } else {
        ComPtr<IFileOpenDialog> o;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&o)))) return L"";
        d = o;
        COMDLG_FILTERSPEC types[] = {{L"A Wii or Dolphin save (data.bin, .zip, GD0)", L"*.bin;*.zip;GD*;Pic*"}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, types);
        d->SetTitle(L"Import a save: a Wii's data.bin, a .zip, or a file in Dolphin's save folder");
    }
    if (FAILED(d->Show(g_wnd))) return L"";
    ComPtr<IShellItem> it;
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out = path;
        CoTaskMemFree(path);
    }
    return out;
}

static void saves_result(const std::wstring& err, const std::wstring& ok) {
    V.saves_err = !err.empty();
    V.saves_msg = err.empty() ? ok : err;
    refresh_saves();
    fast(1);
}

static void saves_click(int h) {
    if (h >= 300 && h < 320) {
        int slot = (h - 300) / 2;
        if ((h - 300) % 2 == 0) {
            saves_result(saves_backup(slot, L"File " + std::to_wstring(slot + 1), false), L"File " + std::to_wstring(slot + 1) + L" backed up.");
        } else {
            std::wstring p = pick_save_file(true, L"Wonderpark save file " + std::to_wstring(slot + 1) + L".zip");
            if (!p.empty()) saves_result(saves_export(slot, p), L"Exported to " + p + L".");
        }
    } else if (h == 320) {
        std::wstring p = pick_save_file(false, L"");
        if (!p.empty()) saves_result(saves_import(p), L"Imported. Your saves from before are in Backups.");
    } else if (h == 321) {
        CreateDirectoryW((mods_data_dir() + L"\\nand").c_str(), nullptr);
        std::wstring d = saves_dir();
        for (size_t i = 3; i <= d.size(); i++)
            if (i == d.size() || d[i] == L'\\') CreateDirectoryW(d.substr(0, i).c_str(), nullptr);
        ShellExecuteW(g_wnd, L"explore", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (h == 322) {
        saves_set_auto_backup(!saves_auto_backup());
        fast(.5);
    } else if (h >= 330 && h < 400 && (size_t)(h - 330) < V.backups.size()) {
        SaveBackup b = V.backups[h - 330];
        saves_result(saves_restore(b), L"Restored \"" + b.label + L"\". Your saves from before are in Backups.");
    }
}

// Files dragged onto the window: the page opens and the drop zone lights up;
// a dropped .zip installs.
class DropTarget : public IDropTarget {
    LONG refs = 1;
    static std::wstring first_file(IDataObject* d) {
        FORMATETC f = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM m;
        std::wstring out;
        if (SUCCEEDED(d->GetData(&f, &m))) {
            HDROP h = (HDROP)GlobalLock(m.hGlobal);
            wchar_t p[MAX_PATH * 4];
            if (h && DragQueryFileW(h, 0, p, (UINT)std::size(p))) out = p;
            GlobalUnlock(m.hGlobal);
            ReleaseStgMedium(&m);
        }
        return out;
    }
    bool ok = false;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&refs); }  // static object
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        FORMATETC f = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        ok = SUCCEEDED(d->QueryGetData(&f));
        if (ok) {
            open_mods();
            V.drag_over = true;
            fast(1);
        }
        *eff = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* eff) override {
        *eff = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        V.drag_over = false;
        fast(.5);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        V.drag_over = false;
        std::wstring p = ok ? first_file(d) : L"";
        *eff = p.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
        if (!p.empty()) start_install(p);
        return S_OK;
    }
};
static DropTarget g_drop;

static std::wstring g_test_iso;

static void pick_disc() {
    if (!g_test_iso.empty()) {
        start_verify(g_test_iso);
        return;
    }
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC types[] = {{L"Wii disc image (*.iso, *.wbfs, *.rvz, *.wia)", L"*.iso;*.wbfs;*.rvz;*.wia"},
                                 {L"All files", L"*.*"}};
    d->SetFileTypes(2, types);
    d->SetTitle(L"Place your PokéPark 2 disc image (US, S2LE01)");
    if (FAILED(d->Show(g_wnd))) return;
    ComPtr<IShellItem> it;
    PWSTR path = nullptr;
    if (SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        start_verify(path);
        CoTaskMemFree(path);
    }
}

static bool g_quit_after_toast = true;

static void launch_game(const std::wstring& exe) {
    // Extras > Saves: "Back up before every Play" (the newest 10 kept).
    if (saves_auto_backup()) saves_backup(-1, L"Before Play", true);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, g_root.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        V.toast_t = now_s();
        fast(3);
    } else {
        std::lock_guard<std::mutex> lk(g.m);
        g.error = L"Wonderpark.exe couldn't be started. Try Rebuild.";
        g.state = S_READY;
        g.rebuild = true;
    }
}

static void press_main() {
    if (!V.menu) {
        open_menu();
        return;
    }
    State st;
    std::wstring exe;
    {
        std::lock_guard<std::mutex> lk(g.m);
        st = g.state;
        exe = g.exe;
    }
    if (st == S_NOISO) pick_disc();
    else if (st == S_READY) start_build();
    else if (st == S_DONE) launch_game(exe);
}

static bool ask_cancel() {
    return MessageBoxW(g_wnd, L"Stop the build? You can start it again later.", L"Wonderpark",
                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

static State current_state() {
    std::lock_guard<std::mutex> lk(g.m);
    return g.state;
}

// ------------------------------------------------------------------------
// Test script

struct Action { double t; std::wstring what; };
static std::vector<Action> g_script;
static size_t g_script_pos;
static std::wstring g_shot;

static void parse_script(const std::wstring& s) {
    size_t p = 0;
    double last = 0;
    while (p < s.size()) {
        size_t e = s.find(L',', p);
        if (e == std::wstring::npos) e = s.size();
        std::wstring item = s.substr(p, e - p);
        p = e + 1;
        size_t c = item.find(L':');
        if (c == std::wstring::npos) continue;
        double t = item[0] == L'+' ? last + _wtof(item.c_str() + 1) : _wtof(item.c_str());
        last = t;
        g_script.push_back({t, item.substr(c + 1)});
    }
}

static void run_script(double t) {
    while (g_script_pos < g_script.size() && g_script[g_script_pos].t <= t) {
        const std::wstring& a = g_script[g_script_pos++].what;
        if (a == L"logo") open_menu();
        else if (a == L"press") press_main();
        else if (a == L"drop") { if (!g_test_iso.empty()) start_verify(g_test_iso); }  // as if --test-iso were dropped on the window
        else if (a == L"cancel") { if (current_state() == S_BUILDING) cancel_build(); }
        else if (a.rfind(L"shot=", 0) == 0) g_shot = a.substr(5);
        else if (a == L"quit") PostMessageW(g_wnd, WM_CLOSE, 0, 1);
        else if (a == L"mods") open_mods();
        else if (a == L"mods-close") close_mods();
        else if (a.rfind(L"page=", 0) == 0) click(200 + _wtoi(a.c_str() + 5));
        else if (a.rfind(L"sub=", 0) == 0) click(210 + _wtoi(a.c_str() + 4));
        else if (a.rfind(L"click=", 0) == 0) click(_wtoi(a.c_str() + 6));
        else if (a.rfind(L"install=", 0) == 0) start_install(a.substr(8));
        else if (a.rfind(L"toggle=", 0) == 0) toggle_mod((size_t)_wtoi(a.c_str() + 7));
        else if (a.rfind(L"wait-install=", 0) == 0) {
            // Holds the script until an install's progress reaches N (or it ends).
            float want = (float)_wtof(a.c_str() + 13);
            bool run;
            float pr;
            {
                std::lock_guard<std::mutex> lk(g_install.m);
                run = g_install.running;
                pr = g_install.progress;
            }
            if (run && pr < want) {
                g_script_pos--;
                double shift = t - g_script[g_script_pos].t;
                for (size_t i = g_script_pos; i < g_script.size(); i++) g_script[i].t += shift + .02;
                return;
            }
        }
        else if (a.rfind(L"wait-state=", 0) == 0) {
            // Holds the script until the state is reached (built, ready, noiso).
            std::wstring want = a.substr(11);
            State st = current_state();
            bool ok = (want == L"built" && st == S_DONE) || (want == L"ready" && st == S_READY) ||
                      (want == L"noiso" && st == S_NOISO) || (want == L"building" && st == S_BUILDING);
            if (!ok) {
                g_script_pos--;
                double shift = t - g_script[g_script_pos].t;
                for (size_t i = g_script_pos; i < g_script.size(); i++) g_script[i].t += shift + .1;
                return;
            }
        } else if (a.rfind(L"wait-progress=", 0) == 0) {
            float want = (float)_wtof(a.c_str() + 14);
            float pr;
            State st;
            {
                std::lock_guard<std::mutex> lk(g.m);
                pr = g.progress;
                st = g.state;
            }
            if (pr < want && st == S_BUILDING) {
                g_script_pos--;
                double shift = t - g_script[g_script_pos].t;
                for (size_t i = g_script_pos; i < g_script.size(); i++) g_script[i].t += shift + .1;
                return;
            }
        }
    }
}

// ------------------------------------------------------------------------
// Window

static bool g_force_reduced;

static bool reduced_motion() {
    if (g_force_reduced) return true;
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    return !anim;
}

static int hit(int x, int y) {
    float sx = (x - G.ox) / G.k, sy = (y - G.oy) / G.k;  // scene units
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };
    auto ins = [&](D2D1_RECT_F r) { return r.right > r.left && sx >= r.left && sx <= r.right && sy >= r.top && sy <= r.bottom; };
    if (V.mods) {  // the panel takes all clicks while open: what it drew as clickable
        for (auto& it : V.items)
            if (ins(it.first)) return it.second;
        return 0;
    }
    if (ins(V.tab)) return 5;
    if (in(V.log_link)) return 4;
    if (V.menu) return in(V.button) ? 3 : 0;
    float dx = sx - 640, dy = sy - 360;  // the logo button: a 340 circle at the stage centre
    if (dx * dx + dy * dy <= 170 * 170) return 1;
    return 0;
}

static void click(int h) {
    if (h == 1) open_menu();
    else if (h == 5) open_mods();
    else if (h == 6) close_mods();
    else if (h == 7) pick_zip();
    else if (h >= 200 && h < 203) {  // the Extras page tabs
        V.page = h - 200;
        V.scroll = 0;
        V.saves_t = -100;
        V.saves_msg.clear();
        fast(1);
    } else if (h == 210 || h == 211) {  // Installed / Get mods
        V.modsSub = h - 210;
        V.scroll = 0;
        fast(1);
    } else if (h >= 300 && h < 400) saves_click(h);
    else if (h >= 100 && h < 200) toggle_mod(h - 100);
    else if (h == 3) press_main();
    else if (h == 4) {
        std::wstring lp;
        {
            std::lock_guard<std::mutex> lk(g.m);
            lp = g.log_path;
        }
        if (!lp.empty()) ShellExecuteW(g_wnd, L"open", lp.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            fast(.5);
            if (G.swap && wp != SIZE_MINIMIZED) resize_targets();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {660, 400};
            return 0;
        }
        case WM_DPICHANGED: {
            RECT* r = (RECT*)lp;
            SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            fast(2);
            V.keys = false;
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            float sx = (x - G.ox) / G.k, sy = (y - G.oy) / G.k;
            V.mx = std::max(-1.f, std::min(1.f, (sx / W - .5f) * 2));
            V.my = std::max(-1.f, std::min(1.f, (sy / H - .5f) * 2));
            int h = hit(x, y);
            if (h != V.hover) V.pressed = false;
            V.hover = h;
            // A cut-off summary under the mouse shows its full text.
            int tip = -1;
            if (V.mods && sy >= V.list.top && sy <= V.list.bottom)
                for (size_t i = 0; i < V.summaries.size(); i++) {
                    D2D1_RECT_F r = V.summaries[i];
                    if (i < V.cut.size() && V.cut[i] && sx >= r.left && sx <= r.right && sy >= r.top && sy <= r.bottom)
                        tip = (int)i;
                }
            V.tip = tip;
            V.tip_x = sx;
            V.tip_y = sy;
            SetCursor(LoadCursor(nullptr, h ? IDC_HAND : IDC_ARROW));
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, w, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            V.hover = 0;
            V.pressed = false;
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursor(nullptr, V.hover ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            V.pressed = V.hover != 0;
            SetCapture(w);
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            int h = hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (V.pressed && h == V.hover && h) click(h);
            V.pressed = false;
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (V.mods) {
                V.scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 48;
                V.tip = -1;
                fast(.5);
            }
            return 0;
        case WM_KEYDOWN:
            fast(1);
            V.keys = true;
            if (V.mods) {  // the Mods page has the keyboard while open
                if (wp == VK_ESCAPE) close_mods();
                else if (wp == VK_UP || (wp == VK_TAB && GetKeyState(VK_SHIFT) < 0)) move_focus(-1);
                else if (wp == VK_DOWN || wp == VK_TAB) move_focus(1);
                else if (wp == VK_RETURN || wp == VK_SPACE) activate_focus();
                return 0;
            }
            if (wp == VK_TAB) {  // Tab focuses the MODS tab (Enter opens it)
                V.focus = V.focus == -2 ? -1 : -2;
                return 0;
            }
            if ((wp == VK_RETURN || wp == VK_SPACE) && V.focus == -2) {
                open_mods();
                return 0;
            }
            if (wp == 'M') {
                open_mods();
                return 0;
            }
            if (wp == VK_RETURN || wp == VK_SPACE) press_main();
            if (wp == VK_ESCAPE && current_state() == S_BUILDING && ask_cancel()) cancel_build();
            return 0;
        case WM_SETTINGCHANGE:
            V.reduced = reduced_motion();
            return 0;
        case WM_CLOSE:
            if (current_state() == S_BUILDING) {
                if (lp != 1 && !ask_cancel()) return 0;
                cancel_build();
            }
            DestroyWindow(w);
            return 0;
        case WM_DESTROY:
            RevokeDragDrop(w);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

// Controller: A presses the main button (or opens the menu on the title).
// Any pad, through SDL3 (pad_sdl.c, shared with the game): buttons by
// position, so the bottom face button is "A" on every pad.
static void poll_controller() {
    static uint32_t prev[4];
    static int connected = 0, wasConnected = -1;
    PadState st;
    for (int i = 0; i < 4; i++) {
        if (!pad_sdl_state(i, &st)) {
            prev[i] = 0;
            connected &= ~(1 << i);
            continue;
        }
        connected |= 1 << i;
        uint32_t down = st.buttons & ~prev[i];
        prev[i] = st.buttons;
        if (down) V.keys = true;
        if (V.mods) {  // the Mods page: d-pad moves, A chooses, B closes
            if (down & 0x0001) move_focus(-1);       // up
            if (down & 0x0002) move_focus(1);        // down
            if (down & 0x1000) activate_focus();     // A
            if (down & 0x2000) close_mods();         // B
            continue;
        }
        if (down & 0x8000 || down & 0x0008) {        // Y or d-pad right: the Mods page
            open_mods();
            continue;
        }
        if (down & 0x1000) press_main();  // A
    }
    if (connected != wasConnected) {
        // A pad came or went: its buttons shown (top right), its type looked at again.
        if (wasConnected >= 0) pad_icons_hotplug();
        wasConnected = connected;
        g_padOn = connected != 0 || _wgetenv(L"WONDERPARK_TEST_PAD");  // (tests: as if a pad were connected)
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);  // COM (STA) plus drag and drop
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t* root = nullptr;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--root" && i + 1 < argc) root = argv[++i];
        else if (a == L"--test-iso" && i + 1 < argc) g_test_iso = argv[++i];
        else if (a == L"--test-zip" && i + 1 < argc) g_test_zip = argv[++i];
        else if (a == L"--test-save" && i + 1 < argc) g_test_save = argv[++i];
        else if (a == L"--test-script" && i + 1 < argc) parse_script(argv[++i]);
        else if (a == L"--reduced-motion") g_force_reduced = true;
    }
    g_root = find_root(root);
    find_python();
    g_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {};
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &jl, sizeof(jl));
    pad_sdl_init();         // controllers (SDL3)
    pad_icons_core_init();  // the controller icons and the pad's type, on a thread of their own

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(6, 10, 28));
    wc.lpszClassName = L"WonderparkLauncher";
    // The app icon (launcher.rc, id 1) at the sizes the title bar and taskbar want.
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    RECT r = {0, 0, (LONG)(1280 * dpi / 96), (LONG)(720 * dpi / 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (ww > wa.right - wa.left || wh > wa.bottom - wa.top) {  // fit the screen, keeping 16:9
        float s = std::min((wa.right - wa.left) * .92f / ww, (wa.bottom - wa.top) * .92f / wh);
        ww = (int)(ww * s);
        wh = (int)(wh * s);
    }
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"Wonderpark", WS_OVERLAPPEDWINDOW,
                            wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh,
                            nullptr, nullptr, inst, nullptr);
    if (!load_fonts() || !create_device()) {
        MessageBoxW(nullptr, L"The launcher couldn't start its graphics (Direct2D).", L"Wonderpark", MB_ICONERROR);
        return 1;
    }
    build_geometry();
    resize_targets();
    V.reduced = reduced_motion();
    V.start = now_s();
    g_mods = mods_list();  // the tab's count
    RegisterDragDrop(g_wnd, &g_drop);
    if (g_root.empty()) {
        std::lock_guard<std::mutex> lk(g.m);
        g.state = S_NOISO;
        g.error = L"The Wonderpark folder wasn't found next to the launcher.";
    } else {
        check_status();
    }
    ShowWindow(g_wnd, show);

    State shown = S_CHECKING;
    int shownLit = 0;
    MSG msg;
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        double t = now_s();
        run_script(t - V.start);
        poll_controller();
        Shared s;
        {
            std::lock_guard<std::mutex> lk(g.m);
            s.state = g.state;
            s.image = g.image;
            s.error = g.error;
            s.error_has_log = g.error_has_log;
            s.warn = g.warn;
            s.rebuild = g.rebuild;
            s.stage = g.stage;
            s.lit = g.lit;
            s.progress = g.progress;
            s.stage_name = g.stage_name;
            s.exe = g.exe;
            s.built_now = g.built_now;
        }
        if (s.state == S_DONE && shown != S_DONE && s.built_now) {
            V.done_t = t;
            V.fast_until = std::max(V.fast_until, t + 4.6);  // confetti
        }
        if (s.state != shown || s.lit != shownLit) V.fast_until = std::max(V.fast_until, t + 1.2);
        shown = s.state;
        shownLit = s.lit;
        // 30 frames a second for the slow ambient animation; 60 for two
        // seconds after input and during transitions; with reduced motion,
        // 20 (nothing moves, so only changes show).
        // With reduced motion nothing moves: draw only after a change (input,
        // a new state, a resize: fast_until) and once a second otherwise.
        double interval = V.reduced ? (t < V.fast_until ? .05 : 1.0) : t < V.fast_until ? 1.0 / 60 : 1.0 / 30;
        if (t - V.last_render < interval - .004) {
            DWORD ms = (DWORD)std::max(1.0, (interval - (t - V.last_render)) * 1000);
            MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);
            continue;
        }
        V.last_render = t;
        // Gems: coloured on the title; in the menu, lit as stages finish (1 s fades).
        float dt = (float)std::min(.1, t - V.last_frame);
        V.last_frame = t;
        for (int i = 0; i < 4; i++) {
            float want = !V.menu ? 1.f : (i < shownLit ? 1.f : 0.f);
            if (V.reduced) V.gem_mix[i] = want;
            else if (V.gem_mix[i] < want) V.gem_mix[i] = std::min(want, V.gem_mix[i] + dt);
            else V.gem_mix[i] = std::max(want, V.gem_mix[i] - dt);
        }
        update_mods(t, dt);
        if (IsIconic(g_wnd)) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            continue;
        }
        double tr0 = now_s();
        G.dc->SetTarget(G.back.Get());
        G.dc->BeginDraw();
        render(t, s);
        double tr1 = now_s();
        HRESULT hr = G.dc->EndDraw();
        double tr2 = now_s();
        if (!g_shot.empty()) {
            ComPtr<ID2D1Bitmap1> off = make_target(G.pw, G.ph);
            G.dc->SetTarget(off.Get());
            G.dc->BeginDraw();
            render(t, s);
            G.dc->EndDraw();
            save_png(off.Get(), g_shot);
            G.dc->SetTarget(G.back.Get());
            g_shot.clear();
        }
        if (hr == D2DERR_RECREATE_TARGET) {
            G = Gfx();  // device lost: start the graphics again
            if (!load_fonts() || !create_device()) break;
            build_geometry();
            resize_targets();
            continue;
        }
        G.swap->Present(1, 0);
        {
            // WONDERPARK_LAUNCHER_STATS=<file>: per-frame times, averaged, written at exit.
            static double sumR, sumE, sumP;
            static int frames;
            static wchar_t statsPath[MAX_PATH] = L"?";
            if (statsPath[0] == L'?' && !GetEnvironmentVariableW(L"WONDERPARK_LAUNCHER_STATS", statsPath, MAX_PATH))
                statsPath[0] = 0;
            if (statsPath[0]) {
                sumR += tr1 - tr0, sumE += tr2 - tr1, sumP += now_s() - tr2, frames++;
                if (FILE* f = _wfopen(statsPath, L"w")) {
                    fprintf(f, "%d frames: render %.3f ms, EndDraw %.3f ms, Present %.3f ms\n", frames,
                            sumR / frames * 1000, sumE / frames * 1000, sumP / frames * 1000);
                    fclose(f);
                }
            }
        }
        // A launched game: close a little after the toast.
        if (V.toast_t > 0 && t - V.toast_t > 2.6 && g_quit_after_toast) PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
    if (current_state() == S_BUILDING) cancel_build();
    join_worker();
    mods_install_join();
    OleUninitialize();
    return 0;
}
