/* Dance Inferno: the Dance timing offset setting (Controls; [controls]
 * rhythm_offset, -100..100 ms).
 *
 * The dance keeps its place in the song as a tick, one a frame, which its
 * judging and the move icons scrolling along the bottom both follow; the
 * music streams on its own. So moving the tick moves when a move is due
 * against the music: a positive offset (presses feel late) puts the tick
 * back, so the judging waits that much longer, and a negative one forward.
 * Applied once per part of the song (and again if the setting changes);
 * whole frames of the game's frame length (0x804B9C0C: 60 a second in the
 * dance, so steps of 17 ms; +100 ms is 6 frames).
 *
 * From MildRain479/PokePark-2-Wiicompiled's hle_dance.c (CC0): the dance's
 * object (vtable 0x80463280) has two parts to its song, each with its own
 * frame function (0x801B8170, then 0x801B8850, r3 the dance) and tick
 * (+0x1F4C, then +0x1F50). */
#include "hle.h"
#include "host_settings.h"

#define DANCE_VTABLE 0x80463280u
#define FRAME_LENGTH 0x804B9C0Cu /* the game's frame length, in time base ticks */

static const u32 kTick[2] = {0x1F4C, 0x1F50};

static void offset_frame(u32 d, int part) {
    static int applied[2];
    static int32_t last[2];
    int32_t tick;
    int want;
    double frameMs;
    if (!d || LD32(d) != DANCE_VTABLE) return;
    tick = (int32_t)LD32(d + kTick[part]);
    if (tick < last[part]) applied[part] = 0; /* the part started again (or a new dance) */
    frameMs = LD32(FRAME_LENGTH) ? LD32(FRAME_LENGTH) * 1000.0 / TB_HZ : 1000.0 / 30;
    want = (int)(g_x[X_RHYTHM_OFFSET] / frameMs + (g_x[X_RHYTHM_OFFSET] >= 0 ? 0.5 : -0.5));
    if (want != applied[part] && tick > 0) {
        tick -= want - applied[part];
        ST32(d + kTick[part], (u32)tick);
        WP_LOG("dance", "part %d: timing offset %d ms = %d frames (tick now %d)", part + 1, g_x[X_RHYTHM_OFFSET], want,
               (int)tick);
        applied[part] = want;
    }
    last[part] = tick;
}

/* The dance's frame, its song's first part. */
WP_HOOK(0x801B8170, DanceInferno_Frame) {
    offset_frame(c->gpr[3], 0);
    return 0;
}

/* The same, the second part. */
WP_HOOK(0x801B8850, DanceInferno_Frame2) {
    offset_frame(c->gpr[3], 1);
    return 0;
}
