/* Hardware register access: everything outside MEM1/MEM2 lands here (DolRecomp
 * calls external_read/external_write). Devices register 64 KB-granular
 * ranges; anything unclaimed is logged once per address and reads as 0. */
#include <string.h>

#include "wp.h"

#define MAX_RANGES 32

static struct {
    u32 base, size;
    WpMmioRead rd;
    WpMmioWrite wr;
} s_ranges[MAX_RANGES];
static int s_count;

/* The uncached 0xCC/0xCD mappings and their physical forms all name the
 * same registers. */
static u32 canon(u32 a) {
    if ((a & 0xFE000000u) == 0xCC000000u) return a & 0x0DFFFFFFu;
    return a;
}

void wp_mmio_register(u32 base, u32 size, WpMmioRead rd, WpMmioWrite wr) {
    if (s_count == MAX_RANGES) wp_fatal("too many MMIO ranges");
    s_ranges[s_count].base = canon(base);
    s_ranges[s_count].size = size;
    s_ranges[s_count].rd = rd;
    s_ranges[s_count].wr = wr;
    s_count++;
}

static int find(u32 a) {
    int i;
    for (i = 0; i < s_count; i++)
        if (a - s_ranges[i].base < s_ranges[i].size) return i;
    return -1;
}

static void unclaimed(const char* what, u32 a, u64 v, int size) {
    static u32 seen[512];
    static int n;
    int i;
    for (i = 0; i < n; i++) if (seen[i] == a) goto log;
    if (n < 512) seen[n++] = a;
    WP_WARN("unhandled %s%d %08X = %llX (pc %08X)", what, size * 8, a, (unsigned long long)v, g_cpu->pc);
    return;
log:
    WP_LOG("mmio", "unhandled %s%d %08X = %llX", what, size * 8, a, (unsigned long long)v);
}

/* Write-gather pipe: 32 bytes at 0xCC008000, every store appends to the GPU FIFO. */
void (*g_wgpipe_write)(u32 value, int size);

static u64 ext_read(CPUState* c, u32 ea, u8 size) {
    u32 a = canon(ea);
    int i = find(a);
    (void)c;
    if (i >= 0 && s_ranges[i].rd) {
        if (size == 8) return (u64)s_ranges[i].rd(a, 4) << 32 | s_ranges[i].rd(a + 4, 4);
        return s_ranges[i].rd(a, size);
    }
    unclaimed("rd", a, 0, size);
    return 0;
}

static void ext_write(CPUState* c, u32 ea, u64 v, u8 size) {
    u32 a = canon(ea);
    int i;
    (void)c;
    if (a - 0x0C008000u < 32u) {
        if (g_wgpipe_write) {
            if (size == 8) { g_wgpipe_write((u32)(v >> 32), 4); g_wgpipe_write((u32)v, 4); }
            else g_wgpipe_write((u32)v, size);
        }
        return;
    }
    i = find(a);
    if (i >= 0 && s_ranges[i].wr) {
        if (size == 8) { s_ranges[i].wr(a, (u32)(v >> 32), 4); s_ranges[i].wr(a + 4, (u32)v, 4); }
        else s_ranges[i].wr(a, (u32)v, size);
        return;
    }
    unclaimed("wr", a, v, size);
}

void wp_mmio_init(CPUState* c) {
    c->external_read = ext_read;
    c->external_write = ext_write;
}

uint8_t* wp_mem_ptr(uint32_t a, uint32_t n) {
    u32 p = a & 0x3FFFFFFFu;
    if (p < g_cpu->ram_size && n <= g_cpu->ram_size - p) return g_cpu->ram + p;
    if (p >= 0x10000000u && p - 0x10000000u < g_cpu->mem2_size && n <= g_cpu->mem2_size - (p - 0x10000000u))
        return g_cpu->mem2 + (p - 0x10000000u);
    return NULL;
}
