/* The GPU's CPU-facing side: the write-gather pipe into the PI FIFO, the
 * command processor (CP, 0x0C000000) and the pixel engine's interrupt and
 * token registers (PE, 0x0C001000). Drawing itself is runtime/gpu (from the
 * reference runtime), fed with everything the CP consumes.
 *
 * Write-gather data always goes to memory at the PI FIFO write pointer, as on
 * hardware. The CP consumes it at once while it is linked to that FIFO (CP
 * control: GP read enable + GP link enable); GXBeginDisplayList points the PI
 * FIFO at a buffer and unlinks, so display lists are recorded, not drawn. */
#include <string.h>

#include "../core/wp.h"

/* PI FIFO registers (hw_pi.c). */
u32 hw_pi_fifo(int which); /* 0 base, 1 end, 2 write pointer */
void hw_pi_set_fifo_wp(u32 wp);

/* The decoder (runtime/gpu/gx_fifo.c); NULL until the GPU module starts. */
extern void (*g_wgpipe_write)(u32 value, int size);

static u16 s_cp[0x80 / 2];
#define CP(off) s_cp[(off) / 2]
enum { CP_SR = 0x00, CP_CR = 0x02, CP_CLEAR = 0x04, CP_TOKEN = 0x0E,
       CP_BASE = 0x20, CP_END = 0x24, CP_HI = 0x28, CP_LO = 0x2C, CP_DIST = 0x30, CP_WP = 0x34, CP_RP = 0x38, CP_BP = 0x3C };

static u32 cp32(u32 off) { return (u32)CP(off + 2) << 16 | CP(off); }

static int linked(void) {
    return (CP(CP_CR) & 0x11) == 0x11 && (cp32(CP_BASE) & 0x03FFFFE0u) == (hw_pi_fifo(0) & 0x03FFFFE0u);
}

void hw_gx_wgpipe(u32 v, int size) {
    u32 wp = hw_pi_fifo(2), base = hw_pi_fifo(0) & 0x03FFFFFFu, end = hw_pi_fifo(1) & 0x03FFFFFFu;
    u32 addr = wp & 0x03FFFFFFu;
    int i;
    for (i = size - 1; i >= 0; i--) {
        ST8(addr, (v >> (8 * i)) & 0xFF);
        addr++;
        if (end && addr > end) { addr = base; wp ^= 0x20000000u; } /* wrap past the end; flag */
    }
    hw_pi_set_fifo_wp((wp & 0x20000000u) | addr);
    if (linked() && g_wgpipe_write) g_wgpipe_write(v, size);
}

static u32 cp_read(u32 a, int size) {
    u32 off = a & 0x7Eu;
    switch (off) {
    case CP_SR: return 0x0C; /* GP idle: read and command */
    case CP_RP: case CP_WP: {
        /* Everything written has been consumed. */
        u32 wp = hw_pi_fifo(2) & 0x03FFFFFFu;
        return size == 4 ? (wp & 0xFFFF) | (wp & 0xFFFF0000u) : (wp & 0xFFFF);
    }
    case CP_RP + 2: case CP_WP + 2: return (hw_pi_fifo(2) & 0x03FFFFFFu) >> 16;
    case CP_DIST: case CP_DIST + 2: return 0;
    default:
        if (size == 4) return (u32)CP(off) << 16 | CP(off + 2);
        return CP(off);
    }
}

static void cp_write(u32 a, u32 v, int size) {
    u32 off = a & 0x7Eu;
    if (size == 4) { CP(off) = (u16)(v >> 16); CP(off + 2) = (u16)v; return; }
    CP(off) = (u16)v;
    WP_LOG("gxhw", "CP %02X = %04X", off, v & 0xFFFF);
}

/* ---- PE: token / finish interrupts. */
static u16 s_pe_ctrl, s_pe_token;
#define TOKEN_QUEUE 32
static u16 s_tokens[TOKEN_QUEUE];
static unsigned s_th, s_tt;
static int s_finish_pending;

static void pe_update(void) {
    hw_pi_set(PI_INT_PE_TOKEN, (s_pe_ctrl & 4) && (s_pe_ctrl & 1));
    hw_pi_set(PI_INT_PE_FINISH, (s_pe_ctrl & 8) && (s_pe_ctrl & 2));
}

static void pe_next(void) {
    if (!(s_pe_ctrl & 4) && s_th != s_tt) {
        s_pe_token = s_tokens[s_th++ % TOKEN_QUEUE];
        s_pe_ctrl |= 4;
    }
    if (!(s_pe_ctrl & 8) && s_finish_pending) {
        s_finish_pending--;
        s_pe_ctrl |= 8;
    }
    pe_update();
}

static u32 pe_read(u32 a, int size) {
    (void)size;
    switch (a & 0xFEu) {
    case 0x0A: return s_pe_ctrl;
    case 0x0E: return s_pe_token;
    default: return 0;
    }
}

static void pe_write(u32 a, u32 v, int size) {
    (void)size;
    switch (a & 0xFEu) {
    case 0x0A:
        /* bits 0/1 enables; writing 1 to 2/3 acknowledges token/finish */
        s_pe_ctrl = (u16)((s_pe_ctrl & 0xC & ~(v & 0xC)) | (v & 3));
        pe_next();
        break;
    case 0x0E: s_pe_token = (u16)v; break;
    default: break;
    }
}

/* Called by the decoder (runtime/gpu/gx_fifo.c), with the OS's interrupt
 * numbers: 18 = PE token (data = the token), 19 = PE finish. The token is
 * queued here and put in the register when its interrupt is raised, so
 * each interrupt reports its own token (the decoder's before() isn't needed). */
void hle_raise_interrupt_ex(int n, void (*before)(uint32_t data), uint32_t data) {
    (void)before;
    if (n == 18) {
        if (s_tt - s_th < TOKEN_QUEUE) s_tokens[s_tt++ % TOKEN_QUEUE] = (u16)data;
    } else if (n == 19) {
        s_finish_pending++;
    } else {
        WP_WARN("GPU raised interrupt %d", n);
        return;
    }
    pe_next();
}

void hle_raise_interrupt(int n) { hle_raise_interrupt_ex(n, NULL, 0); }

void hw_gx_init(void) {
    wp_mmio_register(0x0C000000u, 0x80u, cp_read, cp_write);
    wp_mmio_register(0x0C001000u, 0x100u, pe_read, pe_write);
}
