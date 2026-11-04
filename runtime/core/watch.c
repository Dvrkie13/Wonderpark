/* Store watch (debug builds with WP_STORE_WATCH): WONDERPARK_WATCH=lo-hi
 * (physical, hex) reports which guest code stores into that range: each
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

void wp_watch_init(void) {
    const char* e = getenv("WONDERPARK_WATCH");
    unsigned lo, hi;
    if (e && sscanf(e, "%x-%x", &lo, &hi) == 2) {
        wpf_watch_lo = lo & 0x3FFFFFFFu;
        wpf_watch_hi = hi & 0x3FFFFFFFu;
        fprintf(stderr, "watch: stores to %08X-%08X\n", wpf_watch_lo, wpf_watch_hi);
    }
}
