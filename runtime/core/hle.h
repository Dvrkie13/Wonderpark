/* Hooks: hand-written C that replaces (or wraps) a guest function.
 *
 *   WP_HOOK(0x8028DD40, __OSInitAudioSystem) {
 *       wp_return(c);   // like the function returned at once
 *       return 1;       // handled; 0 runs the original code instead
 *   }
 *
 * tools/recomp.py scans runtime/ for WP_HOOK lines and makes the generated
 * code check for a hook at each of those addresses (every path into an
 * address goes through its label). It also writes the table that maps the
 * addresses to these functions (generated/.../wp_hooks.c). After adding or
 * removing a hook, rerun tools/recomp.py. */
#pragma once

#include "wp.h"

#define WP_HOOK(addr, name) int hle_##name(CPUState* c)

/* Return from the hooked function to its caller. */
static inline void wp_return(CPUState* c) { c->pc = c->lr; }

/* Runs the original code at addr (the hooked function) to completion and
 * comes back. Interrupts aren't delivered meanwhile; don't use it around
 * code that can switch threads. */
void wp_call_original(CPUState* c, u32 addr);

/* Calls any guest function with up to 4 arguments; returns r3. Registers
 * other than r3 are restored afterwards. */
u32 wp_call_guest(CPUState* c, u32 addr, u32 r3, u32 r4, u32 r5, u32 r6);
/* The same with up to 8 arguments, r3-r10 (unused ones 0). */
u32 wp_call_guest8(CPUState* c, u32 addr, const u32 args[8]);

/* Host interrupts (hostirq.c): queue a guest callback fn(r3, r4) to run
 * as from an interrupt handler, at the next point interrupts are enabled. */
void wp_irq_post(u32 fn, u32 r3, u32 r4);
#define WP_MAGIC_IRQ_STEP 0xFFFFFF10u
#define WP_MAGIC_LOAD_CONTEXT 0xFFFFFF20u
int wp_irq_begin(CPUState* c);
void wp_irq_step(CPUState* c);

/* Host timers on host time; fire() runs between slices (it typically posts
 * host interrupts). Ids: */
enum { WP_TIMER_PAD = 0, WP_TIMER_PAD_CONNECT = 1 };
void wp_timer_start(int id, uint64_t period, void (*fire)(void));
void wp_timer_stop(int id);
void wp_timers_tick(uint64_t now);

typedef struct {
    u32 addr;
    int (*fn)(CPUState* c);
    const char* name;
} WpHook;
