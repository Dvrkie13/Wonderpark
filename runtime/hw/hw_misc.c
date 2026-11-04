/* Small register blocks with no behaviour yet: accesses are accepted
 * silently (logged under "hwmisc") so real gaps stand out in the
 * "unhandled" warnings. Each gets a real model when something needs it. */
#include "../core/wp.h"

static u32 s_regs[0x10000 / 4]; /* backing store, by offset within the block */

static u32 misc_read(u32 a, int size) {
    u32 v = s_regs[(a & 0xFFFCu) / 4];
    WP_LOG("hwmisc", "rd%d %08X = %08X", size * 8, a, v);
    if (size == 2) return (a & 2) ? v & 0xFFFF : v >> 16;
    if (size == 1) return (v >> (24 - 8 * (a & 3))) & 0xFF;
    return v;
}

static void misc_write(u32 a, u32 v, int size) {
    u32* r = &s_regs[(a & 0xFFFCu) / 4];
    WP_LOG("hwmisc", "wr%d %08X = %08X", size * 8, a, v);
    if (size == 2) *r = (a & 2) ? (*r & 0xFFFF0000u) | (v & 0xFFFF) : (*r & 0xFFFF) | (v << 16);
    else if (size == 1) { int sh = 24 - 8 * (a & 3); *r = (*r & ~(0xFFu << sh)) | ((v & 0xFF) << sh); }
    else *r = v;
}

void hw_misc_init(void) {
    /* Hollywood GPIO, clocks, and the like (0x0D800000). */
    wp_mmio_register(0x0D800000u, 0x400u, misc_read, misc_write);
}
