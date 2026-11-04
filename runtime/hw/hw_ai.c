/* Audio interface streaming registers (0x0D006C00): control, volume, the
 * sample counter and its interrupt timing. The counter runs on host time at
 * the selected rate while playback is on, which is all the SDK's
 * __AI_SRC_INIT and AIGetStreamSampleCount need. (AX output goes through
 * the DSP's AI DMA, modelled with the DSP.)
 *
 * AICR: bit 0 PSTAT (play), bit 1 AFR (0: 48 kHz, 1: 32 kHz), bit 2 AIINTMSK,
 * bit 3 AIINT, bit 4 AIINTVLD, bit 5 SCRESET, bit 6 DSP sample rate. */
#include "../core/wp.h"

static u32 s_cr, s_vr, s_it;
static uint64_t s_start; /* time base when the counter was last zero */
static u32 s_base;       /* counter value at s_start */

static u32 rate(void) { return (s_cr & 2) ? 32000u : 48000u; }

static u32 count(void) {
    if (!(s_cr & 1)) return s_base;
    return s_base + (u32)((wp_host_time() - s_start) * rate() / TB_HZ);
}

static u32 ai_read(u32 a, int size) {
    (void)size;
    switch (a & 0xFFu) {
    case 0x00: return s_cr;
    case 0x04: return s_vr;
    case 0x08: return count();
    case 0x0C: return s_it;
    default: return 0;
    }
}

static void ai_write(u32 a, u32 v, int size) {
    (void)size;
    switch (a & 0xFFu) {
    case 0x00: {
        u32 now_count = count();
        if ((v & 1) != (s_cr & 1) || (v & 2) != (s_cr & 2)) { s_base = now_count; s_start = wp_host_time(); }
        if (v & 0x20) { s_base = 0; s_start = wp_host_time(); } /* SCRESET */
        if (v & 0x08) s_cr &= ~0x08u;                          /* writing AIINT acknowledges */
        s_cr = (s_cr & 0x08) | (v & ~0x28u);
        break;
    }
    case 0x04: s_vr = v; break;
    case 0x0C: s_it = v; break;
    default: break;
    }
}

void hw_ai_init(void) { wp_mmio_register(0x0D006C00u, 0x20u, ai_read, ai_write); }
