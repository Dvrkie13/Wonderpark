/* Video interface (0x0C002000). The registers are kept as written; what
 * matters for the game is the retrace: once per frame (60 Hz by default,
 * WONDERPARK_VI_HZ to change it) the display-interrupt registers whose
 * enable bit is set get their status bit, which raises PI cause VI. The
 * OS's __VIRetraceHandler reads them and clears the status by writing.
 *
 * DIn (0x30/0x34/0x38/0x3C, 32 bits): bit 31 status, bit 28 enable,
 * bits 16-25 vertical position, 0-9 horizontal position. */
#include <stdlib.h>

#include "../core/wp.h"

static u16 s_regs[0x100 / 2];
static uint64_t s_period, s_next;
static u32 s_line; /* for VCT, advanced through the frame */
u32 g_vi_retraces;

#define R16(off) s_regs[(off) / 2]

static void update_pi(void) {
    int i, on = 0;
    for (i = 0; i < 4; i++) {
        u16 hi = R16(0x30 + 4 * i);
        if ((hi & 0x8000) && (hi & 0x1000)) on = 1;
    }
    hw_pi_set(PI_INT_VI, on);
}

static u32 vi_read(u32 a, int size) {
    u32 off = a & 0xFFu;
    if (off == 0x2C) { /* VCT: current line (1-based), advance so polling loops progress */
        s_line = s_line % 525 + 1;
        R16(0x2C) = (u16)s_line;
    }
    if (size == 4) return (u32)R16(off) << 16 | R16(off + 2);
    if (size == 2) return R16(off);
    return (R16(off & ~1u) >> ((off & 1) ? 0 : 8)) & 0xFF;
}

static void vi_write(u32 a, u32 v, int size) {
    u32 off = a & 0xFFu;
    if (size == 4) {
        vi_write(a, v >> 16, 2);
        vi_write(a + 2, v & 0xFFFF, 2);
        return;
    }
    if (size == 1) v = (off & 1) ? (R16(off & ~1u) & 0xFF00) | (v & 0xFF) : (R16(off) & 0xFF) | (v << 8);
    off &= ~1u;
    if (off >= 0x30 && off <= 0x3C && !(off & 2)) {
        /* Writing a 0 to the status bit acknowledges it. */
        u16 status = R16(off) & 0x8000;
        R16(off) = (u16)((v & 0x7FFF) | (status & v));
        update_pi();
        return;
    }
    R16(off) = (u16)v;
}

void hw_vi_tick(uint64_t now) {
    int i;
    if (!s_period) {
        const char* e = getenv("WONDERPARK_VI_HZ");
        double hz = e ? atof(e) : 60.0;
        if (hz < 1 || hz > 1000) hz = 60.0;
        s_period = (uint64_t)(TB_HZ / hz + 0.5);
        s_next = now + s_period;
    }
    if ((int64_t)(now - s_next) < 0) return;
    s_next += s_period;
    if ((int64_t)(now - s_next) > (int64_t)s_period) s_next = now + s_period; /* after a stall, don't burst */
    g_vi_retraces++;
    for (i = 0; i < 4; i++)
        if (R16(0x30 + 4 * i) & 0x1000) R16(0x30 + 4 * i) |= 0x8000;
    update_pi();
}

uint64_t hw_vi_next_retrace(void) { return s_next; }

void hw_vi_init(void) { wp_mmio_register(0x0C002000u, 0x100u, vi_read, vi_write); }
