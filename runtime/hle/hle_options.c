/* The game's own Options values (main menu > save file > Options), for the
 * F1 tab, which reads and writes them in place (never a copy of its own).
 *
 * Confirmed (2026-11-11, RAM snapshots before and after changing it in the
 * game's menu, three runs): the game keeps the loaded save in memory at
 * the pointer stored at 0x804BA694 (also 0x804BA698/A0/B0; 0x809CBFE0 in
 * every run so far), a copy of the 5,344-byte GD0 file. Byte 0x14A5 is
 * Text Speed: 0 Normal, 1 Fast. It's written to the save file with the
 * game's next save.
 *
 * Not yet confirmed: whether dialogue picks up a change made here at once
 * (the game might keep another copy for its message window; a byte at
 * 0x804CEC8B also went 0 -> 1 -> 0 with the setting), and where Rumble
 * Setting is (likely a neighbouring byte). */
#include "hle.h"

#define SAVE_PTR 0x804BA694u
#define SAVE_SIZE 5344u
#define TEXT_SPEED 0x14A5u

/* The in-memory save, or 0 before one is loaded. */
static uint32_t save_base(void) {
    uint32_t p = LD32(SAVE_PTR);
    if (!wp_mem_ptr(p, SAVE_SIZE)) return 0;
    return p;
}

/* 0 Normal, 1 Fast; -1 when no save is loaded. */
int hle_game_text_speed(void) {
    uint32_t b = save_base();
    if (!b) return -1;
    return LD8(b + TEXT_SPEED) ? 1 : 0;
}

void hle_set_game_text_speed(int fast) {
    uint32_t b = save_base();
    if (b) ST8(b + TEXT_SPEED, fast ? 1 : 0);
}

/* The start menu (title > save file > Play / Multiplayer / Options).
 * Confirmed (2026-11-11, RAM snapshots of each menu state): its manager
 * object is at the pointer stored at 0x804BCDA8; the high half of its word
 * +0x34 is the page shown (2 the save file's menu, 3 Options); its word
 * +0x3C points to the Options page object, whose word +0x114 is the
 * Options cursor (0 Text Speed, 1 Rumble Setting, 2 Delete a Saved Game). */
#define START_MENU 0x804BCDA8u

static uint32_t start_menu(void) {
    uint32_t m = LD32(START_MENU);
    return wp_mem_ptr(m, 0x100) ? m : 0;
}

/* The Options page object, if the start menu is up: its first word (the
 * C++ vtable) is 0x80457D50 in every run so far; in the park the same slot
 * holds other objects, so the check keeps the PC rows out of the game. */
#define OPTIONS_VTABLE 0x80457D50u

static uint32_t options_page(void) {
    uint32_t m = start_menu(), o;
    if (!m) return 0;
    o = LD32(m + 0x3C);
    if (!wp_mem_ptr(o, 0x120) || LD32(o) != OPTIONS_VTABLE) return 0;
    return o;
}

/* 1 while the start menu shows its Options page's list, with none of its
 * dialogs open (Text Speed's, Rumble Setting's: the manager's word +0x15C
 * is 5 while one is up, 0 with the list; snapshots, 2026-11-11). */
int hle_options_page_shown(void) {
    uint32_t m = start_menu();
    return options_page() && (LD32(m + 0x34) >> 16) == 3 && LD32(m + 0x15C) == 0;
}

/* The Options page's cursor (0-2), or -1. */
int hle_options_cursor(void) {
    uint32_t o = options_page();
    return o ? (int)LD32(o + 0x114) : -1;
}
