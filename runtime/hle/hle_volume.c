/* The F1 tab's Music, Sound effects and Pokémon voices sliders
 * (g_x[X_VOLUME_MUSIC], X_VOLUME_SFX, X_VOLUME_VOICES), through the game's
 * own CRI sound categories.
 *
 * Confirmed (2026-11-17): the game's CRI config, Sound/nile_sound.acf,
 * has nine categories (CategoryName table, name = id): PK_VOICE 0, BGM 1,
 * PC_VOICE 2, PK_ACT 3, PC_ACT 4, ENV 5, ALWAYS_SE 6, AT_SE 7, JGL 8.
 * 0x80396244 is criAtomExCategory_SetVolumeById(id, volume in f1): it maps
 * the id to its 76-byte record (the table at word +12 of the category
 * module, whose pointer is at r13-9476; the number of them at +40), stores
 * the volume at +16 and applies it to the sounds playing. The game calls it
 * itself from its sound scripts' commands (0x800697F0), so the hook keeps
 * what the game last asked for and passes on that times the slider; when a
 * slider moves, hle_volume_tick (each frame, on the game's main thread)
 * sets each category again. The ACF is registered once the word at +60 of
 * the AtomEx state (pointer at r13-9160) is nonzero.
 *
 * Music is BGM and JGL (jingles); voices PK_VOICE and PC_VOICE (Pikachu
 * and the other playable Pokémon); everything else is sound effects. */
#include <string.h>

#include "hle.h"
#include "host_settings.h"

#define SET_VOLUME_BY_ID 0x80396244u
#define CATEGORIES 9

static float sGame[CATEGORIES] = {1, 1, 1, 1, 1, 1, 1, 1, 1}; /* what the game asked for */
static int sApplied[3] = {100, 100, 100};                      /* the sliders, as last applied */

static int slider_for(int id) {
    if (id == 1 || id == 8) return X_VOLUME_MUSIC;
    if (id == 0 || id == 2) return X_VOLUME_VOICES;
    return X_VOLUME_SFX;
}

/* A slider's gain: squared, as the master volume's, so it feels even. */
static float gain(int id) {
    float v = g_x[slider_for(id)] / 100.0f;
    return v * v;
}

WP_HOOK(0x80396244, criAtomExCategory_SetVolumeById) {
    int id = (int)c->gpr[3];
    if (id >= 0 && id < CATEGORIES) {
        sGame[id] = (float)c->fpr[1];
        c->fpr[1] = c->ps1[1] = (double)(sGame[id] * gain(id));
        WP_LOG("volume", "game sets category %d to %.2f (x %.2f)", id, sGame[id], gain(id));
    }
    return 0; /* the original, with the volume changed */
}

/* Whether CRI's categories are ready to be set (the ACF registered and the
 * category module holding at least the nine). */
static int categories_ready(CPUState* c) {
    u32 r13 = c->gpr[13], ex = LD32(r13 - 9160), cat = LD32(r13 - 9476);
    if (!wp_mem_ptr(ex, 64) || !LD32(ex + 60)) return 0;
    if (!wp_mem_ptr(cat, 44)) return 0;
    return (int)LD32(cat + 40) >= CATEGORIES;
}

/* Each frame (hle_frame.c, on the game's main thread): when a slider has
 * moved, every category is set again (through the hook above). */
void hle_volume_tick(CPUState* c) {
    int now[3] = {g_x[X_VOLUME_MUSIC], g_x[X_VOLUME_SFX], g_x[X_VOLUME_VOICES]};
    double f1, p1;
    u32 r3;
    int id;
    if (!memcmp(now, sApplied, sizeof(now)) || !categories_ready(c)) return;
    memcpy(sApplied, now, sizeof(now));
    /* wp_call_guest keeps every register but r3 and the floats we set. */
    r3 = c->gpr[3];
    f1 = c->fpr[1];
    p1 = c->ps1[1];
    for (id = 0; id < CATEGORIES; id++) {
        float want = sGame[id];
        c->fpr[1] = c->ps1[1] = want;
        wp_call_guest(c, SET_VOLUME_BY_ID, (u32)id, 0, 0, 0);
        sGame[id] = want; /* the hook saw this as the game's; it is */
    }
    c->gpr[3] = r3;
    c->fpr[1] = f1;
    c->ps1[1] = p1;
    WP_LOG("volume", "sliders: music %d%%, effects %d%%, voices %d%%", now[0], now[1], now[2]);
}
