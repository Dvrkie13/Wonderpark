/* The debug overlay (backtick) and the game-speed toggle (F7 with the
 * overlay open).
 *
 * The overlay is a translucent panel at the top left with last second's
 * numbers: frame rate and slowest frame; each frame's time split into the
 * game's CPU work, GPU work (decoding GX and drawing with GL) and waiting;
 * shader compiles, texture uploads, CPU readbacks of the EFB, vertices,
 * draw batches and EFB copies. The renderer counts these (gx_frames.c,
 * gx_fifo.c, gl_backend.c) into g_wp_stats once a second.
 *
 * It's drawn here with GDI into an image the size of the window, which
 * the renderer lays over each frame (menu_draw, gl_backend.c). The image is
 * redrawn only when the numbers change. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_settings.h"
#include "wp_stats.h"

WpStats g_wp_stats;

static int sOpen;
static uint32_t* sImage;
static int sW, sH;
static unsigned sDrawnSerial = ~0u;

static const int kSpeeds[] = {1, 2, 4, 10};
static int sSpeed; /* index into kSpeeds */

void wp_set_game_speed(int times); /* cpu_host.c */

int overlay_is_open(void) { return sOpen; }

/* The settings menu's hooks (host_settings.h): no menu yet, so these carry
 * only the overlay's keys. */
int menu_window_message(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp) {
    (void)hwnd;
    (void)lp;
    if (msg != WM_KEYDOWN || (lp & (1 << 30))) return 0; /* key down, not repeats */
    if (wp == VK_OEM_3) { /* backtick */
        sOpen = !sOpen;
        sDrawnSerial = ~0u;
        return 1;
    }
    if (wp == VK_F7 && sOpen) {
        sSpeed = (sSpeed + 1) % (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0]));
        wp_set_game_speed(kSpeeds[sSpeed]);
        sDrawnSerial = ~0u;
        return 1;
    }
    return 0;
}

static void draw_panel(int w, int h) {
    enum { PW = 470, LINE = 20, PAD = 10 };
    char lines[16][96];
    int n = 0, i, ph;
    const WpStats* s = &g_wp_stats;
    double frames = s->frames ? s->frames : 1;
    HDC dc;
    HBITMAP bmp, old;
    HFONT font, oldFont;
    BITMAPINFO bi;
    uint32_t* px = NULL;

    snprintf(lines[n++], 96, "Wonderpark debug  (` closes, F7 speed: x%d)", kSpeeds[sSpeed]);
    snprintf(lines[n++], 96, "%.1f fps   slowest frame %.1f ms", s->fps, s->slowest_ms);
    snprintf(lines[n++], 96, "per frame: CPU %.1f ms  GPU %.1f ms  wait %.1f ms", s->cpu_ms / frames,
             s->gpu_ms / frames, s->wait_ms / frames);
    snprintf(lines[n++], 96, "  (GPU = GX decode + GL calls, on %s)", s->render_thread ? "the render thread" : "the game thread");
    snprintf(lines[n++], 96, "shader compiles %u (total %u)", s->shader_compiles, s->shader_compiles_total);
    snprintf(lines[n++], 96, "texture uploads %u  (%.1f ms)", s->texture_uploads, s->texture_ms);
    snprintf(lines[n++], 96, "CPU readbacks %u", s->readbacks);
    snprintf(lines[n++], 96, "vertices %.0f / frame", s->vertices / frames);
    snprintf(lines[n++], 96, "draw batches %.0f / frame  (GX draws %.0f)", s->batches / frames, s->gx_draws / frames);
    snprintf(lines[n++], 96, "EFB copies %.1f / frame  (to textures %.1f)", s->efb_copies / frames,
             s->efb_copies_tex / frames);
    if (!s->render_thread)
        snprintf(lines[n++], 96, "GPU time hidden from the game clock %.1f ms / s", s->hidden_ms);
    {
        extern const char* wp_movie_playing(void); /* dvd.c */
        const char* m = wp_movie_playing();
        if (m) snprintf(lines[n++], 96, "movie: %s", m);
    }

    ph = PAD * 2 + n * LINE;
    if (ph > h) ph = h;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = PW;
    bi.bmiHeader.biHeight = -ph; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, NULL, 0);
    if (!bmp || !px) {
        DeleteDC(dc);
        return;
    }
    old = (HBITMAP)SelectObject(dc, bmp);
    memset(px, 0, (size_t)PW * ph * 4);
    font = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    oldFont = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (i = 0; i < n; i++) TextOutA(dc, PAD, PAD + i * LINE, lines[i], (int)strlen(lines[i]));
    GdiFlush();

    /* White text over a dark, translucent panel. */
    for (i = 0; i < ph && i < h; i++) {
        int x;
        for (x = 0; x < PW && x < w; x++) {
            uint32_t c = px[i * PW + x];
            unsigned lum = ((c >> 16 & 255) + (c >> 8 & 255) + (c & 255)) / 3;
            unsigned a = 170 + lum * (255 - 170) / 255;
            unsigned v = lum;
            if (i == 0 || i == ph - 1 || x == 0 || x == PW - 1) v = 0xC9, a = 255; /* thin gold-ish frame */
            sImage[i * w + x] = (a << 24) | (v << 16) | (v << 8) | (i == 0 || i == ph - 1 || x == 0 || x == PW - 1 ? 0x2A : v);
        }
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

const uint32_t* menu_draw(int w, int h) {
    static int init;
    if (!init) {
        /* WONDERPARK_OVERLAY=1 opens it at start; WONDERPARK_GAME_SPEED=N
         * sets the speed (both for automated runs). */
        const char* e = getenv("WONDERPARK_OVERLAY");
        int i;
        init = 1;
        if (e && *e == '1') sOpen = 1;
        if ((e = getenv("WONDERPARK_GAME_SPEED")) != NULL)
            for (i = 0; i < (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0])); i++)
                if (kSpeeds[i] == atoi(e)) sSpeed = i, wp_set_game_speed(kSpeeds[i]);
    }
    if (!sOpen || w <= 0 || h <= 0) return NULL;
    if (w != sW || h != sH) {
        free(sImage);
        sImage = (uint32_t*)calloc((size_t)w * h, 4);
        sW = w;
        sH = h;
        sDrawnSerial = ~0u;
    }
    if (!sImage) return NULL;
    if (sDrawnSerial != g_wp_stats.serial) {
        draw_panel(w, h);
        sDrawnSerial = g_wp_stats.serial;
    }
    return sImage;
}
