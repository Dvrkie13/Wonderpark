/* From MildRain479/PokePark-Wiicompiled recomp/gpu/gx_frames.c (CC0). */
/* The backend interface (gx.h) over a device (gx_device.h): the frames
 * the game draws, shown as they are at 60 fps, and above it recorded and
 * drawn again between the game's; textures freed once nothing draws
 * them; and the frame rate, in the window title.
 *
 * Frames between the game's
 *
 * Above 60 fps the game still draws 60 frames a second (hle_game.c), and
 * the frames between are drawn here: the newest of the game's frames is
 * recorded (its draws with their state and XF state, clears, EFB copies)
 * and drawn again, each draw's matrices blended from those of the same
 * draw in the frame before, by how far through the frame interval the
 * moment shown is. Shown frames run one game frame behind.
 *
 * A draw is recognized across frames by its key (gx_fifo.c: its display
 * lists and their draws) and, among draws with the same key, by position
 * once the camera's motion is taken out
 * (match_draws). Blended are the position, normal and texture matrices,
 * the post-transform matrices, the projection and the lights' position
 * and direction: everything that moves with the camera or the model. A
 * matrix whose rotation or translation changed too much to be the same
 * object moving (draws matched wrongly, a cut) is shown as it is. Draws
 * with no match, and everything else (vertices, textures), are the
 * newest frame's.
 *
 * Environment:
 *   RECOMP_GPU=vulkan|opengl   draw with this, over the Renderer setting
 *   RECOMP_FPS=1               the frame rate and time per call, each second, on stderr
 *   RECOMP_IGNORE_REFRESH=1    frames above the display's refresh rate are drawn too (for measuring) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
/* The performance counter, from the monotonic clock in nanoseconds. */
typedef long long LONGLONG;
typedef struct {
    LONGLONG QuadPart;
} LARGE_INTEGER;
static void QueryPerformanceCounter(LARGE_INTEGER* t) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->QuadPart = (LONGLONG)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static void QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000; }
#endif

#include "gx_device.h"
#include "host_settings.h"

/* Entry points mark the thread as inside the graphics driver, so the
 * sampling profiler (boot.c) doesn't hold it suspended there. */
volatile long g_in_gl;

/* Time and calls per entry point, for RECOMP_FPS=1. */
enum { GLK_TEXTURE, GLK_TEXTURE_UPDATE, GLK_DRAW, GLK_CLEAR, GLK_COPY, GLK_PRESENT, GL_KINDS };
static const char* const kGlKind[GL_KINDS] = {"texture", "update", "draw", "clear", "copy", "present"};
static LONGLONG sGlTicks[GL_KINDS];
static unsigned sGlCalls[GL_KINDS];

static LONGLONG qpc(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
#define GL_ENTER() LONGLONG t0_ = (g_in_gl = 1, qpc())
#define GL_LEAVE(kind) (sGlTicks[kind] += qpc() - t0_, sGlCalls[kind]++, g_in_gl = 0)

/* Prints and resets the per-entry-point totals. */
static void print_gl_stats(void) {
    LARGE_INTEGER f;
    int k;
    QueryPerformanceFrequency(&f);
    fprintf(stderr, "  gl:");
    for (k = 0; k < GL_KINDS; k++) {
        fprintf(stderr, " %s %u calls %.0f ms%s", kGlKind[k], sGlCalls[k], sGlTicks[k] * 1000.0 / f.QuadPart,
                k + 1 < GL_KINDS ? "," : "\n");
        sGlTicks[k] = 0;
        sGlCalls[k] = 0;
    }
}

static const GXDevice* sDev;
static int sRecording;            /* frames between the game's are drawn */
static unsigned sGameFrames;      /* the game's frames, for the fps report */
static unsigned sPresentsDropped; /* frames left out as the display wasn't ready, for the fps report */

/* Debug overlay counters, reset each second in report() (wp_stats.h). */
#include "wp_stats.h"
unsigned g_cnt_shader_compiles, g_cnt_readbacks, g_cnt_vertices, g_cnt_gx_draws, g_cnt_efb_copies, g_cnt_efb_copies_tex;
uint64_t g_gpu_tb;

/* The game's frames so far (--exit-after's summary, the debug overlay). */
unsigned gxb_game_frames(void) { return sGameFrames; }

/* ------------------------------------------------------------------------ */
/* Showing a frame */

/* Frames per second and the longest frame of the last second, in the
 * window title and, with RECOMP_FPS=1, on stderr. */
static void report(void) {
    static LARGE_INTEGER freq, since, last;
    static unsigned frames;
    static double worst;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        since = last = now;
    } else {
        double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
        if (ms > worst) worst = ms;
        frames++;
        last = now;
    }
    if (now.QuadPart - since.QuadPart >= freq.QuadPart) {
        char title[96];
        double fps = frames * (double)freq.QuadPart / (now.QuadPart - since.QuadPart);
        const char* e = getenv("RECOMP_FPS");
        /* The share of the second the game thread was not waiting for an
         * event: how much headroom is left. */
        extern uint64_t g_wait_ticks;
        static uint64_t lastWait;
        static unsigned lastGameFrames, lastDropped;
        double secs = (double)(now.QuadPart - since.QuadPart) / freq.QuadPart;
        double busy = 100.0 * (1.0 - (double)(g_wait_ticks - lastWait) / PPC_TB_HZ / secs);
        lastWait = g_wait_ticks;
        snprintf(title, sizeof(title), "Wonderpark - %.2f fps, slowest frame %.1f ms, busy %.0f%%", fps,
                 worst, busy);
        win_set_title(title);
        if (e && *e == '1')
            fprintf(stderr, "fps: %.2f, slowest frame %.1f ms, busy %.0f%%, game %u, left out %u\n", fps, worst, busy,
                    sGameFrames - lastGameFrames, sPresentsDropped - lastDropped);
        {
            /* The debug overlay's numbers (overlay_win32.c). */
            extern uint64_t g_wait_ticks;
            static uint64_t lastGpu, lastWaitO;
            static unsigned compilesTotal;
            double tbms = (double)PPC_TB_HZ / 1000.0, frames = sGameFrames - lastGameFrames;
            double waitms = (double)(g_wait_ticks - lastWaitO) / tbms, gpums = (double)(g_gpu_tb - lastGpu) / tbms;
            double busyms = secs * 1000.0 - waitms;
            compilesTotal += g_cnt_shader_compiles;
            g_wp_stats.fps = fps;
            g_wp_stats.slowest_ms = worst;
            g_wp_stats.frames = frames;
            g_wp_stats.wait_ms = waitms;
            g_wp_stats.gpu_ms = gpums;
            {
                /* On the game thread, GPU time is part of its busy time;
                 * on the render thread (gx_thread.c) it is beside it. */
                extern int g_gx_threaded;
                g_wp_stats.cpu_ms = g_gx_threaded ? busyms : busyms > gpums ? busyms - gpums : 0;
                g_wp_stats.render_thread = g_gx_threaded;
            }
            g_wp_stats.hidden_ms = gpums;
            g_wp_stats.shader_compiles = g_cnt_shader_compiles;
            g_wp_stats.shader_compiles_total = compilesTotal;
            g_wp_stats.texture_uploads = sGlCalls[GLK_TEXTURE] + sGlCalls[GLK_TEXTURE_UPDATE];
            {
                LARGE_INTEGER qf;
                QueryPerformanceFrequency(&qf);
                g_wp_stats.texture_ms = (sGlTicks[GLK_TEXTURE] + sGlTicks[GLK_TEXTURE_UPDATE]) * 1000.0 / qf.QuadPart;
            }
            g_wp_stats.readbacks = g_cnt_readbacks;
            g_wp_stats.vertices = g_cnt_vertices;
            g_wp_stats.batches = sGlCalls[GLK_DRAW];
            g_wp_stats.gx_draws = g_cnt_gx_draws;
            g_wp_stats.efb_copies = g_cnt_efb_copies;
            g_wp_stats.efb_copies_tex = g_cnt_efb_copies_tex;
            g_wp_stats.serial++;
            if (e && *e == '1' && frames > 0)
                fprintf(stderr,
                        "stats: per frame cpu %.1f gpu %.1f wait %.1f ms; tex %u (%.1f ms); readbacks %u; "
                        "verts %.0f; batches %.0f; gxdraws %.0f; efbcopies %.1f (tex %.1f)\n",
                        g_wp_stats.cpu_ms / frames, gpums / frames, waitms / frames, g_wp_stats.texture_uploads,
                        g_wp_stats.texture_ms, g_cnt_readbacks, g_cnt_vertices / frames,
                        sGlCalls[GLK_DRAW] / frames, g_cnt_gx_draws / frames, g_cnt_efb_copies / frames,
                        g_cnt_efb_copies_tex / frames);
            {
                extern unsigned g_cnt_disc_reads, g_cnt_disc_ram;
                extern double g_disc_ms, g_disc_max_ms;
                if (e && *e == '1' && g_cnt_disc_reads)
                    fprintf(stderr, "disc: %u reads, %.1f ms, slowest %.1f ms; %u from RAM\n", g_cnt_disc_reads, g_disc_ms,
                            g_disc_max_ms, g_cnt_disc_ram);
                g_cnt_disc_reads = g_cnt_disc_ram = 0;
                g_disc_ms = g_disc_max_ms = 0;
            }
            {
                /* The GPU's own time (timer queries, gl_backend.c): per
                 * present measured, and per game frame (presents of the
                 * second times that, over the game's frames). */
                extern double g_gl_gpu_ms;
                extern unsigned g_gl_gpu_n;
                extern unsigned g_spec_draws, g_uber_draws, g_spec_compiled, g_spec_loaded, g_spec_waiting;
                if (e && *e == '1' && g_gl_gpu_n && frames > 0) {
                    double per = g_gl_gpu_ms / g_gl_gpu_n;
                    unsigned draws = g_spec_draws + g_uber_draws;
                    fprintf(stderr,
                            "gputime: %.2f ms per present, %.2f ms per game frame; specialised draws %.0f%%, "
                            "programs %u compiled %u loaded %u waiting\n",
                            per, per * fps * secs / frames, draws ? 100.0 * g_spec_draws / draws : 0.0,
                            g_spec_compiled, g_spec_loaded, g_spec_waiting);
                }
                g_gl_gpu_ms = 0;
                g_gl_gpu_n = 0;
                g_spec_draws = g_uber_draws = 0;
            }
            g_cnt_shader_compiles = g_cnt_readbacks = g_cnt_vertices = g_cnt_gx_draws = 0;
            g_cnt_efb_copies = g_cnt_efb_copies_tex = 0;
            lastGpu = g_gpu_tb;
            lastWaitO = g_wait_ticks;
        }
        lastGameFrames = sGameFrames;
        lastDropped = sPresentsDropped;
        if (e && *e == '1') print_gl_stats();
        else memset(sGlTicks, 0, sizeof(sGlTicks)), memset(sGlCalls, 0, sizeof(sGlCalls)); /* per second */
        since = now;
        frames = 0;
        worst = 0;
    }
}

/* Presents the EFB rectangle the game copied to the XFB. Nothing slow
 * belongs in here: presenting happens in the GX flush at a retrace, before
 * the retrace interrupt, and a few milliseconds more every frame (reading
 * a controller that isn't plugged in, for one) can leave the game showing
 * every fourth retrace (15 fps). */
static void show(int x, int y, int w, int h) {
    /* Above 60 fps a frame is never waited for: in a window the
     * compositor takes frames at its own rate (with more than one
     * display, maybe a slower one's), and once its queue is full a
     * present waits for it, and the game, drawing these frames while it
     * waits, would slow down. Then the frame is left out. */
    {
        extern void wp_stallwatch_frame(void);
        wp_stallwatch_frame();
    }
    if (sDev->present(x, y, w, h, sRecording)) sPresentsDropped++;
    report();
    {
        /* With the render thread, the window is the game thread's
         * (gx_thread.c handles its messages and fullscreen). */
        extern int g_gx_threaded;
        if (!g_gx_threaded) {
            win_pump();
            win_set_fullscreen(g_settings.fullscreen);
            {
                /* The F1 tab just opened and the game, busy, may not go
                 * idle (where it's normally paused): pause here then. */
                extern int settings_ui_is_open(void);
                extern void gxb_pause_loop(void);
                if (settings_ui_is_open()) gxb_pause_loop();
            }
        }
    }
    /* Between frames, so a change doesn't lose the one being drawn. */
    sDev->apply_settings();
}

/* The F1 settings tab, or the window in the background with "pause in the
 * background" on: called from the idle hook (idle.c), between frames, so
 * the last picture shown is a whole frame. The game waits here with its
 * clock stopped while that picture is shown again (with the tab over it)
 * and the window's messages are handled; settings that change the EFB's
 * size take effect after. */
void gxb_pause_loop(void) {
    extern int settings_ui_wants_pause(void);
    extern void settings_ui_paused_tick(void);
    extern void wp_clock_pause(int on);
    extern int win_has_focus(void);
    extern int g_gx_threaded;
    if (g_gx_threaded || !sDev->present_last) return;
    wp_clock_pause(1);
    while (settings_ui_wants_pause()) {
        int front = win_has_focus();
        LONGLONG t0 = qpc(), t1, t2, t3;
        win_pump();
        t1 = qpc();
        settings_ui_paused_tick();
        win_set_fullscreen(g_settings.fullscreen);
        t2 = qpc();
        if (!sDev->present_last()) break;
        t3 = qpc();
        {
            /* WONDERPARK_LOG=pause: iterations over 50 ms, by part. */
            extern int wp_log_on(const char* cat);
            LARGE_INTEGER f;
            double ms;
            QueryPerformanceFrequency(&f);
            ms = 1000.0 / (double)f.QuadPart;
            if ((t3 - t0) * ms > 50 && wp_log_on("pause"))
                fprintf(stderr, "[pause] pump %.1f ms, tick %.1f ms, present %.1f ms\n", (t1 - t0) * ms, (t2 - t1) * ms,
                        (t3 - t2) * ms);
        }
        Sleep(front ? 15 : 100); /* in the background, rarely */
    }
    wp_clock_pause(0);
    sDev->apply_settings();
}

/* ------------------------------------------------------------------------ */
/* Textures */

int gxb_texture_generation(void) { return g_settings.aniso; }

/* Textures are deleted after the frame is presented: a draw earlier in the
 * frame may still refer to one, and a device may hand a deleted handle
 * straight to the next texture made. Freed while a frame was recorded,
 * they are still drawn by its replays until the next frame is complete. */
#define MAX_PENDING_FREES 1024
static uint32_t sPendingFree[MAX_PENDING_FREES], sFreeLater[MAX_PENDING_FREES];
static int sPendingFreeCount, sFreeLaterCount;

static void free_textures(uint32_t* list, int* count) {
    if (*count) {
        sDev->texture_delete(list, *count);
        *count = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Recording */

enum { OP_DRAW, OP_CLEAR, OP_COPY, OP_PRESENT };

typedef struct RecOp {
    int op;
    GXDrawState st;   /* OP_DRAW (st.xf unused) */
    int xf;           /* OP_DRAW: its XF state, at words xf of the frame's */
    int match;        /* OP_DRAW: the last frame's op of the same draw, or -1 */
    int a[8];         /* the other ops' arguments; OP_DRAW: first vertex, vertices, first index, count */
} RecOp;

typedef struct Frame {
    RecOp* ops;
    int count, cap;
    uint32_t* xf;
    int xf_words, xf_cap;
} Frame;

/* Recording, the newest complete frame and the one before. */
static Frame sFrames[3];
static Frame *sRec = &sFrames[0], *sCur = &sFrames[1], *sPrev = &sFrames[2];
static int sOutputRate = 60, sHaveFrame;
static uint64_t sCurAt, sNextOut, sFramePeriod; /* time base */
static double sLastT = -1;
static int sShownSinceEnd; /* sCur has been drawn (frame_render) since it became the one shown */

static RecOp* rec_op(int op) {
    RecOp* r;
    if (sRec->count == sRec->cap) {
        sRec->cap = sRec->cap ? sRec->cap * 2 : 1024;
        sRec->ops = (RecOp*)realloc(sRec->ops, (size_t)sRec->cap * sizeof(RecOp));
    }
    r = &sRec->ops[sRec->count++];
    r->op = op;
    r->match = -1;
    return r;
}

static void rec_draw(const GXDrawState* st, int first_vertex, int vertices, int first_index, int count) {
    RecOp* r = rec_op(OP_DRAW);
    r->st = *st;
    r->st.xf = NULL;
    if (sRec->xf_words + GX_XF_WORDS > sRec->xf_cap) {
        sRec->xf_cap = (sRec->xf_words + GX_XF_WORDS) * 2;
        sRec->xf = (uint32_t*)realloc(sRec->xf, (size_t)sRec->xf_cap * 4);
    }
    r->xf = sRec->xf_words;
    memcpy(sRec->xf + sRec->xf_words, st->xf, GX_XF_WORDS * 4);
    sRec->xf_words += GX_XF_WORDS;
    r->a[0] = first_vertex;
    r->a[1] = vertices;
    r->a[2] = first_index;
    r->a[3] = count;
}

/* How far apart two draws are: the sum of the differences of their
 * position matrices' translations (the first 10 matrices of XF memory),
 * or at least limit. */
static float matrix_distance(const uint32_t* a, const uint32_t* b, float limit) {
    float d = 0;
    int i;
    for (i = 3; i < 120 && d < limit; i += 4) {
        float x, y;
        if (a[i] == b[i]) continue;
        memcpy(&x, &a[i], 4);
        memcpy(&y, &b[i], 4);
        if (x == x && y == y) d += fabsf(y - x);
    }
    return d;
}

/* A draw's position matrix (MATINDEX_A's), 3x4, in its XF state. */
static const uint32_t* pos_matrix(const uint32_t* xf) {
    return xf + 4 * (xf[GX_XF_MEM_WORDS + 0x18] & 0x3F) % 0x100;
}

static int perspective(const uint32_t* xf) { return xf[GX_XF_MEM_WORDS + 0x26] == 0; }

/* 3x4 matrices (rows of rotation and scale, then translation), as floats. */
typedef struct { float m[3][4]; } Mtx34;

static int load_mtx(Mtx34* out, const uint32_t* w) {
    int i;
    for (i = 0; i < 12; i++) {
        float f;
        memcpy(&f, &w[i], 4);
        if (!(f == f) || fabsf(f) > 1e7f) return 0;
        out->m[i / 4][i % 4] = f;
    }
    return 1;
}

/* out = a * b, as affine transforms. */
static void mtx_mul(Mtx34* out, const Mtx34* a, const Mtx34* b) {
    Mtx34 r;
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + (j == 3 ? a->m[i][3] : 0);
        }
    }
    *out = r;
}

static int mtx_inverse(Mtx34* out, const Mtx34* a) {
    const float(*m)[4] = a->m;
    float c[3][3], det;
    int i, j;
    c[0][0] = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    c[0][1] = m[0][2] * m[2][1] - m[0][1] * m[2][2];
    c[0][2] = m[0][1] * m[1][2] - m[0][2] * m[1][1];
    c[1][0] = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    c[1][1] = m[0][0] * m[2][2] - m[0][2] * m[2][0];
    c[1][2] = m[0][2] * m[1][0] - m[0][0] * m[1][2];
    c[2][0] = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    c[2][1] = m[0][1] * m[2][0] - m[0][0] * m[2][1];
    c[2][2] = m[0][0] * m[1][1] - m[0][1] * m[1][0];
    det = m[0][0] * c[0][0] + m[0][1] * c[1][0] + m[0][2] * c[2][0];
    if (fabsf(det) < 1e-12f) return 0;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) out->m[i][j] = c[i][j] / det;
        out->m[i][3] = -(out->m[i][0] * m[0][3] + out->m[i][1] * m[1][3] + out->m[i][2] * m[2][3]);
    }
    return 1;
}

static int mtx_close(const Mtx34* a, const Mtx34* b) {
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            if (fabsf(a->m[i][j] - b->m[i][j]) > 0.02f) return 0;
        }
        if (fabsf(a->m[i][3] - b->m[i][3]) > 2.0f + 0.02f * fabsf(b->m[i][3])) return 0;
    }
    return 1;
}

/* How the camera moved from sPrev to sCur: what takes a still object's
 * position matrix in the one to its matrix in the other (the view matrix
 * of sCur times the inverse of sPrev's). Found from draws of the 3D scene
 * with a key only one draw has, the transform most of them agree on (the
 * ground, trees and the like stand still; Pokémon don't). Identity if no
 * clear answer. */
static Mtx34 sCamera;
static int sCameraValid;

static void camera_motion(const int* uniqueMatch) {
    enum { SAMPLES = 48 };
    Mtx34 d[SAMPLES];
    int n = 0, i, k, best = -1, bestVotes = 0, step;
    int total = 0;
    for (i = 0; i < sCur->count; i++) total += uniqueMatch[i] >= 0;
    step = total / SAMPLES + 1;
    for (i = 0, k = 0; i < sCur->count && n < SAMPLES; i++) {
        const uint32_t *cur, *prev;
        Mtx34 c, p, pi;
        if (uniqueMatch[i] < 0 || k++ % step) continue;
        cur = sCur->xf + sCur->ops[i].xf;
        prev = sPrev->xf + sPrev->ops[uniqueMatch[i]].xf;
        if (!perspective(cur) || !perspective(prev)) continue;
        if (!load_mtx(&c, pos_matrix(cur)) || !load_mtx(&p, pos_matrix(prev)) || !mtx_inverse(&pi, &p)) continue;
        mtx_mul(&d[n++], &c, &pi);
    }
    for (i = 0; i < n; i++) {
        int votes = 0;
        for (k = 0; k < n; k++) votes += mtx_close(&d[i], &d[k]);
        if (votes > bestVotes) {
            bestVotes = votes;
            best = i;
        }
    }
    sCameraValid = best >= 0 && bestVotes >= 3 && bestVotes * 3 >= n;
    if (sCameraValid) sCamera = d[best];
}

static float wordf(uint32_t w);

/* How far apart a draw of sPrev, moved as the camera moved, and one of
 * sCur are: their position matrices' translations. */
static float camera_distance(const uint32_t* prev, const uint32_t* cur) {
    const uint32_t *p = pos_matrix(prev), *c = pos_matrix(cur);
    float t[3], d = 0;
    int i;
    t[0] = wordf(p[3]);
    t[1] = wordf(p[7]);
    t[2] = wordf(p[11]);
    for (i = 0; i < 3; i++) {
        const float* r = sCamera.m[i];
        d += fabsf(r[0] * t[0] + r[1] * t[1] + r[2] * t[2] + r[3] - wordf(c[4 * i + 3]));
    }
    return d == d ? d : 1e30f;
}

static float draw_distance(const uint32_t* prev, const uint32_t* cur, float limit) {
    if (sCameraValid && perspective(prev) && perspective(cur)) return camera_distance(prev, cur);
    return matrix_distance(prev, cur, limit);
}

/* Pairs each draw of sCur with the same draw of sPrev: of the first few
 * not yet taken with the same key, the one nearest, once moved as the
 * camera moved. (Draws of one key can change order between frames: the
 * save files screen's panels and the prism piece slots on them do, 28 of
 * those, and taken in order they swapped places. Nearest without the
 * camera's motion, a row of crates matched each crate with its neighbour
 * whenever the camera moved further in a frame than they stand apart.) */
static void match_draws(void) {
    enum { CANDIDATES = 48 };
    enum { SLOTS = 8192 };
    static uint64_t keys[SLOTS];
    static uint8_t used[SLOTS];
    static int head[SLOTS], *next, *unique;
    static int nextCap, uniqueCap;
    int i;
    memset(used, 0, sizeof(used));
    if (sPrev->count > nextCap) {
        nextCap = sPrev->count * 2;
        next = (int*)realloc(next, (size_t)nextCap * sizeof(int));
    }
    /* Chains in frame order: walk backwards, pushing on the front. */
    for (i = sPrev->count - 1; i >= 0; i--) {
        uint64_t k = sPrev->ops[i].st.key;
        unsigned slot = (unsigned)(k ^ (k >> 29)) & (SLOTS - 1);
        if (sPrev->ops[i].op != OP_DRAW) continue;
        while (used[slot] && keys[slot] != k) slot = (slot + 1) & (SLOTS - 1);
        if (!used[slot]) {
            used[slot] = 1;
            keys[slot] = k;
            head[slot] = -1;
        }
        next[i] = head[slot];
        head[slot] = i;
    }
    /* Draws whose key only one draw of sPrev has, for the camera's motion. */
    if (sCur->count > uniqueCap) {
        uniqueCap = sCur->count * 2;
        unique = (int*)realloc(unique, (size_t)uniqueCap * sizeof(int));
    }
    for (i = 0; i < sCur->count; i++) {
        uint64_t k = sCur->ops[i].st.key;
        unsigned slot = (unsigned)(k ^ (k >> 29)) & (SLOTS - 1);
        unique[i] = -1;
        if (sCur->ops[i].op != OP_DRAW) continue;
        while (used[slot] && keys[slot] != k) slot = (slot + 1) & (SLOTS - 1);
        if (used[slot] && head[slot] >= 0 && next[head[slot]] < 0) unique[i] = head[slot];
    }
    camera_motion(unique);
    for (i = 0; i < sCur->count; i++) {
        RecOp* r = &sCur->ops[i];
        uint64_t k = r->st.key;
        unsigned slot = (unsigned)(k ^ (k >> 29)) & (SLOTS - 1);
        r->match = -1;
        if (r->op != OP_DRAW) continue;
        while (used[slot] && keys[slot] != k) slot = (slot + 1) & (SLOTS - 1);
        if (used[slot] && head[slot] >= 0) {
            const uint32_t* xf = sCur->xf + r->xf;
            int* link = &head[slot];
            int* best = link;
            float bestD = next[*link] < 0 ? 0 : draw_distance(sPrev->xf + sPrev->ops[*link].xf, xf, 1e30f);
            int n;
            for (n = 1, link = &next[*link]; n < CANDIDATES && *link >= 0 && bestD > 0; n++, link = &next[*link]) {
                float d = draw_distance(sPrev->xf + sPrev->ops[*link].xf, xf, bestD);
                if (d < bestD) {
                    bestD = d;
                    best = link;
                }
            }
            r->match = *best;
            *best = next[*best];
        }
    }
}

static float wordf(uint32_t w) {
    float f;
    memcpy(&f, &w, 4);
    return f;
}

static uint32_t fword(float f) {
    uint32_t w;
    memcpy(&w, &f, 4);
    return w;
}

static void lerp_words(uint32_t* out, const uint32_t* a, const uint32_t* b, int from, int to, float t) {
    int i;
    for (i = from; i < to; i++) {
        if (a[i] != b[i]) out[i] = fword(wordf(a[i]) + (wordf(b[i]) - wordf(a[i])) * t);
    }
}

/* Blends the XF state of a draw from prev (the frame before) to cur. */
static void blend_xf(uint32_t* out, const uint32_t* prev, const uint32_t* cur, float t) {
    int m, j;
    memcpy(out, cur, GX_XF_WORDS * 4);
    /* Position and texture matrices, 3x4, at multiples of 12 words. */
    for (m = 0; m + 12 <= 0x100; m += 12) {
        const uint32_t *a = prev + m, *b = cur + m;
        float scale = 0, rot = 0, dt = 0, tlen = 0;
        int same = 1;
        for (j = 0; j < 12; j++) {
            float x = wordf(a[j]), y = wordf(b[j]);
            if (a[j] != b[j]) same = 0;
            if (!(x == x) || !(y == y)) { same = -1; break; } /* not floats */
            if ((j & 3) == 3) {
                dt += (y - x) * (y - x);
                tlen += y * y;
            } else {
                if (fabsf(y) > scale) scale = fabsf(y);
                if (fabsf(y - x) > rot) rot = fabsf(y - x);
            }
        }
        if (same) continue;
        /* The same object moving turns and moves a little in a frame;
         * another object (a wrong match, a cut) differs wholesale. */
        if (rot > 0.3f * scale || sqrtf(dt) > 0.5f * sqrtf(tlen) + 200.0f) continue;
        lerp_words(out, prev, cur, m, m + 12, t);
    }
    lerp_words(out, prev, cur, 0x400, 0x460, t); /* normal matrices */
    lerp_words(out, prev, cur, 0x500, 0x600, t); /* post-transform matrices */
    for (j = 0; j < 8; j++) lerp_words(out, prev, cur, 0x600 + 16 * j + 10, 0x600 + 16 * j + 16, t); /* lights */
    if (prev[GX_XF_MEM_WORDS + 0x26] == cur[GX_XF_MEM_WORDS + 0x26]) {
        lerp_words(out, prev, cur, GX_XF_MEM_WORDS + 0x20, GX_XF_MEM_WORDS + 0x26, t); /* projection */
    }
}

/* Draws sCur, its matrices t of the way from sPrev's. */
/* 3D glasses (Silly: 3D glasses, red-cyan; g_x[X_STEREO_3D] 1): the
 * recorded frame is drawn twice, the left eye's picture into the red
 * channel alone and the right eye's into green and blue (the device's
 * eye_mask; clears too, so the second eye's clear leaves the first's red).
 * An eye is the camera moved sideways by half the separation: every
 * position matrix of a perspective draw moved along view x (which also
 * moves skinned models' matrices, all in view space), and the projection
 * turned so things at the convergence distance land where they were (no
 * parallax there, nearer things pop out). The HUD (orthographic) is the
 * same for both eyes. Shown once, after the second eye.
 * WONDERPARK_3D_SEPARATION and WONDERPARK_3D_DEPTH (world units) tune it. */
static float sEyeSeparation = -1, sConvergence;

static void eye_xf(uint32_t* out, const uint32_t* in, float shift) {
    int m;
    float p0, p1;
    if (out != in) memcpy(out, in, GX_XF_WORDS * 4);
    if (!perspective(in)) return;
    {
        /* The 2D layouts' camera (as gx_fifo.c tells it apart: 45 degrees,
         * the layout's aspect 1.829): the same for both eyes. */
        float q0 = wordf(in[GX_XF_MEM_WORDS + 0x20]), q2 = wordf(in[GX_XF_MEM_WORDS + 0x22]);
        if (fabsf(q2 - 2.414214f) < 0.001f && q0 > 0 && fabsf(q2 / q0 - 1.8292f) < 0.01f) return;
    }
    {
        /* The draw's position matrix (MATINDEX_A's, anywhere in XF memory),
         * and for skinned models the first 10 (their per-vertex ones). */
        int at = (int)(pos_matrix(in) - in);
        for (m = 0; m + 12 <= 120; m += 12) {
            float x = wordf(out[m + 3]);
            if (m != at && x == x) out[m + 3] = fword(x + shift);
        }
        if (at + 12 <= 0x100) {
            float x = wordf(out[at + 3]);
            if (x == x) out[at + 3] = fword(x + shift);
        }
    }
    p0 = wordf(out[GX_XF_MEM_WORDS + 0x20]);
    p1 = wordf(out[GX_XF_MEM_WORDS + 0x21]);
    out[GX_XF_MEM_WORDS + 0x21] = fword(p1 + p0 * shift / sConvergence);
}

static void render_frame(float t) {
    static uint32_t xf[GX_XF_WORDS];
    int i, pass, mode = sDev->eye_mask ? g_x[X_STEREO_3D] : 0, stereo = mode != 0;
    if (stereo && sEyeSeparation < 0) {
        const char* s = getenv("WONDERPARK_3D_SEPARATION");
        const char* d = getenv("WONDERPARK_3D_DEPTH");
        sEyeSeparation = s && atof(s) > 0 ? (float)atof(s) : 0.8f; /* (measured in the park, Cove Town) */
        sConvergence = d && atof(d) > 0 ? (float)atof(d) : 60.0f;
    }
    for (pass = stereo ? 1 : 0; pass <= (stereo ? 2 : 0); pass++) {
        /* The left eye's camera is to the left: the world to its right. */
        float shift = pass == 1 ? sEyeSeparation / 2 : -sEyeSeparation / 2;
        if (stereo) sDev->eye_mask(mode == 2 ? pass + 2 : pass); /* red, cyan; or the left half, the right */
        for (i = 0; i < sCur->count; i++) {
            const RecOp* r = &sCur->ops[i];
            switch (r->op) {
            case OP_DRAW: {
                const uint32_t* cur = sCur->xf + r->xf;
                if (r->match >= 0 && t < 1.0f) {
                    blend_xf(xf, sPrev->xf + sPrev->ops[r->match].xf, cur, t);
                    cur = xf;
                }
                if (stereo) {
                    eye_xf(xf, cur, shift);
                    cur = xf;
                }
                sDev->draw_recorded(&r->st, cur, r->a[0], r->a[2], r->a[3]);
                break;
            }
            case OP_CLEAR: sDev->clear(r->a[0], r->a[1], r->a[2], (uint32_t)r->a[3], (uint32_t)r->a[4]); break;
            case OP_COPY: sDev->copy(r->a[0], r->a[1], r->a[2], r->a[3], r->a[4], r->a[5], r->a[6], r->a[7]); break;
            default:
                if (mode == 1 && pass == 1) break; /* red-cyan: shown after the second eye */
                if (mode == 1) sDev->eye_mask(0);
                show(r->a[0], r->a[1], r->a[2], r->a[3]);
                if (mode == 1) sDev->eye_mask(pass);
                break;
            }
        }
    }
    if (stereo) sDev->eye_mask(0);
}

/* The game finished a frame: it becomes the one shown. */
static void frame_end(void) {
    Frame* f = sPrev;
    uint64_t now = ppc_timebase();
    /* The frame shown so far was never drawn (the game kept busy, so no
     * frame between came due while it waited): drawn now, whole, so at
     * least every one of the game's frames is seen. */
    if (sHaveFrame && !sShownSinceEnd) render_frame(1.0f);
    sShownSinceEnd = 0;
    sPrev = sCur;
    sCur = sRec;
    sRec = f;
    sRec->count = sRec->xf_words = 0;
    match_draws();
    if (sHaveFrame) {
        /* The game's frame interval, smoothed, within reason. */
        uint64_t d = now - sCurAt, lo = PPC_TB_HZ / 120, hi = PPC_TB_HZ / 20;
        if (d < lo) d = lo;
        if (d > hi) d = hi;
        sFramePeriod = sFramePeriod ? (sFramePeriod * 7 + d) / 8 : d;
    } else {
        sFramePeriod = PPC_TB_HZ / 60;
        sNextOut = now;
    }
    sCurAt = now;
    sHaveFrame = 1;
    sLastT = -1;
    free_textures(sFreeLater, &sFreeLaterCount);
    memcpy(sFreeLater, sPendingFree, (size_t)sPendingFreeCount * sizeof(uint32_t));
    sFreeLaterCount = sPendingFreeCount;
    sPendingFreeCount = 0;
}

/* ------------------------------------------------------------------------ */
/* Pacing */

static uint64_t frame_due(void) { return sRecording && sHaveFrame ? sNextOut : 0; }

/* Frames shown per second: the setting, or the display's refresh rate if
 * lower (more would never be seen). */
static double output_rate(void) {
    static int ignore = -1;
    double hz;
    if (ignore < 0) ignore = getenv("RECOMP_IGNORE_REFRESH") != NULL;
    hz = ignore ? 0 : win_display_hz();
    return hz >= 30 && hz < sOutputRate ? hz : (double)sOutputRate;
}

static void frame_render(void) {
    uint64_t now = ppc_timebase(), step = (uint64_t)(PPC_TB_HZ / output_rate());
    float t = (float)((double)(int64_t)(now - sCurAt) / (double)sFramePeriod);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    sNextOut += step;
    if ((int64_t)(now - sNextOut) > (int64_t)step) sNextOut = now + step; /* fell behind: start again from now */
    if (t == 1.0f && sLastT == 1.0f) return; /* the game is late: nothing new to show */
    sLastT = t;
    GL_ENTER();
    render_frame(t);
    sShownSinceEnd = 1;
    GL_LEAVE(GLK_PRESENT);
}

void gxb_set_output_rate(int fps) { sOutputRate = fps < 30 ? 30 : fps > 360 ? 360 : fps; }

/* ------------------------------------------------------------------------ */
/* Entry points */

void gxb_init(void) {
    const char* v = getenv("RECOMP_GPU");
    win_create();
#ifdef __ANDROID__
    /* Vulkan alone: there is no OpenGL ES device. */
    (void)v;
    sDev = &g_vk_device;
    if (!sDev->init()) {
        fprintf(stderr, "gpu: no Vulkan 1.3 with what the game needs\n");
        exit(1);
    }
#else
    if (v && *v) sDev = strcmp(v, "vulkan") == 0 ? &g_vk_device : &g_gl_device;
    else sDev = g_settings.renderer == RENDERER_VULKAN ? &g_vk_device : &g_gl_device;
    if (!sDev->init()) {
        const GXDevice* other = sDev == &g_gl_device ? &g_vk_device : &g_gl_device;
        fprintf(stderr, "gpu: no %s, trying %s\n", sDev->name, other->name);
        sDev = other;
        if (!sDev->init()) {
            fprintf(stderr, "gpu: no %s either\n", sDev->name);
            exit(1);
        }
    }
#endif
    g_host_frame_due = frame_due;
    g_host_frame_render = frame_render;
}

int gx_wanted_scale(int h) {
    const char* v = getenv("RECOMP_GX_SCALE");
    int s = v && atoi(v) > 0 ? atoi(v) : g_settings.scale;
    if (!s) {
        s = (int)ceil(h / 456.0 - 0.25);
        /* Wonderpark: at most 4x on its own, the highest measured (Wish
         * Park and Seasong Beach at 4x: 12 ms of GPU time a frame on an RX
         * 9060 XT, docs/handoff/PERFORMANCE.md). A 1440p window gets 3x.
         * Higher is still there to choose. */
        if (s > 4) s = 4;
    }
    return s < 1 ? 1 : s > 8 ? 8 : s;
}

int display_scale(void) { return sDev->scale(); }
int display_msaa(void) { return sDev->msaa(); }
int display_renderer(void) { return sDev == &g_vk_device ? RENDERER_VULKAN : RENDERER_OPENGL; }

int gxb_recording(void) {
    int want = sOutputRate > 60 || (g_x[X_STEREO_3D] && sDev->eye_mask); /* 3D glasses: drawn twice from the recording */
    if (sRecording && !want && sHaveFrame) {
        /* Stopping: show the newest frame as it is. */
        GL_ENTER();
        render_frame(1.0f);
        free_textures(sFreeLater, &sFreeLaterCount);
        GL_LEAVE(GLK_PRESENT);
    }
    if (want != sRecording) {
        sRecording = want;
        sHaveFrame = 0;
        sRec->count = sRec->xf_words = 0;
        sCur->count = sCur->xf_words = 0;
        sPrev->count = sPrev->xf_words = 0;
    }
    return sRecording;
}

void gxb_frame_data(const GXVertex* v, int vertices, const uint32_t* index, int count) {
    GL_ENTER();
    sDev->frame_data(v, vertices, index, count);
    GL_LEAVE(GLK_DRAW);
}

int gxb_arena(void) { return sDev->arena_alloc != NULL; }

int gxb_arena_alloc(int n) {
    int r;
    GL_ENTER();
    r = sDev->arena_alloc(n);
    GL_LEAVE(GLK_DRAW);
    return r;
}

void gxb_arena_write(int first, const GXVertex* v, int n) {
    GL_ENTER();
    sDev->arena_write(first, v, n);
    GL_LEAVE(GLK_DRAW);
}

void gxb_arena_free(int first, int n) {
    GL_ENTER();
    sDev->arena_free(first, n);
    GL_LEAVE(GLK_DRAW);
}

uint32_t gxb_texture(uint64_t key, int pix, int width, int height, int levels, const uint8_t* pixels, uint32_t mode0) {
    uint32_t r;
    GL_ENTER();
    (void)key;
    r = sDev->texture(pix, width, height, levels, pixels, mode0);
    GL_LEAVE(GLK_TEXTURE);
    return r;
}

void gxb_texture_update(uint32_t handle, int pix, int width, int height, int levels, const uint8_t* pixels) {
    GL_ENTER();
    sDev->texture_update(handle, pix, width, height, levels, pixels);
    GL_LEAVE(GLK_TEXTURE_UPDATE);
}

unsigned gxb_texture_pixels(void) { return sDev->texture_pixels ? sDev->texture_pixels() : 0; }

void gxb_texture_free(uint32_t handle) {
    if (sPendingFreeCount == MAX_PENDING_FREES) {
        GL_ENTER();
        free_textures(sPendingFree, &sPendingFreeCount);
        GL_LEAVE(GLK_TEXTURE);
    }
    sPendingFree[sPendingFreeCount++] = handle;
}

void gxb_draw_triangles(const GXDrawState* st, const GXVertex* v, int first_vertex, int vertices, const uint32_t* index,
                        int first_index, int count) {
    if (sRecording) {
        rec_draw(st, first_vertex, vertices, first_index, count);
        return;
    }
    GL_ENTER();
    sDev->draw(st, v, first_vertex, vertices, index, first_index, count);
    GL_LEAVE(GLK_DRAW);
}

void gxb_clear(int color, int alpha, int depth, uint32_t rgba, uint32_t z) {
    if (sRecording) {
        RecOp* r = rec_op(OP_CLEAR);
        r->a[0] = color;
        r->a[1] = alpha;
        r->a[2] = depth;
        r->a[3] = (int)rgba;
        r->a[4] = (int)z;
        return;
    }
    GL_ENTER();
    sDev->clear(color, alpha, depth, rgba, z);
    GL_LEAVE(GLK_CLEAR);
}

int gx_register_copy(uint32_t addr, uint32_t handle, int width, int height, int fmt);
void gx_copy_write_back(uint32_t addr, int fmt, int intensity, int w, int h, int stride, const uint8_t* rgba, int force);

/* Copies to put in guest memory too (gx_register_copy says which), once
 * the frame that makes them has been drawn: read back from the GPU, at
 * the EFB's own size. */
typedef struct WriteBack {
    uint32_t addr;
    int target, w, h, fmt, intensity, stride;
} WriteBack;
static WriteBack sWriteBacks[8];
static int sWriteBackCount;
/* Every copy's latest, for gxb_copy_sync. */
static WriteBack sCopiesMade[32];
static int sCopiesMadeCount;

static void write_back(const WriteBack* b, int force) {
    int tw, th, x, y;
    uint8_t* full = sDev->copy_read ? sDev->copy_read(b->target, &tw, &th) : NULL;
    uint8_t* px = full ? (uint8_t*)malloc((size_t)b->w * b->h * 4) : NULL;
    if (px) {
        /* Each EFB pixel: the average of those it was drawn as. */
        for (y = 0; y < b->h; y++) {
            int y0 = y * th / b->h, y1 = (y + 1) * th / b->h;
            for (x = 0; x < b->w; x++) {
                int x0 = x * tw / b->w, x1 = (x + 1) * tw / b->w, sx, sy, k, n = 0;
                unsigned sum[4] = {0, 0, 0, 0};
                if (y1 <= y0) y1 = y0 + 1;
                if (x1 <= x0) x1 = x0 + 1;
                for (sy = y0; sy < y1 && sy < th; sy++) {
                    for (sx = x0; sx < x1 && sx < tw; sx++, n++) {
                        for (k = 0; k < 4; k++) sum[k] += full[((size_t)sy * tw + sx) * 4 + k];
                    }
                }
                for (k = 0; k < 4; k++) px[((size_t)y * b->w + x) * 4 + k] = (uint8_t)(n ? (sum[k] + n / 2) / n : 0);
            }
        }
        gx_copy_write_back(b->addr, b->fmt, b->intensity, b->w, b->h, b->stride, px, force);
    }
    free(px);
    free(full);
}

static void write_back_copies(void) {
    int i;
    for (i = 0; i < sWriteBackCount; i++) write_back(&sWriteBacks[i], 0);
    sWriteBackCount = 0;
}

/* A token or draw-done interrupt is about to tell the game the GPU got
 * this far: copies made before it must be in memory by then, as on a Wii,
 * where a copy is written as the GPU executes it. Once the game knows,
 * it may free the memory (with the render thread, gx_thread.c, a write at
 * the next present landed in memory the game had freed and reused). */
void gxb_sync_point(void) {
    if (sWriteBackCount) write_back_copies();
}

/* g_gx_copy_sync: the copy holding addr, made every frame and so never
 * written back (the game's photos: its JPEG encoder reads the screen's
 * copy), put in memory now, as the frames submitted so far left it. */
void gxb_copy_sync(uint32_t addr) {
    const WriteBack* in = NULL;
    int i;
    for (i = 0; i < sCopiesMadeCount; i++) {
        const WriteBack* b = &sCopiesMade[i];
        /* Its rows of blocks: 8 lines for 4 bits a pixel, else 4. */
        uint32_t bytes = (uint32_t)(b->stride * ((b->h + 7) / 8 * 8)) / ((b->fmt & 15) == 0 ? 8 : 4);
        if (b->addr == addr) {
            in = b;
            break;
        }
        if (!in && addr - b->addr < bytes) in = b;
    }
    if (!in) return;
    if (sRecording) {
        /* The copy is drawn with the frame, not yet ended: what's been
         * recorded of it is drawn now. Nothing more of the frame before is
         * shown until this one ends (the device has this one's vertices). */
        gx_frame_data_so_far();
        GL_ENTER();
        for (i = 0; i < sRec->count; i++) {
            const RecOp* r = &sRec->ops[i];
            if (r->op == OP_DRAW) sDev->draw_recorded(&r->st, sRec->xf + r->xf, r->a[0], r->a[2], r->a[3]);
            else if (r->op == OP_CLEAR) sDev->clear(r->a[0], r->a[1], r->a[2], (uint32_t)r->a[3], (uint32_t)r->a[4]);
            else if (r->op == OP_COPY) sDev->copy(r->a[0], r->a[1], r->a[2], r->a[3], r->a[4], r->a[5], r->a[6], r->a[7]);
        }
        GL_LEAVE(GLK_DRAW);
        sHaveFrame = 0;
    }
    GL_ENTER();
    write_back(in, 1);
    GL_LEAVE(GLK_COPY);
}

void gxb_copy_to_texture(uint32_t addr, int x, int y, int w, int h, int fmt, int intensity, int efb_alpha, int stride) {
    WriteBack made;
    int t, i;
    GL_ENTER();
    t = sDev->copy_target(addr, w, h);
    if (t >= 0 && !sRecording) sDev->copy(t, x, y, w, h, fmt, intensity, efb_alpha);
    GL_LEAVE(GLK_COPY);
    if (t < 0) return;
    if (sRecording) {
        /* The texture is made now, for the draws that follow to name;
         * the copy into it is drawn with the frame. */
        RecOp* r = rec_op(OP_COPY);
        r->a[0] = t;
        r->a[1] = x;
        r->a[2] = y;
        r->a[3] = w;
        r->a[4] = h;
        r->a[5] = fmt;
        r->a[6] = intensity;
        r->a[7] = efb_alpha;
    }
    made.addr = addr;
    made.target = t;
    made.w = w;
    made.h = h;
    made.fmt = fmt;
    made.intensity = intensity;
    made.stride = stride;
    for (i = 0; i < sCopiesMadeCount && sCopiesMade[i].addr != addr; i++) {}
    if (i == sCopiesMadeCount) {
        if (sCopiesMadeCount < 32) sCopiesMadeCount++;
        else i = (int)(addr >> 5) & 31;
    }
    sCopiesMade[i] = made;
    if (!gx_register_copy(addr, sDev->copy_texture(t), w, h, fmt)) return;
    for (i = 0; i < sWriteBackCount && sWriteBacks[i].addr != addr; i++) {}
    if (i == sWriteBackCount) {
        if (sWriteBackCount == 8) return;
        sWriteBackCount++;
    }
    sWriteBacks[i] = made;
}

void gxb_present(int x, int y, int cw, int ch) {
    sGameFrames++;
    {
        extern void wp_ramdump_frame(unsigned frame); /* WONDERPARK_RAMDUMP (ramdump.c) */
        wp_ramdump_frame(sGameFrames);
    }
#ifdef __ANDROID__
    {
        void android_frame_done(void); /* window_android.c: the CPU's clocks kept up */
        android_frame_done();
    }
#endif
    if (sRecording) {
        RecOp* r = rec_op(OP_PRESENT);
        r->a[0] = x;
        r->a[1] = y;
        r->a[2] = cw;
        r->a[3] = ch;
        GL_ENTER();
        frame_end();
        if (sWriteBackCount) write_back_copies();
        GL_LEAVE(GLK_PRESENT);
        return;
    }
    GL_ENTER();
    show(x, y, cw, ch);
    free_textures(sPendingFree, &sPendingFreeCount);
    if (sWriteBackCount) write_back_copies();
    GL_LEAVE(GLK_PRESENT);
}
