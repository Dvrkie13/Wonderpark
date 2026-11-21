/* Time of day and Seasonal touches (Gameplay; [gameplay] time_of_day,
 * seasonal_touches), from the PC's clock and date (settings_local_time).
 *
 * Time of day, "Follow my clock": the park's light graded over the game's
 * own picture (the look pass, gl_backend.c): normal by day, a warm golden
 * evening, a soft blue night, a pale dawn. Never gameplay.
 *
 * Seasonal touches: December a cooler light and light snowfall, from 20
 * October a spooky evening tint and a few drifting leaves, spring (March to
 * May) a few drifting petals. Our own pieces, drawn as small filled
 * rectangles over the picture like the victory confetti (confetti.c): no
 * art. Off on the Potato preset.
 *
 * Both only in the park outside story scenes (not in attractions, lobbies,
 * menus or cutscenes), and never a jump: the grade eases toward where it
 * should be over a couple of seconds (entering or leaving the park), and
 * the clock's own changes take hours. */
#include <math.h>
#include <stdint.h>
#include <time.h>

#include "host_settings.h"

double wp_wall_seconds(void); /* cpu_host.c */
int wp_clock_paused(void);
int hle_scene(void);          /* hle_scene.c: 0 the park */
int hle_event_running(void);
void settings_local_time(struct tm* out);

static float sTint[3] = {1, 1, 1}; /* the grade now (eased) */
static float sAmount;              /* the park's share now, 0-1 (eased) */
static double sLast;

static int in_park(void) { return hle_scene() == 0 && !hle_event_running(); }
static int potato(void) { return g_x[X_GRAPHICS_PRESET] == 0; }

/* The season now: 0 none, 1 December (snow), 2 late October (leaves), 3 spring (petals). */
static int season(const struct tm* t) {
    if (!g_x[X_SEASONAL] || potato()) return 0;
    if (t->tm_mon == 11) return 1;
    if (t->tm_mon == 9 && t->tm_mday >= 20) return 2;
    if (t->tm_mon >= 2 && t->tm_mon <= 4) return 3;
    return 0;
}

/* The clock's light at hour h (0-24): keyframes, eased between. */
static void clock_tint(double h, float out[3]) {
    static const float k[][4] = {
        {0.0f, 0.64f, 0.74f, 1.00f},  {4.5f, 0.64f, 0.74f, 1.00f},  /* night */
        {6.0f, 1.05f, 0.95f, 0.92f},                                /* dawn */
        {7.5f, 1.00f, 1.00f, 1.00f},  {16.5f, 1.00f, 1.00f, 1.00f}, /* day */
        {18.0f, 1.12f, 0.95f, 0.78f}, {19.3f, 1.12f, 0.95f, 0.78f}, /* golden evening */
        {21.0f, 0.64f, 0.74f, 1.00f}, {24.0f, 0.64f, 0.74f, 1.00f}, /* night */
    };
    int i, n = (int)(sizeof(k) / sizeof(k[0]));
    for (i = 0; i + 1 < n; i++)
        if (h >= k[i][0] && h <= k[i + 1][0]) {
            float span = k[i + 1][0] - k[i][0], f = span > 0 ? (float)(h - k[i][0]) / span : 0;
            int c;
            f = f * f * (3 - 2 * f);
            for (c = 0; c < 3; c++) out[c] = k[i][c + 1] + (k[i + 1][c + 1] - k[i][c + 1]) * f;
            return;
        }
    out[0] = out[1] = out[2] = 1;
}

/* Each presented frame (gl_backend.c): the grade, multiplied into the
 * picture (rgb). Returns 0 when there's nothing to do. */
int seasons_grade(float rgb[3]) {
    struct tm t;
    float want[3] = {1, 1, 1}, k;
    double now = wp_wall_seconds(), dt = sLast > 0 ? now - sLast : 0;
    int c, s;
    sLast = now;
    if (dt > 0.25) dt = 0.25;
    settings_local_time(&t);
    if (g_x[X_TIME_OF_DAY]) clock_tint(t.tm_hour + t.tm_min / 60.0, want);
    s = season(&t);
    if (s == 1) want[0] *= 0.95f, want[1] *= 0.99f, want[2] *= 1.06f;           /* December: cooler */
    else if (s == 2) want[0] *= 0.95f, want[1] *= 0.80f, want[2] *= 0.92f;      /* late October: spooky */
    /* Toward where it should be: about 1.5 s to get most of the way. */
    k = (float)(1 - exp(-dt / 0.6));
    sAmount += ((in_park() ? 1.0f : 0.0f) - sAmount) * k;
    for (c = 0; c < 3; c++) sTint[c] += (want[c] - sTint[c]) * k;
    for (c = 0; c < 3; c++) rgb[c] = 1 + (sTint[c] - 1) * sAmount;
    return fabsf(rgb[0] - 1) > 0.002f || fabsf(rgb[1] - 1) > 0.002f || fabsf(rgb[2] - 1) > 0.002f;
}

static float rnd(unsigned i, unsigned k) {
    unsigned h = i * 2654435761u ^ k * 40503u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (h & 0xFFFF) / 65535.0f;
}

/* The falling pieces now, as confetti_pieces: the window's fractions (x
 * right, y down; w and h of the window's height), colour 0xRRGGBB. */
int seasons_pieces(float* x, float* y, float* w, float* h, uint32_t* rgb, int max) {
    static const uint32_t kLeaves[] = {0xD9822B, 0xB5531E, 0x8C4A1C, 0xE0A53A};
    static const uint32_t kPetals[] = {0xF7C6D9, 0xFBE3EC, 0xF2A7C3};
    struct tm t;
    int s, i, count, n = 0;
    float tt;
    if (sAmount < 0.02f) return 0;
    settings_local_time(&t);
    s = season(&t);
    if (!s) return 0;
    count = s == 1 ? 150 : s == 2 ? 16 : 26;
    {
        /* The pieces' own clock: it stops while the game is paused. */
        static double clock, last;
        double now = wp_wall_seconds(), dt = last > 0 ? now - last : 0;
        last = now;
        if (!wp_clock_paused() && dt < 0.25) clock += dt;
        tt = (float)fmod(clock, 3600.0);
    }
    for (i = 0; i < count && n < max; i++) {
        float speed, sway, px, py, flip;
        if (rnd(i, 9) > sAmount) continue; /* fading in and out: fewer pieces */
        speed = s == 1 ? 0.05f + rnd(i, 1) * 0.07f : 0.06f + rnd(i, 1) * 0.05f;
        sway = s == 1 ? 0.012f : 0.05f;
        py = (float)fmod(rnd(i, 2) * 1.2f + tt * speed, 1.2) - 0.1f;
        px = rnd(i, 3) * 1.1f - 0.05f + sinf(tt * (0.6f + rnd(i, 4)) + i) * sway;
        x[n] = px;
        y[n] = py;
        if (s == 1) {
            w[n] = h[n] = 0.0025f + rnd(i, 5) * 0.0035f;
            rgb[n] = 0xFFFFFF;
        } else {
            flip = fabsf(cosf(tt * (1.5f + rnd(i, 6) * 2) + i)); /* turning as it falls */
            w[n] = (s == 2 ? 0.017f : 0.011f) * (0.25f + 0.75f * flip);
            h[n] = s == 2 ? 0.012f : 0.008f;
            rgb[n] = s == 2 ? kLeaves[i % 4] : kPetals[i % 3];
        }
        n++;
    }
    return n;
}
