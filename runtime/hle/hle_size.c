/* Pokémon size (Silly; [silly] pokemon_size): 0 mini, 1 normal, 2 giant.
 *
 * Each character's "posture" (nsNile::nsObject::PostureExtraEntity's
 * record: the words 80446070 80446090, then position at +8, rotation in
 * degrees at +0x14 and a scale at +0x20, then the last frame's position
 * and rotation) is owned by its entity (the pointer at -8); the park's
 * other position records (boxes, signs, spawn points) have none. The
 * scale of every owned record is set each frame: the model is drawn that
 * size, while the game's collision stays as it was.
 *
 * The records are found by scanning MEM1 for the pair of words, on each
 * scene change and every 5 seconds (about 3 ms); only while the setting
 * isn't Normal, or to put records back to 1 after it was. */
#include <string.h>

#include "hle.h"
#include "host_settings.h"

#define MAX_POSTURES 512
#define POSTURE_A 0x80446070u
#define POSTURE_B 0x80446090u
#define POSTURE_SCALE 0x20

unsigned hle_scene_changes(void); /* hle_scene.c */

static u32 sPosture[MAX_POSTURES];
static int sCount;

static int plausible_scale(u32 a) {
    int i;
    for (i = 0; i < 3; i++) {
        u32 w = LD32(a + POSTURE_SCALE + 4 * i);
        float f;
        memcpy(&f, &w, 4);
        if (!(f > 0.02f && f < 20.0f)) return 0; /* (Mini and Paper: 0.04) */
    }
    return 1;
}

static void scan(void) {
    const uint8_t* ram = g_cpu->ram;
    u32 n = g_cpu->ram_size, o;
    sCount = 0;
    for (o = 8; o + 0x30 <= n && sCount < MAX_POSTURES; o += 4) {
        /* (Big-endian 80 44 60 70.) */
        if (ram[o] != 0x80 || ram[o + 1] != 0x44 || ram[o + 2] != 0x60 || ram[o + 3] != 0x70) continue;
        {
            u32 a = 0x80000000u + o, owner = LD32(a - 8);
            if (LD32(a + 4) != POSTURE_B || owner < 0x80000000u || owner >= 0x81800000u) continue;
            if (!plausible_scale(a)) continue;
            sPosture[sCount++] = a;
        }
    }
    WP_LOG("scene", "pokemon size: %d characters found", sCount);
}

/* Each record's scale: s across and up, s * depth front to back (Paper
 * Pokémon: the model's own z, the way it faces). */
static void set_all(float s, float depth) {
    u32 w, wz;
    int i;
    float z = s * depth;
    memcpy(&w, &s, 4);
    memcpy(&wz, &z, 4);
    for (i = 0; i < sCount; i++) {
        u32 a = sPosture[i];
        if (LD32(a) != POSTURE_A || LD32(a + 4) != POSTURE_B || !plausible_scale(a)) continue; /* gone */
        ST32(a + POSTURE_SCALE, w);
        ST32(a + POSTURE_SCALE + 4, w);
        ST32(a + POSTURE_SCALE + 8, wz);
    }
}

/* Each frame (hle_scene.c, SceneManager_RunBegin). Paper Pokémon (Silly;
 * [silly] paper_pokemon) flattens the same records to 8% front to back:
 * flat cut-outs that turn edge-on as they turn. */
void hle_size_frame(void) {
    static const float kScale[3] = {0.5f, 1.0f, 2.0f};
    static unsigned seenScene = ~0u;
    static int frames, applied = 1 * 2;
    int want = g_x[X_POKEMON_SIZE], paper = g_x[X_PAPER_POKEMON] != 0, state;
    if (want < 0 || want > 2) want = 1;
    state = want * 2 + paper;
    if (state == 1 * 2 && applied == 1 * 2) return; /* normal, and already put back */
    if (hle_scene_changes() != seenScene || ++frames >= 300) {
        seenScene = hle_scene_changes();
        frames = 0;
        scan();
    }
    set_all(kScale[want], paper ? 0.08f : 1.0f);
    applied = state;
}
