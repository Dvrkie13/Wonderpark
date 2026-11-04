/* Included by the generated code instead of DolRecomp's cpu/cpu.h
 * (CMake sets DOLRECOMP_CPU_HEADER to this file for the generated library).
 * Same CPUState and helpers, plus inline fast paths for guest memory
 * accesses that hit MEM1 or MEM2 (nearly all of them); everything else
 * (hardware registers, the locked cache, unmapped addresses) still goes
 * through cpu.c's functions and the MMIO handlers.
 *
 * Unlike cpu.c, the fast stores don't clear lwarx reservations: with one
 * CPU and the OS not using lwarx/stwcx., nothing depends on it. */
#pragma once

#include "cpu/cpu.h"

#include <string.h>

#ifdef _MSC_VER
#include <stdlib.h>
#define WPF_BSWAP16 _byteswap_ushort
#define WPF_BSWAP32 _byteswap_ulong
#define WPF_BSWAP64 _byteswap_uint64
#define WPF_INLINE static __forceinline
#else
#define WPF_BSWAP16 __builtin_bswap16
#define WPF_BSWAP32 __builtin_bswap32
#define WPF_BSWAP64 __builtin_bswap64
#define WPF_INLINE static inline __attribute__((always_inline))
#endif

/* Host pointer for an n-byte access at virtual address a, or NULL. Cached
 * (0x8/0x9) and uncached (0xC/0xD) views of MEM1 (24 MB) and MEM2 (64 MB). */
WPF_INLINE u8* wpf_ptr(CPUState* c, u32 a, u32 n) {
    u32 p = a & 0x3FFFFFFFu;
    if (!(a & 0x80000000u)) return NULL;
    if (p <= GC_MAIN_RAM_SIZE - n) return c->ram + p;
    p -= 0x10000000u;
    if (p <= WII_MEM2_SIZE - n) return c->mem2 + p;
    return NULL;
}

WPF_INLINE u32 wpf_read32(CPUState* c, u32 a) {
    u8* h = wpf_ptr(c, a, 4);
    u32 v;
    if (!h) return mem_read32(c, a);
    memcpy(&v, h, 4);
    return WPF_BSWAP32(v);
}
WPF_INLINE u16 wpf_read16(CPUState* c, u32 a) {
    u8* h = wpf_ptr(c, a, 2);
    u16 v;
    if (!h) return mem_read16(c, a);
    memcpy(&v, h, 2);
    return WPF_BSWAP16(v);
}
WPF_INLINE u8 wpf_read8(CPUState* c, u32 a) {
    u8* h = wpf_ptr(c, a, 1);
    return h ? *h : mem_read8(c, a);
}
WPF_INLINE u64 wpf_read64(CPUState* c, u32 a) {
    u8* h = wpf_ptr(c, a, 8);
    u64 v;
    if (!h) return mem_read64(c, a);
    memcpy(&v, h, 8);
    return WPF_BSWAP64(v);
}
#ifdef WPF_WATCH
/* Debug builds (CMake WP_STORE_WATCH): stores into [wpf_watch_lo,
 * wpf_watch_hi) (physical) are reported with the guest pc of their block. */
extern u32 wpf_watch_lo, wpf_watch_hi;
void wpf_watch_hit(CPUState* c, u32 a, u32 n, u32 v);
#define WPF_CHECK(c, a, n, v) do { if (((a) & 0x3FFFFFFFu) - wpf_watch_lo < wpf_watch_hi - wpf_watch_lo) wpf_watch_hit(c, a, n, v); } while (0)
#else
#define WPF_CHECK(c, a, n, v) ((void)0)
#endif

WPF_INLINE void wpf_write32(CPUState* c, u32 a, u32 v) {
    WPF_CHECK(c, a, 4, v);
    u8* h = wpf_ptr(c, a, 4);
    if (!h) { mem_write32(c, a, v); return; }
    v = WPF_BSWAP32(v);
    memcpy(h, &v, 4);
}
WPF_INLINE void wpf_write16(CPUState* c, u32 a, u16 v) {
    WPF_CHECK(c, a, 2, v);
    u8* h = wpf_ptr(c, a, 2);
    if (!h) { mem_write16(c, a, v); return; }
    v = WPF_BSWAP16(v);
    memcpy(h, &v, 2);
}
WPF_INLINE void wpf_write8(CPUState* c, u32 a, u8 v) {
    WPF_CHECK(c, a, 1, v);
    u8* h = wpf_ptr(c, a, 1);
    if (!h) { mem_write8(c, a, v); return; }
    *h = v;
}
WPF_INLINE void wpf_write64(CPUState* c, u32 a, u64 v) {
    WPF_CHECK(c, a, 8, (u32)v);
    u8* h = wpf_ptr(c, a, 8);
    if (!h) { mem_write64(c, a, v); return; }
    v = WPF_BSWAP64(v);
    memcpy(h, &v, 8);
}

#define mem_read32 wpf_read32
#define mem_read16 wpf_read16
#define mem_read8 wpf_read8
#define mem_read64 wpf_read64
#define mem_write32 wpf_write32
#define mem_write16 wpf_write16
#define mem_write8 wpf_write8
#define mem_write64 wpf_write64
