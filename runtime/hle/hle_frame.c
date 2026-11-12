/* The Frame rate setting (30 or 60).
 *
 * The game draws a frame each time its frame timer fires (docs/handoff/
 * FRAMERATE.md, "Confirmed"): timer 1 of two, whose requested period each
 * scene sets (0x1EEE10 time-base ticks, 29.97 Hz, in nearly all of them),
 * and which the game applies at the start of each frame (0x8000EFA0, called
 * for both timers by 0x8000F190). Gameplay doesn't depend on it: each
 * frame runs one logic step per 60 Hz controller sample, two at 30 fps,
 * one at 60. With Frame rate 60, a scene's request for 0x1EEE10 is turned
 * into 0xF7708 (59.94 Hz), the period the game itself uses at start, just
 * before it is applied each frame, and back again at 30, so the setting
 * takes effect at once. Other periods a scene asks for are left alone, and
 * while a movie plays it stays 30 (below).
 * (As the community's 60 FPS Gecko code does, but in memory and only while
 * the setting says so.) */
#include "hle.h"
#include "host_settings.h"

#define PERIOD_30 0x1EEE10u /* the scenes' */
#define PERIOD_60 0xF7708u  /* the start-up screens', half */

static u32 sGameAsked;   /* the period the scene asked for */
static u32 sWeWrote = ~0u; /* what we last put there instead */

/* (settings.c calls it when the setting changes; the hook reads the
 * setting itself each frame, so a change from F1 or the Options page takes
 * effect at once too.) */
void hle_set_frame_rate(int fps) { (void)fps; }

/* void 0x8000F190(FrameTimer timers[2]): applies each timer's requested
 * period. Timer 1 is the one the main loop waits for; its requested period
 * is the 64-bit time-base count at +0x40 (the low word at +0x44). */
WP_HOOK(0x8000F190, frame_timers) {
    u32 t1 = c->gpr[3] + 112, hi = LD32(t1 + 0x40), lo = LD32(t1 + 0x44), want;
    extern const char* wp_movie_playing(void); /* dvd.c */
    if (hi == 0 && lo != sWeWrote) sGameAsked = lo; /* the scene's own request */
    /* Movies (Movie/*.mo, the title's and the story's) play one picture a
     * drawn frame: at 60 they ran twice as fast (the title's was read 1.8x
     * as fast). While one is being read (and 5 s after), 30. */
    want = g_settings.frame_rate >= 60 && sGameAsked == PERIOD_30 && !wp_movie_playing() ? PERIOD_60 : sGameAsked;
    if (hi == 0 && lo != want) ST32(t1 + 0x44, want);
    sWeWrote = want == sGameAsked ? ~0u : want;
    return 0; /* the game's own code then applies it */
}
