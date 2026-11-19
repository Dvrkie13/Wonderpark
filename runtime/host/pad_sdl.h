/* Controllers through SDL3 (pad_sdl.c). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A pad's state as XInput gave it: buttons as XPAD_* (by position),
 * triggers 0-255, sticks -32768..32767 with y up. */
typedef struct {
    uint32_t buttons;
    uint8_t lt, rt;
    short lx, ly, rx, ry;
} PadState;

void pad_sdl_init(void);
/* Player n's pad (0 first); 0 if none is connected. Player 0's call also
 * takes in pads connected or gone since the last. */
int pad_sdl_state(int player, PadState* out);
void pad_sdl_rumble(int player, uint16_t low, uint16_t high);
/* Player n's pad type for the button icons (PADSET_XB/PS/NS), 0 if none. */
int pad_sdl_type(int player);
/* Changes whenever a pad connects or goes away. */
unsigned pad_sdl_changes(void);

#ifdef __cplusplus
}
#endif
