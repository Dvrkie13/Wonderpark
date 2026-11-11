/* The GPU's CPU-facing side: the write-gather pipe into the PI FIFO, the
 * command processor (CP, 0x0C000000) and the pixel engine's interrupt and
 * token registers (PE, 0x0C001000). Drawing itself is runtime/gpu (from the
 * reference runtime), fed with everything the CP consumes.
 *
 * Write-gather data always goes to memory at the PI FIFO write pointer, as on
 * hardware. The CP consumes it at once while it is linked to that FIFO (CP
 * control: GP read enable + GP link enable); GXBeginDisplayList points the PI
 * FIFO at a buffer and unlinks, so display lists are recorded, not drawn. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
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

/* Every GX command, vertex and matrix the game sends comes through here, tens
 * of thousands of times a frame, so the common case (no wrap at the FIFO's
 * end) is one store to host memory. */
void hw_gx_wgpipe(u32 v, int size) {
    u32 wp = hw_pi_fifo(2), end = hw_pi_fifo(1) & 0x03FFFFFFu;
    u32 addr = wp & 0x03FFFFFFu;
    u8* p = (!end || addr + (u32)size - 1 <= end) ? wp_ram(addr, (u32)size) : NULL;
    if (p) {
        if (size == 4) { u32 be = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); memcpy(p, &be, 4); }
        else if (size == 2) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
        else p[0] = (u8)v;
        addr += (u32)size;
        if (end && addr > end) { addr = hw_pi_fifo(0) & 0x03FFFFFFu; wp ^= 0x20000000u; }
    } else {
        u32 base = hw_pi_fifo(0) & 0x03FFFFFFu;
        int i;
        for (i = size - 1; i >= 0; i--) {
            ST8(addr, (v >> (8 * i)) & 0xFF);
            addr++;
            if (end && addr > end) { addr = base; wp ^= 0x20000000u; } /* wrap past the end; flag */
        }
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
/* Token and finish interrupts in the order the GPU reached them: a token
 * (0x10000 | token) or a finish (0x20000). One is shown at a time: the
 * game's token handler acknowledges by writing back the register as it
 * read it, which would acknowledge a finish shown beside it too, and the
 * game would wait in GXDrawDone for ever (seen with the render thread,
 * where both can arrive at once). */
#define PE_QUEUE 64
static u32 s_pe_queue[PE_QUEUE];
static unsigned s_th, s_tt;

static void pe_update(void) {
    hw_pi_set(PI_INT_PE_TOKEN, (s_pe_ctrl & 4) && (s_pe_ctrl & 1));
    hw_pi_set(PI_INT_PE_FINISH, (s_pe_ctrl & 8) && (s_pe_ctrl & 2));
}

static void pe_next(void) {
    if (!(s_pe_ctrl & 0xC) && s_th != s_tt) {
        u32 e = s_pe_queue[s_th++ % PE_QUEUE];
        if (e & 0x10000) {
            s_pe_token = (u16)e;
            s_pe_ctrl |= 4;
        } else {
            s_pe_ctrl |= 8;
        }
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
        WP_LOG("gxpe", "%.2f ctrl write %X (was %X) pc %08X", wp_wall_seconds() * 1000, v, s_pe_ctrl, g_cpu ? g_cpu->pc : 0);
        /* bits 0/1 enables; writing 1 to 2/3 acknowledges token/finish */
        s_pe_ctrl = (u16)((s_pe_ctrl & 0xC & ~(v & 0xC)) | (v & 3));
        pe_next();
        break;
    case 0x0E: s_pe_token = (u16)v; break;
    default: break;
    }
}

/* With the render thread (runtime/gpu/gx_thread.c), the decoder's
 * interrupts and token writes come from that thread: they are queued and
 * taken in on the game thread at the next device tick (hw_gx_tick), and
 * the game is woken if it sleeps in its idle loop. */
#ifdef _WIN32
#define GPU_EVENTS 256
static CRITICAL_SECTION s_ev_lock;
static DWORD s_game_thread;
static struct { int n; u32 data; } s_ev[GPU_EVENTS];
static volatile unsigned s_ev_head, s_ev_tail;

static int from_render_thread(int n, u32 data) {
    if (GetCurrentThreadId() == s_game_thread) return 0;
    EnterCriticalSection(&s_ev_lock);
    if (s_ev_tail - s_ev_head < GPU_EVENTS) {
        s_ev[s_ev_tail % GPU_EVENTS].n = n;
        s_ev[s_ev_tail % GPU_EVENTS].data = data;
        s_ev_tail++;
    } else {
        WP_WARN("GPU event queue full");
    }
    LeaveCriticalSection(&s_ev_lock);
    wp_wake();
    return 1;
}
#endif

static void raise_now(int n, u32 data);

/* BP 0x47: the token register, without an interrupt. */
void hle_pe_set_token(uint32_t token) {
#ifdef _WIN32
    if (from_render_thread(0x47, token)) return;
#endif
    s_pe_token = (u16)token;
}

void hw_gx_tick(void) {
#ifdef _WIN32
    if (s_ev_head == s_ev_tail) return;
    EnterCriticalSection(&s_ev_lock);
    while (s_ev_head != s_ev_tail) {
        int n = s_ev[s_ev_head % GPU_EVENTS].n;
        u32 data = s_ev[s_ev_head % GPU_EVENTS].data;
        s_ev_head++;
        if (n == 0x47) s_pe_token = (u16)data;
        else raise_now(n, data);
    }
    LeaveCriticalSection(&s_ev_lock);
#endif
}

/* Called by the decoder (runtime/gpu/gx_fifo.c), with the OS's interrupt
 * numbers: 18 = PE token (data = the token), 19 = PE finish. The token is
 * queued here and put in the register when its interrupt is raised, so
 * each interrupt reports its own token (the decoder's before() isn't needed). */
void hle_raise_interrupt_ex(int n, void (*before)(uint32_t data), uint32_t data) {
    (void)before;
#ifdef _WIN32
    if (from_render_thread(n, data)) return;
#endif
    raise_now(n, data);
}

static void raise_now(int n, u32 data) {
    WP_LOG("gxpe", "%.2f raise %d data %X (ctrl %X)", wp_wall_seconds() * 1000, n, data, s_pe_ctrl);
    if (wp_log_on("gxirq")) {
        static unsigned tok, fin;
        static u64 since;
        u64 now = wp_host_time();
        tok += n == 18;
        fin += n == 19;
        if (now - since >= TB_HZ) {
            WP_LOG("gxirq", "last second: %u PE token, %u PE finish interrupts", tok, fin);
            tok = fin = 0;
            since = now;
        }
    }
    if (n == 18) {
        if (s_tt - s_th < PE_QUEUE) s_pe_queue[s_tt++ % PE_QUEUE] = 0x10000u | (u16)data;
    } else if (n == 19) {
        if (s_tt - s_th < PE_QUEUE) s_pe_queue[s_tt++ % PE_QUEUE] = 0x20000u;
    } else {
        WP_WARN("GPU raised interrupt %d", n);
        return;
    }
    pe_next();
}

void hle_raise_interrupt(int n) { hle_raise_interrupt_ex(n, NULL, 0); }

void hw_gx_init(void) {
#ifdef _WIN32
    InitializeCriticalSection(&s_ev_lock);
    s_game_thread = GetCurrentThreadId();
#endif
    wp_mmio_register(0x0C000000u, 0x80u, cp_read, cp_write);
    wp_mmio_register(0x0C001000u, 0x100u, pe_read, pe_write);
}

/* WONDERPARK_RT_DEBUG: the PE state, at exit (gx_thread.c). */
void hw_gx_debug_print(void) {
    fprintf(stderr, "gpu: PE ctrl %X token %04X, interrupts queued %u, events %u of %u taken\n",
            s_pe_ctrl, s_pe_token, s_tt - s_th,
#ifdef _WIN32
            s_ev_head, s_ev_tail
#else
            0u, 0u
#endif
    );
}
