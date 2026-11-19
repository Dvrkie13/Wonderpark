// Mods for the launcher (mods.h): installed mods, mods.ini, and installing
// a mod from a .zip, safely, on a worker thread.
//
// The .zip is read with our own reader (no third-party code): the central
// directory, then each entry, stored or deflated (inflate below, after
// zlib's puff.c, written from the DEFLATE spec, RFC 1951), each checked
// against its CRC-32. Refused: anything that isn't a zip or is damaged,
// encrypted or zip64 entries, absolute paths and "..", more than 20,000
// entries, an entry over 512 MB or 2 GB in all unpacked, and entries that
// unpack to over 1,000 times their packed size (zip bombs). Files are
// written to mods\.install-<id>\ first and moved to mods\<id>\ only when
// all of them are in, so a failed install leaves nothing behind and an old
// copy of the same mod is replaced only by a whole new one.
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <map>

#include "mods.h"

ModInstall g_install;

// ------------------------------------------------------------------------
// Paths

static std::wstring env(const wchar_t* name) {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n && n < 1024 ? std::wstring(buf, n) : std::wstring();
}

std::wstring mods_data_dir() {
    std::wstring d = env(L"WONDERPARK_DATA_DIR");
    if (!d.empty()) {
        for (auto& c : d) if (c == L'/') c = L'\\';
        return d;
    }
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) r = std::wstring(p) + L"\\Wonderpark";
    CoTaskMemFree(p);
    return r;
}
std::wstring mods_dir() { return mods_data_dir() + L"\\mods"; }

static void make_dirs(const std::wstring& path) {
    for (size_t i = 3; i <= path.size(); i++)
        if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
}

// Deletes a folder and everything in it; only ever one inside the mods folder.
static void delete_tree(const std::wstring& dir) {
    std::wstring root = mods_dir() + L"\\";
    if (dir.size() <= root.size() || _wcsnicmp(dir.c_str(), root.c_str(), root.size()) != 0) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
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

static bool read_all(const std::wstring& path, std::string& out, size_t limit) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(h, &sz) && (size_t)sz.QuadPart <= limit;
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = sz.QuadPart == 0 || (ReadFile(h, &out[0], (DWORD)sz.QuadPart, &got, nullptr) && got == sz.QuadPart);
    }
    CloseHandle(h);
    return ok;
}

// ------------------------------------------------------------------------
// mod.json: a flat object of strings (other values are kept as their text)

static std::map<std::string, std::string> parse_json(const std::string& s) {
    std::map<std::string, std::string> m;
    size_t i = 0;
    auto ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++; };
    auto str = [&](std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                char e = s[i++];
                if (e == 'n') out += '\n';
                else if (e == 't') out += '\t';
                else if (e == 'u' && i + 4 <= s.size()) {
                    unsigned cp = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) out += (char)(0xC0 | cp >> 6), out += (char)(0x80 | (cp & 63));
                    else out += (char)(0xE0 | cp >> 12), out += (char)(0x80 | ((cp >> 6) & 63)), out += (char)(0x80 | (cp & 63));
                } else out += e;
            } else out += c;
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    };
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF) i = 3;  // UTF-8 BOM
    ws();
    if (i >= s.size() || s[i] != '{') return m;
    i++;
    for (;;) {
        ws();
        std::string k, v;
        if (!str(k)) break;
        ws();
        if (i >= s.size() || s[i] != ':') break;
        i++;
        ws();
        if (i < s.size() && s[i] == '"') {
            if (!str(v)) break;
        } else {
            size_t b = i;
            int depth = 0;
            while (i < s.size() && (depth > 0 || (s[i] != ',' && s[i] != '}'))) {
                if (s[i] == '{' || s[i] == '[') depth++;
                if (s[i] == '}' || s[i] == ']') depth--;
                i++;
            }
            v = s.substr(b, i - b);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
        }
        m[k] = v;
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        break;
    }
    return m;
}

// "My Mod_v2" -> "my-mod-v2": the folder name and the id in mods.ini.
static std::string slug(const std::string& s) {
    std::string r;
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (isalnum(u)) r += (char)tolower(u);
        else if (!r.empty() && r.back() != '-') r += '-';
    }
    while (!r.empty() && r.back() == '-') r.pop_back();
    if (r.size() > 64) r.resize(64);
    return r;
}

// "cool_texture-pack.zip" -> "Cool Texture Pack"
static std::wstring nice_name(std::wstring f) {
    if (f.size() > 4 && _wcsicmp(f.c_str() + f.size() - 4, L".zip") == 0) f.resize(f.size() - 4);
    std::wstring r;
    bool start = true;
    for (wchar_t c : f) {
        if (c == L'_' || c == L'-' || c == L' ') {
            if (!r.empty() && r.back() != L' ') r += L' ';
            start = true;
            continue;
        }
        r += start ? (wchar_t)towupper(c) : c;
        start = false;
    }
    while (!r.empty() && r.back() == L' ') r.pop_back();
    return r.empty() ? L"Unnamed mod" : r;
}

static ModInfo read_mod(const std::wstring& id) {
    ModInfo m;
    m.id = id;
    std::string js;
    std::map<std::string, std::string> j;
    if (read_all(mods_dir() + L"\\" + id + L"\\mod.json", js, 1 << 20)) j = parse_json(js);
    m.name = j.count("name") && !j["name"].empty() ? widen(j["name"]) : nice_name(id);
    m.summary = j.count("summary") && !j["summary"].empty() ? widen(j["summary"])
                : j.count("description") && !j["description"].empty() ? widen(j["description"])
                                                                         : L"No description included with this mod.";
    m.version = widen(j.count("version") ? j["version"] : "");
    m.author = widen(j.count("author") ? j["author"] : "");
    return m;
}

// ------------------------------------------------------------------------
// mods.ini

std::vector<ModInfo> mods_list() {
    std::vector<ModInfo> out;
    std::vector<std::wstring> installed;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((mods_dir() + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n[0] != L'.') installed.push_back(n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::string ini;
    read_all(mods_data_dir() + L"\\mods.ini", ini, 1 << 20);
    size_t p = 0;
    while (p < ini.size()) {
        size_t e = ini.find('\n', p);
        if (e == std::string::npos) e = ini.size();
        std::string line = ini.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        if (line.empty() || line[0] == ';' || line[0] == '[' || eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        std::wstring id = widen(k);
        auto it = std::find(installed.begin(), installed.end(), id);
        if (it == installed.end()) continue;  // listed but not installed
        installed.erase(it);
        ModInfo m = read_mod(id);
        m.enabled = v.empty() || v[0] != '0';
        out.push_back(m);
    }
    for (auto& id : installed) out.push_back(read_mod(id));  // installed but not listed: on, at the end
    return out;
}

bool mods_save(const std::vector<ModInfo>& mods) {
    std::string s =
        "; Wonderpark mods: 1 on, 0 off. Where two mods change the same file or texture,\n"
        "; the one listed first wins. Changes apply the next time you press Play.\n[mods]\n";
    for (auto& m : mods) s += narrow(m.id) + " = " + (m.enabled ? "1" : "0") + "\n";
    make_dirs(mods_data_dir());
    std::wstring path = mods_data_dir() + L"\\mods.ini", tmp = path + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, s.data(), (DWORD)s.size(), &put, nullptr) && put == s.size();
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ------------------------------------------------------------------------
// Inflate (RFC 1951), after puff.c. Throws on bad data.

namespace {
struct Bad {};
struct Inflate {
    const uint8_t* in;
    size_t inlen, incnt = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    uint8_t* out;
    size_t outlen, outcnt = 0;

    int bits(int need) {
        uint32_t val = bitbuf;
        while (bitcnt < need) {
            if (incnt == inlen) throw Bad();
            val |= (uint32_t)in[incnt++] << bitcnt;
            bitcnt += 8;
        }
        bitbuf = val >> need;
        bitcnt -= need;
        return (int)(val & ((1u << need) - 1));
    }
    void stored() {
        bitbuf = 0;
        bitcnt = 0;
        if (incnt + 4 > inlen) throw Bad();
        unsigned len = in[incnt] | in[incnt + 1] << 8;
        unsigned nlen = in[incnt + 2] | in[incnt + 3] << 8;
        incnt += 4;
        if (len != (~nlen & 0xFFFF) || incnt + len > inlen || outcnt + len > outlen) throw Bad();
        memcpy(out + outcnt, in + incnt, len);
        incnt += len;
        outcnt += len;
    }
    struct Huff { short count[16]; short symbol[288]; };
    int decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; len++) {
            code |= bits(1);
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw Bad();
    }
    static int construct(Huff& h, const short* length, int n) {
        short offs[16];
        for (int len = 0; len <= 15; len++) h.count[len] = 0;
        for (int s = 0; s < n; s++) h.count[length[s]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len <= 15; len++) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return left;  // over-subscribed
        }
        offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
        for (int s = 0; s < n; s++)
            if (length[s]) h.symbol[offs[length[s]]++] = (short)s;
        return left;
    }
    void codes(const Huff& lencode, const Huff& distcode) {
        static const short lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = decode(lencode);
            if (symbol < 256) {
                if (outcnt == outlen) throw Bad();
                out[outcnt++] = (uint8_t)symbol;
            } else if (symbol > 256) {
                symbol -= 257;
                if (symbol >= 29) throw Bad();
                int len = lbase[symbol] + bits(lext[symbol]);
                symbol = decode(distcode);
                if (symbol >= 30) throw Bad();
                size_t dist = (size_t)(dbase[symbol] + bits(dext[symbol]));
                if (dist > outcnt || outcnt + len > outlen) throw Bad();
                for (int k = 0; k < len; k++, outcnt++) out[outcnt] = out[outcnt - dist];
            } else {
                return;
            }
        }
    }
    void fixed() {
        static Huff lencode, distcode;
        static bool made;
        if (!made) {
            short lengths[288];
            int s = 0;
            for (; s < 144; s++) lengths[s] = 8;
            for (; s < 256; s++) lengths[s] = 9;
            for (; s < 280; s++) lengths[s] = 7;
            for (; s < 288; s++) lengths[s] = 8;
            construct(lencode, lengths, 288);
            for (s = 0; s < 30; s++) lengths[s] = 5;
            construct(distcode, lengths, 30);
            made = true;
        }
        codes(lencode, distcode);
    }
    void dynamic() {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        short lengths[320];
        Huff lencode, distcode;
        int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4, index;
        if (nlen > 286 || ndist > 30) throw Bad();
        for (index = 0; index < ncode; index++) lengths[order[index]] = (short)bits(3);
        for (; index < 19; index++) lengths[order[index]] = 0;
        if (construct(lencode, lengths, 19) != 0) throw Bad();
        index = 0;
        while (index < nlen + ndist) {
            int symbol = decode(lencode), len;
            if (symbol < 16) {
                lengths[index++] = (short)symbol;
                continue;
            }
            len = 0;
            if (symbol == 16) {
                if (index == 0) throw Bad();
                len = lengths[index - 1];
                symbol = 3 + bits(2);
            } else if (symbol == 17) symbol = 3 + bits(3);
            else symbol = 11 + bits(7);
            if (index + symbol > nlen + ndist) throw Bad();
            while (symbol--) lengths[index++] = (short)len;
        }
        if (lengths[256] == 0) throw Bad();
        int err = construct(lencode, lengths, nlen);
        if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) throw Bad();
        err = construct(distcode, lengths + nlen, ndist);
        if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) throw Bad();
        codes(lencode, distcode);
    }
    void run() {
        int last;
        do {
            last = bits(1);
            int type = bits(2);
            if (type == 0) stored();
            else if (type == 1) fixed();
            else if (type == 2) dynamic();
            else throw Bad();
        } while (!last);
    }
};

uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------------------
// Zip

struct Entry {
    std::string name;
    uint32_t crc, csize, usize, local;
    uint16_t method, flags;
};

struct Fail { std::wstring msg; };
const wchar_t* DAMAGED = L"Couldn’t read that .zip. It may be damaged or not a Wonderpark mod.";

const uint64_t MAX_FILE = 512ull << 20, MAX_TOTAL = 2048ull << 20;
const size_t MAX_ENTRIES = 20000;

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

struct Zip {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t size = 0;
    std::vector<Entry> entries;
    ~Zip() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    void read_at(uint64_t pos, void* out, size_t n) {
        LARGE_INTEGER p;
        p.QuadPart = (LONGLONG)pos;
        DWORD got = 0;
        if (pos + n > size || !SetFilePointerEx(h, p, nullptr, FILE_BEGIN) || !ReadFile(h, out, (DWORD)n, &got, nullptr) ||
            got != n)
            throw Fail{DAMAGED};
    }
    void open(const std::wstring& path) {
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                        nullptr);
        if (h == INVALID_HANDLE_VALUE) throw Fail{L"That file couldn’t be opened."};
        LARGE_INTEGER sz;
        GetFileSizeEx(h, &sz);
        size = (uint64_t)sz.QuadPart;
        if (size < 22) throw Fail{DAMAGED};
        // The end of central directory record: in the last 64 KB + 22 bytes.
        size_t tail = (size_t)std::min<uint64_t>(size, 65557);
        std::vector<uint8_t> t(tail);
        read_at(size - tail, t.data(), tail);
        long eocd = -1;
        for (long i = (long)tail - 22; i >= 0; i--)
            if (le32(&t[i]) == 0x06054b50) { eocd = i; break; }
        if (eocd < 0) throw Fail{DAMAGED};
        const uint8_t* e = &t[eocd];
        uint32_t count = le16(e + 10), cdsize = le32(e + 12), cdoff = le32(e + 16);
        if (count == 0xFFFF || cdoff == 0xFFFFFFFF) throw Fail{L"That .zip is too large (zip64) to install."};
        if (count > MAX_ENTRIES) throw Fail{L"That .zip has too many files to be a mod."};
        if ((uint64_t)cdoff + cdsize > size) throw Fail{DAMAGED};
        std::vector<uint8_t> cd(cdsize);
        read_at(cdoff, cd.data(), cdsize);
        size_t p = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (p + 46 > cd.size() || le32(&cd[p]) != 0x02014b50) throw Fail{DAMAGED};
            Entry en;
            en.flags = le16(&cd[p + 8]);
            en.method = le16(&cd[p + 10]);
            en.crc = le32(&cd[p + 16]);
            en.csize = le32(&cd[p + 20]);
            en.usize = le32(&cd[p + 24]);
            uint16_t nl = le16(&cd[p + 28]), xl = le16(&cd[p + 30]), cl = le16(&cd[p + 32]);
            en.local = le32(&cd[p + 42]);
            if (p + 46 + nl > cd.size()) throw Fail{DAMAGED};
            en.name.assign((const char*)&cd[p + 46], nl);
            p += 46 + nl + xl + cl;
            entries.push_back(en);
        }
    }
    // An entry's bytes, unpacked and checked.
    std::vector<uint8_t> extract(const Entry& en) {
        uint8_t lh[30];
        read_at(en.local, lh, 30);
        if (le32(lh) != 0x04034b50) throw Fail{DAMAGED};
        uint64_t data = (uint64_t)en.local + 30 + le16(lh + 26) + le16(lh + 28);
        std::vector<uint8_t> packed(en.csize), out(en.usize);
        if (en.csize) read_at(data, packed.data(), en.csize);
        if (en.method == 0) {
            if (en.csize != en.usize) throw Fail{DAMAGED};
            out = packed;
        } else {
            Inflate z{packed.data(), packed.size()};
            z.out = out.data();
            z.outlen = out.size();
            try {
                z.run();
            } catch (Bad&) {
                throw Fail{DAMAGED};
            }
            if (z.outcnt != out.size()) throw Fail{DAMAGED};
        }
        if (crc32(out.data(), out.size()) != en.crc) throw Fail{DAMAGED};
        return out;
    }
};

// Safe relative paths only: no drive, no leading slash, no "..", no ":".
bool safe_path(const std::string& n) {
    if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find(':') != std::string::npos) return false;
    size_t p = 0;
    while (p <= n.size()) {
        size_t e = n.find_first_of("/\\", p);
        if (e == std::string::npos) e = n.size();
        std::string part = n.substr(p, e - p);
        if (part == ".." || part == ".") return false;
        p = e + 1;
    }
    return true;
}

void set_stage(const wchar_t* stage, float progress, void (*wake)()) {
    {
        std::lock_guard<std::mutex> lk(g_install.m);
        if (stage) g_install.stage = stage;
        g_install.progress = progress;
    }
    wake();
}

void install(std::wstring path, void (*wake)()) {
    std::wstring tmp;
    try {
        std::wstring file = path.substr(path.find_last_of(L"\\/") + 1);
        if (file.size() < 4 || _wcsicmp(file.c_str() + file.size() - 4, L".zip") != 0)
            throw Fail{L"That isn’t a .zip file. Choose a mod packaged as .zip."};
        set_stage(L"Reading zip…", 2, wake);
        Zip z;
        z.open(path);
        set_stage(nullptr, 10, wake);
        // Checks before anything is written: paths, sizes, methods.
        uint64_t total = 0, packed = 0;
        for (auto& e : z.entries) {
            if (!safe_path(e.name))
                throw Fail{L"That .zip has files with unsafe paths (\"..\" or absolute), so it wasn’t installed."};
            if (e.flags & 1) throw Fail{L"That .zip is password-protected; Wonderpark can’t open it."};
            if (e.method != 0 && e.method != 8) throw Fail{L"That .zip uses a kind of compression Wonderpark can’t read."};
            if (e.usize == 0xFFFFFFFF || e.csize == 0xFFFFFFFF) throw Fail{L"That .zip is too large (zip64) to install."};
            if (e.usize > MAX_FILE) throw Fail{L"That mod has a file over 512 MB, which is too large to install."};
            if (e.usize > 1048576 && e.usize / std::max<uint32_t>(1, e.csize) > 1000)
                throw Fail{L"That .zip unpacks to far more than its size, so it wasn’t installed."};
            total += e.usize;
            packed += e.csize;
        }
        if (total > MAX_TOTAL) throw Fail{L"That mod is too large to install (over 2 GB unpacked)."};

        // mod.json at the root, or in one top folder (the folder is then the root).
        set_stage(L"Checking mod.json…", 12, wake);
        std::string prefix;
        const Entry* manifest = nullptr;
        for (auto& e : z.entries) {
            std::string n = e.name;
            for (auto& c : n) if (c == '\\') c = '/';
            size_t slash = n.find('/');
            if (n == "mod.json" || (slash != std::string::npos && n.substr(slash + 1) == "mod.json" &&
                                    n.find('/', slash + 1) == std::string::npos)) {
                if (!manifest || n == "mod.json") {
                    manifest = &e;
                    prefix = n == "mod.json" ? "" : n.substr(0, slash + 1);
                }
            }
        }
        std::map<std::string, std::string> j;
        if (manifest) {
            std::vector<uint8_t> m = z.extract(*manifest);
            j = parse_json(std::string(m.begin(), m.end()));
            if (j.count("game") && !j["game"].empty() && j["game"] != "S2LE01")
                throw Fail{L"That mod is for another game (" + widen(j["game"]) + L"), not PokéPark 2 (S2LE01)."};
        } else {
            // No mod.json: everything in one top folder counts as the root.
            std::string top;
            bool one = !z.entries.empty();
            for (auto& e : z.entries) {
                std::string n = e.name;
                for (auto& c : n) if (c == '\\') c = '/';
                size_t s = n.find('/');
                std::string t = s == std::string::npos ? "" : n.substr(0, s + 1);
                if (t.empty() || (!top.empty() && t != top)) one = false;
                top = t;
            }
            if (one) prefix = top;
        }
        std::wstring name = j.count("name") && !j["name"].empty() ? widen(j["name"]) : nice_name(file);
        std::string id = slug(j.count("id") && !j["id"].empty() ? j["id"] : narrow(name));
        if (id.empty()) id = "mod";
        std::wstring wid = widen(id);
        set_stage(nullptr, 15, wake);

        // Copy into a fresh .install-<id> folder, then swap it in.
        std::wstring root = mods_dir();
        tmp = root + L"\\.install-" + wid;
        delete_tree(tmp);
        make_dirs(tmp);
        uint64_t done = 0;
        for (auto& e : z.entries) {
            std::string n = e.name;
            for (auto& c : n) if (c == '\\') c = '/';
            if (n.compare(0, prefix.size(), prefix) != 0) continue;
            n = n.substr(prefix.size());
            bool dir = !n.empty() && n.back() == '/';
            bool wanted = n == "mod.json" || n.compare(0, 9, "textures/") == 0 || n.compare(0, 6, "files/") == 0;
            if (n.empty() || dir || !wanted) {
                done += e.csize;
                continue;
            }
            std::vector<uint8_t> data = z.extract(e);
            std::wstring out = tmp + L"\\" + widen(n);
            for (auto& c : out) if (c == L'/') c = L'\\';
            make_dirs(out.substr(0, out.find_last_of(L'\\')));
            HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD put = 0;
            bool ok = h != INVALID_HANDLE_VALUE &&
                      (data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size()));
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            if (!ok) throw Fail{L"Couldn’t write the mod’s files. Is the drive full?"};
            done += e.csize;
            set_stage(L"Copying files…", 15 + 84 * (float)((double)done / std::max<uint64_t>(1, packed)), wake);
        }
        if (!manifest) {
            // Our own mod.json, so the card shows a name next time.
            std::string m = "{\"id\": \"" + id + "\", \"name\": \"" + narrow(name) +
                            "\", \"summary\": \"No description included with this mod.\"}\n";
            HANDLE h = CreateFileW((tmp + L"\\mod.json").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD put = 0;
            if (h != INVALID_HANDLE_VALUE) {
                WriteFile(h, m.data(), (DWORD)m.size(), &put, nullptr);
                CloseHandle(h);
            }
        }
        std::wstring final_dir = root + L"\\" + wid;
        delete_tree(final_dir);
        // Just-written files are often still open in a virus scanner for a
        // moment, which makes the folder's move fail: try for up to 15 s
        // (this is the worker thread; the window keeps running).
        bool moved = false;
        for (int tries = 0; tries < 60 && !moved; tries++) {
            moved = MoveFileExW(tmp.c_str(), final_dir.c_str(), 0) != 0;
            if (!moved) Sleep(250);
        }
        if (!moved) throw Fail{L"Couldn’t finish installing (the mods folder is in use?). Try again."};
        tmp.clear();
        // On, at the top of the list.
        std::vector<ModInfo> list = mods_list();
        auto it = std::find_if(list.begin(), list.end(), [&](const ModInfo& m) { return m.id == wid; });
        ModInfo me = it != list.end() ? *it : read_mod(wid);
        if (it != list.end()) list.erase(it);
        me.enabled = true;
        list.insert(list.begin(), me);
        mods_save(list);
        {
            std::lock_guard<std::mutex> lk(g_install.m);
            g_install.stage = L"Done";
            g_install.progress = 100;
            g_install.new_id = wid;
        }
    } catch (Fail& f) {
        if (!tmp.empty()) delete_tree(tmp);
        std::lock_guard<std::mutex> lk(g_install.m);
        g_install.error = f.msg;
    } catch (...) {
        if (!tmp.empty()) delete_tree(tmp);
        std::lock_guard<std::mutex> lk(g_install.m);
        g_install.error = L"Couldn’t install that mod (out of memory?).";
    }
    {
        std::lock_guard<std::mutex> lk(g_install.m);
        g_install.running = false;
        g_install.finished = true;
    }
    wake();
}

std::thread g_thread;
}  // namespace

void mods_install_start(const std::wstring& path, void (*wake)()) {
    {
        std::lock_guard<std::mutex> lk(g_install.m);
        if (g_install.running) return;
        g_install.running = true;
        g_install.finished = false;
        g_install.file = path.substr(path.find_last_of(L"\\/") + 1);
        g_install.stage = L"Reading zip…";
        g_install.progress = 0;
        g_install.error.clear();
        g_install.new_id.clear();
    }
    if (g_thread.joinable()) g_thread.join();
    g_thread = std::thread(install, path, wake);
}

void mods_install_join() {
    if (g_thread.joinable()) g_thread.join();
}

// One file out of a .zip (the HD Pack takes ONNX Runtime out of its package).
bool mods_zip_read(const std::wstring& zip, const std::string& name, std::vector<uint8_t>& out) {
    try {
        Zip z;
        z.open(zip);
        for (auto& e : z.entries)
            if (e.name == name) {
                out = z.extract(e);
                return true;
            }
    } catch (Fail&) {
    }
    return false;
}
