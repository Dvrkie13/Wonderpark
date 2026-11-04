/* CPU host services for DolRecomp's generated code: supervisor
 * instructions, the time base and decrementer, and exception delivery
 * through the Wii OS's own handlers. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "wp.h"

CPUState* g_cpu;

/* ------------------------------------------------------------ logging */

int wp_log_on(const char* cat) {
    static const char* spec;
    static int init;
    if (!init) { spec = getenv("WONDERPARK_LOG"); init = 1; }
    if (!spec) return 0;
    if (strstr(spec, "all")) return 1;
    return strstr(spec, cat) != NULL;
}

void wp_fatal(const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "wonderpark: fatal: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_cpu)
        fprintf(stderr, "\n  pc %08X lr %08X r1 %08X r3 %08X srr0 %08X msr %08X\n",
                g_cpu->pc, g_cpu->lr, g_cpu->gpr[1], g_cpu->gpr[3], g_cpu->srr0, g_cpu->msr);
    fflush(stderr);
    exit(3);
}

/* ------------------------------------------------------------ time base */

static uint64_t s_host_freq, s_host_start, s_tb_offset;

static uint64_t host_ticks(void) {
#ifdef _WIN32
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t wp_timebase(void) {
    uint64_t d = host_ticks() - s_host_start;
    /* d * TB_HZ / freq without overflow for long runs */
    return s_tb_offset + (d / s_host_freq) * TB_HZ + (d % s_host_freq) * TB_HZ / s_host_freq;
}

void wp_timebase_set(uint64_t tb) { s_tb_offset += tb - wp_timebase(); }

/* ------------------------------------------------------------ SPRs */

enum { SPR_DEC = 22, SPR_TBL_W = 284, SPR_TBU_W = 285, SPR_PVR = 287, SPR_WPAR = 921 };

static u32 s_spr[1024];
static uint64_t s_dec_deadline; /* time base at which DEC goes negative */
static int s_dec_armed;

static int spr_has_field(u16 spr) {
    switch (spr) {
    case 1: case 8: case 9: case 18: case 19: case 26: case 27: case 282:
    case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: case 920:
        return 1;
    default:
        return 0;
    }
}

static u32 spr_read(CPUState* c, u16 spr, u32 cia) {
    (void)c; (void)cia;
    switch (spr) {
    case SPR_PVR: return 0x00087102u; /* Broadway */
    case SPR_DEC: return (u32)(s_dec_deadline - wp_timebase());
    case SPR_WPAR: return s_spr[spr] & ~0x1Fu; /* write-gather buffer empty */
    default: return s_spr[spr & 1023];
    }
}

static void spr_write(CPUState* c, u16 spr, u32 v, u32 cia) {
    (void)c; (void)cia;
    switch (spr) {
    case SPR_DEC:
        /* The exception fires when DEC goes from 0 to -1. */
        s_dec_deadline = wp_timebase() + (uint64_t)v + 1;
        s_dec_armed = !(v & 0x80000000u);
        WP_LOG("dec", "DEC = %08X", v);
        break;
    case SPR_TBL_W: wp_timebase_set((wp_timebase() & 0xFFFFFFFF00000000ull) | v); break;
    case SPR_TBU_W: wp_timebase_set((wp_timebase() & 0xFFFFFFFFull) | ((uint64_t)v << 32)); break;
    default: s_spr[spr & 1023] = v; break;
    }
}

/* DolRecomp hands supervisor SPR moves and cache maintenance to the host,
 * then returns to the dispatcher: emulate and move pc past the instruction. */
static void system_instruction(CPUState* c, u32 raw, u32 cia) {
    u32 op = raw >> 26, xo = (raw >> 1) & 0x3FF, rs = (raw >> 21) & 31;
    u16 spr = (u16)(((raw >> 16) & 0x1F) | ((raw >> 6) & 0x3E0));
    u32 before = c->exception;
    if (op == 31) {
        switch (xo) {
        case 467: /* mtspr */
            if (spr_has_field(spr)) ppc_mtspr(c, spr, c->gpr[rs], cia);
            else spr_write(c, spr, c->gpr[rs], cia);
            break;
        case 339: /* mfspr */
            c->gpr[rs] = spr_has_field(spr) ? ppc_mfspr(c, spr, cia) : spr_read(c, spr, cia);
            break;
        case 86: case 54: case 470: case 278: case 246: case 982:
            break; /* dcbf dcbst dcbi dcbt dcbtst icbi: no caches to keep coherent */
        default:
            goto unsupported;
        }
        if (c->exception == before) c->pc = cia + 4;
        return;
    }
unsupported:
    wp_fatal("unsupported instruction %08X at %08X", raw, cia);
}

/* ------------------------------------------------------------ exceptions
 *
 * The OS copies __OSEVStart (0x8028CC00 in S2LE01) to each vector at boot,
 * patching the exception number into its `li r3, N`. That copy lives in low
 * memory and isn't recompiled, so the runtime does what it does: save r3-r5,
 * CR, LR, CTR, XER, SRR0/1 in OSCurrentContext, mark the context as saved by
 * an exception, and continue in the handler from __OSExceptionTable (physical
 * 0x3000) with r3 = number, r4 = context, translation on. The handlers
 * themselves are recompiled OS code. */

#define OS_CURRENT_CONTEXT_PHYS 0x800000C0u
#define OS_CURRENT_CONTEXT 0x800000D4u
#define OS_EXCEPTION_TABLE 0x80003000u
#define OS_DEFAULT_EXCEPTION_HANDLER 0x8028CCA0u /* S2LE01 */

#define MSR_EE 0x8000u
#define MSR_RI 0x0002u

static int vector_to_number(u32 vector) {
    switch (vector & 0xFFFFu) {
    case 0x0100: return 0;  case 0x0200: return 1;  case 0x0300: return 2;  case 0x0400: return 3;
    case 0x0500: return 4;  case 0x0600: return 5;  case 0x0700: return 6;  case 0x0800: return 7;
    case 0x0900: return 8;  case 0x0C00: return 9;  case 0x0D00: return 10; case 0x0F00: return 11;
    case 0x1300: return 12; case 0x1400: return 13; case 0x1700: return 14;
    default: return -1;
    }
}

/* Hardware entry, for exceptions the runtime raises itself (interrupts). */
static void hw_exception_entry(CPUState* c, u32 vector) {
    c->srr0 = c->pc;
    c->srr1 = c->msr & 0x87C0FFFFu;
    /* Clears POW EE PR FP FE0 SE BE FE1 IR DR PM RI LE (see cpu.c). */
    c->msr &= ~(0x00040000u | 0x8000u | 0x4000u | 0x2000u | 0x0800u | 0x0400u | 0x0200u | 0x0100u |
                0x0020u | 0x0010u | 0x0004u | 0x0002u | 0x0001u);
    c->pc = vector;
}

static int os_vector_stub(CPUState* c, int n) {
    u32 ctx = LD32(OS_CURRENT_CONTEXT_PHYS) | 0x80000000u;
    u32 handler;
    if (ctx == 0x80000000u) return 0;
    ST32(ctx + 12, c->gpr[3]);
    ST32(ctx + 16, c->gpr[4]);
    ST32(ctx + 20, c->gpr[5]);
    ST16(ctx + 418, LD16(ctx + 418) | 2);
    ST32(ctx + 128, c->cr);
    ST32(ctx + 132, c->lr);
    ST32(ctx + 136, c->ctr);
    ST32(ctx + 140, c->xer);
    ST32(ctx + 408, c->srr0);
    ST32(ctx + 412, c->srr1);
    c->gpr[5] = c->srr1;
    c->msr |= 0x30u; /* the stub's rfi turns translation back on */
    c->gpr[3] = (u32)n;
    c->gpr[4] = LD32(OS_CURRENT_CONTEXT);
    handler = (c->srr1 & MSR_RI) ? LD32(OS_EXCEPTION_TABLE + 4u * (u32)n) : OS_DEFAULT_EXCEPTION_HANDLER;
    if (!handler) return 0;
    WP_LOG("exc", "exception %d from %08X -> handler %08X", n, c->srr0, handler);
    c->pc = handler;
    return 1;
}

int wp_dispatch_exception(CPUState* c) {
    int n = vector_to_number(c->pc);
    u32 exc = c->exception;
    c->exception = 0;
    c->program_exception = 0;
    if (n < 0) return 0;
    if (n == 9) {
        /* System call: the OS puts its own stub at 0xC00 (not through the
         * table) that only syncs the bus (DCFlushRange, PPCSync); return. */
        c->pc = c->srr0;
        c->msr = c->srr1;
        return 1;
    }
    if (n == 6 && wp_log_on("exc"))
        WP_LOG("exc", "program exception (%08X) at %08X", exc, c->srr0);
    return os_vector_stub(c, n);
}

void wp_check_interrupts(CPUState* c) {
    if (!(c->msr & MSR_EE)) return;
    if (hw_pi_pending()) {
        hw_exception_entry(c, 0x500);
        if (!os_vector_stub(c, 4)) wp_fatal("external interrupt before the OS installed handlers");
        return;
    }
    if (s_dec_armed && (int64_t)(wp_timebase() - s_dec_deadline) >= 0) {
        s_dec_armed = 0;
        hw_exception_entry(c, 0x900);
        if (!os_vector_stub(c, 8)) wp_fatal("decrementer before the OS installed handlers");
    }
}

void wp_cpu_host_init(CPUState* c) {
#ifdef _WIN32
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_host_freq = (uint64_t)f.QuadPart;
#else
    s_host_freq = 1000000000ull;
#endif
    s_host_start = host_ticks();
    c->instruction_fallback = system_instruction;
    c->spr_read = spr_read;
    c->spr_write = spr_write;
}
