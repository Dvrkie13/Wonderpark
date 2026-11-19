// Mods: reads mods.ini and the enabled mods at start; serves replacement
// disc files (dvd.c) and replacement textures (gx_fifo.c). See host_mods.h.
//
// Everything that touches the disk at run time happens on a loader thread:
// the renderer asks for a texture, gets "being loaded", and asks again on
// later frames (D: is a spinning drive; never block the game or renderer).
#include "host_mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>

extern "C" const char* wp_data_dir(void);
extern "C" int wp_log_on(const char* cat);

namespace {

struct ModFile {
    std::string path;
    uint64_t size;
};

std::unordered_map<std::string, ModFile> g_files;    // "common/x.arc" (lower case) -> file
std::unordered_map<uint32_t, std::string> g_texPath;  // hash -> image file

struct Tex {
    int state = 1;  // 1 loading, 2 ready, 0 failed
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
};
std::mutex g_m;
std::condition_variable g_cv;
std::unordered_map<uint32_t, Tex> g_tex;
std::deque<uint32_t> g_queue;
bool g_thread_started;

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool safe_id(const std::string& id) {
    if (id.empty() || id[0] == '.') return false;
    for (char c : id)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    return true;
}

bool is_dir(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Every file under dir, as (path relative to dir with '/', size).
void walk(const std::string& dir, const std::string& rel, int depth,
          std::vector<std::pair<std::string, uint64_t>>& out) {
    if (depth > 16) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\" + rel + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // no links out of the mod
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk(dir, rel + n + "\\", depth + 1, out);
        else {
            std::string r = rel + n;
            std::replace(r.begin(), r.end(), '\\', '/');
            out.push_back({r, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow});
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// The enabled mods in mods.ini's order.
std::vector<std::string> enabled_mods(const std::string& data) {
    std::vector<std::string> ids;
    FILE* f = fopen((data + "\\mods.ini").c_str(), "rb");
    if (!f) return ids;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.size() >= 3 && (uint8_t)s[0] == 0xEF) s = s.substr(3);  // UTF-8 BOM
        if (s.empty() || s[0] == ';' || s[0] == '#' || s[0] == '[') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string id = trim(s.substr(0, eq)), v = trim(s.substr(eq + 1));
        if (!safe_id(id) || std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
        if (v == "1" || lower(v) == "on" || lower(v) == "true") ids.push_back(id);
        else ids.push_back("\x01" + id);  // listed but off: keeps its place, never used
    }
    fclose(f);
    ids.erase(std::remove_if(ids.begin(), ids.end(), [](const std::string& s) { return s[0] == '\x01'; }), ids.end());
    return ids;
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// ---- Decoding (the loader thread), through WIC: PNG, BMP, JPEG.
bool decode(IWICImagingFactory* wic, const std::string& path, Tex& t) {
    int n = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &w[0], n);
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    UINT W = 0, H = 0;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(w.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&W, &H)) && W && H && W <= 8192 && H <= 8192 &&
        SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        t.rgba.resize((size_t)W * H * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, W * 4, (UINT)t.rgba.size(), t.rgba.data()));
        t.w = (int)W, t.h = (int)H;
    }
    if (conv) conv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    return ok;
}

void loader() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    for (;;) {
        uint32_t hash;
        std::string path;
        {
            std::unique_lock<std::mutex> lk(g_m);
            g_cv.wait(lk, [] { return !g_queue.empty(); });
            hash = g_queue.front();
            g_queue.pop_front();
            path = g_texPath[hash];
        }
        Tex t;
        bool ok = wic && decode(wic, path, t);
        if (!ok) fprintf(stderr, "mods: couldn't read the texture %s\n", path.c_str());
        else if (wp_log_on("mods")) fprintf(stderr, "[mods] texture %08x: %s (%dx%d)\n", hash, path.c_str(), t.w, t.h);
        std::lock_guard<std::mutex> lk(g_m);
        Tex& slot = g_tex[hash];
        slot.rgba.swap(t.rgba);
        slot.w = t.w, slot.h = t.h;
        slot.state = ok ? 2 : 0;
    }
}

}  // namespace

extern "C" void wp_mods_init(void) {
    const char* off = getenv("WONDERPARK_NO_MODS");
    if (off && *off && strcmp(off, "0") != 0) {
        fprintf(stderr, "mods: off (WONDERPARK_NO_MODS)\n");
        return;
    }
    std::string data = wp_data_dir();
    std::vector<std::string> ids = enabled_mods(data);
    if (ids.empty()) return;
    for (const std::string& id : ids) {
        std::string root = data + "\\mods\\" + id;
        if (!is_dir(root)) {
            fprintf(stderr, "mods: %s is in mods.ini but not installed\n", id.c_str());
            continue;
        }
        std::vector<std::pair<std::string, uint64_t>> list;
        walk(root, "", 0, list);
        // textures.txt: the folder the mod's textures are kept in instead
        // of its textures\ (the HD Pack keeps its gigabytes beside the
        // player's build, not in %APPDATA%).
        std::string ext_dir;
        std::unordered_set<std::string> ext_names;
        if (FILE* tf = fopen((root + "\\textures.txt").c_str(), "rb")) {
            char b[1024] = {0};
            if (fgets(b, sizeof(b), tf)) ext_dir = trim(b);
            fclose(tf);
            if (!ext_dir.empty() && is_dir(ext_dir)) {
                std::vector<std::pair<std::string, uint64_t>> more;
                walk(ext_dir, "", 0, more);
                for (auto& e : more) {
                    list.push_back({"textures/" + e.first, e.second});
                    ext_names.insert("textures/" + e.first);
                }
            }
        }
        auto path_of = [&](const std::string& r) {
            std::string p = ext_names.count(r) ? ext_dir + "\\" + r.substr(9) : root + "\\" + r;
            std::replace(p.begin(), p.end(), '/', '\\');
            return p;
        };
        int nf = 0, nt = 0;
        std::vector<std::string> alias_files;
        for (auto& e : list) {
            const std::string& r = e.first;
            if (r.compare(0, 6, "files/") == 0 && r.size() > 6) {
                std::string key = lower(r.substr(6));
                if (g_files.count(key)) continue;  // an earlier mod wins
                std::string p = root + "\\" + r;
                std::replace(p.begin(), p.end(), '/', '\\');
                g_files[key] = {p, e.second};
                nf++;
            } else if (r.compare(0, 9, "textures/") == 0) {
                std::string name = r.substr(r.find_last_of('/') + 1);
                if (lower(name) == "aliases.txt") {  // read once all its pictures are known (below)
                    alias_files.push_back(path_of(r));
                    continue;
                }
                std::string ext = lower(name.substr(name.find_last_of('.') + 1));
                if (name.size() < 12 || (ext != "png" && ext != "bmp" && ext != "jpg" && ext != "jpeg")) continue;
                uint32_t hash = 0;
                bool hex = true;
                for (int i = 0; i < 8; i++) {
                    int v = hexval(name[i]);
                    if (v < 0) hex = false;
                    hash = hash << 4 | (uint32_t)(v & 15);
                }
                if (!hex || (name[8] != '.' && name[8] != '_')) continue;
                if (g_texPath.count(hash)) continue;
                g_texPath[hash] = path_of(r);
                nt++;
            }
        }
        // textures\aliases.txt: "<hash> <hash>" lines, the first drawn with
        // the second's picture (the HD Pack: the same texture by each of its
        // mipmap counts). Read at start, like the rest of the index.
        int na = 0;
        for (const std::string& af : alias_files) {
            FILE* f = fopen(af.c_str(), "rb");
            if (!f) continue;
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                unsigned a = 0, b = 0;
                if (line[0] == ';' || sscanf(line, "%x %x", &a, &b) != 2) continue;
                auto it = g_texPath.find(b);
                if (it == g_texPath.end() || g_texPath.count(a)) continue;
                g_texPath[a] = it->second;
                na++;
            }
            fclose(f);
        }
        fprintf(stderr, "mods: %s on: %d file%s, %d texture%s%s\n", id.c_str(), nf, nf == 1 ? "" : "s", nt,
                nt == 1 ? "" : "s", na ? (" (+" + std::to_string(na) + " aliases)").c_str() : "");
    }
    if (!g_texPath.empty()) {
        g_thread_started = true;
        std::thread t(loader);
        SetThreadPriority(t.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
        t.detach();
    }
}

extern "C" int wp_mod_file(const char* path, char* out, size_t n, uint64_t* size) {
    if (g_files.empty()) return 0;
    auto it = g_files.find(lower(path));
    if (it == g_files.end()) return 0;
    snprintf(out, n, "%s", it->second.path.c_str());
    *size = it->second.size;
    return 1;
}

extern "C" int wp_mods_have_textures(void) { return !g_texPath.empty(); }

extern "C" int wp_mod_texture(uint32_t hash, const uint8_t** rgba, int* w, int* h) {
    if (!g_thread_started || !g_texPath.count(hash)) return 0;  // the index never changes after init
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_tex.find(hash);
    if (it == g_tex.end()) {
        g_tex[hash];  // state 1: loading
        g_queue.push_back(hash);
        g_cv.notify_one();
        return 1;
    }
    if (it->second.state != 2) return it->second.state;
    *rgba = it->second.rgba.data();  // never changes or moves once ready
    *w = it->second.w;
    *h = it->second.h;
    return 2;
}
