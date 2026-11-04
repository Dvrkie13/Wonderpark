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

/* One slice: generated code runs block after block until the cycle budget
 * is spent (or a magic pc needs the host), handling exceptions as they come. */
static void slice(CPUState* c) {
    u64 now;
    wp_clock_tick();
    now = wp_timebase();
    c->timebase = now;
    c->downcount = SLICE_CYCLES;
    do {
        if (c->pc == WP_MAGIC_IRQ_STEP || c->pc == WP_MAGIC_LOAD_CONTEXT || c->pc == RETURN_MAGIC) {
            if (c->pc != RETURN_MAGIC) wp_irq_step(c);
            break;
        }
        if (!dolrecomp_call(c, c->pc))
            wp_fatal("no recompiled code at %08X", c->pc);
        if (c->exception) {
            if (!wp_dispatch_exception(c))
                wp_fatal("exception at %08X (srr0 %08X) with no OS handler", c->pc, c->srr0);
            break; /* let interrupts and devices catch up after an exception */
        }
    } while (c->downcount > 0 && !wp_interrupt_due(c));
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

/* WONDERPARK_LOG=prof: where slices end, by function, printed at exit. */
#define PROF_SLOTS 4096
static struct { u32 fn; unsigned long long n; } s_prof[PROF_SLOTS];

static void prof_note(u32 pc) {
    u32 off = 0, h;
    const char* name = wp_sym(pc, &off);
    u32 fn = name ? pc - off : pc & ~0xFFu;
    h = (fn >> 2) % PROF_SLOTS;
    while (s_prof[h].n && s_prof[h].fn != fn) h = (h + 1) % PROF_SLOTS;
    s_prof[h].fn = fn;
    s_prof[h].n++;
}

static void prof_print(void) {
    int i, k;
    unsigned long long total = 0;
    for (i = 0; i < PROF_SLOTS; i++) total += s_prof[i].n;
    if (!total) return;
    fprintf(stderr, "profile (%llu samples):\n", total);
    for (k = 0; k < 15; k++) {
        int best = -1;
        for (i = 0; i < PROF_SLOTS; i++)
            if (s_prof[i].n && (best < 0 || s_prof[i].n > s_prof[best].n)) best = i;
        if (best < 0) break;
        {
            const char* name = wp_sym(s_prof[best].fn, NULL);
            fprintf(stderr, "  %5.1f%%  %08X %s\n", 100.0 * s_prof[best].n / total, s_prof[best].fn, name ? name : "?");
        }
        s_prof[best].n = 0;
    }
}

extern u32 g_vi_retraces;

void wp_run(CPUState* c, unsigned long long max_slices) {
    unsigned long long n = 0;
    int prof = wp_log_on("prof");
    for (;;) {
        slice(c);
        if (prof) prof_note(c->pc);
        {
            u64 now = wp_host_time();
            wp_devices_tick(now);
            wp_timers_tick(now);
        }
        if (!wp_check_interrupts(c)) wp_irq_begin(c);
        if (max_slices && ++n >= max_slices) {
            fprintf(stderr, "wonderpark: stopped after %llu slices at %08X (lr %08X), %u retraces, %.1f s\n", n, c->pc,
                    c->lr, g_vi_retraces, (double)wp_host_time() / TB_HZ);
            if (prof) prof_print();
            if (wp_log_on("threads")) { wp_backtrace(c); wp_dump_threads(); }
            {
                /* WONDERPARK_PEEK=addr,len: dump guest memory (words) at the stop. */
                const char* e = getenv("WONDERPARK_PEEK");
                unsigned a, len, i;
                if (e && sscanf(e, "%x,%x", &a, &len) == 2)
                    for (i = 0; i < len; i += 4) fprintf(stderr, "%s%08X", i % 32 ? " " : (i ? "\n" : "peek "), LD32(a + i));
                if (e) fputc('\n', stderr);
            }
            return;
        }
    }
}
