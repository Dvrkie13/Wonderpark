/* Fast floating point for the recompiled code (CMake WP_FAST_FP, on by
 * default); included at the end of wp_cpu_fast.h.
 *
 * Inline versions of the hottest FP helpers. They compute the same values as
 * DolRecomp's cpu.c (single-precision results rounded to single and written
 * to both paired-single halves; fmuls and paired-single multiplies round the
 * multiplier to 25 bits, as Broadway does; NaN bit patterns copied exactly by
 * paired-single loads and stores), but they don't update FPSCR's status
 * flags (FPRF, FI/FR, exception bits), which games rarely read. fctiw, fma
 * and the reciprocal estimates stay in cpu.c. */
#pragma once

#include <math.h>
#include <stdbool.h>

WPF_INLINE f64 wpf_bits_f64(u64 b) { f64 d; memcpy(&d, &b, 8); return d; }
WPF_INLINE u64 wpf_f64_bits(f64 d) { u64 b; memcpy(&b, &d, 8); return b; }

/* Broadway rounds the multiplier of single-precision multiplies to 25 bits. */
WPF_INLINE f64 wpf_r25(f64 c) {
    u64 b = wpf_f64_bits(c);
    u64 e = b & 0x7FF0000000000000ull;
    if (e == 0 || e == 0x7FF0000000000000ull) return c;
    return wpf_bits_f64((b & 0xFFFFFFFFF8000000ull) + (b & 0x8000000ull));
}

/* f32 bits <-> f64, keeping NaN payloads (a C cast would quiet signalling NaNs). */
WPF_INLINE f64 wpf_from_f32_bits(u32 v) {
    f32 f;
    if ((v & 0x7F800000u) == 0x7F800000u)
        return wpf_bits_f64((u64)(v & 0x80000000u) << 32 | 0x7FF0000000000000ull | (u64)(v & 0x007FFFFFu) << 29);
    memcpy(&f, &v, 4);
    return (f64)f;
}
WPF_INLINE u32 wpf_to_f32_bits(f64 d) {
    u64 b = wpf_f64_bits(d);
    f32 f;
    u32 v;
    if ((b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (b & 0x000FFFFFFFFFFFFFull))
        return (u32)(b >> 32 & 0x80000000u) | 0x7F800000u | (u32)(b >> 29 & 0x007FFFFFu);
    f = (f32)d;
    memcpy(&v, &f, 4);
    return v;
}

WPF_INLINE f64 wpf_single(f64 v) { return (f64)(f32)v; }
WPF_INLINE void wpf_set_single(CPUState* c, u8 d, f64 v) { c->fpr[d] = c->ps1[d] = wpf_single(v); }

WPF_INLINE void wpf_fadds(CPUState* c, u8 d, u8 a, u8 b) { wpf_set_single(c, d, c->fpr[a] + c->fpr[b]); }
WPF_INLINE void wpf_fsubs(CPUState* c, u8 d, u8 a, u8 b) { wpf_set_single(c, d, c->fpr[a] - c->fpr[b]); }
WPF_INLINE void wpf_fmuls(CPUState* c, u8 d, u8 a, u8 m) { wpf_set_single(c, d, c->fpr[a] * wpf_r25(c->fpr[m])); }
WPF_INLINE void wpf_fdivs(CPUState* c, u8 d, u8 a, u8 b) { wpf_set_single(c, d, c->fpr[a] / c->fpr[b]); }
WPF_INLINE void wpf_fadd(CPUState* c, u8 d, u8 a, u8 b) { c->fpr[d] = c->fpr[a] + c->fpr[b]; }
WPF_INLINE void wpf_fsub(CPUState* c, u8 d, u8 a, u8 b) { c->fpr[d] = c->fpr[a] - c->fpr[b]; }
WPF_INLINE void wpf_fmul(CPUState* c, u8 d, u8 a, u8 m) { c->fpr[d] = c->fpr[a] * c->fpr[m]; }
WPF_INLINE void wpf_frsp(CPUState* c, u8 d, u8 b) { c->fpr[d] = wpf_single(c->fpr[b]); }

/* fcmpu/fcmpo: the CR field and FPSCR[FPCC] (no VXSNAN/VXVC). */
WPF_INLINE void wpf_fcmp(CPUState* c, u8 crfd, f64 a, f64 b, bool ordered) {
    u32 bits = a < b ? 8u : a > b ? 4u : a == b ? 2u : 1u;
    u32 sh = 28u - 4u * crfd;
    (void)ordered;
    c->cr = (c->cr & ~(0xFu << sh)) | (bits << sh);
    c->fpscr = (c->fpscr & ~0xF000u) | (bits << 12);
}

/* Paired singles. Results are computed before any register is written, as
 * the destination may also be a source. */
#define WPF_PS2(c, d, e0, e1) do { f64 p0_ = (e0), p1_ = (e1); (c)->fpr[d] = p0_; (c)->ps1[d] = p1_; } while (0)
WPF_INLINE void wpf_ps_add(CPUState* c, u8 d, u8 a, u8 b) {
    WPF_PS2(c, d, wpf_single(c->fpr[a] + c->fpr[b]), wpf_single(c->ps1[a] + c->ps1[b]));
}
WPF_INLINE void wpf_ps_sub(CPUState* c, u8 d, u8 a, u8 b) {
    WPF_PS2(c, d, wpf_single(c->fpr[a] - c->fpr[b]), wpf_single(c->ps1[a] - c->ps1[b]));
}
WPF_INLINE void wpf_ps_mul(CPUState* c, u8 d, u8 a, u8 m) {
    WPF_PS2(c, d, wpf_single(c->fpr[a] * wpf_r25(c->fpr[m])), wpf_single(c->ps1[a] * wpf_r25(c->ps1[m])));
}
WPF_INLINE void wpf_ps_muls0(CPUState* c, u8 d, u8 a, u8 m) {
    f64 k = wpf_r25(c->fpr[m]);
    WPF_PS2(c, d, wpf_single(c->fpr[a] * k), wpf_single(c->ps1[a] * k));
}
WPF_INLINE void wpf_ps_muls1(CPUState* c, u8 d, u8 a, u8 m) {
    f64 k = wpf_r25(c->ps1[m]);
    WPF_PS2(c, d, wpf_single(c->fpr[a] * k), wpf_single(c->ps1[a] * k));
}
/* ps_madd/ps_msub/ps_nmadd/ps_nmsub: a*c +- b, negated after rounding for the n forms. */
WPF_INLINE void wpf_ps_madd(CPUState* c, u8 d, u8 a, u8 m, u8 b, bool subtract, bool negative) {
    f64 b0 = subtract ? -c->fpr[b] : c->fpr[b], b1 = subtract ? -c->ps1[b] : c->ps1[b];
    f64 p0 = wpf_single(c->fpr[a] * wpf_r25(c->fpr[m]) + b0);
    f64 p1 = wpf_single(c->ps1[a] * wpf_r25(c->ps1[m]) + b1);
    if (negative) {
        if (p0 == p0) p0 = -p0;
        if (p1 == p1) p1 = -p1;
    }
    WPF_PS2(c, d, p0, p1);
}
WPF_INLINE void wpf_ps_madds0(CPUState* c, u8 d, u8 a, u8 m, u8 b) {
    f64 k = wpf_r25(c->fpr[m]);
    WPF_PS2(c, d, wpf_single(c->fpr[a] * k + c->fpr[b]), wpf_single(c->ps1[a] * k + c->ps1[b]));
}
WPF_INLINE void wpf_ps_madds1(CPUState* c, u8 d, u8 a, u8 m, u8 b) {
    f64 k = wpf_r25(c->ps1[m]);
    WPF_PS2(c, d, wpf_single(c->fpr[a] * k + c->fpr[b]), wpf_single(c->ps1[a] * k + c->ps1[b]));
}
/* ps_sum0: ps0 = a0 + b1, ps1 = c1. ps_sum1: ps0 = c0, ps1 = a0 + b1. */
WPF_INLINE void wpf_ps_sum0(CPUState* c, u8 d, u8 a, u8 m, u8 b) {
    WPF_PS2(c, d, wpf_single(c->fpr[a] + c->ps1[b]), c->ps1[m]);
}
WPF_INLINE void wpf_ps_sum1(CPUState* c, u8 d, u8 a, u8 m, u8 b) {
    WPF_PS2(c, d, c->fpr[m], wpf_single(c->fpr[a] + c->ps1[b]));
}

/* psq_l / psq_st with the GQR's type and scale. */
WPF_INLINE f64 wpf_pow2(int k) { return wpf_bits_f64((u64)(1023 + k) << 52); }
WPF_INLINE int wpf_gqr_scale(u32 s6) { return (s6 & 0x20) ? (int)s6 - 64 : (int)s6; }

WPF_INLINE f64 wpf_psq_value(CPUState* c, u32 ea, u32 type, int scale) {
    switch (type) {
    case 4: return (f64)(f32)((f64)wpf_read8(c, ea) * wpf_pow2(-scale));
    case 5: return (f64)(f32)((f64)wpf_read16(c, ea) * wpf_pow2(-scale));
    case 6: return (f64)(f32)((f64)(s8)wpf_read8(c, ea) * wpf_pow2(-scale));
    case 7: return (f64)(f32)((f64)(s16)wpf_read16(c, ea) * wpf_pow2(-scale));
    default: return wpf_from_f32_bits(wpf_read32(c, ea));
    }
}

WPF_INLINE bool wpf_psq_load(CPUState* c, u8 frD, u32 ea, bool w, u8 gi, bool indexed, u32 cia) {
    u32 gqr = c->gqr[gi & 7u], type = (gqr >> 16) & 7u;
    int scale = wpf_gqr_scale((gqr >> 24) & 0x3Fu);
    u32 size = (type == 4 || type == 6) ? 1u : (type == 5 || type == 7) ? 2u : 4u;
    if ((type >= 1 && type <= 3) || (type == 0 && (ea & 3u))) return ppc_psq_load(c, frD, ea, w, gi, indexed, cia);
    {
        f64 p0 = wpf_psq_value(c, ea, type, scale);
        f64 p1 = w ? 1.0 : wpf_psq_value(c, ea + size, type, scale);
        c->fpr[frD] = p0;
        c->ps1[frD] = p1;
    }
    return true;
}

WPF_INLINE s32 wpf_quant(f64 v, s32 lo, s32 hi, int scale) {
    f64 s;
    if (v != v) return hi;
    s = v * wpf_pow2(scale);
    if (s <= (f64)lo) return lo;
    if (s >= (f64)hi) return hi;
    return (s32)s; /* truncates, as cpu.c's trunc() */
}

WPF_INLINE void wpf_psq_put(CPUState* c, u32 ea, u32 type, int scale, f64 v) {
    switch (type) {
    case 4: wpf_write8(c, ea, (u8)wpf_quant(v, 0, 255, scale)); break;
    case 5: wpf_write16(c, ea, (u16)wpf_quant(v, 0, 65535, scale)); break;
    case 6: wpf_write8(c, ea, (u8)(s8)wpf_quant(v, -128, 127, scale)); break;
    case 7: wpf_write16(c, ea, (u16)(s16)wpf_quant(v, -32768, 32767, scale)); break;
    default: {
        u32 bits = wpf_to_f32_bits(v);
        if ((bits & 0x7F800000u) == 0 && (bits & 0x007FFFFFu)) bits &= 0x80000000u; /* denormals as zero, as cpu.c */
        wpf_write32(c, ea, bits);
        break;
    }
    }
}

WPF_INLINE bool wpf_psq_store(CPUState* c, u8 frS, u32 ea, bool w, u8 gi, bool indexed, u32 cia) {
    u32 gqr = c->gqr[gi & 7u], type = gqr & 7u;
    int scale = wpf_gqr_scale((gqr >> 8) & 0x3Fu);
    u32 size = (type == 4 || type == 6) ? 1u : (type == 5 || type == 7) ? 2u : 4u;
    if ((type >= 1 && type <= 3) || (type == 0 && (ea & 3u))) return ppc_psq_store(c, frS, ea, w, gi, indexed, cia);
    wpf_psq_put(c, ea, type, scale, c->fpr[frS]);
    if (!w) wpf_psq_put(c, ea + size, type, scale, c->ps1[frS]);
    return true;
}

#define ppc_fadds wpf_fadds
#define ppc_fsubs wpf_fsubs
#define ppc_fmuls wpf_fmuls
#define ppc_fdivs wpf_fdivs
#define ppc_fadd wpf_fadd
#define ppc_fsub wpf_fsub
#define ppc_fmul wpf_fmul
#define ppc_frsp wpf_frsp
#define ppc_fcmp wpf_fcmp
#define ppc_ps_add_op wpf_ps_add
#define ppc_ps_sub_op wpf_ps_sub
#define ppc_ps_mul_op wpf_ps_mul
#define ppc_ps_muls0 wpf_ps_muls0
#define ppc_ps_muls1 wpf_ps_muls1
#define ppc_ps_madd_op wpf_ps_madd
#define ppc_ps_madds0 wpf_ps_madds0
#define ppc_ps_madds1 wpf_ps_madds1
#define ppc_ps_sum0 wpf_ps_sum0
#define ppc_ps_sum1 wpf_ps_sum1
#define ppc_psq_load_inline wpf_psq_load
#define ppc_psq_store_inline wpf_psq_store
