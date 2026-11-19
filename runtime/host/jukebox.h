/* The F1 notebook's Jukebox (jukebox.cpp): the game's own music, read from
 * the player's extracted disc and played over the paused game. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { JUKEBOX_PARK = 1, JUKEBOX_WISH = 2, JUKEBOX_ATTRACTION = 3, JUKEBOX_STORY = 4 };

typedef struct {
    char bank[48]; /* the disc's music file, without _streamfiles.awb ("S_BGM_Fd0102") */
    int index;     /* the song's number in it, from 0 */
    int kind;      /* JUKEBOX_PARK...: which tab */
    int area;      /* Park, Wish Park: area << 8 | zone (Cove Town 0x0102); Attraction: its AttractionId */
    int seconds;   /* how long, once through */
    int heard;     /* the game has played it (since Wonderpark noted it) */
} JukeboxTrack;

void jukebox_init(void);              /* main.c, before the game runs: what's been heard */
void jukebox_note_read(const char* path, unsigned long long offset);  /* ios/dvd.c: the game read a music file */
void jukebox_scan_start(void);        /* finds the songs on the disc (a thread of its own; once) */
int jukebox_state(void);              /* 0 not started, 1 reading, 2 ready, 3 none found */
int jukebox_count(void);
int jukebox_track(int i, JukeboxTrack* out);

/* Playing (only while the notebook is open: the game is paused). */
void jukebox_play(int i);
void jukebox_toggle_pause(void);
void jukebox_stop(void);              /* when the notebook closes */
void jukebox_step(int dir);           /* the next (1) or the one before (-1), among those heard */
int jukebox_playing(void);            /* the song playing (or paused), -1 none */
int jukebox_paused(void);
double jukebox_position(void);        /* seconds into it */
int jukebox_shuffle(void);
int jukebox_repeat(void);
void jukebox_set_shuffle(int on);
void jukebox_set_repeat(int on);

#ifdef __cplusplus
}
#endif
