/* The dispatcher: runs DolRecomp's generated code, delivers exceptions and
 * interrupts between slices, and implements hooks. The only runtime file
 * that includes the generated header. */
#include <stdlib.h>
#include <string.h>

#include "S2LE01.h" /* generated */
#include "hle.h"

extern const WpHook wp_hook_table[]; /* generated/.../wp_hooks.c */
extern const unsigned wp_hook_count;

#define SLICE_CYCLES 20000
#define RETURN_MAGIC 0xFFFFFF00u /* lr for host-made calls; no code there */

static u32 s_bypass; /* run the original once instead of the hook */

int wp_hook(CPUState* c, u32 addr) {
    unsigned lo = 0, hi = wp_hook_count;
    if (s_bypass == addr) {
        s_bypass = 0;
        return 0;
    }
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (wp_hook_table[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo == wp_hook_count || wp_hook_table[lo].addr != addr) return 0;
    WP_LOG("hook", "%s (%08X) lr %08X", wp_hook_table[lo].name, addr, c->lr);
    return wp_hook_table[lo].fn(c); /* when handled, the hook has set pc (normally to lr) */
}

/* One slice of guest execution plus exception handling. */
static void slice(CPUState* c) {
    u64 now = wp_timebase();
    c->timebase = now;
    c->downcount = SLICE_CYCLES;
    if (!dolrecomp_call(c, c->pc))
        wp_fatal("no recompiled code at %08X", c->pc);
    if (c->exception && !wp_dispatch_exception(c))
        wp_fatal("exception at %08X (srr0 %08X) with no OS handler", c->pc, c->srr0);
    if (c->timebase != now) wp_timebase_set(c->timebase); /* the game wrote TBL/TBU */
}

static void run_until(CPUState* c, u32 stop) {
    while (c->pc != stop) slice(c);
}

void wp_call_original(CPUState* c, u32 addr) {
    u32 ret = c->lr;
    c->lr = RETURN_MAGIC;
    c->pc = addr;
    s_bypass = addr;
    run_until(c, RETURN_MAGIC);
    c->lr = ret;
    c->pc = ret;
}

u32 wp_call_guest(CPUState* c, u32 addr, u32 r3, u32 r4, u32 r5, u32 r6) {
    CPUState saved;
    u32 result;
    memcpy(&saved, c, sizeof(saved));
    c->gpr[1] = (c->gpr[1] - 0x100) & ~7u; /* keep the caller's frame intact */
    c->gpr[3] = r3; c->gpr[4] = r4; c->gpr[5] = r5; c->gpr[6] = r6;
    c->lr = RETURN_MAGIC;
    c->pc = addr;
    run_until(c, RETURN_MAGIC);
    result = c->gpr[3];
    memcpy(c, &saved, sizeof(saved));
    return result;
}

void wp_run(CPUState* c, unsigned long long max_slices) {
    unsigned long long n = 0;
    for (;;) {
        slice(c);
        wp_devices_tick(wp_timebase());
        wp_check_interrupts(c);
        if (max_slices && ++n >= max_slices) {
            fprintf(stderr, "wonderpark: stopped after %llu slices at %08X (lr %08X)\n", n, c->pc, c->lr);
            return;
        }
    }
}
