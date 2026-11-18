/* Victory confetti (Silly; [silly] victory_confetti): a burst of confetti
 * over the picture when an attraction is won.
 *
 * The moment: the game draws its "Attraction Cleared!" banner
 * (MenuAttraction.dac, AttractionMenu02.tpl), found as it's bound
 * (hle_prompts.c) and noticed when a draw samples it (gx_fifo.c,
 * confetti_texture_drawn). A burst starts the first time it's drawn after
 * at least 10 seconds without it.
 *
 * The pieces: 160 paper rectangles in the park's colours, thrown up from
 * the bottom corners, falling and fluttering (their width follows a turning
 * cosine, as paper seen edge-on) for 4.5 seconds. Positions are worked out
 * here from the time alone; the renderer draws them as small filled
 * rectangles over the presented picture (gl_backend.c). No art. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "host_settings.h"

double wp_wall_seconds(void); /* cpu_host.c */

#define PIECES 160
#define LENGTH 4.5

uint32_t g_confetti_texture; /* the banner's image, physical address; 0 none */
static double sStart = -100, sLastSeen = -100;

void confetti_texture_drawn(void) {
    double now = wp_wall_seconds();
    if (now - sLastSeen > 10 && g_x[X_VICTORY_CONFETTI]) sStart = now;
    sLastSeen = now;
}

/* Tests: confetti now. */
void confetti_start(void) { sStart = wp_wall_seconds(); }

static float rnd(unsigned i, unsigned k) {
    unsigned h = i * 2654435761u ^ k * 40503u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (h & 0xFFFF) / 65535.0f;
}

/* The pieces now, in the window's fractions (x right, y down, w and h of
 * the window's height), colour 0xRRGGBB, alpha 0-1. Returns how many. */
int confetti_pieces(float* x, float* y, float* w, float* h, uint32_t* rgb, float* alpha, int max) {
    static const uint32_t kColours[] = {0xF6D44A, 0x4A7FE0, 0xE2457F, 0x5BC26A, 0xF39A2B, 0x8B5BD2, 0xFFFFFF};
    double t;
    {
        /* Tests: WONDERPARK_CONFETTI=<seconds> throws some then. */
        static double testAt = -2;
        if (testAt == -2) {
            const char* e = getenv("WONDERPARK_CONFETTI");
            testAt = e && *e ? atof(e) : -1;
        }
        if (testAt >= 0 && wp_wall_seconds() >= testAt) testAt = -1, confetti_start();
    }
    t = wp_wall_seconds() - sStart;
    int i, n = 0;
    if (t < 0 || t > LENGTH) return 0;
    for (i = 0; i < PIECES && n < max; i++) {
        float side = i & 1 ? 1.0f : -1.0f;
        float delay = rnd(i, 1) * 0.6f;
        float tt = (float)t - delay, vx, vy, px, py, flip;
        if (tt < 0) continue;
        /* Thrown up and inward from a bottom corner, then falling slowly
         * (air), drifting. */
        vx = side * -(0.25f + rnd(i, 2) * 0.45f);
        vy = -(0.9f + rnd(i, 3) * 0.6f);
        px = (side > 0 ? 1.0f : 0.0f) + vx * (1 - expf(-tt * 1.6f)) / 1.6f + sinf(tt * 2.0f + i) * 0.02f;
        py = 1.02f + vy * (1 - expf(-tt * 1.6f)) / 1.6f + 0.18f * tt * tt;
        flip = fabsf(cosf(tt * (5.0f + rnd(i, 4) * 6.0f) + i));
        x[n] = px;
        y[n] = py;
        w[n] = (0.008f + rnd(i, 5) * 0.006f) * (0.2f + 0.8f * flip);
        h[n] = 0.012f + rnd(i, 6) * 0.008f;
        rgb[n] = kColours[i % (sizeof(kColours) / sizeof(kColours[0]))];
        alpha[n] = t > LENGTH - 1 ? (float)(LENGTH - t) : 1.0f;
        n++;
    }
    return n;
}
