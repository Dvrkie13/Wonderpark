/* Store and load watch (debug builds: build_game.py --watch): WONDERPARK_WATCH=lo-hi
 * (physical, hex) reports which guest code stores into or loads from that range: each
 * distinct block pc once, with a count every 5 s. */
#include <stdlib.h>

#include "wp.h"

u32 wpf_watch_lo = 1, wpf_watch_hi = 0;
#define PCS 64
static struct { u32 pc, n, last; } s_pcs[PCS];

void wpf_watch_hit(CPUState* c, u32 a, u32 n, u32 v) {
    int i;
    for (i = 0; i < PCS; i++) {
        if (s_pcs[i].pc == c->pc || !s_pcs[i].pc) {
            if (!s_pcs[i].pc) {
                u32 off = 0;
                const char* name = wp_sym(c->pc, &off);
                fprintf(stderr, "[watch] store%u %08X = %08X from block %08X %s+0x%X (lr %08X)\n", n * 8, a, v, c->pc,
                        name ? name : "?", off, c->lr);
                s_pcs[i].pc = c->pc;
            }
            s_pcs[i].n++;
            return;
        }
    }
}

/* Loads from the range: each distinct block pc once, with the value and
 * the link register (the caller, for a leaf function). */
static struct { u32 pc; } s_loads[PCS];

void wpf_watch_load(CPUState* c, u32 a, u32 n) {
    int i;
    for (i = 0; i < PCS; i++) {
        if (s_loads[i].pc == c->pc) return;
        if (!s_loads[i].pc) {
            u32 off = 0, loff = 0, v = n == 4 ? LD32(a) : n == 2 ? LD16(a) : n == 1 ? LD8(a) : LD32(a);
            const char* name = wp_sym(c->pc, &off);
            const char* lname = wp_sym(c->lr, &loff);
            fprintf(stderr, "[watch] load%u %08X (= %08X) from block %08X %s+0x%X (lr %08X %s+0x%X)\n", n * 8, a, v, c->pc,
                    name ? name : "?", off, c->lr, lname ? lname : "?", loff);
            s_loads[i].pc = c->pc;
            return;
        }
    }
}

void wp_watch_init(void) {
    const char* e = getenv("WONDERPARK_WATCH");
    unsigned lo, hi;
    if (e && sscanf(e, "%x-%x", &lo, &hi) == 2) {
        wpf_watch_lo = lo & 0x3FFFFFFFu;
        wpf_watch_hi = hi & 0x3FFFFFFFu;
        fprintf(stderr, "watch: stores to %08X-%08X\n", wpf_watch_lo, wpf_watch_hi);
    }
}
