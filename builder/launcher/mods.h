// Mods for the launcher's Mods page: installed mods, mods.ini, and
// installing a mod from a .zip on a worker thread (mods.cpp).
//
// A mod is a .zip with mod.json at its root (or in one top folder):
//   {"id", "name", "summary", "version", "author", "game": "S2LE01"}
// and textures/ (replacement textures, by hash) and files/ (replacement disc
// files at the same paths). It is installed to
// %APPDATA%\Wonderpark\mods\<id>\ (WONDERPARK_DATA_DIR\mods\ if that's set);
// on/off and load order are in %APPDATA%\Wonderpark\mods.ini, which the
// game reads at start (runtime/host/mods.cpp).
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ModInfo {
    std::wstring id, name, summary, version, author;
    bool enabled = true;
};

// The data folder (%APPDATA%\Wonderpark, or WONDERPARK_DATA_DIR) and its mods folder.
std::wstring mods_data_dir();
std::wstring mods_dir();

// Installed mods in mods.ini's order (the first listed wins where two change
// the same thing), then any not listed there.
std::vector<ModInfo> mods_list();
// Writes mods.ini from the list (its order and switches).
bool mods_save(const std::vector<ModInfo>& mods);

// One install at a time, on its own thread. The window reads the state.
struct ModInstall {
    std::mutex m;
    bool running = false, finished = false;
    std::wstring file;     // the .zip's name
    std::wstring stage;    // Reading zip…, Checking mod.json…, Copying files…, Done
    float progress = 0;    // 0-100
    std::wstring error;    // a plain-language message when it failed
    std::wstring new_id;   // the mod installed
};
extern ModInstall g_install;

// Starts installing path (a .zip); wake() is called when something changes.
void mods_install_start(const std::wstring& path, void (*wake)());
void mods_install_join();

// One file (by its full name in the zip) out of a .zip, unpacked and
// checked; false if it isn't there or the zip is damaged.
bool mods_zip_read(const std::wstring& zip, const std::string& name, std::vector<uint8_t>& out);
