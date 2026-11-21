// The launcher's music (music.cpp): songs unlocked in the game's Jukebox,
// from the player's own disc, only once the full build has finished.
#pragma once

#include <string>

void music_init(const std::wstring& root, const std::wstring& dataDir);
void music_allow(bool allowed);      // true only in the Play state (a finished build); else silent
void music_active(bool active);      // the launcher in front (paused in the background)
void music_stop_for_play();          // Play pressed: no more music this run
void music_skip();                   // the next song
bool music_muted();
void music_set_muted(bool muted);    // remembered (launcher.ini, [music] muted)
std::wstring music_now_playing();    // the song's title; "" when nothing plays
void music_shutdown();
