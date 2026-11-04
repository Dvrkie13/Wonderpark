/* Development aids: function names from config/S2LE01/symbols.txt (if it is
 * there) and a guest backtrace from the r1 back-chain. */
#include <stdlib.h>
#include <string.h>

#include "wp.h"

typedef struct { u32 addr, size; char name[48]; } Sym;
static Sym* s_syms;
static int s_nsyms, s_loaded;

static int sym_cmp(const void* a, const void* b) {
    u32 x = ((const Sym*)a)->addr, y = ((const Sym*)b)->addr;
    return x < y ? -1 : x > y;
}

static void load(void) {
    static const char* const kPaths[] = {"config/S2LE01/symbols.txt", "../config/S2LE01/symbols.txt",
                                         "../../config/S2LE01/symbols.txt", "../../../config/S2LE01/symbols.txt"};
    char line[256];
    FILE* f = NULL;
    size_t i;
    int cap = 0;
    s_loaded = 1;
    for (i = 0; i < sizeof(kPaths) / sizeof(kPaths[0]) && !f; i++) f = fopen(kPaths[i], "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char name[48];
        unsigned addr, size = 0;
        if (sscanf(line, "%47s = .text:0x%x; // size:0x%x", name, &addr, &size) < 2) continue;
        if (s_nsyms == cap) {
            cap = cap ? cap * 2 : 4096;
            s_syms = (Sym*)realloc(s_syms, (size_t)cap * sizeof(Sym));
        }
        s_syms[s_nsyms].addr = addr;
        s_syms[s_nsyms].size = size;
        strcpy(s_syms[s_nsyms].name, name);
        s_nsyms++;
    }
    fclose(f);
    qsort(s_syms, (size_t)s_nsyms, sizeof(Sym), sym_cmp);
}

const char* wp_sym(u32 addr, u32* offset) {
    int lo = 0, hi;
    if (!s_loaded) load();
    hi = s_nsyms;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s_syms[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return NULL;
    lo--;
    if (s_syms[lo].size && addr >= s_syms[lo].addr + s_syms[lo].size) return NULL;
    if (offset) *offset = addr - s_syms[lo].addr;
    return s_syms[lo].name;
}

static void print_addr(const char* label, u32 a) {
    u32 off = 0;
    const char* n = wp_sym(a, &off);
    if (n) fprintf(stderr, "  %s %08X %s+0x%X\n", label, a, n, off);
    else fprintf(stderr, "  %s %08X\n", label, a);
}

static void backtrace_from(u32 sp, int max);

/* The OS's threads (__OSActiveThreadQueue at 0x800000DC): state, priority,
 * what each waits on, and where it stopped (from its saved context). */
void wp_dump_threads(void) {
    static const char* const kState[] = {"?", "ready", "running", "?", "waiting", "?", "?", "?", "moribund"};
    u32 t = LD32(0x800000DC), cur = LD32(0x800000E4);
    int n = 0;
    fprintf(stderr, "threads (current %08X):\n", cur);
    while (t && wp_mem_ptr(t, 0x310) && n++ < 32) {
        u32 state = LD16(t + 0x2C8), pc = LD32(t + 0x198), lr = LD32(t + 0x84), sp = LD32(t + 4);
        u32 off = 0;
        const char* name = wp_sym(pc, &off);
        fprintf(stderr, "  %08X %-8s prio %2d suspend %d queue %08X pc %08X %s+0x%X lr %08X\n", t,
                state < 9 ? kState[state] : "?", (int)LD32(t + 0x2D0), (int)LD32(t + 0x2CC), LD32(t + 0x2DC), pc,
                name ? name : "?", off, lr);
        if (t != cur) backtrace_from(sp, 8);
        t = LD32(t + 0x2FC);
    }
}

static void print_addr(const char* label, u32 a);

static void backtrace_from(u32 sp, int max) {
    int i;
    for (i = 0; i < max; i++) {
        u32 next, lr;
        if (!wp_mem_ptr(sp, 8)) break;
        next = LD32(sp);
        if (!next || !wp_mem_ptr(next, 8) || next <= sp) break;
        lr = LD32(next + 4);
        print_addr("      ", lr);
        sp = next;
    }
}

void wp_backtrace(CPUState* c) {
    u32 sp = c->gpr[1];
    int i;
    print_addr("pc", c->pc);
    print_addr("lr", c->lr);
    for (i = 0; i < 24; i++) {
        u32 next, lr;
        if (!wp_mem_ptr(sp, 8)) break;
        next = LD32(sp);
        if (!next || !wp_mem_ptr(next, 8) || next <= sp) break;
        lr = LD32(next + 4);
        print_addr("  ", lr);
        sp = next;
    }
}
