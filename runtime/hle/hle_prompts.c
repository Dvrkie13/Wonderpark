/* Button prompts: the game's Wii Remote buttons shown as what the player
 * has bound to them, on the device used last (pad_icons.cpp: the pad's own
 * icons, or key caps for the keyboard and mouse).
 *
 * Adapted from MildRain479/PokePark-2-Wiicompiled's recomp/patches/
 * hle_prompts.c (CC0), whose addresses match our build.
 *
 * Text: the game's messages show a Wii Remote button with a tag that draws
 * a character of its font, U+E040 + n (power, the d-pad, A, B, HOME, +, -,
 * 1, 2). Those glyphs are redrawn, in the font itself (its I4 sheets, one
 * colour: the icon's light parts as ink), when the font is loaded and
 * whenever the bindings, the set or the device change; text drawn after
 * that shows them. A button with nothing bound keeps the game's glyph.
 *
 * Pictures: the field's action bubble (MenuField.dac, ActionWin.tpl: "Talk",
 * "Search", over a 2 in a grey disc) and the title's "Please Press"
 * (MenuStartMenu.dac, StartButtonTwo.tpl), found as the game binds them
 * (TPLBind) by size and format, and redrawn the same way in their own
 * colours.
 *
 * Our texture cache sees the changed pixels (gx_fifo.c: Windows' write
 * watch on guest memory), so nothing more is needed to show them. */
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "host_input.h"

int (*g_host_prompt_icon)(uint32_t button, uint8_t* coverage, int w, int h);
uint32_t (*g_host_prompt_generation)(void);

#define FIRST_ICON 0xE041 /* the d-pad; then A, B, HOME, +, -, 1, 2 */
#define ICONS 8
static const uint32_t kButton[ICONS] = {
    HOST_PAD_SCREEN_UP, HOST_PAD_A, HOST_PAD_B, HOST_PAD_HOME, HOST_PAD_PLUS, HOST_PAD_MINUS, HOST_PAD_1, HOST_PAD_2,
};

#define MAX_FONTS 4
#define MAX_CELL (64 * 64)

typedef struct {
    u32 font, sheets;
    int cw, ch, baseline, sheetSize, perRow, perSheet, sheetWidth, fmt;
    int glyph[ICONS];         /* glyph index, or -1 */
    int width[ICONS];         /* how much of its cell is drawn, from the left */
    uint8_t* original[ICONS]; /* the game's pixels, 0-15 per pixel, cw x ch */
} Font;

static Font sFonts[MAX_FONTS];
static int sFontCount;
static uint32_t sGeneration = 0xFFFFFFFFu;

/* A pixel of a 4-bit (I4) sheet: 8x8 tiles of 32 bytes, row by row. */
static u32 i4_byte(const Font* f, int sheet, int x, int y) {
    int tiles = f->sheetWidth / 8;
    return f->sheets + (u32)sheet * f->sheetSize + (u32)(((y / 8) * tiles + x / 8) * 32 + (y % 8) * 4 + (x % 8) / 2);
}

static int get_px(const Font* f, int sheet, int x, int y) {
    uint8_t b = (uint8_t)LD8(i4_byte(f, sheet, x, y));
    return x % 2 ? b & 15 : b >> 4;
}

static void set_px(const Font* f, int sheet, int x, int y, int v) {
    u32 a = i4_byte(f, sheet, x, y);
    uint8_t b = (uint8_t)LD8(a);
    ST8(a, (uint8_t)(x % 2 ? (b & 0xF0) | v : (b & 0x0F) | (v << 4)));
}

/* Where a glyph's cell is: its sheet and top-left pixel. */
static void cell_of(const Font* f, int glyph, int* sheet, int* x0, int* y0) {
    int k = glyph % f->perSheet;
    *sheet = glyph / f->perSheet;
    *x0 = (k % f->perRow) * (f->cw + 1);
    *y0 = (k / f->perRow) * (f->ch + 1);
}

/* The glyph a character maps to in font data at base (block by block). */
static int glyph_of(u32 base, u32 size, uint16_t code) {
    u32 pos = base + LD16(base + 0xC), end = base + size;
    while (pos + 8 <= end) {
        u32 blockSize = LD32(pos + 4);
        if (LD32(pos) == 0x434D4150) { /* CMAP */
            uint16_t lo = (uint16_t)LD16(pos + 8), hi = (uint16_t)LD16(pos + 10), method = (uint16_t)LD16(pos + 12);
            if (code >= lo && code <= hi) {
                if (method == 0) return (int)LD16(pos + 0x14) + (code - lo); /* direct */
                if (method == 1) {
                    uint16_t g = (uint16_t)LD16(pos + 0x14 + 2 * (code - lo));
                    return g == 0xFFFF ? -1 : g;
                }
                if (method == 2) { /* scan */
                    int n = (int)LD16(pos + 0x14), i;
                    for (i = 0; i < n; i++) {
                        if (LD16(pos + 0x16 + 4 * i) == code) return (int)LD16(pos + 0x18 + 4 * i);
                    }
                }
            }
        }
        if (blockSize < 8) break;
        pos += blockSize;
    }
    return -1;
}

/* A glyph's widths (CWDH): where its pixels start, how many columns of its
 * cell are drawn, and how far the pen moves; 0 if it has none. */
static int glyph_widths(const Font* f, int glyph, int* left, int* width, int* advance) {
    u32 size = LD32(f->font + 8), pos;
    for (pos = f->font + LD16(f->font + 0xC); pos + 8 <= f->font + size; pos += LD32(pos + 4)) {
        if (LD32(pos + 4) < 8) break;
        if (LD32(pos) == 0x43574448) { /* CWDH */
            int first = (int)LD16(pos + 8), last = (int)LD16(pos + 10);
            if (glyph >= first && glyph <= last) {
                u32 e = pos + 0x10 + 3 * (u32)(glyph - first);
                *left = (int8_t)LD8(e);
                *width = (int)LD8(e + 1);
                *advance = (int)LD8(e + 2);
                return 1;
            }
        }
    }
    return 0;
}

static void draw_icons(Font* f) {
    static uint8_t cov[MAX_CELL];
    int i, x, y;
    for (i = 0; i < ICONS; i++) {
        int sheet, x0, y0, s, ox, oy, drew = 0;
        if (f->glyph[i] < 0) continue;
        cell_of(f, f->glyph[i], &sheet, &x0, &y0);
        if (!f->original[i]) {
            f->original[i] = (uint8_t*)malloc((size_t)f->cw * f->ch);
            if (!f->original[i]) continue;
            for (y = 0; y < f->ch; y++) {
                for (x = 0; x < f->cw; x++) f->original[i][y * f->cw + x] = (uint8_t)get_px(f, sheet, x0 + x, y0 + y);
            }
        }
        /* Square, up to the baseline, as the Wii's glyphs sit, and within
         * the part of the cell that's drawn (its glyph width). */
        s = f->baseline < f->width[i] ? f->baseline : f->width[i];
        s -= 2;
        ox = (f->width[i] - s) / 2;
        oy = f->baseline - s - 1;
        if (oy < 0) oy = 0;
        if (s > 0 && s * s <= MAX_CELL && g_host_prompt_icon) drew = g_host_prompt_icon(kButton[i], cov, s, s);
        for (y = 0; y < f->ch; y++) {
            for (x = 0; x < f->cw; x++) {
                int v = f->original[i][y * f->cw + x];
                if (drew) {
                    int ix = x - ox, iy = y - oy;
                    v = ix >= 0 && iy >= 0 && ix < s && iy < s ? (cov[iy * s + ix] * 15 + 127) / 255 : 0;
                }
                set_px(f, sheet, x0 + x, y0 + y, v);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Pictures in layouts' textures (TPL) with a button drawn on them. */

#define MAX_COLOURS 256
enum { TPL_BUBBLE, TPL_TITLE };

typedef struct {
    int w, h, colours;          /* the texture: CI8, this many RGB5A3 colours */
    int cx, cy, r;              /* the disc inside the ring: centre and radius */
    int ring;                   /* the ring's outer radius */
    int kx, kw, kh;             /* the box a key is drawn in: its centre's x (cy its y), size */
    uint32_t button, ink, fill; /* 0xRRGGBB */
    int style;                  /* TPL_* */
} TplKind;

static const TplKind kTplKinds[] = {
    {110, 71, 80, 54, 23, 7, 11, 54, 22, 16, HOST_PAD_2, 0x505050, 0xE8E8E8, TPL_BUBBLE}, /* ActionWin.tpl */
    {290, 34, 64, 258, 17, 13, 17, 265, 42, 26, HOST_PAD_2, 0x383838, 0, TPL_TITLE},       /* StartButtonTwo.tpl */
};
#define TPL_KINDS ((int)(sizeof(kTplKinds) / sizeof(kTplKinds[0])))
#define MAX_TPLS 4

typedef struct {
    const TplKind* kind;
    u32 tpl, header, image;
    uint8_t rgba[MAX_COLOURS][4];
    uint8_t* original;
} TplPicture;

static TplPicture sTpls[MAX_TPLS];
static int sTplCount;

/* A texel of an 8-bit (CI8) texture w wide: 8x4 tiles of 32 bytes. */
static u32 ci8_byte(u32 image, int w, int x, int y) {
    return image + (u32)(((y / 4) * ((w + 7) / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8);
}

static int tpl_nearest(const TplPicture* t, int r, int g, int b) {
    int i, best = 0;
    long bestD = -1;
    for (i = 0; i < t->kind->colours; i++) {
        const uint8_t* p = t->rgba[i];
        long dr = p[0] - r, dg = p[1] - g, db = p[2] - b, d;
        if (p[3] < 250) continue; /* opaque ones only */
        d = dr * dr + dg * dg + db * db;
        if (bestD < 0 || d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

/* The colour nearest r, g, b at alpha a, of all of them. */
static int tpl_nearest_alpha(const TplPicture* t, int r, int g, int b, int a) {
    int i, best = 0;
    long bestD = -1;
    for (i = 0; i < t->kind->colours; i++) {
        const uint8_t* p = t->rgba[i];
        long dr = p[0] - r, dg = p[1] - g, db = p[2] - b, da = p[3] - a, d;
        d = (dr * dr + dg * dg + db * db) * (p[3] + a) / 510 + 4 * da * da;
        if (bestD < 0 || d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

/* Whether the picture is still where it was (its memory may have been
 * freed and reused since). */
static int tpl_here(const TplPicture* t) {
    return LD32(t->tpl) == 0x0020AF30 && LD32(t->tpl + 4) == 1 && LD32(LD32(t->tpl + 8)) == t->header &&
           LD32(t->header + 8) == t->image;
}

/* The title's: a pad's button in the disc, drawn over its shading; a key,
 * the disc and the box wiped, the key white with a grey edge. */
static void draw_title_tpl(TplPicture* t, int key, const uint8_t* cov, int bw, int bh) {
    const TplKind* k = t->kind;
    int x0 = (key ? k->kx : k->cx) - bw / 2, y0 = k->cy - bh / 2, x, y;
    int left = k->cx - k->ring - 1, right = k->cx + k->ring, top = k->cy - k->ring - 1, bottom = k->cy + k->ring;
    int ir = (int)(k->ink >> 16 & 255), ig = (int)(k->ink >> 8 & 255), ib = (int)(k->ink & 255);
    if (key) {
        if (x0 < left) left = x0;
        if (x0 + bw > right) right = x0 + bw;
        if (y0 < top) top = y0;
        if (y0 + bh > bottom) bottom = y0 + bh;
    }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= k->w) right = k->w - 1;
    if (bottom >= k->h) bottom = k->h - 1;
    for (y = top; y <= bottom; y++) {
        for (x = left; x <= right; x++) {
            int ix = x - x0, iy = y - y0, dx = 2 * x + 1 - 2 * k->cx, dy = 2 * y + 1 - 2 * k->cy;
            int a = ix >= 0 && iy >= 0 && ix < bw && iy < bh ? cov[iy * bw + ix] : 0;
            uint8_t px = t->original[y * k->w + x];
            if (key) {
                /* Thin lines thickened to the words' strokes: white within
                 * a pixel of them, the grey edge within three. */
                int edge = 0, ex, ey, r, g, b;
                a = 0;
                for (ey = iy - 3; ey <= iy + 3; ey++) {
                    for (ex = ix - 3; ex <= ix + 3; ex++) {
                        int d2 = (ex - ix) * (ex - ix) + (ey - iy) * (ey - iy), v;
                        if (ex < 0 || ey < 0 || ex >= bw || ey >= bh || d2 > 9) continue;
                        v = cov[ey * bw + ex] * 2 > 255 ? 255 : cov[ey * bw + ex] * 2;
                        if (v > edge) edge = v;
                        if (d2 <= 1 && v > a) a = v;
                    }
                }
                r = g = b = (0x66 * (255 - a) + 255 * a) / 255;
                px = (uint8_t)tpl_nearest_alpha(t, r, g, b, edge);
            } else if (dx * dx + dy * dy <= 4 * k->r * k->r) {
                /* The 2 (in the disc's middle rows): darker than the
                 * shading beside it on its row. */
                const uint8_t *o = t->rgba[px], *s = t->rgba[t->original[y * k->w + k->cx - 7]];
                if (dy * dy <= 4 * (k->r - 3) * (k->r - 3) && o[0] + 12 < s[0]) o = s;
                px = (uint8_t)tpl_nearest(t, (o[0] * (255 - a) + ir * a) / 255, (o[1] * (255 - a) + ig * a) / 255,
                                          (o[2] * (255 - a) + ib * a) / 255);
            }
            ST8(ci8_byte(t->image, k->w, x, y), px);
        }
    }
}

static void draw_tpl(TplPicture* t) {
    enum { MAX_S = 48 };
    static uint8_t cov[MAX_S * MAX_S];
    const TplKind* k = t->kind;
    /* A key (the generation's low bit clear) is drawn as itself, on the
     * bubble's white with the ring wiped: keys like Space are wide. A pad's
     * button stays in the disc, as the game draws its 2. */
    int key = !(sGeneration & 1), R = key ? k->ring : k->r;
    int bw = key ? k->kw : 2 * k->r, bh = key ? k->kh : 2 * k->r;
    int x0 = k->cx - bw / 2, y0 = k->cy - bh / 2, x, y, drew = 0;
    uint32_t bg = key ? 0xFFFFFFu : k->fill;
    int fr = (int)(bg >> 16 & 255), fg = (int)(bg >> 8 & 255), fb = (int)(bg & 255);
    int ir = (int)(k->ink >> 16 & 255), ig = (int)(k->ink >> 8 & 255), ib = (int)(k->ink & 255);
    if (bw > MAX_S || bh > MAX_S) return;
    if (g_host_prompt_icon) drew = g_host_prompt_icon(k->button, cov, bw, bh);
    if (k->style == TPL_TITLE) {
        if (drew) {
            draw_title_tpl(t, key, cov, bw, bh);
        } else { /* the game's 2 back */
            for (y = 0; y < k->h; y++)
                for (x = 0; x < k->w; x++) ST8(ci8_byte(t->image, k->w, x, y), t->original[y * k->w + x]);
        }
        return;
    }
    for (y = k->cy - k->ring - 1; y <= k->cy + k->ring; y++) {
        for (x = k->cx - k->ring - 1; x <= k->cx + k->ring; x++) {
            int dx = 2 * x + 1 - 2 * k->cx, dy = 2 * y + 1 - 2 * k->cy;
            uint8_t px = t->original[y * k->w + x];
            if (drew && dx * dx + dy * dy <= 4 * R * R) { /* in the disc, or the ring for a key */
                int ix = x - x0, iy = y - y0, a = ix >= 0 && iy >= 0 && ix < bw && iy < bh ? cov[iy * bw + ix] : 0;
                px = (uint8_t)tpl_nearest(t, (fr * (255 - a) + ir * a) / 255, (fg * (255 - a) + ig * a) / 255,
                                          (fb * (255 - a) + ib * a) / 255);
            }
            ST8(ci8_byte(t->image, k->w, x, y), px);
        }
    }
}

static void draw_tpls(void) {
    int i;
    for (i = 0; i < sTplCount; i++) {
        if (!tpl_here(&sTpls[i])) {
            free(sTpls[i].original);
            sTpls[i--] = sTpls[--sTplCount];
            continue;
        }
        draw_tpl(&sTpls[i]);
    }
}

/* void TPLBind(TPLPalette*): a TPL, its offsets made pointers. */
WP_HOOK(0x8031E2A0, TPLBind) {
    u32 tpl = c->gpr[3], header, pal, data;
    const TplKind* k = NULL;
    TplPicture* t;
    int i, x, y;
    wp_call_original(c, 0x8031E2A0u);
    if (!tpl || LD32(tpl) != 0x0020AF30 || LD32(tpl + 4) != 1) return 1;
    header = LD32(LD32(tpl + 8));
    pal = LD32(LD32(tpl + 8) + 4);
    if (!header || !pal || LD32(header + 4) != 9 || LD32(pal + 4) != 2) return 1; /* CI8, RGB5A3 */
    for (i = 0; i < TPL_KINDS; i++) {
        if ((int)LD16(header + 2) == kTplKinds[i].w && (int)LD16(header) == kTplKinds[i].h &&
            (int)LD16(pal) == kTplKinds[i].colours)
            k = &kTplKinds[i];
    }
    if (!k) return 1;
    for (i = 0; i < sTplCount && sTpls[i].tpl != tpl; i++) {}
    if (i == sTplCount) {
        if (sTplCount == MAX_TPLS) return 1;
        t = &sTpls[sTplCount];
        if (!(t->original = (uint8_t*)malloc((size_t)k->w * k->h))) return 1;
        sTplCount++;
    }
    t = &sTpls[i];
    t->kind = k;
    t->tpl = tpl;
    t->header = header;
    t->image = LD32(header + 8);
    data = LD32(pal + 8);
    for (x = 0; x < k->colours; x++) {
        uint16_t v = (uint16_t)LD16(data + 2 * x);
        uint8_t* p = t->rgba[x];
        if (v & 0x8000) {
            p[0] = (uint8_t)((v >> 10 & 31) * 255 / 31);
            p[1] = (uint8_t)((v >> 5 & 31) * 255 / 31);
            p[2] = (uint8_t)((v & 31) * 255 / 31);
            p[3] = 255;
        } else {
            p[0] = (uint8_t)((v >> 8 & 15) * 17);
            p[1] = (uint8_t)((v >> 4 & 15) * 17);
            p[2] = (uint8_t)((v & 15) * 17);
            p[3] = (uint8_t)((v >> 12 & 7) * 255 / 7);
        }
    }
    for (y = 0; y < k->h; y++) {
        for (x = 0; x < k->w; x++) t->original[y * k->w + x] = (uint8_t)LD8(ci8_byte(t->image, k->w, x, y));
    }
    WP_LOG("prompts", "layout texture %08X (%dx%d)", tpl, k->w, k->h);
    sGeneration = 0xFFFFFFFFu; /* draw it */
    return 1;
}

/* Redraws the glyphs and the pictures if what they should show has changed.
 * Called on each read of the remote (hle_wpad.c). */
void hle_prompts_update(void) {
    uint32_t gen;
    int i;
    if ((!sFontCount && !sTplCount) || !g_host_prompt_generation) return;
    gen = g_host_prompt_generation();
    if (gen == sGeneration) return;
    sGeneration = gen;
    for (i = 0; i < sFontCount; i++) draw_icons(&sFonts[i]);
    draw_tpls();
    WP_LOG("prompts", "redrawn (%s)", gen & 1 ? "pad" : "keys");
}

/* void ResFontBase::SetResourceBuffer(void* font, FontInformation*): a
 * font, loaded and its offsets made pointers. */
WP_HOOK(0x80277510, ResFontBase_SetResourceBuffer) {
    u32 base = c->gpr[4], size, pos, image;
    Font* f;
    int i, have = 0;
    wp_call_original(c, 0x80277510u);
    /* RFNT, or RFNU once unpacked (its offsets made pointers). */
    if (!base || (LD32(base) != 0x52464E54 && LD32(base) != 0x52464E55)) return 1;
    for (i = 0; i < sFontCount; i++) {
        if (sFonts[i].font == base) return 1;
    }
    if (sFontCount == MAX_FONTS) return 1;
    size = LD32(base + 8);
    for (pos = base + LD16(base + 0xC); pos + 8 <= base + size && LD32(pos) != 0x54474C50; pos += LD32(pos + 4)) { /* TGLP */
        if (LD32(pos + 4) < 8) return 1;
    }
    if (pos + 0x20 > base + size) return 1;
    f = &sFonts[sFontCount];
    memset(f, 0, sizeof(*f));
    f->font = base;
    f->cw = (int)LD8(pos + 8);
    f->ch = (int)LD8(pos + 9);
    f->baseline = (int8_t)LD8(pos + 10);
    f->sheetSize = (int)LD32(pos + 12);
    f->fmt = (int)LD16(pos + 18);
    f->perRow = (int)LD16(pos + 20);
    f->perSheet = (int)(LD16(pos + 20) * LD16(pos + 22));
    f->sheetWidth = (int)LD16(pos + 24);
    image = LD32(pos + 28);
    f->sheets = image >= 0x80000000u ? image : base + image;
    if (f->fmt != 0 || f->cw * f->ch > MAX_CELL || !f->perSheet) return 1; /* I4 only */
    for (i = 0; i < ICONS; i++) {
        int left, width, advance;
        f->glyph[i] = glyph_of(base, size, (uint16_t)(FIRST_ICON + i));
        f->width[i] = f->cw;
        if (f->glyph[i] >= 0) {
            have = 1;
            if (glyph_widths(f, f->glyph[i], &left, &width, &advance) && width > 0 && width < f->cw) f->width[i] = width;
        }
    }
    if (!have) return 1;
    sFontCount++;
    WP_LOG("prompts", "font %08X, cells %dx%d, button glyphs from %d", base, f->cw, f->ch, f->glyph[0]);
    sGeneration = 0xFFFFFFFFu; /* draw them */
    return 1;
}
