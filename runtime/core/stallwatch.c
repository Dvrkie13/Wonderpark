/* WONDERPARK_LOG=stalls: finds where long frames go. A thread looks at the
 * game thread every 2 ms; once a frame has taken over 250 ms it records the
 * game thread's call stack each time, and when the frame finally ends it
 * prints the most common stacks (names from the PDB). The game thread
 * marks each shown frame with wp_stallwatch_frame(). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wp.h"

#pragma comment(lib, "dbghelp.lib")

#define DEPTH 18
#define MAXSTACKS 512
/* A frame longer than this is a stall: 250 ms, or WONDERPARK_STALL_MS. */
static int s_long_ms = 250;

static HANDLE s_target;
static volatile LONG s_frames;
static volatile LONGLONG s_frame_at; /* QPC of the last frame */
static struct { DWORD64 pc[DEPTH]; unsigned n; } s_stacks[MAXSTACKS];
static unsigned s_nstacks, s_samples;

void wp_stallwatch_frame(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    s_frame_at = t.QuadPart;
    InterlockedIncrement(&s_frames);
}

#define MAXLRS 128
static struct { u32 lr; unsigned n; } s_lrs[MAXLRS];
static unsigned s_nlrs;

static unsigned lr_slot(u32 lr) {
    unsigned i;
    for (i = 0; i < s_nlrs; i++)
        if (s_lrs[i].lr == lr) return i;
    if (s_nlrs == MAXLRS) return MAXLRS;
    s_lrs[s_nlrs].lr = lr;
    s_lrs[s_nlrs].n = 0;
    return s_nlrs++;
}

static void record(void) {
    CONTEXT ctx;
    STACKFRAME64 sf;
    DWORD64 pc[DEPTH];
    int n = 0;
    unsigned i;
    memset(pc, 0, sizeof(pc));
    if (SuspendThread(s_target) == (DWORD)-1) return;
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(s_target, &ctx)) {
        memset(&sf, 0, sizeof(sf));
        sf.AddrPC.Offset = ctx.Rip, sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp, sf.AddrStack.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp, sf.AddrFrame.Mode = AddrModeFlat;
        while (n < DEPTH && StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), s_target, &sf, &ctx, NULL,
                                        SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
            pc[n++] = sf.AddrPC.Offset;
            if (!sf.AddrPC.Offset) break;
        }
    }
    ResumeThread(s_target);
    if (g_cpu) {
        /* The guest link register: the generated chunks have no line info,
         * so this is what says which game function the time went to. */
        unsigned slot = lr_slot(g_cpu->lr);
        if (slot < MAXLRS) s_lrs[slot].n++;
    }
    s_samples++;
    for (i = 0; i < s_nstacks; i++)
        if (!memcmp(s_stacks[i].pc, pc, sizeof(pc))) { s_stacks[i].n++; return; }
    if (s_nstacks < MAXSTACKS) {
        memcpy(s_stacks[s_nstacks].pc, pc, sizeof(pc));
        s_stacks[s_nstacks++].n = 1;
    }
}

static void name(DWORD64 pc, char* out, size_t size) {
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
    DWORD64 disp = 0;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), pc, &disp, sym)) snprintf(out, size, "%s+%llx", sym->Name, disp);
    else snprintf(out, size, "%llx", (unsigned long long)pc);
}

/* Per function: samples with it innermost ("self") and anywhere on the
 * stack ("total", once per sample), so a stall spread over many slightly
 * different stacks still shows where it went. */
#define MAXFUNCS 256
static struct { DWORD64 at; unsigned self, total; } s_funcs[MAXFUNCS];
static unsigned s_nfuncs;

static DWORD64 func_of(DWORD64 pc) {
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
    DWORD64 disp = 0;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    return SymFromAddr(GetCurrentProcess(), pc, &disp, sym) ? sym->Address : pc;
}

static unsigned func_slot(DWORD64 at) {
    unsigned i;
    for (i = 0; i < s_nfuncs; i++)
        if (s_funcs[i].at == at) return i;
    if (s_nfuncs == MAXFUNCS) return MAXFUNCS;
    s_funcs[s_nfuncs].at = at;
    s_funcs[s_nfuncs].self = s_funcs[s_nfuncs].total = 0;
    return s_nfuncs++;
}

static void report_funcs(void) {
    unsigned i, k, shown;
    s_nfuncs = 0;
    for (i = 0; i < s_nstacks; i++) {
        DWORD64 seen[DEPTH];
        int nseen = 0;
        for (k = 0; k < DEPTH && s_stacks[i].pc[k]; k++) {
            DWORD64 at = func_of(s_stacks[i].pc[k]);
            unsigned slot = func_slot(at);
            int j, dup = 0;
            if (slot == MAXFUNCS) continue;
            if (k == 0) s_funcs[slot].self += s_stacks[i].n;
            for (j = 0; j < nseen; j++) dup |= seen[j] == at;
            if (!dup) { s_funcs[slot].total += s_stacks[i].n; seen[nseen++] = at; }
        }
    }
    for (k = 0; k < 2; k++) {
        fprintf(stderr, k ? "  total:" : "  self:");
        for (shown = 0; shown < 10; shown++) {
            unsigned best = MAXFUNCS, n;
            char nm[128];
            for (i = 0; i < s_nfuncs; i++) {
                n = k ? s_funcs[i].total : s_funcs[i].self;
                if (n && (best == MAXFUNCS || n > (k ? s_funcs[best].total : s_funcs[best].self))) best = i;
            }
            if (best == MAXFUNCS) break;
            n = k ? s_funcs[best].total : s_funcs[best].self;
            name(s_funcs[best].at, nm, sizeof(nm));
            fprintf(stderr, " %s %.0f%%", nm, 100.0 * n / s_samples);
            if (k) s_funcs[best].total = 0; else s_funcs[best].self = 0;
        }
        fprintf(stderr, "\n");
    }
    /* Innermost source lines: the generated chunks are one big function
     * each, so the line says which guest code it was. */
    s_nfuncs = 0;
    for (i = 0; i < s_nstacks; i++) {
        IMAGEHLP_LINE64 ln;
        DWORD disp = 0;
        unsigned slot;
        memset(&ln, 0, sizeof(ln));
        ln.SizeOfStruct = sizeof(ln);
        if (!s_stacks[i].pc[0] || !SymGetLineFromAddr64(GetCurrentProcess(), s_stacks[i].pc[0], &disp, &ln)) continue;
        /* key: the function's address with the line number in the top bits */
        slot = func_slot(func_of(s_stacks[i].pc[0]) | ((DWORD64)ln.LineNumber << 44));
        if (slot != MAXFUNCS) s_funcs[slot].self += s_stacks[i].n;
    }
    fprintf(stderr, "  lines:");
    for (shown = 0; shown < 12; shown++) {
        unsigned best = MAXFUNCS;
        char nm[128];
        for (i = 0; i < s_nfuncs; i++)
            if (s_funcs[i].self && (best == MAXFUNCS || s_funcs[i].self > s_funcs[best].self)) best = i;
        if (best == MAXFUNCS) break;
        name(s_funcs[best].at & ((1ull << 44) - 1), nm, sizeof(nm));
        fprintf(stderr, " %s:%u %.0f%%", nm, (unsigned)(s_funcs[best].at >> 44), 100.0 * s_funcs[best].self / s_samples);
        s_funcs[best].self = 0;
    }
    fprintf(stderr, "\n  guest lr:");
    for (shown = 0; shown < 12; shown++) {
        unsigned best = MAXLRS;
        for (i = 0; i < s_nlrs; i++)
            if (s_lrs[i].n && (best == MAXLRS || s_lrs[i].n > s_lrs[best].n)) best = i;
        if (best == MAXLRS) break;
        fprintf(stderr, " %08X %.0f%%", s_lrs[best].lr, 100.0 * s_lrs[best].n / s_samples);
        s_lrs[best].n = 0;
    }
    s_nlrs = 0;
    fprintf(stderr, "\n");
}

static void report(double ms) {
    unsigned shown, i, best;
    fprintf(stderr, "stall: a %.0f ms frame at %.1f s; game thread stacks (%u samples):\n", ms, wp_wall_seconds(),
            s_samples);
    report_funcs();
    for (shown = 0; shown < 3; shown++) {
        best = MAXSTACKS;
        for (i = 0; i < s_nstacks; i++)
            if (s_stacks[i].n && (best == MAXSTACKS || s_stacks[i].n > s_stacks[best].n)) best = i;
        if (best == MAXSTACKS) break;
        fprintf(stderr, "  %3.0f%%:", 100.0 * s_stacks[best].n / s_samples);
        for (i = 0; i < DEPTH && s_stacks[best].pc[i]; i++) {
            char nm[128];
            name(s_stacks[best].pc[i], nm, sizeof(nm));
            fprintf(stderr, " %s", nm);
        }
        fprintf(stderr, "\n");
        s_stacks[best].n = 0;
    }
    fflush(stderr);
    s_nstacks = s_samples = 0;
}

static DWORD WINAPI watcher(LPVOID arg) {
    LARGE_INTEGER f, now;
    LONG seen = 0;
    LONGLONG since = 0;
    (void)arg;
    /* Symbols for every module now, not while the game thread is paused. */
    SymSetOptions(SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    QueryPerformanceFrequency(&f);
    timeBeginPeriod(1);
    for (;;) {
        LONG frames = s_frames;
        Sleep(2);
        QueryPerformanceCounter(&now);
        if (frames != seen) {
            /* A frame ended: report it if it was long. */
            if (s_samples) report((double)(s_frame_at - since) * 1000.0 / f.QuadPart);
            seen = frames;
            since = s_frame_at;
            continue;
        }
        if (since && (now.QuadPart - since) * 1000 / f.QuadPart > s_long_ms) record();
    }
    return 0;
}

void wp_stallwatch_start(void) {
    if (!wp_log_on("stalls")) return;
    if (getenv("WONDERPARK_STALL_MS")) s_long_ms = atoi(getenv("WONDERPARK_STALL_MS"));
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_target, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, watcher, NULL, 0, NULL);
}
#else
void wp_stallwatch_start(void) {}
void wp_stallwatch_frame(void) {}
#endif
