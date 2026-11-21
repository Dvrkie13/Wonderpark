// The launcher's music (music.h): songs the player has unlocked in the game's
// Jukebox, read from their own disc's files at run time. Nothing bundled.
//
// The Game sound rule: the launcher calls music_allow(true) only once the
// build from the player's verified disc has finished (its state Play: the
// pipeline says the build is current, or one just finished); anything else
// (no disc, verifying, building, a failed, stopped or deleted build) is
// music_allow(false) and silence. Nothing plays before a song has been heard
// in the game (jukebox_heard.ini, written by the game): a fresh build stays
// silent, so the first music anyone hears is inside the game.
//
// What plays: the "♥ Launcher" songs if any are marked (jukebox_launcher.ini),
// else every song heard, shuffled, round again. Titles from the game's
// jukebox_titles.ini. Quiet (30%), faded in over 2 s, paused while the
// launcher is in the background, muted by its speaker button (remembered in
// launcher.ini, [music] muted), stopped for good when Play is pressed.
//
// The songs are CRI ADX in files/Sound/*BGM*_streamfiles.awb, found and
// decoded as the game's Jukebox does (runtime/host/jukebox.cpp, the same
// steps): read in small pieces on a thread of its own, out through waveOut.
#include "music.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace {

struct Song {
    std::wstring path;
    std::string key;  // <bank>:<index>
    uint64_t data = 0, end = 0;
    int ch = 2, rate = 32000;
    uint32_t samples = 0;
    int coef1 = 0, coef2 = 0;
};

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

bool read_at(HANDLE h, uint64_t pos, void* out, uint32_t n) {
    OVERLAPPED o = {};
    o.Offset = (DWORD)pos;
    o.OffsetHigh = (DWORD)(pos >> 32);
    DWORD got = 0;
    return ReadFile(h, out, n, &got, &o) && got == n;
}

std::string narrow(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], (int)s.size(), nullptr, nullptr);
    return s;
}
std::wstring widen(const std::string& s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], (int)w.size());
    return w;
}

// The songs in one archive (as jukebox.cpp scan_file).
void scan_file(const std::wstring& path, const std::string& bank, std::vector<Song>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    uint64_t size = (uint64_t)sz.QuadPart, from = 0;
    std::vector<uint8_t> buf(0x4000);
    int index = 0;
    while (from + 0x40 < size) {
        uint32_t n = (uint32_t)std::min<uint64_t>(buf.size(), size - from);
        if (!read_at(h, from, buf.data(), n)) break;
        int64_t at = -1;
        for (uint32_t i = 0; i + 0x40 <= n && at < 0; i++) {
            if (buf[i] != 0x80 || buf[i + 1] != 0) continue;
            uint32_t c = be16(&buf[i + 2]);
            if (c < 0x20 || c > 0x1000) continue;
            if (i + c - 2 + 6 <= n && !memcmp(&buf[i + c - 2], "(c)CRI", 6)) at = i;
            else if (i + c - 2 + 6 > n) {
                uint8_t cr[6];
                if (read_at(h, from + i + c - 2, cr, 6) && !memcmp(cr, "(c)CRI", 6)) at = i;
            }
        }
        if (at < 0) break;
        uint64_t hp = from + (uint64_t)at;
        uint8_t hd[0x40];
        if (!read_at(h, hp, hd, sizeof(hd))) break;
        Song s;
        int copy = be16(&hd[2]);
        s.ch = hd[7];
        s.rate = (int)be32(&hd[8]);
        s.samples = be32(&hd[12]);
        int highpass = be16(&hd[16]);
        s.data = hp + copy + 4;
        uint64_t frames = (s.samples + 31) / 32;
        s.end = s.data + frames * 18 * std::max(1, s.ch);
        if (hd[4] == 3 && hd[5] == 18 && hd[6] == 4 && s.ch >= 1 && s.ch <= 2 && s.rate > 0 && s.samples > (uint32_t)s.rate / 2) {
            double x = std::sqrt(2.0) - std::cos(2 * 3.14159265358979 * highpass / s.rate), y = std::sqrt(2.0) - 1;
            double c = (x - std::sqrt((x + y) * (x - y))) / y;
            s.coef1 = (int)std::floor(c * 8192);
            s.coef2 = (int)std::floor(c * c * -4096);
            s.key = bank + ":" + std::to_string(index++);
            s.path = path;
            out.push_back(s);
        }
        from = std::max(hp + 0x40, s.end > 0x100 ? s.end - 0x100 : hp + 0x40);
    }
    CloseHandle(h);
}

// The data folder's small text files, as lines (UTF-8).
std::vector<std::string> lines_of(const std::wstring& path) {
    std::vector<std::string> out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    std::string cur;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else cur += (char)c;
    }
    if (!cur.empty()) out.push_back(cur);
    fclose(f);
    return out;
}

std::wstring g_root, g_data;
std::atomic<bool> g_allowed{false}, g_active{true}, g_played{false}, g_quit{false}, g_skip{false};
std::atomic<bool> g_muted{false};
std::mutex g_m;
std::wstring g_title;  // the song playing
std::thread g_thread;
HWAVEOUT g_wo;
const int RATE = 32000, BUF_FRAMES = RATE / 4, NBUF = 4;
WAVEHDR g_hdr[NBUF];
std::vector<int16_t> g_buf[NBUF];
HANDLE g_done;

std::wstring ini() { return g_data + L"\\launcher.ini"; }

// The songs that may play, read when the music is first allowed.
std::vector<Song> playlist(std::map<std::string, std::wstring>& titles) {
    std::vector<Song> all, out;
    std::wstring dir = g_root + L"\\game\\files\\Sound\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*BGM*_streamfiles.awb").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            std::wstring bank = n.substr(0, n.size() - wcslen(L"_streamfiles.awb"));
            scan_file(dir + n, narrow(bank), all);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // Heard: the offsets the game read (rounded to 2 KB) in each music file.
    std::map<std::string, std::set<uint64_t>> heard;
    for (auto& l : lines_of(g_data + L"\\jukebox_heard.ini")) {
        size_t eq = l.find('=');
        if (eq == std::string::npos || l[0] == ';' || l[0] == '[') continue;
        auto& set = heard[l.substr(0, eq)];
        const char* p = l.c_str() + eq + 1;
        while (*p) {
            char* e;
            unsigned long long v = strtoull(p, &e, 10);
            if (e == p) break;
            set.insert(v);
            p = *e == ',' ? e + 1 : e;
        }
    }
    std::set<std::string> fav;
    for (auto& l : lines_of(g_data + L"\\jukebox_launcher.ini"))
        if (!l.empty() && l[0] != ';' && l[0] != '[') fav.insert(l);
    for (auto& l : lines_of(g_data + L"\\jukebox_titles.ini")) {
        size_t eq = l.find('=');
        if (eq != std::string::npos && l[0] != ';' && l[0] != '[') titles[l.substr(0, eq)] = widen(l.substr(eq + 1));
    }
    std::vector<Song> heardSongs;
    for (auto& s : all) {
        std::string bank = s.key.substr(0, s.key.find(':'));
        auto it = heard.find(bank);
        if (it == heard.end()) continue;
        auto o = it->second.lower_bound(s.data & ~0x7FFull);
        if (o != it->second.end() && *o < s.end) heardSongs.push_back(s);
    }
    for (auto& s : heardSongs)
        if (fav.count(s.key)) out.push_back(s);
    return out.empty() ? heardSongs : out;
}

void play_thread() {
    std::map<std::string, std::wstring> titles;
    std::vector<Song> list = playlist(titles);
    if (list.empty()) return;  // nothing unlocked yet: silent
    WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0};
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, (DWORD_PTR)g_done, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) return;
    for (int b = 0; b < NBUF; b++) {
        g_buf[b].assign(BUF_FRAMES * 2, 0);
        g_hdr[b] = {};
        g_hdr[b].lpData = (LPSTR)g_buf[b].data();
        g_hdr[b].dwBufferLength = BUF_FRAMES * 4;
        g_hdr[b].dwFlags = WHDR_DONE;
        waveOutPrepareHeader(g_wo, &g_hdr[b], sizeof(WAVEHDR));
    }
    std::mt19937 rng((unsigned)GetTickCount64());
    std::vector<int> order;
    size_t next = 0;
    double gain = 0;  // faded in
    bool paused = false;
    while (!g_quit) {
        if (next >= order.size()) {
            order.clear();
            for (int i = 0; i < (int)list.size(); i++) order.push_back(i);
            std::shuffle(order.begin(), order.end(), rng);
            next = 0;
        }
        const Song& s = list[order[next++]];
        {
            std::lock_guard<std::mutex> lk(g_m);
            auto t = titles.find(s.key);
            g_title = t != titles.end() && !t->second.empty() ? t->second : L"A song from the park";
        }
        HANDLE h = CreateFileW(s.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) break;
        const int fb = 18 * s.ch;
        uint32_t frames = (s.samples + 31) / 32, f = 0;
        int hist[2][2] = {};
        std::vector<uint8_t> in;
        std::vector<int16_t> pcm;  // decoded, waiting for a buffer
        size_t pcmAt = 0;
        g_skip = false;
        while (!g_quit && !g_skip) {
            // Allowed, the launcher in front, not muted: play; else wait (paused).
            bool run = g_allowed && g_active && !g_played;
            if (!run) {
                if (!paused) waveOutPause(g_wo), paused = true;
                if (!g_allowed || g_played) gain = 0;  // (in again from silence)
                Sleep(50);
                continue;
            }
            if (paused) waveOutRestart(g_wo), paused = false;
            int free = -1;
            for (int b = 0; b < NBUF && free < 0; b++)
                if (g_hdr[b].dwFlags & WHDR_DONE) free = b;
            if (free < 0) {
                WaitForSingleObject(g_done, 100);
                continue;
            }
            // Fill one buffer: decode as needed.
            int16_t* out = g_buf[free].data();
            int filled = 0;
            while (filled < BUF_FRAMES) {
                if (pcmAt >= pcm.size()) {
                    if (f >= frames) break;
                    uint32_t take = std::min<uint32_t>(64, frames - f);
                    in.resize((size_t)take * fb);
                    if (!read_at(h, s.data + (uint64_t)f * fb, in.data(), (uint32_t)in.size())) {
                        f = frames;
                        break;
                    }
                    pcm.assign((size_t)take * 32 * 2, 0);
                    for (uint32_t k = 0; k < take; k++)
                        for (int c = 0; c < s.ch; c++) {
                            const uint8_t* fr = &in[(size_t)k * fb + c * 18];
                            int scale = be16(fr) + 1;
                            for (int i = 0; i < 32; i++) {
                                int nib = i & 1 ? fr[2 + i / 2] & 15 : fr[2 + i / 2] >> 4;
                                if (nib & 8) nib -= 16;
                                int v = nib * scale + ((s.coef1 * hist[c][0] + s.coef2 * hist[c][1]) >> 12);
                                v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                                hist[c][1] = hist[c][0];
                                hist[c][0] = v;
                                size_t o = ((size_t)k * 32 + i) * 2;
                                pcm[o + c] = (int16_t)v;
                                if (s.ch == 1) pcm[o + 1] = (int16_t)v;
                            }
                        }
                    uint32_t at = f * 32, n = std::min<uint32_t>(take * 32, s.samples > at ? s.samples - at : 0);
                    pcm.resize((size_t)n * 2);
                    pcmAt = 0;
                    f += take;
                    continue;
                }
                // 30%, faded in over 2 s; muted: silence (the song goes on).
                double target = g_muted ? 0 : 0.3;
                gain += (target - gain) * (target > gain ? 1.0 / (2 * RATE) * 4 : 1.0 / (RATE / 10));
                out[filled * 2] = (int16_t)(pcm[pcmAt] * gain);
                out[filled * 2 + 1] = (int16_t)(pcm[pcmAt + 1] * gain);
                pcmAt += 2;
                filled++;
            }
            if (!filled) break;  // the song's end
            g_hdr[free].dwBufferLength = filled * 4;
            g_hdr[free].dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_wo, &g_hdr[free], sizeof(WAVEHDR));
        }
        CloseHandle(h);
    }
    waveOutReset(g_wo);
    for (int b = 0; b < NBUF; b++) waveOutUnprepareHeader(g_wo, &g_hdr[b], sizeof(WAVEHDR));
    waveOutClose(g_wo);
    g_wo = nullptr;
    std::lock_guard<std::mutex> lk(g_m);
    g_title.clear();
}

}  // namespace

void music_init(const std::wstring& root, const std::wstring& dataDir) {
    g_root = root;
    g_data = dataDir;
    g_muted = GetPrivateProfileIntW(L"music", L"muted", 0, ini().c_str()) != 0;
}

void music_allow(bool allowed) {
    g_allowed = allowed;
    // The songs are found the first time it's allowed (a thread of its own).
    if (allowed && !g_thread.joinable() && !g_played && !g_root.empty()) g_thread = std::thread(play_thread);
}
void music_active(bool active) { g_active = active; }
void music_stop_for_play() { g_played = true; }
void music_skip() { g_skip = true; }
bool music_muted() { return g_muted; }
void music_set_muted(bool m) {
    g_muted = m;
    WritePrivateProfileStringW(L"music", L"muted", m ? L"1" : L"0", ini().c_str());
}
std::wstring music_now_playing() {
    if (!g_allowed || g_played || !g_active) return L"";
    std::lock_guard<std::mutex> lk(g_m);
    return g_title;
}
void music_shutdown() {
    g_quit = true;
    if (g_thread.joinable()) g_thread.join();
}
