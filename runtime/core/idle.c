/* Idle: when no thread can run, the OS's SelectThread spins in a loop
 * until an interrupt readies one. The hook below sleeps instead, until the
 * next scheduled event (retrace, decrementer, audio DMA, host timers), then
 * lets the loop go on so the interrupt is taken. The time slept is counted
 * in g_wait_ticks, which the window title's "busy %" is computed from. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <stdlib.h>

#include "hle.h"

uint64_t hw_vi_next_retrace(void);
uint64_t hw_dsp_next_event(void);
uint64_t wp_dec_next_host(void);
uint64_t wp_timers_next(void);
int wp_irq_pending(void);
extern uint64_t g_wait_ticks;

#define MAX_SLEEP (TB_HZ / 100) /* never more than 10 ms at once */

static uint64_t next_event(uint64_t now) {
    uint64_t c[4] = {hw_vi_next_retrace(), hw_dsp_next_event(), wp_dec_next_host(), wp_timers_next()};
    uint64_t best = now + MAX_SLEEP;
    int i;
    for (i = 0; i < 4; i++)
        if (c[i] && (int64_t)(c[i] - best) < 0) best = c[i];
    return best;
}

static void sleep_until(uint64_t deadline) {
#ifdef _WIN32
    static HANDLE timer;
    int64_t left;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    wp_clock_tick();
    left = (int64_t)(deadline - wp_host_time());
    if (left <= (int64_t)(TB_HZ / 4000)) return; /* under 0.25 ms: not worth it */
    if (timer) {
        LARGE_INTEGER due;
        /* The clock may run faster than real time (game speed, cpu_host.c). */
        due.QuadPart = -(left * 10000000 / (int64_t)TB_HZ / wp_game_speed());
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    } else {
        Sleep(1);
    }
#else
    (void)deadline;
#endif
}

/* The idle loop's first instruction (S2LE01): "lwz r0, RunQueueHint(r13)". */
WP_HOOK(0x80296090, SelectThread_idle) {
    static int off = -1;
    uint64_t start;
    if (off < 0) off = getenv("WONDERPARK_NO_IDLE_SLEEP") != NULL;
    if (LD32(c->gpr[13] - 11648) != 0 || wp_irq_pending()) return 0; /* something to run: go on */
    if (wp_parallel_time_pending()) {
        /* The guest's clock was paused while the GPU or DSP worked: give the
         * time back now, while nothing runs, and let any alarm now due fire. */
        wp_parallel_time_restore();
        c->timebase = wp_timebase();
        c->downcount = -100000;
        return 0;
    }
    if (off) return 0;
    wp_clock_tick();
    start = wp_host_time();
    sleep_until(next_event(start));
    wp_clock_tick();
    g_wait_ticks += wp_host_time() - start;
    if (wp_host_time() - start > TB_HZ / 1000)
        WP_LOG("idle", "%9.2f idle %.1f ms", (double)start * 1000.0 / TB_HZ,
               (double)(wp_host_time() - start) * 1000.0 / TB_HZ);
    c->timebase = wp_timebase();
    c->downcount = -100000; /* end the slice at the loop's branch: devices tick, interrupts are taken */
    return 0;
}
