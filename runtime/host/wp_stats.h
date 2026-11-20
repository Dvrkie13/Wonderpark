/* Last second's performance numbers, for the debug overlay (overlay_win32.c).
 * gx_frames.c fills them once a second from the counters below. */
#pragma once

#include <stdint.h>

typedef struct {
    unsigned serial;      /* bumped each update */
    double fps, slowest_ms;
    double frames;        /* the game's frames in the second */
    double cpu_ms, gpu_ms, wait_ms, hidden_ms;
    double gpu_present_ms; /* the GPU's own time per frame shown (timer queries); -1 not known */
    int render_thread; /* GPU work on its own thread (gpu/gx_thread.c) */
    unsigned shader_compiles, shader_compiles_total;
    unsigned texture_uploads;
    double texture_ms;
    unsigned readbacks;
    double vertices, batches, gx_draws, efb_copies, efb_copies_tex;
} WpStats;

extern WpStats g_wp_stats;

/* Running counters, reset each second by gx_frames.c. */
extern unsigned g_cnt_shader_compiles, g_cnt_readbacks, g_cnt_vertices, g_cnt_gx_draws, g_cnt_efb_copies,
    g_cnt_efb_copies_tex;
