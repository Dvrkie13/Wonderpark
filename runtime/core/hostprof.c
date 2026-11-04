/* WONDERPARK_LOG=hostprof: a sampling profiler of the host. A thread
 * samples the game thread's instruction pointer about 2000 times a second
 * and every 15 s prints the hottest host functions (names from the PDB via
 * DbgHelp). For finding where the recompiled code spends its time. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <stdio.h>
#include <stdlib.h>

#include "wp.h"

#pragma comment(lib, "dbghelp.lib")

#define SLOTS 65536
static struct { DWORD64 fn; unsigned n; char name[64]; } s_tab[SLOTS];
static DWORD64 s_ips[1 << 20];
static unsigned s_nips;
static HANDLE s_target;
static unsigned s_total;

static void note(DWORD64 ip) {
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
    DWORD64 disp = 0, fn = ip;
    unsigned h;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), ip, &disp, sym)) fn = sym->Address;
    h = (unsigned)((fn >> 4) % SLOTS);
    while (s_tab[h].n && s_tab[h].fn != fn) h = (h + 1) % SLOTS;
    if (!s_tab[h].n) {
        s_tab[h].fn = fn;
        if (fn != ip || disp == 0) snprintf(s_tab[h].name, sizeof(s_tab[h].name), "%s", sym->Name);
        else snprintf(s_tab[h].name, sizeof(s_tab[h].name), "?%llx", (unsigned long long)ip);
    }
    s_tab[h].n++;
    s_total++;
    if (s_nips < (1u << 20)) s_ips[s_nips++] = ip;
}

static int by_count(const void* a, const void* b) {
    unsigned x = *(const unsigned*)a, y = *(const unsigned*)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* Prints the top functions, and writes every sample's source line to
 * build/hostprof_lines.txt (file:line), for tools/analysis/hostprof.py. */
static void report(void) {
    static unsigned order[SLOTS * 2];
    int i, n = 0;
    FILE* f;
    for (i = 0; i < SLOTS; i++) if (s_tab[i].n) { order[n * 2] = s_tab[i].n; order[n * 2 + 1] = (unsigned)i; n++; }
    qsort(order, (size_t)n, 2 * sizeof(unsigned), by_count);
    fprintf(stderr, "host profile (%u samples):\n", s_total);
    for (i = 0; i < n && i < 25; i++)
        fprintf(stderr, "  %5.1f%%  %s\n", 100.0 * order[i * 2] / s_total, s_tab[order[i * 2 + 1]].name);
    f = fopen("build/hostprof_lines.txt", "w");
    if (f) {
        unsigned k;
        for (k = 0; k < s_nips; k++) {
            IMAGEHLP_LINE64 line;
            DWORD disp = 0;
            line.SizeOfStruct = sizeof(line);
            if (SymGetLineFromAddr64(GetCurrentProcess(), s_ips[k], &disp, &line))
                fprintf(f, "%s:%lu\n", line.FileName, line.LineNumber);
            else
                fprintf(f, "?:%llx\n", (unsigned long long)s_ips[k]);
        }
        fclose(f);
    }
    for (i = 0; i < SLOTS; i++) s_tab[i].n = 0, s_tab[i].fn = 0;
    s_total = 0;
    s_nips = 0;
    fflush(stderr);
}

static DWORD WINAPI sampler(LPVOID arg) {
    DWORD start = GetTickCount();
    (void)arg;
    timeBeginPeriod(1);
    for (;;) {
        CONTEXT ctx;
        Sleep(0);
        Sleep(1);
        if (SuspendThread(s_target) == (DWORD)-1) break;
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(s_target, &ctx)) {
            ResumeThread(s_target);
            note(ctx.Rip);
        } else {
            ResumeThread(s_target);
        }
        if (GetTickCount() - start >= 15000) {
            report();
            start = GetTickCount();
        }
    }
    return 0;
}

void wp_hostprof_start(void) {
    if (!wp_log_on("hostprof")) return;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_target, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, sampler, NULL, 0, NULL);
}
#else
void wp_hostprof_start(void) {}
#endif
