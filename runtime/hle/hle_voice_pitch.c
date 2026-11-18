/* The Voice pitch setting (g_x[X_VOICE_PITCH], -50..50 % of the frequency;
 * the F1 tab's Silly page): Pokémon voices higher or deeper.
 *
 * Confirmed (2026-11-17, from main.dol; CRI AtomEx 1.17):
 * - The game loads its sound banks (ACB) in three places (0x80066D30/48,
 *   0x80066F50/68, 0x80067160/78), from data in memory: 0x80393A64 for a
 *   bank without a stream archive, 0x80393C60 for one with its archive
 *   already open (r5 the archive's handle, whose word +8 points to its
 *   path), and stores the handle each returns right after (0x80066D4C,
 *   0x80066F6C, 0x8006717C). The voice banks are the ones whose stream
 *   archive is a "*_VOICE_*" one (S_POKE_VOICE, S_PIKA/MIJU/POKA/TUTA_VOICE,
 *   S_AT_02_POKE_VOICE; logged with WONDERPARK_LOG=voicepitch).
 * - Every sound is played through the game's wrapper: it resets the
 *   player's parameters (0x8039A6D4, criAtomExPlayer_ResetParameters),
 *   sets the cue (the bank handle lands at player +32, +28 nonzero once
 *   set), may set a pitch of its own (0x8039A7C0, criAtomExPlayer_SetPitch,
 *   cents in f1; called from 0x80067C08 and 0x800698E0), and starts it
 *   (0x8039955C, criAtomExPlayer_Start, its only caller 0x80065CF8).
 * - Parameter 3 of a player's parameter set (word +92) is its pitch in
 *   cents: the low-level player's frequency ratio is computed with
 *   pow(2, parameter 3 / 1200) at 0x803A0018-0x803A005C before
 *   criAtomPlayer_SetFrequencyRatio (0x8038678C); 0x803A1480 sets a
 *   parameter (as SetPitch does). (A category's parameter set ignores
 *   pitch: only volume is used from categories, tried 2026-11-17.)
 * So: the banks are sorted as they load; the game's own pitch for each
 * player is remembered (0 after a reset); and at Start, a voice gets the
 * game's pitch plus the setting's. A player that got it and then starts
 * something else without a reset gets the game's own back.
 *
 * Checked (2026-11-17, Wish Park from a save): voices got a frequency
 * ratio of 1.501 at +50% and 0.500 at -50% on their low-level players;
 * other sounds kept their own.
 *
 * Not covered: the attractions' voice banks without a stream archive
 * (M_PIKA/MIJU/POKA/TUTA_AT_VOICE), loaded from memory with no path. */
#include <math.h>
#include <string.h>

#include "hle.h"
#include "host_settings.h"

#define SET_PARAMETER 0x803A1480u
#define PARAMETER_PITCH 3u
#define BANKS 128
#define PLAYERS 128

static u32 sVoiceBank[BANKS];
static int sPending = -1; /* the bank being loaded: 1 voice, 0 not, -1 none */

static struct {
    u32 player;
    float game;  /* the game's own pitch, cents */
    int applied; /* ours was added at the last start */
} sPlayer[PLAYERS];
static unsigned sNextPlayer;

/* Whether the guest string at p mentions VOICE (paths of voice banks). */
static int mentions_voice(u32 p) {
    char s[160];
    unsigned i;
    if (!p || !wp_mem_ptr(p, 1)) return 0;
    for (i = 0; i + 1 < sizeof(s) && wp_mem_ptr(p + i, 1) && (s[i] = (char)LD8(p + i)) != 0; i++) {
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) return 0; /* not a string */
    }
    s[i] = 0;
    WP_LOG("voicepitch", "  path \"%s\"", s);
    return strstr(s, "VOICE") != NULL;
}

/* A bank without a stream archive (data, size, 0, 0, 0, 0 in this game):
 * not a voice bank. */
WP_HOOK(0x80393A64, LoadAcbData) {
    sPending = mentions_voice(c->gpr[6]);
    return 0;
}

/* A bank with its stream archive already open (data, size, archive): the
 * archive handle's word +8 points to its path, as
 * "Sound/S_PIKA_VOICE_streamfiles.awb". */
WP_HOOK(0x80393C60, LoadAcbDataAwb) {
    u32 awb = c->gpr[5];
    sPending = wp_mem_ptr(awb, 12) && mentions_voice(LD32(awb + 8));
    return 0;
}

/* Right after each load in the game: r3 is the new bank's handle. */
static void bank_loaded(CPUState* c) {
    u32 h = c->gpr[3];
    int i, free = -1;
    if (sPending < 0 || !h) return;
    for (i = 0; i < BANKS; i++) {
        if (sVoiceBank[i] == h) sVoiceBank[i] = 0; /* a handle used again */
        if (!sVoiceBank[i] && free < 0) free = i;
    }
    if (sPending && free >= 0) sVoiceBank[free] = h;
    WP_LOG("voicepitch", "bank %08X loaded%s", h, sPending ? ": voices" : "");
    sPending = -1;
}

WP_HOOK(0x80066D4C, AcbStored1) { bank_loaded(c); return 0; }
WP_HOOK(0x80066F6C, AcbStored2) { bank_loaded(c); return 0; }
WP_HOOK(0x8006717C, AcbStored3) { bank_loaded(c); return 0; }

static int is_voice_bank(u32 h) {
    int i;
    for (i = 0; h && i < BANKS; i++)
        if (sVoiceBank[i] == h) return 1;
    return 0;
}

static int player_slot(u32 player) {
    unsigned i;
    for (i = 0; i < PLAYERS; i++)
        if (sPlayer[i].player == player) return (int)i;
    i = sNextPlayer++ % PLAYERS;
    sPlayer[i].player = player;
    sPlayer[i].game = 0;
    sPlayer[i].applied = 0;
    return (int)i;
}

/* criAtomExPlayer_ResetParameters(player) */
WP_HOOK(0x8039A6D4, ResetParameters) {
    if (c->gpr[3]) {
        int s = player_slot(c->gpr[3]);
        sPlayer[s].game = 0;
        sPlayer[s].applied = 0;
    }
    return 0;
}

/* criAtomExPlayer_SetPitch(player, f1 cents): the game's own pitch. */
WP_HOOK(0x8039A7C0, SetPitch) {
    if (c->gpr[3]) sPlayer[player_slot(c->gpr[3])].game = (float)c->fpr[1];
    return 0;
}

static void set_player_pitch(CPUState* c, u32 player, float cents) {
    u32 r3 = c->gpr[3], params = LD32(player + 92);
    double f1 = c->fpr[1], p1 = c->ps1[1];
    if (!wp_mem_ptr(params, 4)) return;
    c->fpr[1] = c->ps1[1] = cents;
    wp_call_guest(c, SET_PARAMETER, params, PARAMETER_PITCH, 0, 0);
    c->gpr[3] = r3;
    c->fpr[1] = f1;
    c->ps1[1] = p1;
}

/* The voice pools (criAtomExVoicePool_Allocate..., 0x803948B8(config,
 * work, size)), made by the game at start (0x80066338, 0x8006635C) with
 * max_sampling_rate (config +12) 32768 and no work area of its own (CRI
 * allocates it, sized from the config). A player won't play faster than
 * that: a 32 kHz voice could go up 2.4% at most (CRI clamps the frequency
 * ratio, 0x80386814). Raised to 49152, 1.5 x 32768, so +50% fits. */
#define POOL_RATE_GAME 32768u
#define POOL_RATE_OURS 49152u

WP_HOOK(0x803948B8, VoicePoolAllocate) {
    u32 cfg = c->gpr[3];
    if (wp_mem_ptr(cfg, 16) && LD32(cfg + 12) == POOL_RATE_GAME && c->gpr[4] == 0) {
        ST32(cfg + 12, POOL_RATE_OURS);
        WP_LOG("voicepitch", "voice pool %08X/%08X/%08X: max sampling rate %u -> %u", LD32(cfg), LD32(cfg + 4),
               LD32(cfg + 8), POOL_RATE_GAME, POOL_RATE_OURS);
    }
    return 0;
}

/* criAtomExPlayer_Start(player) */
WP_HOOK(0x8039955C, PlayerStart) {
    u32 player = c->gpr[3];
    int s, voice;
    if (!player || !wp_mem_ptr(player + 92, 4)) return 0;
    voice = LD32(player + 28) && is_voice_bank(LD32(player + 32));
    s = player_slot(player);
    if (voice && g_x[X_VOICE_PITCH]) {
        float ours = (float)(1200.0 * log2(1.0 + g_x[X_VOICE_PITCH] / 100.0));
        set_player_pitch(c, player, sPlayer[s].game + ours);
        sPlayer[s].applied = 1;
        WP_LOG("voicepitch", "voice on player %08X: %+.0f cents", player, sPlayer[s].game + ours);
    } else if (sPlayer[s].applied) {
        set_player_pitch(c, player, sPlayer[s].game);
        sPlayer[s].applied = 0;
    }
    return 0;
}
