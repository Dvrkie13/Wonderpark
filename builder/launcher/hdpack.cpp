// The HD Pack: see hdpack.h.
//
// 1. The upscaler, downloaded once and checked (SHA-256 pinned below).
// 2. Every texture on the disc: the .dac archives (LZ11-packed U8 archives),
//    .brres, .efb and .tpl files hold BRRES TEX0 (with PLT0 palettes) and TPL
//    textures. Each is named by the hash the game's renderer gives it
//    (runtime/gpu/gx_fifo.c, texture_for_map): its image bytes as they sit
//    in memory, the first n mipmap levels (the material's LOD limit picks n,
//    so every n is listed), XORed with the hash of its palette's bytes. The
//    hash function is a copy of gx_fifo.c's hash_bytes.
// 3. Each one decoded (runtime/gpu/gx_texture.c), upscaled 4x by the model
//    (alpha on its own, as grey), saved as textures\<hash>.png; the other
//    hashes of the same texture go in textures\aliases.txt
//    (runtime/host/mods.cpp reads both).
// Skipped: textures under 16 pixels on a side (too small to gain), over 512
// (already large; 4x would be 2048+), and palette textures whose palette
// isn't found.
#include "hdpack.h"

#include "mods.h"

#include <windows.h>
#include <bcrypt.h>
#include <urlmon.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

extern "C" int gx_texture_size(int fmt, int width, int height);
extern "C" void gx_decode_texture(uint8_t* out, const uint8_t* src, int fmt, int width, int height, const uint8_t* tlut,
                                  int tlut_fmt);

HdBuild g_hd;

namespace {

// ------------------------------------------------------------------------
// The upscaler's files (checked 2026-11-19)

const wchar_t* kModelUrl =
    L"https://github.com/Phhofm/models/releases/download/4xNomosUni_span_multijpg/"
    L"4xNomosUni_span_multijpg_fp32_opset17.onnx";
const char* kModelSha = "a435b009109e72c50ce95927dab0a6dde63e594cf57ba5a18ba63da67355698a";
const wchar_t* kModelFile = L"4xNomosUni_span_multijpg_fp32_opset17.onnx";
const wchar_t* kOrtUrl = L"https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.24.4";
const char* kOrtSha = "57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681";
const char* kModelNotice =
    "4xNomosUni_span_multijpg by Philip Hofmann (Phhofm), CC BY 4.0\r\n"
    "https://github.com/Phhofm/models/releases/tag/4xNomosUni_span_multijpg\r\n"
    "https://creativecommons.org/licenses/by/4.0/\r\n"
    "Downloaded by the Wonderpark launcher to make the HD Pack on this PC.\r\n";

std::atomic<bool> g_cancel{false};
std::thread g_thread;
void (*g_wake)() = nullptr;

void set_stage(const std::wstring& s, float progress) {
    {
        std::lock_guard<std::mutex> lk(g_hd.m);
        g_hd.stage = s;
        g_hd.progress = progress;
    }
    if (g_wake) g_wake();
}

struct Stop {
    std::wstring msg;
};

// In the Wonderpark folder, beside the build (gigabytes: not in %APPDATA%,
// which is usually on the system drive): hd\upscaler\ and hd\textures\.
// The mod's own folder in the data folder holds only mod.json, pack.ini and
// textures.txt (the textures' folder, for the game).
std::wstring g_hd_root;
std::wstring hd_dir() { return g_hd_root + L"\\hd"; }
std::wstring tool_dir() { return hd_dir() + L"\\upscaler"; }
std::wstring tex_dir() { return hd_dir() + L"\\textures"; }
std::wstring pack_dir() { return mods_dir() + L"\\hd-pack"; }

bool read_file(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart < (1LL << 31);
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

bool write_file(const std::wstring& path, const void* data, size_t n) {
    std::wstring tmp = path + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data, (DWORD)n, &put, nullptr) && put == n;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void make_dirs(const std::wstring& path) {
    for (size_t i = 3; i <= path.size(); i++)
        if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
}

std::string sha256_hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    uint8_t out[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0);
            BCryptFinishHash(h, out, 32, 0);
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (uint8_t b : out) s += hex[b >> 4], s += hex[b & 15];
    return s;
}

// url to path, kept only if its SHA-256 is sha.
void download(const wchar_t* url, const std::wstring& path, const char* sha, const wchar_t* what) {
    std::wstring tmp = path + L".download";
    DeleteFileW(tmp.c_str());
    if (FAILED(URLDownloadToFileW(nullptr, url, tmp.c_str(), 0, nullptr)))
        throw Stop{std::wstring(L"Couldn't download ") + what + L". Check your internet connection and try again."};
    std::vector<uint8_t> d;
    if (!read_file(tmp, d) || sha256_hex(d) != sha) {
        DeleteFileW(tmp.c_str());
        throw Stop{std::wstring(L"The download of ") + what +
                   L" wasn't the expected file (its checksum didn't match), so it wasn't used."};
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) throw Stop{L"Couldn't save the download."};
}

bool file_sha_is(const std::wstring& path, const std::string& sha) {
    std::vector<uint8_t> d;
    return read_file(path, d) && sha256_hex(d) == sha;
}

void get_upscaler() {
    std::wstring dir = tool_dir();
    make_dirs(dir);
    std::wstring model = dir + L"\\" + kModelFile;
    if (!file_sha_is(model, kModelSha)) {
        set_stage(L"Downloading the upscaler's model (1.7 MB)…", 1);
        download(kModelUrl, model, kModelSha, L"the upscaler's model");
        write_file(dir + L"\\4xNomosUni_span_multijpg-LICENSE.txt", kModelNotice, strlen(kModelNotice));
    }
    // onnxruntime.dll is checked against the hash noted when it was taken
    // out of the checked package.
    std::string noted;
    {
        std::vector<uint8_t> n;
        if (read_file(dir + L"\\onnxruntime.sha256", n)) noted.assign(n.begin(), n.end());
    }
    if (noted.size() == 64 && file_sha_is(dir + L"\\onnxruntime.dll", noted)) return;
    set_stage(L"Downloading ONNX Runtime (12 MB)…", 2);
    std::wstring pkg = dir + L"\\onnxruntime.nupkg";
    download(kOrtUrl, pkg, kOrtSha, L"ONNX Runtime");
    struct Part {
        const char* in;
        const wchar_t* out;
    } parts[] = {{"runtimes/win-x64/native/onnxruntime.dll", L"onnxruntime.dll"},
                 {"runtimes/win-x64/native/onnxruntime_providers_shared.dll", L"onnxruntime_providers_shared.dll"},
                 {"LICENSE", L"onnxruntime-LICENSE.txt"},
                 {"ThirdPartyNotices.txt", L"onnxruntime-ThirdPartyNotices.txt"}};
    std::string dll_sha;
    for (auto& p : parts) {
        std::vector<uint8_t> d;
        if (!mods_zip_read(pkg, p.in, d) || !write_file(dir + L"\\" + p.out, d.data(), d.size())) {
            DeleteFileW(pkg.c_str());
            throw Stop{L"Couldn't unpack ONNX Runtime."};
        }
        if (wcscmp(p.out, L"onnxruntime.dll") == 0) dll_sha = sha256_hex(d);
    }
    DeleteFileW(pkg.c_str());
    write_file(dir + L"\\onnxruntime.sha256", dll_sha.data(), dll_sha.size());
}

// ------------------------------------------------------------------------
// ONNX Runtime, through its C API's function table (OrtApi, version 24).
// The positions are those of onnxruntime_c_api.h 1.24.4; the table only
// grows at its end between versions.

enum {
    kGetErrorMessage = 2,
    kCreateEnv = 3,
    kCreateSession = 7,
    kRun = 9,
    kCreateSessionOptions = 10,
    kSetSessionExecutionMode = 13,
    kDisableMemPattern = 17,
    kSetSessionGraphOptimizationLevel = 23,
    kSessionGetInputName = 36,
    kSessionGetOutputName = 37,
    kCreateTensorWithDataAsOrtValue = 49,
    kGetTensorMutableData = 51,
    kGetDimensionsCount = 61,
    kGetDimensions = 62,
    kGetTensorTypeAndShape = 65,
    kCreateCpuMemoryInfo = 69,
    kAllocatorFree = 76,
    kGetAllocatorWithDefaultOptions = 78,
    kReleaseEnv = 92,
    kReleaseStatus = 93,
    kReleaseMemoryInfo = 94,
    kReleaseSession = 95,
    kReleaseValue = 96,
    kReleaseTensorTypeAndShapeInfo = 99,
    kReleaseSessionOptions = 100,
    kGetExecutionProviderApi = 195,
};

typedef void* S;  // an OrtStatus*: null is success

struct Ort {
    HMODULE dll = nullptr;
    void* const* api = nullptr;
    void *env = nullptr, *session = nullptr, *mem = nullptr, *alloc = nullptr;
    std::string in_name, out_name;
    bool gpu = false;

    template <typename F>
    F fn(int i) const { return (F)api[i]; }
    void check(S st, const wchar_t* what) {
        if (!st) return;
        std::string m = fn<const char* (*)(S)>(kGetErrorMessage)(st);
        fn<void (*)(S)>(kReleaseStatus)(st);
        std::wstring w(m.begin(), m.end());
        throw Stop{std::wstring(L"The upscaler couldn't ") + what + L" (" + w + L")."};
    }
    void load(const std::wstring& dir) {
        dll = LoadLibraryExW((dir + L"\\onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) throw Stop{L"ONNX Runtime couldn't be loaded on this PC."};
        struct Base {
            void* const* (*GetApi)(uint32_t);
            const char* (*GetVersionString)();
        };
        auto get = (const Base* (*)())GetProcAddress(dll, "OrtGetApiBase");
        if (!get || !(api = get()->GetApi(24))) throw Stop{L"ONNX Runtime on this PC is a version Wonderpark can't use."};
        check(fn<S (*)(int, const char*, void**)>(kCreateEnv)(3, "wonderpark", &env), L"start");
        check(fn<S (*)(int, int, void**)>(kCreateCpuMemoryInfo)(1, 0, &mem), L"start");
        check(fn<S (*)(void**)>(kGetAllocatorWithDefaultOptions)(&alloc), L"start");
    }
    // A session on the graphics card (DirectML) if it can, else the processor.
    void open(const std::wstring& model, bool try_gpu) {
        if (session) fn<void (*)(void*)>(kReleaseSession)(session), session = nullptr;
        void* opts = nullptr;
        check(fn<S (*)(void**)>(kCreateSessionOptions)(&opts), L"start");
        fn<S (*)(void*, int)>(kSetSessionGraphOptimizationLevel)(opts, 99);
        gpu = false;
        if (try_gpu) {
            const void* dml = nullptr;
            S st = fn<S (*)(const char*, uint32_t, const void**)>(kGetExecutionProviderApi)("DML", 24, &dml);
            if (!st && dml) {
                // DirectML wants these (ONNX Runtime's DirectML notes).
                fn<S (*)(void*)>(kDisableMemPattern)(opts);
                fn<S (*)(void*, int)>(kSetSessionExecutionMode)(opts, 0);
                S st2 = ((S(*)(void*, int))((void* const*)dml)[0])(opts, 0);
                if (!st2) gpu = true;
                else fn<void (*)(S)>(kReleaseStatus)(st2);
            } else if (st) {
                fn<void (*)(S)>(kReleaseStatus)(st);
            }
        }
        S st = fn<S (*)(void*, const wchar_t*, void*, void**)>(kCreateSession)(env, model.c_str(), opts, &session);
        fn<void (*)(void*)>(kReleaseSessionOptions)(opts);
        if (st && gpu) {  // the graphics card couldn't take it: the processor
            fn<void (*)(S)>(kReleaseStatus)(st);
            open(model, false);
            return;
        }
        check(st, L"load its model");
        char* name = nullptr;
        check(fn<S (*)(void*, size_t, void*, char**)>(kSessionGetInputName)(session, 0, alloc, &name), L"load its model");
        in_name = name;
        fn<S (*)(void*, void*)>(kAllocatorFree)(alloc, name);
        check(fn<S (*)(void*, size_t, void*, char**)>(kSessionGetOutputName)(session, 0, alloc, &name), L"load its model");
        out_name = name;
        fn<S (*)(void*, void*)>(kAllocatorFree)(alloc, name);
    }
    // planar RGB (3 x h x w, 0-1) in, 4x planar RGB out (*ow, *oh).
    std::vector<float> run(std::vector<float>& in, int w, int h, int* ow, int* oh) {
        int64_t shape[4] = {1, 3, h, w};
        void* tin = nullptr;
        check(fn<S (*)(void*, void*, size_t, const int64_t*, size_t, int, void**)>(kCreateTensorWithDataAsOrtValue)(
                  mem, in.data(), in.size() * sizeof(float), shape, 4, 1, &tin),
              L"take a texture");
        const char* in_names[1] = {in_name.c_str()};
        const char* out_names[1] = {out_name.c_str()};
        void* tout = nullptr;
        S st = fn<S (*)(void*, void*, const char* const*, void* const*, size_t, const char* const*, size_t, void**)>(kRun)(
            session, nullptr, in_names, &tin, 1, out_names, 1, &tout);
        fn<void (*)(void*)>(kReleaseValue)(tin);
        check(st, L"upscale a texture");
        void* info = nullptr;
        size_t nd = 0;
        int64_t dims[8] = {};
        check(fn<S (*)(void*, void**)>(kGetTensorTypeAndShape)(tout, &info), L"upscale a texture");
        fn<S (*)(void*, size_t*)>(kGetDimensionsCount)(info, &nd);
        if (nd == 4) fn<S (*)(void*, int64_t*, size_t)>(kGetDimensions)(info, dims, 4);
        fn<void (*)(void*)>(kReleaseTensorTypeAndShapeInfo)(info);
        float* p = nullptr;
        fn<S (*)(void*, void**)>(kGetTensorMutableData)(tout, (void**)&p);
        std::vector<float> out;
        if (nd == 4 && dims[1] == 3 && p) {
            *oh = (int)dims[2], *ow = (int)dims[3];
            out.assign(p, p + (size_t)3 * dims[2] * dims[3]);
        }
        fn<void (*)(void*)>(kReleaseValue)(tout);
        if (out.empty()) throw Stop{L"The upscaler gave back something unexpected."};
        return out;
    }
};

// ------------------------------------------------------------------------
// Textures on the disc

uint64_t hash64(const uint8_t* p, size_t n) {  // gx_fifo.c's hash_bytes64, byte for byte
    const uint64_t m = 0xFF51AFD7ED558CCDull;
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ (uint64_t)n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull,
             h3 = 0x27D4EB2F165667C5ull, h;
    size_t i;
    for (i = 0; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        h0 = (h0 ^ w[0]) * m;
        h1 = (h1 ^ w[1]) * m;
        h2 = (h2 ^ w[2]) * m;
        h3 = (h3 ^ w[3]) * m;
        h0 ^= h0 >> 29;
        h1 ^= h1 >> 29;
        h2 ^= h2 >> 29;
        h3 ^= h3 >> 29;
    }
    h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * m;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
uint32_t hash32(const uint8_t* p, size_t n) {
    uint64_t h = hash64(p, n);
    return (uint32_t)(h ^ (h >> 32));
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

struct Tex {
    std::string where;
    int fmt = 0, w = 0, h = 0, pal_fmt = 0;
    std::vector<uint8_t> image;    // level 0
    std::vector<uint8_t> palette;  // its palette's bytes, as loaded
    std::vector<uint32_t> hashes;  // first 1, 2, ... levels
};

bool known_format(int f) { return f <= 6 || (f >= 8 && f <= 10) || f == 14; }

// The texture at d[off], fmt, w x h, levels of them, with its palette.
void add_tex(std::vector<Tex>& out, const std::string& where, const std::vector<uint8_t>& d, size_t off, int fmt, int w,
             int h, int levels, const uint8_t* pal, size_t pal_bytes, int pal_fmt) {
    if (!known_format(fmt) || w <= 0 || h <= 0 || w > 1024 || h > 1024) return;
    bool palettized = fmt >= 8 && fmt <= 10;
    if (palettized && (!pal || !pal_bytes)) return;
    Tex t;
    t.where = where, t.fmt = fmt, t.w = w, t.h = h, t.pal_fmt = pal_fmt;
    size_t want = fmt == 8 ? 32 : 512;
    if (palettized) t.palette.assign(pal, pal + std::min(want, pal_bytes));
    uint32_t pal_hash = palettized ? hash32(t.palette.data(), t.palette.size()) : 0;
    size_t size = 0;
    int lw = w, lh = h;
    for (int n = 0; n < std::max(1, std::min(levels, 11)); n++) {
        size += (size_t)gx_texture_size(fmt, lw, lh);
        lw = std::max(1, lw / 2), lh = std::max(1, lh / 2);
        if (off + size > d.size()) break;
        t.hashes.push_back(hash32(&d[off], size) ^ pal_hash);
    }
    if (t.hashes.empty()) return;
    size_t l0 = (size_t)gx_texture_size(fmt, w, h);
    t.image.assign(&d[off], &d[off] + l0);
    out.push_back(std::move(t));
}

bool lz11(const std::vector<uint8_t>& d, std::vector<uint8_t>& out) {
    if (d.size() < 8) return false;
    size_t size = d[1] | d[2] << 8 | d[3] << 16, i = 4;
    if (!size) size = d[4] | d[5] << 8 | d[6] << 16 | (size_t)d[7] << 24, i = 8;
    if (size > (256u << 20)) return false;
    out.clear();
    out.reserve(size);
    while (out.size() < size && i < d.size()) {
        uint8_t flags = d[i++];
        for (int b = 0; b < 8 && out.size() < size; b++) {
            if (!(flags & (0x80 >> b))) {
                if (i >= d.size()) return false;
                out.push_back(d[i++]);
                continue;
            }
            if (i + 1 >= d.size()) return false;
            size_t n, disp;
            int t = d[i] >> 4;
            if (t == 0) {
                if (i + 2 >= d.size()) return false;
                n = (((d[i] & 15) << 4) | (d[i + 1] >> 4)) + 0x11;
                disp = (((d[i + 1] & 15) << 8) | d[i + 2]) + 1;
                i += 3;
            } else if (t == 1) {
                if (i + 3 >= d.size()) return false;
                n = (((d[i] & 15) << 12) | (d[i + 1] << 4) | (d[i + 2] >> 4)) + 0x111;
                disp = (((d[i + 2] & 15) << 8) | d[i + 3]) + 1;
                i += 4;
            } else {
                n = t + 1;
                disp = (((d[i] & 15) << 8) | d[i + 1]) + 1;
                i += 2;
            }
            if (disp > out.size()) return false;
            for (size_t k = 0; k < n; k++) out.push_back(out[out.size() - disp]);
        }
    }
    return out.size() == size;
}

std::string cstr(const std::vector<uint8_t>& d, int64_t o) {
    std::string s;
    while (o >= 0 && (size_t)o < d.size() && d[(size_t)o] && s.size() < 128) s += (char)d[(size_t)o++];
    return s;
}

void scan(const std::vector<uint8_t>& din, const std::string& where, std::vector<Tex>& out, int depth) {
    if (depth > 4 || din.size() < 16) return;
    std::vector<uint8_t> unpacked;
    const std::vector<uint8_t>* dp = &din;
    if (din[0] == 0x11) {
        if (!lz11(din, unpacked)) return;
        dp = &unpacked;
    }
    const std::vector<uint8_t>& d = *dp;
    if (d.size() < 16) return;
    uint32_t magic = be32(&d[0]);
    if (magic == 0x55AA382D) {  // U8 archive
        uint32_t root = be32(&d[4]);
        if ((size_t)root + 12 > d.size()) return;
        uint32_t total = be32(&d[root + 8]);
        size_t strs = root + (size_t)total * 12;
        if (strs > d.size()) return;
        for (uint32_t k = 1; k < total; k++) {
            const uint8_t* e = &d[root + (size_t)k * 12];
            if (e[0] != 0) continue;
            uint32_t off = be32(e + 4), size = be32(e + 8);
            if ((uint64_t)off + size > d.size()) continue;
            std::vector<uint8_t> sub(d.begin() + off, d.begin() + off + size);
            scan(sub, where + "/" + cstr(d, (int64_t)strs + (be32(e) & 0xFFFFFF)), out, depth + 1);
        }
        return;
    }
    if (magic == 0x0020AF30) {  // TPL
        uint32_t n = be32(&d[4]), tab = be32(&d[8]);
        for (uint32_t k = 0; k < n && (size_t)tab + 8 * (k + 1) <= d.size(); k++) {
            uint32_t ih = be32(&d[tab + 8 * k]), ph = be32(&d[tab + 8 * k + 4]);
            if ((size_t)ih + 0x24 > d.size()) continue;
            int hgt = be16(&d[ih]), wid = be16(&d[ih + 2]), fmt = (int)be32(&d[ih + 4]);
            uint32_t off = be32(&d[ih + 8]);
            int minlod = d[ih + 0x21], maxlod = d[ih + 0x22];
            const uint8_t* pal = nullptr;
            size_t pal_bytes = 0;
            int pal_fmt = 0;
            if (ph && (size_t)ph + 12 <= d.size()) {
                uint32_t cnt = be16(&d[ph]), poff = be32(&d[ph + 8]);
                pal_fmt = (int)be32(&d[ph + 4]);
                if ((size_t)poff + cnt * 2 <= d.size()) pal = &d[poff], pal_bytes = cnt * 2;
            }
            add_tex(out, where + "#" + std::to_string(k), d, off, fmt, wid, hgt, maxlod >= minlod ? maxlod - minlod + 1 : 1,
                    pal, pal_bytes, pal_fmt);
        }
        return;
    }
    // BRRES or anything else: PLT0 and TEX0 sections by their magic.
    struct Pal {
        size_t off, bytes;
        int fmt;
    };
    std::unordered_map<std::string, Pal> pals;
    for (size_t i = 0; i + 0x20 <= d.size(); i += 4) {
        if (memcmp(&d[i], "PLT0", 4)) continue;
        uint32_t size = be32(&d[i + 4]), ver = be32(&d[i + 8]);
        if ((ver != 1 && ver != 3) || !size || size > d.size() - i) continue;
        int32_t doff = (int32_t)be32(&d[i + 0x10]), noff = (int32_t)be32(&d[i + 0x14]);
        size_t cnt = be16(&d[i + 0x1C]);
        if (doff <= 0 || i + doff + cnt * 2 > d.size()) continue;
        pals[cstr(d, (int64_t)i + noff)] = {i + doff, cnt * 2, (int)be32(&d[i + 0x18])};
    }
    for (size_t i = 0; i + 0x40 <= d.size(); i += 4) {
        if (memcmp(&d[i], "TEX0", 4)) continue;
        uint32_t size = be32(&d[i + 4]), ver = be32(&d[i + 8]);
        if ((ver != 1 && ver != 3) || !size || size > d.size() - i) continue;
        int32_t doff = (int32_t)be32(&d[i + 0x10]), noff = (int32_t)be32(&d[i + 0x14]);
        int w = be16(&d[i + 0x1C]), h = be16(&d[i + 0x1E]), fmt = (int)be32(&d[i + 0x20]);
        int levels = (int)be32(&d[i + 0x24]);
        if (doff <= 0 || levels < 1 || levels > 11) continue;
        std::string name = cstr(d, (int64_t)i + noff);
        const uint8_t* pal = nullptr;
        size_t pal_bytes = 0;
        int pal_fmt = 0;
        auto it = pals.find(name);
        if (it != pals.end()) pal = &d[it->second.off], pal_bytes = it->second.bytes, pal_fmt = it->second.fmt;
        add_tex(out, where + ":" + name, d, i + doff, fmt, w, h, levels, pal, pal_bytes, pal_fmt);
    }
}

void walk(const std::wstring& dir, std::vector<std::wstring>& files) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring p = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (n != L"Movie" && n != L"Sound") walk(p, files);
            continue;
        }
        size_t dot = n.find_last_of(L'.');
        std::wstring ext = dot == std::wstring::npos ? L"" : n.substr(dot);
        for (auto& c : ext) c = (wchar_t)towlower(c);
        if (ext == L".dac" || ext == L".brres" || ext == L".tpl" || ext == L".arc" || ext == L".efb" || ext == L".bin")
            files.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ------------------------------------------------------------------------
// Pictures

bool save_png(IWICImagingFactory* wic, const std::wstring& path, const std::vector<uint8_t>& bgra, int w, int h) {
    std::wstring tmp = path + L".new";
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    bool ok = SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
              SUCCEEDED(fr->SetSize(w, h));
    if (ok) {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(fr->SetPixelFormat(&pf)) && pf == GUID_WICPixelFormat32bppBGRA &&
             SUCCEEDED(fr->WritePixels(h, w * 4, (UINT)bgra.size(), (BYTE*)bgra.data())) && SUCCEEDED(fr->Commit()) &&
             SUCCEEDED(enc->Commit());
    }
    fr.Reset(), enc.Reset(), st.Reset();
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::wstring hex8(uint32_t h) {
    wchar_t b[9];
    swprintf(b, 9, L"%08x", h);
    return b;
}

uint8_t to8(float v) { return (uint8_t)std::max(0.f, std::min(255.f, v * 255.f + .5f)); }

// ------------------------------------------------------------------------
// The build

void build(std::wstring root, int limit) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        std::wstring files_dir = root + L"\\game\\files";
        if (GetFileAttributesW(files_dir.c_str()) == INVALID_FILE_ATTRIBUTES)
            throw Stop{L"Build the game from your disc first: the HD Pack is made from its files."};
        get_upscaler();

        set_stage(L"Reading textures from your disc…", 4);
        std::vector<std::wstring> files;
        walk(files_dir, files);
        std::vector<Tex> texs;
        for (size_t i = 0; i < files.size(); i++) {
            if (g_cancel) throw Stop{L"Stopped."};
            std::vector<uint8_t> d;
            if (!read_file(files[i], d)) continue;
            std::string where;  // for messages only (the disc's names are ASCII)
            for (size_t c = files_dir.size() + 1; c < files[i].size(); c++) where += (char)files[i][c];
            scan(d, where, texs, 0);
            if (i % 8 == 0) set_stage(L"Reading textures from your disc…", 4 + 6.f * i / files.size());
        }

        // One picture per texture (the same one may be in several files);
        // the other hashes it may be drawn by point at it.
        std::vector<Tex*> todo;
        std::unordered_map<uint32_t, uint32_t> alias;  // other hash -> the picture's hash
        std::set<uint32_t> named;
        for (auto& t : texs) {
            if (t.w < 16 || t.h < 16 || t.w > 512 || t.h > 512) continue;
            uint32_t main = t.hashes.back();
            if (named.count(main)) continue;
            named.insert(main);
            todo.push_back(&t);
        }
        for (Tex* t : todo)
            for (uint32_t h : t->hashes)
                if (h != t->hashes.back() && !named.count(h) && !alias.count(h)) alias[h] = t->hashes.back();
        if (limit > 0 && (int)todo.size() > limit) todo.resize(limit);

        std::wstring tdir = tex_dir();
        make_dirs(tdir);
        make_dirs(pack_dir());
        {
            // The game opens files with the ANSI code page's names.
            char a[1024] = {0};
            WideCharToMultiByte(CP_ACP, 0, tdir.c_str(), -1, a, sizeof(a) - 1, nullptr, nullptr);
            std::string s = std::string(a) + "\r\n";
            write_file(pack_dir() + L"\\textures.txt", s.data(), s.size());
        }
        const char* mod_json =
            "{\"id\": \"hd-pack\", \"name\": \"HD Pack\", \"summary\": \"The game's textures upscaled 4x on this PC from "
            "your own disc. Made from the game's art: never share it.\", \"version\": \"1\", \"author\": \"Made by the "
            "Wonderpark launcher (upscaler: 4xNomosUni_span_multijpg by Philip Hofmann, CC BY 4.0)\", \"game\": "
            "\"S2LE01\"}\r\n";
        write_file(pack_dir() + L"\\mod.json", mod_json, strlen(mod_json));

        set_stage(L"Starting the upscaler…", 10);
        Ort ort;
        ort.load(tool_dir());
        ort.open(tool_dir() + L"\\" + kModelFile, true);
        {
            std::lock_guard<std::mutex> lk(g_hd.m);
            g_hd.device = ort.gpu ? L"your graphics card" : L"the processor";
            g_hd.total = (int)todo.size();
        }
        ComPtr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
            throw Stop{L"Windows couldn't save pictures."};

        int made = 0;
        for (size_t k = 0; k < todo.size(); k++) {
            if (g_cancel) throw Stop{L"Stopped. Press Build again to go on where it stopped."};
            Tex& t = *todo[k];
            std::wstring out = tdir + L"\\" + hex8(t.hashes.back()) + L".png";
            if (GetFileAttributesW(out.c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::vector<uint8_t> rgba((size_t)t.w * t.h * 4);
                gx_decode_texture(rgba.data(), t.image.data(), t.fmt, t.w, t.h,
                                  t.palette.empty() ? nullptr : t.palette.data(), t.pal_fmt);
                size_t n = (size_t)t.w * t.h;
                std::vector<float> in(3 * n);
                bool alpha = false;
                for (size_t i = 0; i < n; i++) {
                    for (int c = 0; c < 3; c++) in[c * n + i] = rgba[4 * i + c] / 255.f;
                    if (rgba[4 * i + 3] != 255) alpha = true;
                }
                int ow = 0, oh = 0;
                std::vector<float> up;
                try {
                    up = ort.run(in, t.w, t.h, &ow, &oh);
                } catch (Stop&) {
                    if (!ort.gpu) throw;
                    ort.open(tool_dir() + L"\\" + kModelFile, false);  // the graphics card failed: the processor
                    {
                        std::lock_guard<std::mutex> lk(g_hd.m);
                        g_hd.device = L"the processor";
                    }
                    up = ort.run(in, t.w, t.h, &ow, &oh);
                }
                size_t on = (size_t)ow * oh;
                std::vector<float> upa;
                if (alpha) {
                    for (size_t i = 0; i < n; i++) in[i] = in[n + i] = in[2 * n + i] = rgba[4 * i + 3] / 255.f;
                    int aw = 0, ah = 0;
                    upa = ort.run(in, t.w, t.h, &aw, &ah);
                    if (aw != ow || ah != oh) upa.clear();
                }
                std::vector<uint8_t> bgra(on * 4);
                for (size_t i = 0; i < on; i++) {
                    bgra[4 * i + 0] = to8(up[2 * on + i]);
                    bgra[4 * i + 1] = to8(up[on + i]);
                    bgra[4 * i + 2] = to8(up[i]);
                    bgra[4 * i + 3] = upa.empty() ? 255 : to8((upa[i] + upa[on + i] + upa[2 * on + i]) / 3);
                }
                if (!save_png(wic.Get(), out, bgra, ow, oh)) throw Stop{L"Couldn't save the upscaled textures."};
            }
            made++;
            {
                std::lock_guard<std::mutex> lk(g_hd.m);
                g_hd.done = (int)k + 1;
                g_hd.made = made;
                g_hd.progress = 10 + 89.f * (k + 1) / std::max<size_t>(1, todo.size());
                g_hd.stage = L"Upscaling textures";
            }
            if (g_wake) g_wake();
        }

        // aliases.txt: "<hash> <picture's hash>", for the pictures made.
        std::set<uint32_t> have;
        for (Tex* t : todo) have.insert(t->hashes.back());
        std::string al = "; Other hashes the same textures are drawn by (Wonderpark's HD Pack)\r\n";
        for (auto& a : alias)
            if (have.count(a.second)) {
                char line[32];
                snprintf(line, sizeof(line), "%08x %08x\r\n", a.first, a.second);
                al += line;
            }
        write_file(tdir + L"\\aliases.txt", al.data(), al.size());
        std::string info = "[pack]\r\ntextures = " + std::to_string(made) + "\r\n";
        write_file(pack_dir() + L"\\pack.ini", info.data(), info.size());

        // On in mods.ini (installed but not listed would be on too; this keeps it).
        auto mods = mods_list();
        mods_save(mods);
        set_stage(L"Done", 100);
    } catch (Stop& s) {
        std::lock_guard<std::mutex> lk(g_hd.m);
        g_hd.error = s.msg;
    }
    {
        std::lock_guard<std::mutex> lk(g_hd.m);
        g_hd.running = false;
        g_hd.finished = true;
    }
    if (g_wake) g_wake();
    CoUninitialize();
}

bool remove_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) != 0;
}

}  // namespace

void hd_start(const std::wstring& root, int limit, void (*wake)()) {
    hd_join();
    {
        std::lock_guard<std::mutex> lk(g_hd.m);
        g_hd.running = true;
        g_hd.finished = false;
        g_hd.error.clear();
        g_hd.stage = L"Starting…";
        g_hd.device.clear();
        g_hd.progress = 0;
        g_hd.done = g_hd.total = g_hd.made = 0;
    }
    g_cancel = false;
    g_wake = wake;
    g_hd_root = root;
    g_thread = std::thread(build, root, limit);
}

void hd_cancel() { g_cancel = true; }

void hd_join() {
    if (g_thread.joinable()) g_thread.join();
}

bool hd_installed(int* textures) {
    std::vector<uint8_t> d;
    if (!read_file(pack_dir() + L"\\pack.ini", d)) return false;
    std::string s(d.begin(), d.end());
    size_t p = s.find("textures = ");
    if (textures) *textures = p == std::string::npos ? 0 : atoi(s.c_str() + p + 11);
    return true;
}

bool hd_remove(const std::wstring& root) {
    hd_join();
    g_hd_root = root;
    bool ok = remove_tree(pack_dir());
    remove_tree(tex_dir());
    auto mods = mods_list();  // without it now
    mods_save(mods);
    return ok;
}
