/* Native versions of the SDK's matrix routines (MTX library, paired-single
 * code on a Wii). The game calls them hundreds of thousands of times a
 * second; recompiled, each paired-single instruction is several host
 * operations. Matrices are 3x4 (Mtx, 48 bytes) or 3-vectors, big-endian
 * floats in guest memory. Each result element is summed in double and
 * rounded to single once, as a fused multiply-add chain on the Wii rounds
 * nearly the same (last-bit differences at most).
 *
 * A hook that can't reach its operands in RAM returns 0 and the original
 * code runs. WONDERPARK_NO_NATIVE_MTX=1 turns them all off (comparisons). */
#include <stdlib.h>
#include <string.h>

#include "hle.h"

static int off(void) {
    static int v = -1;
    if (v < 0) v = getenv("WONDERPARK_NO_NATIVE_MTX") != NULL;
    return v;
}

static inline float ldf(const uint8_t* p) {
    uint32_t u = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static inline void stf(uint8_t* p, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    p[0] = (uint8_t)(u >> 24);
    p[1] = (uint8_t)(u >> 16);
    p[2] = (uint8_t)(u >> 8);
    p[3] = (uint8_t)u;
}

static void load_mtx(const uint8_t* p, float m[12]) {
    int i;
    for (i = 0; i < 12; i++) m[i] = ldf(p + 4 * i);
}

static void store_mtx(uint8_t* p, const float m[12]) {
    int i;
    for (i = 0; i < 12; i++) stf(p + 4 * i, m[i]);
}

/* void PSMTXIdentity(Mtx m) */
WP_HOOK(0x802A19B0, PSMTXIdentity) {
    static const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    uint8_t* m;
    if (off() || !(m = wp_ram(c->gpr[3], 48))) return 0;
    store_mtx(m, id);
    wp_return(c);
    return 1;
}

/* void PSMTXCopy(const Mtx src, Mtx dst) */
WP_HOOK(0x802A19E0, PSMTXCopy) {
    uint8_t *s, *d;
    if (off() || !(s = wp_ram(c->gpr[3], 48)) || !(d = wp_ram(c->gpr[4], 48))) return 0;
    memmove(d, s, 48);
    wp_return(c);
    return 1;
}

/* void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab): ab = a * b (ab may be a or b). */
WP_HOOK(0x802A1A20, PSMTXConcat) {
    uint8_t *pa, *pb, *pab;
    float a[12], b[12], r[12];
    int i;
    if (off() || !(pa = wp_ram(c->gpr[3], 48)) || !(pb = wp_ram(c->gpr[4], 48)) || !(pab = wp_ram(c->gpr[5], 48)))
        return 0;
    load_mtx(pa, a);
    load_mtx(pb, b);
    for (i = 0; i < 3; i++) {
        const float* ar = a + 4 * i;
        int j;
        for (j = 0; j < 4; j++) {
            double v = (double)ar[0] * b[j] + (double)ar[1] * b[4 + j] + (double)ar[2] * b[8 + j];
            if (j == 3) v += ar[3];
            r[4 * i + j] = (float)v;
        }
    }
    store_mtx(pab, r);
    wp_return(c);
    return 1;
}

/* void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst): dst = m * (src, 1) (dst may be src). */
WP_HOOK(0x802A2570, PSMTXMultVec) {
    uint8_t *pm, *ps, *pd;
    float m[12], v[3];
    int i;
    if (off() || !(pm = wp_ram(c->gpr[3], 48)) || !(ps = wp_ram(c->gpr[4], 12)) || !(pd = wp_ram(c->gpr[5], 12)))
        return 0;
    load_mtx(pm, m);
    for (i = 0; i < 3; i++) v[i] = ldf(ps + 4 * i);
    for (i = 0; i < 3; i++) {
        const float* r = m + 4 * i;
        stf(pd + 4 * i, (float)((double)r[0] * v[0] + (double)r[1] * v[1] + (double)r[2] * v[2] + r[3]));
    }
    wp_return(c);
    return 1;
}
