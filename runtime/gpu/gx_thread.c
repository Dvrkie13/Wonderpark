/* The render thread: GX decoding and OpenGL drawing run beside the game.
 *
 * The game thread only copies what it writes to the GPU (the write-gather
 * pipe, hw_gx.c) into a ring of 16 MB; this thread takes it from there,
 * decodes it (gx_fifo.c) and draws with the device (gl_backend.c), whose
 * context moves here at start. As on a Wii, the GPU runs behind the CPU,
 * and the game keeps the two in step itself: it waits for the GPU's token
 * and draw-done interrupts (about 30 and 15 a second), which this thread
 * raises through hw_gx.c's queue. A full ring makes the game thread wait.
 *
 * The window stays with the game thread, which created it: its messages
 * are handled there (gxt_game_tick, from the device tick and the idle
 * loop), and so is switching to fullscreen. Window titles set here are
 * passed over (window_win32.c).
 *
 * Off by default until it has been measured on a quiet machine (the first
 * measurements, with another game using most of the CPU, were noise):
 * WONDERPARK_RENDER_THREAD=1 turns it on; otherwise decoding and drawing
 * happen on the game thread, as each command arrives.
 * WONDERPARK_RT_DEBUG=1 prints its state and the PE state at exit. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <GL/gl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "host_settings.h"

#define RING (16u << 20)

void gx_fifo_consume(const uint8_t* p, size_t n); /* gx_fifo.c */
extern void (*g_wgpipe_write)(uint32_t value, int size);
extern uint64_t g_gpu_tb;
void win_pump(void);
void win_set_fullscreen(int on);
uint64_t gx_tsc_to_tb(uint64_t tsc); /* gx_fifo.c */

int g_gx_threaded;
void (*g_gx_game_tick)(void); /* hw devices tick and idle.c call it on the game thread */

static uint8_t* sRing;
/* Bytes written (published) and read so far, each on a cache line of its
 * own: the game thread publishes what it wrote in batches, as one store
 * per write had the two cores trade the line millions of times a second. */
static __declspec(align(64)) volatile size_t sW;
static __declspec(align(64)) volatile size_t sR;
static __declspec(align(64)) volatile LONG sSleeping;
/* The game thread's own: written so far, and sR as last seen. */
static __declspec(align(64)) size_t sWLocal;
static size_t sReadSeen;
static volatile LONG sStop, sWaitingForSpace;
static HANDLE sData, sSpace, sThread;
static HDC sDC;
static HGLRC sRC;

#define PUBLISH_EVERY 16384

/* Hands what was written to the render thread, waking it if it sleeps. */
static void publish(void) {
    _WriteBarrier();
    sW = sWLocal;
    MemoryBarrier(); /* the store before reading sSleeping */
    if (sSleeping) SetEvent(sData);
}

/* The ring is full: the render thread is a long way behind. Window
 * messages are still handled meanwhile (the render thread may be waiting
 * on one, setting a swap chain's size). */
static void wait_for_space(size_t need) {
    publish();
    InterlockedExchange(&sWaitingForSpace, 1);
    while (sWLocal + need - (sReadSeen = sR) > RING) {
        win_pump();
        WaitForSingleObject(sSpace, 1);
    }
    InterlockedExchange(&sWaitingForSpace, 0);
}

/* g_wgpipe_write on the game thread: the bytes, most significant first. */
static void push(uint32_t v, int size) {
    size_t w = sWLocal;
    int i;
    if (w + 4 - sReadSeen > RING && w + 4 - (sReadSeen = sR) > RING) wait_for_space(4);
    for (i = size - 1; i >= 0; i--) sRing[(w++) & (RING - 1)] = (uint8_t)(v >> (8 * i));
    sWLocal = w;
    if (w - sW >= PUBLISH_EVERY) publish();
}

static DWORD WINAPI render_main(LPVOID arg) {
    int idle_spins = 0;
    (void)arg;
    if (!wglMakeCurrent(sDC, sRC)) fprintf(stderr, "gpu: render thread: wglMakeCurrent failed (%lu)\n", GetLastError());
    while (!sStop) {
        size_t r = sR, w = sW, n, off;
        uint64_t t0;
        if (w == r) {
            /* Nothing to do: spin a little (the game sends in bursts), then sleep. */
            if (++idle_spins < 200) {
                YieldProcessor();
                continue;
            }
            InterlockedExchange(&sSleeping, 1);
            if (sW == r) WaitForSingleObject(sData, 1);
            InterlockedExchange(&sSleeping, 0);
            idle_spins = 0;
            continue;
        }
        idle_spins = 0;
        _ReadBarrier();
        off = r & (RING - 1);
        n = w - r;
        if (off + n > RING) n = RING - off;
        t0 = __rdtsc();
        gx_fifo_consume(sRing + off, n);
        g_gpu_tb += gx_tsc_to_tb(__rdtsc() - t0);
        _WriteBarrier();
        sR = r + n;
        if (sWaitingForSpace) SetEvent(sSpace);
    }
    wglMakeCurrent(NULL, NULL);
    return 0;
}

/* At exit: stop drawing before the process's teardown frees what the
 * render thread uses. */
static void stop(void) {
    if (!sThread) return;
    if (getenv("WONDERPARK_RT_DEBUG"))
        fprintf(stderr, "gpu: render thread at exit: %zu of %zu bytes taken, %s\n", (size_t)sR, (size_t)sW,
                sSleeping ? "sleeping" : "working");
    if (getenv("WONDERPARK_RT_DEBUG")) {
        void hw_gx_debug_print(void);
        hw_gx_debug_print();
    }
    sStop = 1;
    SetEvent(sData);
    if (GetThreadId(sThread) != GetCurrentThreadId()) WaitForSingleObject(sThread, 1000);
}

/* On the game thread, often (every slice and in the idle loop): wakes the
 * render thread for what was sent, and every 2 ms handles the window's
 * messages and the fullscreen setting. */
static void game_tick(void) {
    static unsigned calls;
    static ULONGLONG last;
    if (sWLocal != sW) publish();
    if (++calls & 63) return;
    {
        ULONGLONG now = GetTickCount64();
        if (now - last < 2) return;
        last = now;
    }
    win_pump();
    win_set_fullscreen(g_settings.fullscreen);
}

/* Moves the device's GL context to a new render thread; 0 if it stays. */
int gxt_start(void) {
    const char* e = getenv("WONDERPARK_RENDER_THREAD");
    if (!e || *e != '1') return 0;
    sDC = wglGetCurrentDC();
    sRC = wglGetCurrentContext();
    if (!sDC || !sRC) return 0; /* not OpenGL */
    sRing = (uint8_t*)malloc(RING);
    sData = CreateEventA(NULL, FALSE, FALSE, NULL);
    sSpace = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!sRing || !sData || !sSpace) return 0;
    glFinish();
    wglMakeCurrent(NULL, NULL);
    sThread = CreateThread(NULL, 0, render_main, NULL, 0, NULL);
    if (!sThread) {
        wglMakeCurrent(sDC, sRC);
        return 0;
    }
    SetThreadPriority(sThread, THREAD_PRIORITY_ABOVE_NORMAL);
    g_gx_threaded = 1;
    g_gx_game_tick = game_tick;
    g_wgpipe_write = push;
    atexit(stop);
    fprintf(stderr, "gpu: drawing on a render thread\n");
    return 1;
}
#else
int g_gx_threaded;
void (*g_gx_game_tick)(void);
int gxt_start(void) { return 0; }
#endif
