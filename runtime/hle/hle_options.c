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
