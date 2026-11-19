// Extras > Saves (saves.h).
//
// What a save file says (the game's GD0 / GD1, 5,344 bytes, big-endian):
//   +0x1470  play time, seconds (the game's "Select a Saved Game" shows it
//            as h:mm: 465 there was 0:07)
//   +0x1474  berries
//   +0x14A6  where it was saved: area, then zone (Cove Town is 01 02),
//            named by the disc's PlaceName.msbt as Fd<area><zone>
//   friendship points: a u16 at +0x28 of each Pokémon's 8-byte entry, the
//            entry from the table at 0x80413100 in main.dol (as
//            runtime/hle/hle_friends.c reads it, from
//            MildRain479/PokePark-2-Wiicompiled, CC0); 100 or more is a friend.
// Place names and the table are read from the player's extracted disc
// (game\) when it's there; without it, those are left out.
#include "saves.h"

#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "mods.h"
#include "wii_save.h"

namespace {

const int kSlots = 2;

std::vector<uint8_t> read_file(const std::wstring& path, size_t limit = 64u << 20) {
    std::vector<uint8_t> d;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return d;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && (uint64_t)sz.QuadPart <= limit) {
        d.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, d.data(), (DWORD)d.size(), &got, nullptr) || got != d.size()) d.clear();
    }
    CloseHandle(h);
    return d;
}
bool write_file(const std::wstring& path, const uint8_t* p, size_t n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, p, (DWORD)n, &put, nullptr) && put == n;
    return CloseHandle(h) && ok;
}
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
std::string narrow(const std::wstring& w) {  // for wii_save.c's fopen (the ANSI code page)
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 1) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

// ---- The disc: LZ11, U8, MSBT, and a word of main.dol.

std::vector<uint8_t> lz11(const std::vector<uint8_t>& d) {
    if (d.size() < 4 || d[0] != 0x11) return d;
    size_t size = d[1] | d[2] << 8 | d[3] << 16, p = 4;
    if (!size && d.size() >= 8) size = d[4] | d[5] << 8 | d[6] << 16 | (size_t)d[7] << 24, p = 8;
    std::vector<uint8_t> out;
    out.reserve(size);
    while (out.size() < size && p < d.size()) {
        uint8_t flags = d[p++];
        for (int bit = 0; bit < 8 && out.size() < size && p < d.size(); bit++) {
            if (!(flags & (0x80 >> bit))) {
                out.push_back(d[p++]);
                continue;
            }
            int ind = d[p] >> 4;
            size_t n, disp;
            if (p + (ind == 0 ? 3 : ind == 1 ? 4 : 2) > d.size()) return out;
            if (ind == 0) n = (((d[p] & 0xF) << 4) | (d[p + 1] >> 4)) + 0x11, disp = (((d[p + 1] & 0xF) << 8) | d[p + 2]) + 1, p += 3;
            else if (ind == 1)
                n = (((d[p] & 0xF) << 12) | (d[p + 1] << 4) | (d[p + 2] >> 4)) + 0x111, disp = (((d[p + 2] & 0xF) << 8) | d[p + 3]) + 1, p += 4;
            else n = ind + 1, disp = (((d[p] & 0xF) << 8) | d[p + 1]) + 1, p += 2;
            if (disp > out.size()) return out;
            for (size_t k = 0; k < n; k++) out.push_back(out[out.size() - disp]);
        }
    }
    return out;
}
std::vector<uint8_t> u8_file(const std::vector<uint8_t>& a, const char* want) {
    if (a.size() < 0x20 || be32(&a[0]) != 0x55AA382D) return {};
    uint32_t root = be32(&a[4]);
    if (root + 12 > a.size()) return {};
    uint32_t n = be32(&a[root + 8]), strs = root + n * 12;
    for (uint32_t i = 1; i < n && root + i * 12 + 12 <= a.size(); i++) {
        const uint8_t* e = &a[root + i * 12];
        if (e[0] == 1) continue;
        uint32_t no = be32(e) & 0xFFFFFF, off = be32(e + 4), size = be32(e + 8);
        if (strs + no >= a.size() || (size_t)off + size > a.size()) continue;
        if (!strcmp((const char*)&a[strs + no], want)) return std::vector<uint8_t>(a.begin() + off, a.begin() + off + size);
    }
    return {};
}
std::map<std::string, std::wstring> msbt_texts(const std::vector<uint8_t>& m) {
    std::map<std::string, std::wstring> out;
    if (m.size() < 0x20 || memcmp(m.data(), "MsgStdBn", 8) || m[8] != 0xFE) return out;
    const uint8_t *lbl = nullptr, *txt = nullptr;
    size_t p = 0x20;
    for (uint32_t s = 0, ns = be16(&m[0x0E]); s < ns && p + 16 <= m.size(); s++) {
        uint32_t len = be32(&m[p + 4]);
        if (p + 16 + len > m.size()) break;
        if (!memcmp(&m[p], "LBL1", 4)) lbl = &m[p + 16];
        if (!memcmp(&m[p], "TXT2", 4)) txt = &m[p + 16];
        p += 16 + ((len + 15) & ~15u);
    }
    if (!lbl || !txt) return out;
    uint32_t texts = be32(txt);
    for (uint32_t i = 0; i < be32(lbl); i++) {
        const uint8_t* e = lbl + be32(lbl + 8 + i * 8);
        for (uint32_t k = 0; k < be32(lbl + 4 + i * 8); k++) {
            uint32_t ln = e[0], idx = be32(e + 1 + ln);
            std::string label((const char*)e + 1, ln);
            e += 5 + ln;
            if (idx >= texts) continue;
            std::wstring s;
            const uint8_t* t = txt + be32(txt + 4 + idx * 4);
            for (int guard = 0; guard < 256; guard++, t += 2) {
                uint32_t ch = be16(t);
                if (!ch) break;
                if (ch == 0x0E) {
                    t += 6 + be16(t + 6);  // the tag: group, type, size, then size bytes
                    continue;
                }
                if (ch >= 32) s += (wchar_t)ch;
            }
            out[label] = s;
        }
    }
    return out;
}
// count u32s at address addr in main.dol (its text and data sections).
std::vector<uint32_t> dol_words(const std::vector<uint8_t>& d, uint32_t addr, int count) {
    std::vector<uint32_t> out;
    if (d.size() < 0x100) return out;
    for (int s = 0; s < 18; s++) {
        uint32_t off = be32(&d[s * 4]), at = be32(&d[0x48 + s * 4]), size = be32(&d[0x90 + s * 4]);
        if (addr >= at && addr + count * 4 <= at + size && off + (addr - at) + count * 4 <= d.size()) {
            for (int i = 0; i < count; i++) out.push_back(be32(&d[off + (addr - at) + i * 4]));
            return out;
        }
    }
    return out;
}

// What's read from the disc once.
struct Disc {
    bool tried = false;
    std::map<std::string, std::wstring> places;
    std::vector<uint32_t> entries;  // per Pokémon: its friendship entry
} g_disc;

void read_disc(const std::wstring& root) {
    if (g_disc.tried) return;
    g_disc.tried = true;
    auto msgs = lz11(read_file(root + L"\\game\\files\\Archive\\ResidentMessage.dac"));
    g_disc.places = msbt_texts(u8_file(msgs, "PlaceName.msbt"));
    g_disc.entries = dol_words(read_file(root + L"\\game\\sys\\main.dol"), 0x80413100u, 198);
}

std::wstring slot_name(int slot, const wchar_t* kind) { return std::wstring(kind) + std::to_wstring(slot); }
// The files of save file slot (and the banner), as named in the folder.
std::vector<std::wstring> slot_files(int slot) {
    return {slot_name(slot, L"GD"), slot_name(slot, L"GD") + L"M", slot_name(slot, L"Pic"), L"banner.bin"};
}
bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
void make_dirs(const std::wstring& path) {
    for (size_t i = 3; i <= path.size(); i++)
        if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
}
void delete_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}
uint64_t now_ft() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
}
std::wstring ini_path() { return mods_data_dir() + L"\\launcher.ini"; }

}  // namespace

std::wstring saves_dir() { return mods_data_dir() + L"\\nand\\title\\00010000\\53324c45\\data"; }
std::wstring saves_backup_dir() { return mods_data_dir() + L"\\backups\\saves"; }

std::vector<SaveFile> saves_read(const std::wstring& root) {
    read_disc(root);
    std::vector<SaveFile> out;
    for (int s = 0; s < kSlots; s++) {
        SaveFile f;
        f.slot = s;
        std::wstring path = saves_dir() + L"\\" + slot_name(s, L"GD");
        auto d = read_file(path, 1 << 20);
        if (d.size() >= 5344) {
            f.play_seconds = (int)be32(&d[0x1470]);
            f.berries = (int)be32(&d[0x1474]);
            // A file the game made but never played (a new game's) is empty.
            f.exists = f.play_seconds > 0 || f.berries > 0;
            int area = d[0x14A6], zone = d[0x14A7];
            char label[16];
            snprintf(label, sizeof(label), "Fd%02d%02d", area, zone);
            auto it = g_disc.places.find(label);
            if (it == g_disc.places.end()) snprintf(label, sizeof(label), "Fd%02d", area), it = g_disc.places.find(label);
            if (it != g_disc.places.end()) f.place = it->second;
            if (g_disc.entries.size() == 198) {
                f.friends = 0;
                for (int p = 4; p < 198; p++) {
                    size_t at = (size_t)g_disc.entries[p] * 8 + 0x28;
                    if (at + 2 <= 5344 && be16(&d[at]) >= 100) f.friends++;
                }
            }
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
                f.last_played = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
        }
        out.push_back(f);
    }
    return out;
}

std::vector<SaveBackup> saves_backups() {
    std::vector<SaveBackup> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((saves_backup_dir() + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        SaveBackup b;
        b.folder = saves_backup_dir() + L"\\" + n;
        std::wstring about = b.folder + L"\\about.ini";
        wchar_t label[128];
        GetPrivateProfileStringW(L"backup", L"label", L"Backup", label, 128, about.c_str());
        b.label = label;
        b.automatic = GetPrivateProfileIntW(L"backup", L"automatic", 0, about.c_str()) != 0;
        b.when = (uint64_t)fd.ftCreationTime.dwHighDateTime << 32 | fd.ftCreationTime.dwLowDateTime;
        out.push_back(b);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const SaveBackup& a, const SaveBackup& b) { return a.when > b.when; });
    return out;
}

bool saves_game_running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = !_wcsicmp(pe.szExeFile, L"Wonderpark.exe");
    CloseHandle(snap);
    return found;
}

std::wstring saves_backup(int slot, const std::wstring& label, bool automatic) {
    if (saves_game_running()) return L"Close the game first: it may be saving.";
    std::vector<std::wstring> names;
    if (slot >= 0) names = slot_files(slot);
    else
        for (int s = 0; s < kSlots; s++)
            for (auto& n : slot_files(s))
                if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    bool any = false;
    for (auto& n : names) any |= exists(saves_dir() + L"\\" + n);
    if (!any) return L"There's no save to back up yet.";
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"%04d%02d%02d-%02d%02d%02d-%03d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);
    std::wstring dir = saves_backup_dir() + L"\\" + stamp;
    make_dirs(dir);
    for (auto& n : names) {
        std::wstring from = saves_dir() + L"\\" + n;
        if (exists(from) && !CopyFileW(from.c_str(), (dir + L"\\" + n).c_str(), FALSE)) {
            delete_tree(dir);
            return L"The backup couldn't be written.";
        }
    }
    std::wstring about = dir + L"\\about.ini";
    WritePrivateProfileStringW(L"backup", L"label", label.c_str(), about.c_str());
    WritePrivateProfileStringW(L"backup", L"automatic", automatic ? L"1" : L"0", about.c_str());
    if (automatic) {  // the newest 10 automatic backups are kept
        int kept = 0;
        for (auto& b : saves_backups())
            if (b.automatic && ++kept > 10) delete_tree(b.folder);
    }
    return L"";
}

std::wstring saves_restore(const SaveBackup& b) {
    if (saves_game_running()) return L"Close the game first.";
    std::wstring err = saves_backup(-1, L"Before restore", false);
    if (!err.empty() && err != L"There's no save to back up yet.") return err;
    make_dirs(saves_dir());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((b.folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"That backup is gone.";
    bool ok = true;
    do {
        std::wstring n = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n == L"about.ini") continue;
        ok &= CopyFileW((b.folder + L"\\" + n).c_str(), (saves_dir() + L"\\" + n).c_str(), FALSE) != 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok ? L"" : L"Some files couldn't be put back.";
}

std::wstring saves_export(int slot, const std::wstring& path) {
    std::vector<std::vector<uint8_t>> keep;
    std::vector<WiiSaveFile> files;
    for (auto& n : slot_files(slot)) {
        auto d = read_file(saves_dir() + L"\\" + n);
        if (d.empty()) continue;
        keep.push_back(std::move(d));
        WiiSaveFile f = {};
        snprintf(f.name, sizeof(f.name), "%ls", n.c_str());
        f.data = keep.back().data();
        f.size = (uint32_t)keep.back().size();
        files.push_back(f);
    }
    // (the pointers stay good: keep only grows above, and is reserved enough)
    if (files.empty()) return L"That file has no save in it.";
    for (size_t i = 0; i < files.size(); i++) files[i].data = keep[i].data();
    size_t size = 0;
    uint8_t* zip = wiisave_write_zip(files.data(), (int)files.size(), wiisave_dos_now(), &size);
    if (!zip) return L"The .zip couldn't be made.";
    bool ok = write_file(path, zip, size);
    free(zip);
    return ok ? L"" : L"The .zip couldn't be written there.";
}

const wchar_t kWiiSaveHint[] =
    L"For a real Wii save: Import it into Dolphin first (Tools > Import Wii Save), then import the Dolphin folder here.";

std::wstring saves_import(const std::wstring& path) {
    if (saves_game_running()) return L"Close the game first.";
    size_t slash = path.find_last_of(L"\\/");
    std::wstring folder = slash == std::wstring::npos ? L"." : path.substr(0, slash);
    std::string base = narrow(path.substr(slash == std::wstring::npos ? 0 : slash + 1));
    std::vector<std::vector<uint8_t>> keep;
    std::vector<WiiSaveFile> files;
    auto add = [&](const std::wstring& p, const std::string& name) {
        auto d = read_file(p);
        if (d.empty()) return;
        keep.push_back(std::move(d));
        WiiSaveFile f = {};
        snprintf(f.name, sizeof(f.name), "%s", name.c_str());
        files.push_back(f);
    };
    if (wiisave_own_name(base.c_str())) {
        // One of the files in Dolphin's save folder: the whole folder.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::string n = narrow(fd.cFileName);
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wiisave_own_name(n.c_str()))
                    add(folder + L"\\" + fd.cFileName, n);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    } else {
        add(path, base);  // a .zip
    }
    if (files.empty()) return L"That file couldn't be read.";
    for (size_t i = 0; i < files.size(); i++) files[i].data = keep[i].data(), files[i].size = (uint32_t)keep[i].size();
    // A lone file: a zip (unpacked). A real Wii's data.bin isn't read.
    WiiSaveFile unpacked[32];
    int count = 0;
    bool fromZip = false;
    if (files.size() == 1 && !wiisave_own_name(files[0].name)) {
        if (!wiisave_is_zip(files[0].data, files[0].size))
            return std::wstring(L"That isn't a .zip or a file from Dolphin's save folder. ") + kWiiSaveHint;
        int r = wiisave_read_zip(files[0].data, files[0].size, unpacked, 32, &count);
        fromZip = r == WIISAVE_OK;
        if (r != WIISAVE_OK) return widen(wiisave_message(r)) + L".";
    } else {
        count = (int)std::min<size_t>(files.size(), 32);
        for (int i = 0; i < count; i++) unpacked[i] = files[i];
    }
    std::wstring result;
    if (!wiisave_check_files(unpacked, count)) {
        result = L"That isn't a save of this game.";
    } else {
        std::wstring err = saves_backup(-1, L"Before import", false);
        if (!err.empty() && err != L"There's no save to back up yet.") {
            result = err;
        } else {
            // Merged in: the files brought in replace theirs; the other save
            // file is left as it is.
            make_dirs(saves_dir());
            for (int i = 0; i < count && result.empty(); i++)
                if (!write_file(saves_dir() + L"\\" + widen(unpacked[i].name), unpacked[i].data, unpacked[i].size))
                    result = L"The save couldn't be written.";
        }
    }
    if (fromZip)
        for (int i = 0; i < count; i++) free((void*)unpacked[i].data);
    return result;
}

bool saves_auto_backup() { return GetPrivateProfileIntW(L"saves", L"backup_before_play", 1, ini_path().c_str()) != 0; }
void saves_set_auto_backup(bool on) {
    make_dirs(mods_data_dir());
    WritePrivateProfileStringW(L"saves", L"backup_before_play", on ? L"1" : L"0", ini_path().c_str());
}
