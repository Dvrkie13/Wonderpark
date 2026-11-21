// Pokémon cries for the Gallery's "Play cry" button (cries.h), from the
// player's own disc.
//
// The cries are the game's voice bank, files/Sound/S_POKE_VOICE.acb (its
// cue table) with its stream archive S_POKE_VOICE_streamfiles.awb (a CPK
// container). Cues are named SND_ID_<character><sound>: the character is
// the game's own number (the models' table, hle_friends.c
// hle_character_name: 179 Pochama), and "A" is each one's first sound,
// taken as its cry. A cue points (ReferenceType 2) to a synth whose
// reference items (type 1) name a waveform; the waveform's Id is the file's
// ID in the CPK's ITOC table, the files laid one after another from the
// content offset, each rounded up to the CPK's alignment. Each is CRI ADX
// (mono, 32 kHz, under a second). 195 of the 199 characters with cues have
// one; Victini's is a sequence (not read), a few have none: no button.
//
// Read once, on a thread of its own: the cue file (280 KB) and the
// archive's tables; a cry's few KB when it's played, on that thread too.
// Played over everything at the Pokémon voices volume (audio_play_sound).
// The Japanese voices don't replace this bank, so both languages share it.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "host_audio.h"
#include "host_settings.h"
#include "cries.h"
const char* dvd_root(void);
}

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

std::vector<uint8_t> read_part(const std::string& path, uint64_t at, uint32_t n) {
    std::vector<uint8_t> out;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    if (n == 0) {
        LARGE_INTEGER sz;
        GetFileSizeEx(h, &sz);
        n = (uint32_t)std::min<int64_t>(sz.QuadPart - (int64_t)at, 64 << 20);
    }
    out.resize(n);
    OVERLAPPED o = {};
    o.Offset = (DWORD)at;
    o.OffsetHigh = (DWORD)(at >> 32);
    DWORD got = 0;
    if (!ReadFile(h, out.data(), n, &got, &o) || got != n) out.clear();
    CloseHandle(h);
    return out;
}

// A CRI @UTF table: its rows as column name -> value (numbers as uint64,
// strings and data as offsets into the buffer).
struct Value {
    uint64_t n = 0;
    std::string s;
    size_t data = 0, size = 0;  // data: absolute offset in the buffer
};
using UtfRow = std::map<std::string, Value>;

bool utf(const std::vector<uint8_t>& b, size_t off, std::vector<UtfRow>& rows) {
    if (off + 32 > b.size() || memcmp(&b[off], "@UTF", 4)) return false;
    size_t base = off + 8, end = base + be32(&b[off + 4]);
    if (end > b.size()) return false;
    uint32_t rowsOff = be16(&b[base + 2]), strOff = be32(&b[base + 4]), dataOff = be32(&b[base + 8]);
    int ncol = be16(&b[base + 16]), rowW = be16(&b[base + 18]);
    uint32_t nrows = be32(&b[base + 20]);
    auto str = [&](uint32_t o) {
        std::string s;
        for (size_t p = base + strOff + o; p < end && b[p]; p++) s += (char)b[p];
        return s;
    };
    auto read = [&](int type, size_t& p, Value& v) -> bool {
        static const int sizes[16] = {1, 1, 2, 2, 4, 4, 8, 8, 4, 0, 4, 8, 0, 0, 0, 0};
        type &= 15;
        int n = sizes[type];
        if (!n || p + n > end) return false;
        if (type <= 7)
            for (int i = 0; i < n; i++) v.n = v.n << 8 | b[p + i];
        else if (type == 10) v.s = str(be32(&b[p]));
        else if (type == 11) v.data = base + dataOff + be32(&b[p]), v.size = be32(&b[p + 4]);
        p += n;
        return true;
    };
    struct Col { int flag; std::string name; Value constant; };
    std::vector<Col> cols;
    size_t p = base + 24;
    for (int c = 0; c < ncol; c++) {
        if (p + 5 > end) return false;
        Col col;
        col.flag = b[p];
        col.name = str(be32(&b[p + 1]));
        p += 5;
        if ((col.flag & 0xF0) == 0x30 && !read(col.flag, p, col.constant)) return false;
        cols.push_back(col);
    }
    rows.assign(nrows, {});
    for (uint32_t r = 0; r < nrows; r++) {
        size_t q = base + rowsOff + (size_t)r * rowW;
        for (auto& c : cols) {
            Value v;
            if ((c.flag & 0xF0) == 0x50) {
                if (!read(c.flag, q, v)) return false;
            } else if ((c.flag & 0xF0) == 0x30) v = c.constant;
            rows[r][c.name] = v;
        }
    }
    return true;
}

struct Cry {
    uint64_t at = 0;
    uint32_t size = 0;
};
std::map<int, Cry> g_cries;  // by character
std::string g_awb;
std::atomic<int> g_state{0};
std::mutex g_playM;

void load() {
    std::string dir = std::string(dvd_root() ? dvd_root() : "") + "/files/Sound/";
    auto acb = read_part(dir + "S_POKE_VOICE.acb", 0, 0);
    g_awb = dir + "S_POKE_VOICE_streamfiles.awb";
    std::vector<UtfRow> head, cues, names, synths, waves;
    if (acb.empty() || !utf(acb, 0, head) || head.empty()) return;
    auto sub = [&](const char* k, std::vector<UtfRow>& out) {
        const Value& v = head[0][k];
        return v.size && utf(acb, v.data, out);
    };
    if (!sub("CueTable", cues) || !sub("CueNameTable", names) || !sub("SynthTable", synths) || !sub("WaveformTable", waves)) return;
    // The archive: its ITOC's files (ID, size), laid out in ID order.
    auto hdr = read_part(g_awb, 0, 0x800);
    std::vector<UtfRow> cpk, itoc, files;
    if (hdr.size() < 0x800 || memcmp(hdr.data(), "CPK ", 4) || !utf(hdr, 0x10, cpk) || cpk.empty()) return;
    uint64_t itocAt = cpk[0]["ItocOffset"].n, content = cpk[0]["ContentOffset"].n, align = std::max<uint64_t>(1, cpk[0]["Align"].n);
    auto it = read_part(g_awb, itocAt, (uint32_t)std::min<uint64_t>(content > itocAt ? content - itocAt : 0, 1 << 20));
    if (it.size() < 0x20 || !utf(it, 0x10, itoc) || itoc.empty()) return;
    std::map<uint64_t, uint64_t> sizes;
    for (const char* k : {"DataL", "DataH"}) {
        const Value& v = itoc[0][k];
        if (!v.size || !utf(it, v.data, files)) continue;
        for (auto& f : files) sizes[f.at("ID").n] = f.at("FileSize").n;
    }
    std::map<uint64_t, Cry> where;
    uint64_t pos = content;
    for (auto& s : sizes) {
        where[s.first] = {pos, (uint32_t)s.second};
        pos = (pos + s.second + align - 1) / align * align;
    }
    std::map<int, Cry> out;
    for (auto& n : names) {
        const std::string& name = n["CueName"].s;
        int ch = 0;
        char tail[8] = {};
        if (sscanf(name.c_str(), "SND_ID_%d%7s", &ch, tail) != 2 || strcmp(tail, "A")) continue;
        uint64_t ci = n["CueIndex"].n;
        if (ci >= cues.size() || cues[ci]["ReferenceType"].n != 2) continue;
        uint64_t si = cues[ci]["ReferenceIndex"].n;
        // The synth (and a synth it names), to its first waveform.
        for (int depth = 0; depth < 3 && si < synths.size(); depth++) {
            const Value& items = synths[si]["ReferenceItems"];
            if (items.size < 4) break;
            int type = be16(&acb[items.data]), index = be16(&acb[items.data + 2]);
            if (type == 2) {
                si = (uint64_t)index;
                continue;
            }
            if (type == 1 && (size_t)index < waves.size() && waves[index]["EncodeType"].n == 0 &&
                waves[index]["NumChannels"].n == 1) {
                auto w = where.find(waves[index]["Id"].n);
                if (w != where.end()) out[ch] = w->second;
            }
            break;
        }
    }
    g_cries = std::move(out);
    fprintf(stderr, "cries: %zu cries on the disc\n", g_cries.size());
}

// A cry's ADX (mono, 4-bit) decoded to 16-bit samples.
bool decode(const std::vector<uint8_t>& a, std::vector<int16_t>& out, int& rate) {
    if (a.size() < 0x20 || a[0] != 0x80 || a[1] != 0 || a[4] != 3 || a[5] != 18 || a[6] != 4 || a[7] != 1) return false;
    size_t data = be16(&a[2]) + 4;
    rate = (int)be32(&a[8]);
    uint32_t samples = be32(&a[12]);
    int highpass = be16(&a[16]);
    if (rate <= 0 || data >= a.size()) return false;
    double x = std::sqrt(2.0) - std::cos(2 * 3.14159265358979 * highpass / rate), y = std::sqrt(2.0) - 1;
    double c = (x - std::sqrt((x + y) * (x - y))) / y;
    int c1 = (int)std::floor(c * 8192), c2 = (int)std::floor(c * c * -4096), h0 = 0, h1 = 0;
    out.clear();
    for (size_t f = data; f + 18 <= a.size() && out.size() < samples; f += 18) {
        int scale = be16(&a[f]) + 1;
        for (int i = 0; i < 32 && out.size() < samples; i++) {
            int nib = i & 1 ? a[f + 2 + i / 2] & 15 : a[f + 2 + i / 2] >> 4;
            if (nib & 8) nib -= 16;
            int s = nib * scale + ((c1 * h0 + c2 * h1) >> 12);
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            h1 = h0;
            h0 = s;
            out.push_back((int16_t)s);
        }
    }
    return !out.empty();
}

}  // namespace

extern "C" {

void cries_load(void) {
    int expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1)) return;
    std::thread([] {
        load();
        g_state = g_cries.empty() ? 3 : 2;
    }).detach();
}

int cry_available(int character) { return g_state == 2 && g_cries.count(character) != 0; }
int cry_state_pending(void) { return g_state == 1; }

void cry_play(int character) {
    if (!cry_available(character)) return;
    Cry c = g_cries[character];
    std::thread([c] {
        std::lock_guard<std::mutex> lk(g_playM);
        std::vector<int16_t> pcm;
        int rate = 0;
        if (!decode(read_part(g_awb, c.at, c.size), pcm, rate)) return;
        int vol = g_x[X_VOLUME_VOICES];
        audio_play_sound(pcm.data(), (int)pcm.size(), rate, vol * vol / 10000.f);
    }).detach();
}

}  // extern "C"
