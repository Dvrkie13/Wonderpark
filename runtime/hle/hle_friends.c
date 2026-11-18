/* The friendship tracker in F1 (Gameplay, Friends: settings_ui.cpp): which
 * Pokémon are friends and which are left.
 *
 * From MildRain479/PokePark-2-Wiicompiled's recomp/patches/S2LE01/
 * hle_friends.c (CC0): the PokéPark Pad numbers the Pokémon that can be
 * befriended 0-197 (the partners 0-3); its names' labels are a table
 * (0x80435C10: "NAME_POCHAMA"), and they're found by slot: 258 slots, each a
 * Pokémon found in a zone (0x804141F0). Each one's friendship points are in
 * the loaded save: a u16 at +0x28 of its 8-byte entry, the entry given by
 * the table at 0x80413100; 100 or more is a friend, as the Pad counts its
 * friends. Characters (the models' own numbering) have internal names, a
 * table of char* (0x8044DA48), which name the icons (Ic<name>.tpl).
 *
 * The tables are the game's code's own data (never written), and the save
 * is the same in-memory GD0 copy the berry options use (the pointer at
 * 0x804BA694, hle_balance.c), so they're read from the F1 tab's thread. */
#include <string.h>

#include "hle.h"

#define POKEMON 198
#define PARTNERS 4
#define LABELS 0x80435C10u
#define SLOTS 258
#define SLOT_POKEMON 0x804141F0u
#define CHARACTERS 258
#define CHARACTER_NAMES 0x8044DA48u
#define POINTS_ENTRY 0x80413100u
#define POINTS_AT 0x28u
#define SAVE_PTR 0x804BA694u
#define SAVE_SIZE 5344u

static void read_name(u32 at, char* out, int size) {
    int n;
    for (n = 0; n < size - 1 && wp_mem_ptr(at + n, 1) && LD8(at + n) >= 32 && LD8(at + n) < 127; n++)
        out[n] = (char)LD8(at + n);
    out[n] = 0;
}

int hle_friends_pokemon(void) { return POKEMON; }
int hle_friends_first(void) { return PARTNERS; }

/* Whether Pokémon p is found in any zone's slot. */
int hle_friend_found(int p) {
    int i;
    for (i = 0; i < SLOTS; i++)
        if ((int)LD32(SLOT_POKEMON + i * 4) == p) return 1;
    return 0;
}

/* Its name's label ("NAME_POCHAMA") into out. */
void hle_friend_label(int p, char* out, int size) {
    out[0] = 0;
    if (p >= 0 && p < POKEMON) read_name(LD32(LABELS + p * 4), out, size);
}

int hle_characters(void) { return CHARACTERS; }

/* A character's internal name ("Pochama"), which names its icon. */
void hle_character_name(int c, char* out, int size) {
    out[0] = 0;
    if (c >= 0 && c < CHARACTERS) read_name(LD32(CHARACTER_NAMES + c * 4), out, size);
}

/* Its friendship points in the loaded save; -1 with none loaded. */
int hle_friend_points(int p) {
    u32 save = LD32(SAVE_PTR), at;
    if (p < 0 || p >= POKEMON || !wp_mem_ptr(save, SAVE_SIZE)) return -1;
    at = save + LD32(POINTS_ENTRY + p * 4) * 8 + POINTS_AT;
    if (at + 2 > save + SAVE_SIZE) return -1;
    return (int)LD16(at);
}
