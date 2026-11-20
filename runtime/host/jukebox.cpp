// The Jukebox (jukebox.h): the game's music, from the player's own disc.
//
// The music is CRI ADX (type 3, 4-bit, 32 kHz stereo; one mono 22 kHz cue
// with no samples is left out) inside the music files' stream archives,
// files/Sound/*BGM*_streamfiles.awb (CPK containers): 95 songs on the US
// disc. Each ADX starts 0x80 0x00 <copyright offset>, with "(c)CRI" just
// before its data; the songs follow each other, so they're found by
// reading each header and looking for the next one just past its data (a
// few small reads per file, on a thread of its own).
//
// Heard: the game streams a song from its archive as it plays it, so a disc
// read inside a song's data (ios/dvd.c calls jukebox_note_read) marks it
// heard; noted in jukebox_heard.ini in the data folder, written on a thread
// of its own.
//
// Playing: only while the notebook is open, when the game is paused and its
// own sound has stopped: a thread of its own reads and decodes the song and
// queues it for the sound device (g_host_audio_push, 32 kHz like the game's
// mix), keeping about 150 ms queued, at the Music volume. When the notebook
// closes it stops, and the game's music goes on.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "host_audio.h"
#include "host_settings.h"
#include "jukebox.h"
const char* dvd_root(void);
const char* wp_data_dir(void);
int audio_queued_ms(void);
int audio_target_ms(void);
}

namespace {

struct Track {
    JukeboxTrack t;
    std::string path;       // the archive
    uint64_t data = 0, end = 0;  // the song's sound data in it
    int ch = 2, rate = 32000;
    uint32_t samples = 0, loopStart = 0, loopEnd = 0;
    bool loop = false;
    int coef1 = 0, coef2 = 0;
};
std::vector<Track> g_tracks;
std::atomic<int> g_state{0};

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

bool read_at(HANDLE h, uint64_t pos, void* out, uint32_t n) {
    OVERLAPPED o = {};
    o.Offset = (DWORD)pos;
    o.OffsetHigh = (DWORD)(pos >> 32);
    DWORD got = 0;
    return ReadFile(h, out, n, &got, &o) && got == n;
}

// ---- Heard: the offsets (rounded down to 2 KB) the game read in each archive.

std::mutex g_heardM;
std::map<std::string, std::set<uint64_t>> g_heard;
bool g_heardDirty;
double g_heardSaved;

std::string heard_path() { return std::string(wp_data_dir()) + "/jukebox_heard.ini"; }

void save_heard() {
    std::string s = "; The Jukebox: where in each music file the game has read (so those songs are heard).\n[heard]\n";
    {
        std::lock_guard<std::mutex> lk(g_heardM);
        for (auto& f : g_heard) {
            s += f.first + "=";
            bool first = true;
            for (uint64_t o : f.second) s += (first ? "" : ",") + std::to_string(o), first = false;
            s += "\n";
        }
        g_heardDirty = false;
    }
    std::thread([s] {
        std::string p = heard_path(), tmp = p + ".new";
        if (FILE* f = fopen(tmp.c_str(), "wb")) {
            bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
            if (fclose(f) == 0 && ok) MoveFileExA(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }).detach();
}

bool is_heard(const Track& tr) {
    std::lock_guard<std::mutex> lk(g_heardM);
    auto it = g_heard.find(tr.t.bank);
    if (it == g_heard.end()) return false;
    auto o = it->second.lower_bound(tr.data & ~0x7FFull);
    return o != it->second.end() && *o < tr.end;
}

// Which tab and place a music file is for, by its name.
void classify(Track& tr) {
    const char* b = tr.t.bank;
    int a = 0, z = 0;
    if (!strcmp(b, "S_Field_BGM")) tr.t.kind = JUKEBOX_STORY, tr.t.area = 0;
    else if (sscanf(b, "S_AT_%d_BGM", &a) == 1) tr.t.kind = JUKEBOX_ATTRACTION, tr.t.area = a;
    else if (!strncmp(b, "S_AT_", 5)) tr.t.kind = JUKEBOX_ATTRACTION, tr.t.area = -1;
    else if (sscanf(b, "S_BGM_Fd%2d%2d", &a, &z) == 2 || sscanf(b, "S_Fd_BGM_%2d%2d", &a, &z) == 2) {
        tr.t.area = a << 8 | z;
        tr.t.kind = a == 6 || a == 61 ? JUKEBOX_WISH : a == 11 ? JUKEBOX_STORY : JUKEBOX_PARK;
    } else tr.t.kind = JUKEBOX_STORY, tr.t.area = 0;
}

// The songs in one archive.
void scan_file(const std::string& path, const std::string& bank, std::vector<Track>& out) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    uint64_t size = (uint64_t)sz.QuadPart, from = 0;
    std::vector<uint8_t> buf(0x4000);
    int index = 0;
    while (from + 0x40 < size) {
        uint32_t n = (uint32_t)std::min<uint64_t>(buf.size(), size - from);
        if (!read_at(h, from, buf.data(), n)) break;
        // The next header: 0x80 0x00, its copyright offset pointing at "(c)CRI".
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
        Track tr;
        int copy = be16(&hd[2]);
        tr.ch = hd[7];
        tr.rate = (int)be32(&hd[8]);
        tr.samples = be32(&hd[12]);
        int highpass = be16(&hd[16]), version = hd[18];
        if (version == 4 && copy >= 0x34) {
            tr.loop = be32(&hd[0x24]) != 0;
            tr.loopStart = be32(&hd[0x28]);
            tr.loopEnd = be32(&hd[0x30]);
        } else if (version == 3 && copy >= 0x28) {
            tr.loop = be32(&hd[0x18]) != 0;
            tr.loopStart = be32(&hd[0x1C]);
            tr.loopEnd = be32(&hd[0x24]);
        }
        tr.data = hp + copy + 4;
        uint64_t frames = (tr.samples + 31) / 32;
        tr.end = tr.data + frames * 18 * std::max(1, tr.ch);
        if (hd[4] == 3 && hd[5] == 18 && hd[6] == 4 && tr.ch >= 1 && tr.ch <= 2 && tr.rate > 0 && tr.samples > tr.rate / 2) {
            double x = std::sqrt(2.0) - std::cos(2 * 3.14159265358979 * highpass / tr.rate), y = std::sqrt(2.0) - 1;
            double c = (x - std::sqrt((x + y) * (x - y))) / y;
            tr.coef1 = (int)std::floor(c * 8192);
            tr.coef2 = (int)std::floor(c * c * -4096);
            snprintf(tr.t.bank, sizeof(tr.t.bank), "%s", bank.c_str());
            tr.t.index = index++;
            tr.t.seconds = (int)(tr.samples / tr.rate);
            classify(tr);
            tr.path = path;
            out.push_back(tr);
        }
        from = std::max(hp + 0x40, tr.end > 0x100 ? tr.end - 0x100 : hp + 0x40);
    }
    CloseHandle(h);
}

// ---- Playing: a thread that decodes and queues the song.

std::thread g_player;
std::atomic<bool> g_stop{false}, g_paused{false};
std::atomic<int> g_playing{-1};
std::atomic<uint64_t> g_done{0};  // samples queued so far
std::atomic<int> g_shuffle{0}, g_repeat{0};
std::mutex g_playM;

int next_track(int from, int dir) {
    int n = (int)g_tracks.size();
    if (!n) return -1;
    if (g_shuffle && dir > 0) {
        std::vector<int> ok;
        for (int i = 0; i < n; i++)
            if (i != from && is_heard(g_tracks[i])) ok.push_back(i);
        return ok.empty() ? -1 : ok[rand() % ok.size()];
    }
    for (int k = 1; k < n; k++) {
        int i = ((from + dir * k) % n + n) % n;
        if (is_heard(g_tracks[i])) return i;
    }
    return -1;
}

void play_thread(int index) {
    std::vector<uint8_t> in;
    std::vector<int16_t> out;
    while (index >= 0 && !g_stop) {
        const Track& tr = g_tracks[index];
        g_playing = index;
        g_done = 0;
        HANDLE h = CreateFileA(tr.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) break;
        const int ch = tr.ch, fb = 18 * ch;
        uint32_t frames = (tr.samples + 31) / 32, f = 0;
        int hist[2][2] = {};
        bool ended = false;
        while (!g_stop && !ended) {
            if (g_paused) {
                Sleep(20);
                continue;
            }
            if (audio_queued_ms() > audio_target_ms() + 10) {  // as full as the sound thread wants it
                Sleep(4);
                continue;
            }
            uint32_t take = std::min<uint32_t>(16, frames - f);
            in.resize((size_t)take * fb);
            if (!take || !read_at(h, tr.data + (uint64_t)f * fb, in.data(), (uint32_t)in.size())) break;
            out.assign((size_t)take * 32 * 2, 0);
            for (uint32_t k = 0; k < take; k++)
                for (int c = 0; c < ch; c++) {
                    const uint8_t* fr = &in[(size_t)k * fb + c * 18];
                    int scale = be16(fr) + 1;
                    for (int i = 0; i < 32; i++) {
                        int nib = i & 1 ? fr[2 + i / 2] & 15 : fr[2 + i / 2] >> 4;
                        if (nib & 8) nib -= 16;
                        int s = nib * scale + ((tr.coef1 * hist[c][0] + tr.coef2 * hist[c][1]) >> 12);
                        s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
                        hist[c][1] = hist[c][0];
                        hist[c][0] = s;
                        size_t o = ((size_t)k * 32 + i) * 2;
                        out[o + c] = (int16_t)s;
                        if (ch == 1) out[o + 1] = (int16_t)s;
                    }
                }
            // Only the song's samples (the last frame may run past its end).
            uint32_t at = f * 32, n = std::min<uint32_t>(take * 32, tr.samples > at ? tr.samples - at : 0);
            int vol = g_x[X_VOLUME_MUSIC];
            if (vol < 100) {
                int g = vol * vol * 65536 / 10000;
                for (uint32_t i = 0; i < n * 2; i++) out[i] = (int16_t)((out[i] * g) >> 16);
            }
            if (n && g_host_audio_push) g_host_audio_push(out.data(), (int)n);
            g_done += n;
            f += take;
            if (f >= frames) {
                if (g_repeat && tr.loop && tr.loopStart < tr.samples) {  // round again from the loop's start
                    f = tr.loopStart / 32;
                    g_done = f * 32ull;
                    hist[0][0] = hist[0][1] = hist[1][0] = hist[1][1] = 0;
                } else {
                    ended = true;
                }
            }
        }
        CloseHandle(h);
        if (!ended) break;
        index = g_repeat && !tr.loop ? index : next_track(index, 1);
    }
    g_playing = -1;
}

}  // namespace

extern "C" {

void jukebox_init(void) {
    FILE* f = fopen(heard_path().c_str(), "rb");
    if (!f) return;
    char line[1 << 16];
    std::lock_guard<std::mutex> lk(g_heardM);
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '[') continue;
        *eq = 0;
        auto& set = g_heard[line];
        for (char* p = eq + 1; *p;) {
            char* e;
            unsigned long long v = strtoull(p, &e, 10);
            if (e == p) break;
            set.insert(v);
            p = *e == ',' ? e + 1 : e;
            if (*p == '\n' || *p == '\r') break;
        }
    }
    fclose(f);
}

void jukebox_note_read(const char* path, unsigned long long offset) {
    const char* base = strrchr(path, '/');
    const char* b2 = strrchr(path, '\\');
    if (b2 && (!base || b2 > base)) base = b2;
    base = base ? base + 1 : path;
    const char* suffix = strstr(base, "_streamfiles.awb");
    if (!suffix || !strstr(base, "BGM")) return;
    std::string bank(base, suffix);
    {
        std::lock_guard<std::mutex> lk(g_heardM);
        if (!g_heard[bank].insert(offset & ~0x7FFull).second) return;
        g_heardDirty = true;
    }
    double now = (double)GetTickCount64() / 1000;
    if (now - g_heardSaved > 20) {
        g_heardSaved = now;
        save_heard();
    }
}

void jukebox_scan_start(void) {
    int expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1)) return;
    std::thread([] {
        std::string root = dvd_root() ? dvd_root() : "";
        std::string dir = root + "/files/Sound/";
        std::vector<Track> list;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "*BGM*_streamfiles.awb").c_str(), &fd);
        std::vector<std::string> names;
        if (h != INVALID_HANDLE_VALUE) {
            do names.push_back(fd.cFileName);
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        for (auto& n : names) scan_file(dir + n, n.substr(0, n.size() - strlen("_streamfiles.awb")), list);
        // Park (by place), Wish Park, the attractions, then the story and events.
        std::stable_sort(list.begin(), list.end(), [](const Track& a, const Track& b) {
            if (a.t.kind != b.t.kind) return a.t.kind < b.t.kind;
            if (a.t.area != b.t.area) return a.t.area < b.t.area;
            return strcmp(a.t.bank, b.t.bank) < 0 || (!strcmp(a.t.bank, b.t.bank) && a.t.index < b.t.index);
        });
        g_tracks = std::move(list);
        g_state = g_tracks.empty() ? 3 : 2;
        fprintf(stderr, "jukebox: %zu songs in %zu music files\n", g_tracks.size(), names.size());
    }).detach();
}

int jukebox_state(void) { return g_state; }
int jukebox_count(void) { return g_state == 2 ? (int)g_tracks.size() : 0; }
int jukebox_track(int i, JukeboxTrack* out) {
    if (g_state != 2 || i < 0 || i >= (int)g_tracks.size()) return 0;
    *out = g_tracks[i].t;
    out->heard = is_heard(g_tracks[i]);
    return 1;
}

void jukebox_stop(void) {
    std::lock_guard<std::mutex> lk(g_playM);
    g_stop = true;
    if (g_player.joinable()) g_player.join();
    g_stop = false;
    g_paused = false;
    g_playing = -1;
    if (g_heardDirty) save_heard();
}
void jukebox_play(int i) {
    if (g_state != 2 || i < 0 || i >= (int)g_tracks.size() || !is_heard(g_tracks[i])) return;
    jukebox_stop();
    std::lock_guard<std::mutex> lk(g_playM);
    g_playing = i;
    g_player = std::thread(play_thread, i);
}
void jukebox_toggle_pause(void) {
    if (g_playing >= 0) g_paused = !g_paused;
}
void jukebox_step(int dir) {
    int from = g_playing;
    int n = next_track(from < 0 ? -1 : from, dir);
    if (n >= 0) jukebox_play(n);
}
int jukebox_playing(void) { return g_playing; }
int jukebox_paused(void) { return g_paused; }
double jukebox_position(void) {
    int i = g_playing;
    if (i < 0 || g_state != 2) return 0;
    double s = (double)g_done / g_tracks[i].rate - (g_paused ? 0 : audio_queued_ms() / 1000.0);
    return s < 0 ? 0 : s;
}
int jukebox_shuffle(void) { return g_shuffle; }
int jukebox_repeat(void) { return g_repeat; }
void jukebox_set_shuffle(int on) { g_shuffle = on; }
void jukebox_set_repeat(int on) { g_repeat = on; }

}  // extern "C"
