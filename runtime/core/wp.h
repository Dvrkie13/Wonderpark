/* Wonderpark runtime core: shared declarations. */
#pragma once

#include <stdint.h>
#include <stdio.h>

#include "cpu/cpu.h" /* DolRecomp CPUState (tools/dolrecomp/src) */

#ifdef __cplusplus
extern "C" {
#endif

extern CPUState* g_cpu;

/* ---- logging. WONDERPARK_LOG=<categories> e.g. "mmio,exc,ipc" or "all". */
int wp_log_on(const char* cat);
#define WP_LOG(cat, ...) do { if (wp_log_on(cat)) { fprintf(stderr, "[" cat "] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define WP_WARN(...) do { fprintf(stderr, "warning: " __VA_ARGS__); fputc('\n', stderr); } while (0)
void wp_fatal(const char* fmt, ...);
/* Debug: symbol for an address (config/S2LE01/symbols.txt), guest backtrace. */
const char* wp_sym(u32 addr, u32* offset);
void wp_backtrace(CPUState* c);
void wp_dump_threads(void);

/* ---- guest memory helpers, big-endian. Accept virtual (0x8/0x9/0xC/0xD)
 * and physical (0x0/0x1) addresses alike, as hardware DMA and IOS do;
 * anything that isn't RAM goes to the MMIO handlers. */
static inline uint8_t* wp_ram(uint32_t a, uint32_t n) {
    uint32_t p = a & 0x3FFFFFFFu;
    if (p < g_cpu->ram_size) return n <= g_cpu->ram_size - p ? g_cpu->ram + p : NULL;
    p -= 0x10000000u;
    if (p < g_cpu->mem2_size) return n <= g_cpu->mem2_size - p ? g_cpu->mem2 + p : NULL;
    return NULL;
}
static inline uint32_t wp_p2v(uint32_t a) { return (a & 0x3FFFFFFFu) | 0x80000000u; }
static inline uint32_t LD32(uint32_t a) {
    const uint8_t* p = wp_ram(a, 4);
    return p ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : mem_read32(g_cpu, wp_p2v(a));
}
static inline uint32_t LD16(uint32_t a) {
    const uint8_t* p = wp_ram(a, 2);
    return p ? (uint32_t)p[0] << 8 | p[1] : mem_read16(g_cpu, wp_p2v(a));
}
static inline uint32_t LD8(uint32_t a) {
    const uint8_t* p = wp_ram(a, 1);
    return p ? *p : mem_read8(g_cpu, wp_p2v(a));
}
static inline void ST32(uint32_t a, uint32_t v) {
    uint8_t* p = wp_ram(a, 4);
    if (p) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
    else mem_write32(g_cpu, wp_p2v(a), v);
}
static inline void ST16(uint32_t a, uint32_t v) {
    uint8_t* p = wp_ram(a, 2);
    if (p) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
    else mem_write16(g_cpu, wp_p2v(a), (u16)v);
}
static inline void ST8(uint32_t a, uint32_t v) {
    uint8_t* p = wp_ram(a, 1);
    if (p) *p = (uint8_t)v;
    else mem_write8(g_cpu, wp_p2v(a), (u8)v);
}
/* Host pointer for [a, a+n) in MEM1 or MEM2, or NULL. */
static inline uint8_t* wp_mem_ptr(uint32_t a, uint32_t n) { return wp_ram(a, n); }

/* ---- time base: 60.75 MHz, paced on host time. */
#define TB_HZ 60750000ull
uint64_t wp_timebase(void);
void wp_timebase_set(uint64_t tb);
/* Monotonic host time in the same units, for device timers (the game can't move it). */
uint64_t wp_host_time(void);
/* GPU time hidden from the guest clock until it next idles (cpu_host.c). */
void wp_parallel_time_hide(uint64_t ticks);
void wp_parallel_time_restore(void);
int wp_parallel_time_pending(void);
/* Reads the host clock; the dispatcher calls it once per slice. */
void wp_clock_tick(void);

/* ---- CPU host services (cpu_host.c). */
void wp_cpu_host_init(CPUState* c);
/* Called by the dispatcher between slices: delivers pending interrupts
 * (decrementer, external) when MSR[EE] is set. */
int wp_check_interrupts(CPUState* c); /* 1 if one was taken */
int wp_interrupt_due(CPUState* c);
/* Turns an exception DolRecomp raised (ctx->exception, pc at the vector)
 * into a call of the OS's handler. 0 if there is none. */
int wp_dispatch_exception(CPUState* c);

/* ---- MMIO (mmio.c). Devices register handlers for a 64 KB page range. */
typedef u32 (*WpMmioRead)(u32 addr, int size);
typedef void (*WpMmioWrite)(u32 addr, u32 value, int size);
void wp_mmio_init(CPUState* c);
void wp_mmio_register(u32 base, u32 size, WpMmioRead rd, WpMmioWrite wr);

/* ---- processor interface: interrupt causes (hw_pi.c). */
enum {
    PI_INT_ERROR = 1u << 0, PI_INT_RSW = 1u << 1, PI_INT_DI = 1u << 2, PI_INT_SI = 1u << 3,
    PI_INT_EXI = 1u << 4, PI_INT_AI = 1u << 5, PI_INT_DSP = 1u << 6, PI_INT_MEM = 1u << 7,
    PI_INT_VI = 1u << 8, PI_INT_PE_TOKEN = 1u << 9, PI_INT_PE_FINISH = 1u << 10, PI_INT_CP = 1u << 11,
    PI_INT_DEBUG = 1u << 12, PI_INT_HSP = 1u << 13, PI_INT_IPC = 1u << 14,
};
void hw_pi_init(void);
void hw_pi_set(u32 cause, int on);
int hw_pi_pending(void);

/* ---- per-slice device tick (hw_*.c), called by the dispatcher with wp_host_time(). */
void wp_devices_tick(uint64_t now);

#ifdef __cplusplus
}
#endif
