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
#include <string.h>

#include "wp.h"

#pragma comment(lib, "dbghelp.lib")

#define DEPTH 18
#define MAXSTACKS 512
#define LONG_MS 250

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

static void report(double ms) {
    unsigned shown, i, best;
    fprintf(stderr, "stall: a %.0f ms frame at %.1f s; game thread stacks (%u samples):\n", ms, wp_wall_seconds(),
            s_samples);
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
        if (since && (now.QuadPart - since) * 1000 / f.QuadPart > LONG_MS) record();
    }
    return 0;
}

void wp_stallwatch_start(void) {
    if (!wp_log_on("stalls")) return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_target, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, watcher, NULL, 0, NULL);
}
#else
void wp_stallwatch_start(void) {}
void wp_stallwatch_frame(void) {}
#endif
