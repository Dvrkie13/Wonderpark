// The launcher's Extras > Saves page (saves.cpp): the game's save files in
// our NAND folder, backups of them, import (Dolphin's save folder or a .zip
// of it) and export (a .zip in Dolphin's layout).
//
// The game keeps two save files, GD0 and GD1 (each with a mirror, GD0M and
// GD1M, and its photos, Pic0 and Pic1), in
//   <data folder>\nand\title\00010000\53324c45\data
// (the data folder: %APPDATA%\Wonderpark, or WONDERPARK_DATA_DIR). Backups
// are folders in <data folder>\backups\saves\, one per backup, holding
// copies of the files backed up. Changes happen only while the game is
// closed, and restoring or importing always backs up the saves first.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SaveFile {
    int slot = 0;            // 0 or 1 (the game's file 1 or 2)
    bool exists = false;     // false: "Empty"
    int play_seconds = 0;    // the game's own play time
    int berries = 0;
    int friends = -1;        // -1: not known (the disc's tables not read)
    std::wstring place;      // where it was saved ("Cove Town"), "" if not known
    uint64_t last_played = 0;  // the file's time (FILETIME), 0 if none
};

struct SaveBackup {
    std::wstring folder;     // its folder's full path
    std::wstring label;      // "Before Play", "File 1", "Before restore"...
    uint64_t when = 0;       // FILETIME
    bool automatic = false;  // made before Play (the last 10 are kept)
};

// The save files' folder, and the backups' folder.
std::wstring saves_dir();
std::wstring saves_backup_dir();
// The game's two save files. root: the Wonderpark folder (its game\ holds
// the extracted disc, for place names and the friends table).
std::vector<SaveFile> saves_read(const std::wstring& root);
// Backups, newest first.
std::vector<SaveBackup> saves_backups();
// Whether Wonderpark.exe (the game) is running: then nothing may change.
bool saves_game_running();

// Backs up file slot (0, 1) or, with slot -1, every save file. label as in
// SaveBackup; automatic ones beyond the newest 10 are removed. Returns ""
// or a plain-language error.
std::wstring saves_backup(int slot, const std::wstring& label, bool automatic);
// Puts a backup back (after backing up the saves as they are now).
std::wstring saves_restore(const SaveBackup& b);
// Writes file slot as a .zip in Dolphin's layout (its GD, mirror, photos and
// the banner) to path.
std::wstring saves_export(int slot, const std::wstring& path);
// Brings in a save from path: a .zip, or one of the files in Dolphin's save
// folder (the whole folder is taken). Backs up first. A real Wii's data.bin
// isn't read (Wonderpark doesn't ship the Wii's SD key): kWiiSaveHint.
std::wstring saves_import(const std::wstring& path);
// What to do with a real Wii save, shown on the Saves page.
extern const wchar_t kWiiSaveHint[];

// "Back up before every Play" (launcher.ini in the data folder; on by default).
bool saves_auto_backup();
void saves_set_auto_backup(bool on);
