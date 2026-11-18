/* The F1 tab's Berries collected and Shop prices sliders (g_settings.berries,
 * g_settings.prices: percent of the game's).
 *
 * Confirmed (2026-11-17): the Berry count is the u32 at +0x1474 of the
 * loaded save in memory (the GD0 copy at the pointer stored at 0x804BA694,
 * hle_options.c): 1 in build/nand_wish_park's save, as the PokéPark Pad's
 * "Berries 1" showed, and 21 at +0x11EC (likely all ever collected). The
 * game changes it in about ten places (0x8014E9DC, 0x801D71F8, 0x801D78B0,
 * 0x802174F0, 0x802232E8, 0x802233F8, 0x80223508, 0x802238C8, 0x802239D0,
 * 0x80223AD8: script commands, shops, rewards), each as
 * berries = clamp(berries + change, 0, 99999), then adds the change to a
 * "collected" or "spent" tally.
 *
 * So rather than each place, the count is watched once a frame (on the game
 * thread, hle_frame.c): a rise is scaled by Berries collected, a drop by
 * Shop prices (the game's price is still shown and still needed to buy;
 * the slider changes what is taken). A frame in which much of the save
 * changed at once (a save loaded, a new game) only sets the count to
 * follow. */
#include <math.h>
#include <string.h>

#include "hle.h"
#include "host_settings.h"

#define SAVE_PTR 0x804BA694u
#define SAVE_SIZE 5344u
#define BERRIES 0x1474u
#define BERRIES_MAX 99999
/* More words than this changed in one frame: a load, not play. */
#define LOAD_WORDS 24

static uint8_t sShadow[SAVE_SIZE];
static u32 sBase;
static int sLast = -1;

static int scaled(int change, int percent) {
    double v = change * (percent / 100.0);
    return (int)(v < 0 ? floor(v) : floor(v + 0.5));
}

void hle_balance_tick(CPUState* c) {
    u32 base = LD32(SAVE_PTR);
    const uint8_t* save;
    int now, changed = 0, want;
    u32 i;
    (void)c;
    if (!wp_mem_ptr(base, SAVE_SIZE)) {
        sLast = -1;
        return;
    }
    save = wp_mem_ptr(base, SAVE_SIZE);
    now = (int)LD32(base + BERRIES);
    if (base != sBase || sLast < 0) {
        sBase = base;
        memcpy(sShadow, save, SAVE_SIZE);
        sLast = now;
        return;
    }
    if (now == sLast) {
        memcpy(sShadow, save, SAVE_SIZE);
        return;
    }
    for (i = 0; i < SAVE_SIZE; i += 4)
        if (memcmp(sShadow + i, save + i, 4) != 0 && ++changed > LOAD_WORDS) break;
    want = now;
    if (changed <= LOAD_WORDS) {
        int d = now - sLast;
        if (d > 0 && g_settings.berries != 100) want = sLast + scaled(d, g_settings.berries);
        else if (d < 0 && g_settings.prices != 100) want = sLast + scaled(d, g_settings.prices);
        if (want < 0) want = 0;
        if (want > BERRIES_MAX) want = BERRIES_MAX;
        if (want != now) {
            ST32(base + BERRIES, (u32)want);
            WP_LOG("balance", "berries %d -> %d: %d instead (%d%% collected, %d%% prices)", sLast, now, want,
                   g_settings.berries, g_settings.prices);
        }
    } else {
        WP_LOG("balance", "save changed (%s %d words): berries %d", changed > LOAD_WORDS ? "over" : "", changed, now);
    }
    sLast = want;
    memcpy(sShadow, save, SAVE_SIZE);
}
