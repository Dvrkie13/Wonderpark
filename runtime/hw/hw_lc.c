/* The locked cache: half of the L1 data cache used as 16 KB of scratch
 * memory at 0xE0000000, filled and flushed by DMA (the DMAU/DMAL SPRs,
 * see cpu_host.c). GX and the game's matrix code use it. */
#include <string.h>

#include "../core/wp.h"

#define LC_SIZE 0x4000u

/* Also read and written directly by the recompiled code (wp_cpu_fast.h). */
uint8_t g_wp_lc[LC_SIZE];
#define s_lc g_wp_lc

uint8_t* hw_lc_ptr(u32 a, u32 n) {
    u32 off = a & 0x0FFFFFFFu;
    if ((a & 0xF0000000u) != 0xE0000000u || off >= LC_SIZE || n > LC_SIZE - off) return NULL;
    return s_lc + off;
}

static u32 lc_read(u32 a, int size) {
    uint8_t* p = hw_lc_ptr(a, (u32)size);
    if (!p) return 0;
    if (size == 4) return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
    if (size == 2) return (u32)p[0] << 8 | p[1];
    return p[0];
}

static void lc_write(u32 a, u32 v, int size) {
    uint8_t* p = hw_lc_ptr(a, (u32)size);
    if (!p) return;
    if (size == 4) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
    else if (size == 2) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
    else p[0] = (uint8_t)v;
}

/* DMAL write with the trigger bit: DMAU = memory address | length high bits,
 * DMAL = cache address | load (1) or store (0) | length low bits | trigger. */
void hw_lc_dma(u32 dmau, u32 dmal) {
    u32 mem = dmau & ~0x1Fu, cache = dmal & ~0x1Fu;
    u32 lines = ((dmau & 0x1F) << 2) | ((dmal >> 2) & 3);
    u32 len = (lines ? lines : 128) * 32;
    int load = (dmal >> 4) & 1;
    uint8_t* lc = hw_lc_ptr(cache, len);
    uint8_t* ram = wp_mem_ptr(mem, len);
    if (!lc || !ram) {
        WP_WARN("locked-cache DMA %s %08X <-> %08X (+%X) out of range", load ? "load" : "store", mem, cache, len);
        return;
    }
    if (load) memcpy(lc, ram, len);
    else memcpy(ram, lc, len);
}

void hw_lc_init(void) {
    /* MMIO canonicalisation leaves 0xE... addresses as they are. */
    wp_mmio_register(0xE0000000u, LC_SIZE, lc_read, lc_write);
}
