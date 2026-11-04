/* Processor interface (0x0C003000): interrupt cause and mask, reset,
 * and the GPU FIFO pointers the GX library reads back. */
#include "../core/wp.h"

static u32 s_cause, s_mask;
static u32 s_fifo[3]; /* 0x0C: FIFO base, 0x10: end, 0x14: write pointer */

void hw_pi_set(u32 cause, int on) {
    if (on) s_cause |= cause;
    else s_cause &= ~cause;
}

int hw_pi_pending(void) { return (s_cause & s_mask) != 0; }

static u32 pi_read(u32 a, int size) {
    (void)size;
    switch (a & 0xFFFu) {
    case 0x000: return s_cause;
    case 0x004: return s_mask;
    case 0x00C: return s_fifo[0];
    case 0x010: return s_fifo[1];
    case 0x014: return s_fifo[2];
    case 0x02C: return 0x20000000u; /* console revision flags as Dolphin reports */
    default:
        WP_LOG("pi", "read %08X", a);
        return 0;
    }
}

static void pi_write(u32 a, u32 v, int size) {
    (void)size;
    switch (a & 0xFFFu) {
    case 0x000: s_cause &= ~v; break; /* writing 1 acknowledges */
    case 0x004: s_mask = v; WP_LOG("pi", "mask = %08X", v); break;
    case 0x00C: s_fifo[0] = v; break;
    case 0x010: s_fifo[1] = v; break;
    case 0x014: s_fifo[2] = v; break;
    case 0x024: break; /* reset control */
    default: WP_LOG("pi", "write %08X = %08X", a, v); break;
    }
}

u32 hw_pi_fifo(int which) { return s_fifo[which]; }
void hw_pi_set_fifo_wp(u32 wp) { s_fifo[2] = wp; }

void hw_pi_init(void) { wp_mmio_register(0x0C003000u, 0x1000u, pi_read, pi_write); }
