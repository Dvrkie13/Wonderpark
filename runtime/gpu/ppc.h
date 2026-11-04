/* What the GPU module (from the reference runtime, which included its own
 * ppc.h) needs from the CPU side, on top of Wonderpark's core. */
#pragma once

#include <stdint.h>
#include <string.h>

#include "wp.h"

#ifdef _MSC_VER
#include <stdlib.h>
#define BSWAP16 _byteswap_ushort
#define BSWAP32 _byteswap_ulong
#define BSWAP64 _byteswap_uint64
#else
#define BSWAP16 __builtin_bswap16
#define BSWAP32 __builtin_bswap32
#define BSWAP64 __builtin_bswap64
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Guest memory by address (physical or virtual), as the reference's MA(). */
#define MA(a) wp_mem_ptr((uint32_t)(a), 1)

#define WGPIPE_ADDR 0xCC008000u
extern void (*g_wgpipe_write)(uint32_t value, int size);
extern void (*g_gx_flush)(void);
extern void (*g_gx_copy_sync)(uint32_t addr);
extern uint64_t (*g_host_frame_due)(void);
extern void (*g_host_frame_render)(void);

static inline uint64_t ppc_timebase(void) { return wp_timebase(); }
#define PPC_TB_HZ TB_HZ

static inline uint64_t f64_bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double bits_f64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline float bits_f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

#ifdef __cplusplus
}
#endif
