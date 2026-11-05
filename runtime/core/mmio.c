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

/* Every virtual alias of physical 0x0C000000-0x0DFFFFFF (0xCC/0xCD uncached,
 * and the cached 0x8C/0x8D the SDK sometimes uses) names the same registers. */
static u32 canon(u32 a) {
    u32 p = a & 0x3FFFFFFFu;
    if (p - 0x0C000000u < 0x02000000u) return p;
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

/* Write-gather pipe: 32 bytes at 0xCC008000, every store appends to the PI FIFO (hw_gx.c). */
void hw_gx_wgpipe(u32 v, int size);
void (*g_wgpipe_write)(u32 value, int size);

/* WONDERPARK_LOG=mmiostat: accesses per second that missed the RAM fast
 * path, by 64 KB region (debugging speed). */
static void mmio_stat(u32 a) {
    static int on = -1;
    static u32 reg[16];
    static unsigned cnt[16], other;
    static u64 since;
    int k;
    if (on < 0) on = wp_log_on("mmiostat");
    if (!on) return;
    for (k = 0; k < 16 && reg[k] && reg[k] != (a >> 16); k++) {}
    if (k < 16) { reg[k] = a >> 16; cnt[k]++; } else other++;
    if (wp_host_time() - since >= TB_HZ) {
        char line[512];
        int n = 0;
        for (k = 0; k < 16 && reg[k]; k++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %04X:%u", reg[k], cnt[k]);
            reg[k] = 0;
            cnt[k] = 0;
        }
        WP_LOG("mmiostat", "last second:%s other %u", line, other);
        other = 0;
        since = wp_host_time();
    }
}

static u64 ext_read(CPUState* c, u32 ea, u8 size) {
    u32 a = canon(ea);
    int i = find(a);
    (void)c;
    mmio_stat(a);
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
    mmio_stat(a);
    if (a - 0x0C008000u < 32u) {
        if (size == 8) { hw_gx_wgpipe((u32)(v >> 32), 4); hw_gx_wgpipe((u32)v, 4); }
        else hw_gx_wgpipe((u32)v, size);
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
