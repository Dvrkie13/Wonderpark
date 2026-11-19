/* Which scene the game is in: the park, an attraction (minigame), a menu...
 * For the camera (hle_camera.c), which must leave minigames alone.
 *
 * From MildRain479/PokePark-2-Wiicompiled's recomp/patches/S2LE01/hle_game.c
 * (CC0): 0x80210660 picks the next scene by its index in the scene table
 * (0x80469360), r3 the index. */
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "host_settings.h"

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

/* Which attraction: the scripts and menus pass it from scene to scene as a
 * parameter, "AttractionId" (0-3). From MildRain479/PokePark-2-
 * Wiicompiled's hle_game.c (CC0). */
static int sAttraction;

/* void AddSceneParameter(Params*, const char* name, s32 value) */
WP_HOOK(0x80206DC0, AddSceneParameter) {
    static const char kWant[] = "AttractionId";
    u32 name = c->gpr[4];
    int i;
    for (i = 0; kWant[i] && LD8(name + i) == (u32)(uint8_t)kWant[i]; i++) {}
    if (!kWant[i] && !LD8(name + i)) {
        sAttraction = (int)c->gpr[5] + 1;
        WP_LOG("scene", "attraction %d", sAttraction);
    }
    return 0;
}

/* The attraction being played (1 Cake Contraption, 2 Dance Inferno, 3
 * Power Bash, 4 Flight of Fancy), or 0 outside one. */
int hle_minigame(void) { return sScene == SCENE_ATTRACTION ? sAttraction : 0; }

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

/* Tests: WONDERPARK_ATTRACTION=<n>,<seconds> enters attraction n (1 Cake
 * Contraption, 2 Dance Inferno, 3 Power Bash, 4 Flight of Fancy) that many
 * seconds after the park first shows, the way its gate does: the index in
 * the next scene's parameters ("AttractionId"), then the park scene ended.
 * From MildRain479/PokePark-2-Wiicompiled's hle_game.c (CC0, its
 * RECOMP_ATTRACTION). SceneManager::RunBegin runs each frame at a point
 * where the scene can be changed. */
#define SCENE_NEXT_PARAMS 0xAC    /* SceneManager: the next scene's parameters */
#define ATTRACTION_ID_NAME 0x804530C4u /* "AttractionId" */
/* Skip startup logos (Gameplay; [gameplay] skip_logos): the boot screens
 * (Strap: the logos and the wrist strap warning, up for a fixed time while
 * the park loads in the background) run at 10 times the speed. Not Hold
 * (the "Loading..." screen after it): sped up, it took longer. Leaving
 * Strap early instead (as MildRain479/PokePark-2-Wiicompiled's hle_game.c
 * does, once the park's data is loaded: here 0x804BA89C reaches 2) left our
 * build on a blank screen. Only our own speed: one set from the debug
 * overlay (F7) or the speed-up key is left alone; movies play at their own
 * pace. */
static void boot_speed(void) {
    extern const char* wp_movie_playing(void);
    static int ours = 1;
    int want = g_x[X_SKIP_LOGOS] && sScene == SCENE_STRAP && !wp_movie_playing() ? 10 : 1;
    if (want == ours || wp_game_speed() != ours) return; /* nothing to do, or someone else's speed */
    wp_set_game_speed(want);
    if (want == 10) WP_LOG("scene", "boot screens sped up");
    ours = want;
}

WP_HOOK(0x80206A70, SceneManager_RunBegin) {
    static int state = -1; /* -1 not read, 0 nothing to do, else the attraction */
    static double at, parkSince = -1;
    boot_speed();
    {
        extern void hle_size_frame(void); /* hle_size.c: Pokémon size */
        hle_size_frame();
    }
    if (state < 0) {
        const char* e = getenv("WONDERPARK_ATTRACTION");
        const char* comma = e ? strchr(e, ',') : NULL;
        state = e && *e ? atoi(e) : 0;
        if (state < 1 || state > 4) state = 0;
        at = comma ? atof(comma + 1) : 0;
    }
    if (!state || sScene != SCENE_PARK) return 0;
    if (parkSince < 0) parkSince = wp_wall_seconds();
    if (wp_wall_seconds() - parkSince < at) return 0;
    {
        static const char kName[] = "AttractionId";
        u32 mgr, i;
        for (i = 0; i < sizeof(kName); i++)
            if (LD8(ATTRACTION_ID_NAME + i) != (u32)(uint8_t)kName[i]) {
                WP_WARN("WONDERPARK_ATTRACTION: the parameter's name isn't where it should be");
                state = 0;
                return 0;
            }
        mgr = wp_call_guest(c, 0x80074AE0u, 0, 0, 0, 0); /* SceneManager::Get */
        wp_call_guest(c, 0x80206DC0u, mgr + SCENE_NEXT_PARAMS, ATTRACTION_ID_NAME, (u32)(state - 1), 0);
        mgr = wp_call_guest(c, 0x80074AE0u, 0, 0, 0, 0);
        wp_call_guest(c, 0x80206D30u, mgr, 0, 0, 0); /* EndScene */
        WP_LOG("scene", "entering attraction %d (WONDERPARK_ATTRACTION)", state);
        state = 0;
    }
    return 0;
}
