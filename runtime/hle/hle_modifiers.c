/* The F1 tab's Damage you deal, Damage you take and Chase speed sliders
 * (g_settings.damage_dealt, damage_taken, chase_speed: percent of the
 * game's).
 *
 * Adapted from MildRain479/PokePark-2-Wiicompiled's
 * recomp/patches/S2LE01/hle_modifiers.c (CC0): its addresses and offsets,
 * our hooks. Its addresses were checked against our recompiled code
 * (docs/camera-and-extras.md): each is the function's entry, and
 * 0x800B0200's first lines are "left = left - damage".
 *
 * Battles: every Pokémon's hit points are a LifeModuleImpl (+0x08 left,
 * +0x0C most); damage is taken by its virt0x38 (0x800B0200, f1 the
 * damage). The player's is the one its ModuleAccessorPlayer (vtable
 * 0x8044A850) holds at +0x60; damage to any other was dealt by the player.
 * The player's accessor is learned from the movement update below, which
 * runs for it every frame.
 *
 * Chases: the Pokémon being chased is steered by its AI's chase action
 * (FieldAIActionChallengeChase), whose step (0x801E1210) is handed the
 * Pokémon's module accessor (r4). It moves like any Pokémon: each frame its
 * KineticEnergyControl (0x800A9340, r3 the control, r4 the accessor) eases
 * its velocity toward the input times a target speed (+0x28, +0x2C) at a
 * rate (+0x38, +0x3C), capped at a top speed (+0x30, unless -1). For a
 * Pokémon being chased those are scaled for that update and put back.
 *
 * WONDERPARK_CHASE_ALL=1 (tests): every Pokémon but the player counts as
 * being chased, so the slider shows on anyone walking about. */
#include <stdlib.h>

#include "hle.h"
#include "host_settings.h"

#define ACCESSOR_PLAYER_VT 0x8044A850u
#define PLAYER_LIFE 0x60u
#define KINETIC_UPDATE 0x800A9340u

static u32 sPlayer; /* the player's module accessor */

static float ldf(u32 a) {
    union { float f; u32 u; } x;
    x.u = LD32(a);
    return x.f;
}

static void stf(u32 a, float v) {
    union { float f; u32 u; } x;
    x.f = v;
    ST32(a, x.u);
}

/* void LifeModuleImpl::virt0x38(this, f32 damage) */
WP_HOOK(0x800B0200, LifeModuleImpl_TakeDamage) {
    int taken = sPlayer && c->gpr[3] == LD32(sPlayer + PLAYER_LIFE);
    int percent = taken ? g_settings.damage_taken : g_settings.damage_dealt;
    if (percent != 100) {
        double damage = c->fpr[1];
        c->fpr[1] = c->ps1[1] = (double)(float)(damage * percent / 100.0);
        WP_LOG("modifiers", "damage %g becomes %g (%s)", damage, c->fpr[1], taken ? "taken" : "dealt");
    }
    return 0; /* the game's own, with the damage changed */
}

#define CHASED_SLOTS 4
#define CHASED_FRAMES 4 /* updates without a chase step before it no longer counts */

static struct {
    u32 accessor;
    int age;
} sChased[CHASED_SLOTS];

/* The chase actions' steps: void virt0x14(this, ModuleAccessor*) */
WP_HOOK(0x801E1210, FieldAIActionChallengeChase_Step) {
    u32 accessor = c->gpr[4];
    int i, free = -1, oldest = 0;
    for (i = 0; i < CHASED_SLOTS; i++) {
        if (sChased[i].accessor == accessor) {
            sChased[i].age = 0;
            return 0;
        }
        if (!sChased[i].accessor && free < 0) free = i;
        if (sChased[i].age > sChased[oldest].age) oldest = i;
    }
    i = free >= 0 ? free : oldest;
    WP_LOG("modifiers", "chase: %08X is being chased", accessor);
    sChased[i].accessor = accessor;
    sChased[i].age = 0;
    return 0;
}

static int chased(u32 accessor) {
    static int all = -1;
    int i;
    if (all < 0) {
        const char* e = getenv("WONDERPARK_CHASE_ALL");
        all = e && *e && *e != '0';
    }
    if (all) return accessor != sPlayer;
    for (i = 0; i < CHASED_SLOTS; i++) {
        if (sChased[i].accessor != accessor) continue;
        if (++sChased[i].age > CHASED_FRAMES) {
            sChased[i].accessor = 0;
            return 0;
        }
        return 1;
    }
    return 0;
}

/* void KineticEnergyControl::virt0x4_0xC(this, ModuleAccessor*): a
 * Pokémon's movement update. */
WP_HOOK(0x800A9340, KineticEnergyControl_Update) {
    static const u32 kScaled[] = {0x28, 0x2C, 0x30, 0x38, 0x3C};
    u32 energy = c->gpr[3], accessor = c->gpr[4], saved[5];
    float k = g_settings.chase_speed / 100.0f;
    int i;
    if (LD32(accessor) == ACCESSOR_PLAYER_VT) {
        if (accessor != sPlayer) WP_LOG("modifiers", "the player's module accessor: %08X", accessor);
        sPlayer = accessor;
    }
    if (g_settings.chase_speed == 100 || accessor == sPlayer || !wp_mem_ptr(energy + 0x28, 0x18) || !chased(accessor))
        return 0;
    for (i = 0; i < 5; i++) {
        saved[i] = LD32(energy + kScaled[i]);
        if (kScaled[i] != 0x30 || ldf(energy + 0x30) > 0) stf(energy + kScaled[i], ldf(energy + kScaled[i]) * k);
    }
    {
        static u32 logged;
        if (accessor != logged && wp_log_on("modifiers")) {
            union { u32 u; float f; } was, top;
            logged = accessor;
            was.u = saved[0];
            top.u = saved[2];
            WP_LOG("modifiers", "chased %08X: target speed %.2f -> %.2f, top %.2f -> %.2f", accessor, was.f,
                   ldf(energy + 0x28), top.f, ldf(energy + 0x30));
        }
    }
    wp_call_original(c, KINETIC_UPDATE);
    for (i = 0; i < 5; i++) ST32(energy + kScaled[i], saved[i]);
    return 1;
}
