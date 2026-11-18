/* Which scene the game is in: the park, an attraction (minigame), a menu...
 * For the camera (hle_camera.c), which must leave minigames alone.
 *
 * From MildRain479/PokePark-2-Wiicompiled's recomp/patches/S2LE01/hle_game.c
 * (CC0): 0x80210660 picks the next scene by its index in the scene table
 * (0x80469360), r3 the index. */
#include "hle.h"

enum {
    SCENE_PARK,
    SCENE_STRAP,
    SCENE_HOLD,
    SCENE_CHECK_DATA,
    SCENE_ATTRACTION,
    SCENE_LOBBY,
    SCENE_START_MENU,
    SCENE_COLOSSEUM_MENU,
    SCENE_ACHIEVEMENT_MENU,
    SCENE_MUMA_MENU,
    SCENE_MOVIE,
    SCENE_STAFF_ROLL
};

static int sScene = -1;
static unsigned sChanges;

/* BOOL fn_80210660(s32 scene): picks the next scene. */
WP_HOOK(0x80210660, NextScene) {
    if ((int)c->gpr[3] != sScene) {
        extern unsigned gxb_game_frames(void); /* gx_frames.c */
        WP_LOG("scene", "next scene: %d (frame %u)", (int)c->gpr[3], gxb_game_frames());
        sScene = (int)c->gpr[3];
        sChanges++;
    }
    return 0;
}

int hle_scene(void) { return sScene; }
int hle_in_park(void) { return sScene == SCENE_PARK; }
int hle_in_minigame(void) { return sScene == SCENE_ATTRACTION; }
/* Changes whenever the scene does (the camera starts over). */
unsigned hle_scene_changes(void) { return sChanges; }

/* Whether an event script (a story scene, a talk, a favour) is running:
 * the script interface's (pointer at 0x804B04F0) +0x248 is nonzero while
 * one runs, as the EventManager checks before it starts one (from
 * MildRain479/PokePark-2-Wiicompiled's hle_autosave.c, CC0). */
int hle_event_running(void) {
    u32 esi = LD32(0x804B04F0u);
    return wp_mem_ptr(esi, 0x24C) && LD32(esi + 0x248) != 0;
}
