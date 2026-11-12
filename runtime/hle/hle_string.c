/* Native string compare and the resource-by-name lookups built on it.
 *
 * When a scene loads (the Cake Contraption's character select, for one)
 * the game finds each of its resources by name in a table, a straight
 * walk with strcmp on every entry. Recompiled, one burst of these took
 * 650 ms of a single frame; sound is mixed on the game thread, so that
 * was the crackle/gap the player heard. Done natively it is a few ms.
 *
 * A hook whose strings or table aren't wholly in RAM returns 0 and the
 * original code runs. WONDERPARK_NO_NATIVE_STRING=1 turns them all off
 * (comparisons). */
#include <stdlib.h>

#include "hle.h"

static int off(void) {
    static int v = -1;
    if (v < 0) v = getenv("WONDERPARK_NO_NATIVE_STRING") != NULL;
    return v;
}

/* Host pointer to a guest string and how many bytes may be read there
 * (to the end of its RAM block), or NULL. */
static const uint8_t* str_at(u32 a, u32* room) {
    u32 p = a & 0x3FFFFFFFu;
    if (p < g_cpu->ram_size) { *room = g_cpu->ram_size - p; return g_cpu->ram + p; }
    p -= 0x10000000u;
    if (p < g_cpu->mem2_size) { *room = g_cpu->mem2_size - p; return g_cpu->mem2 + p; }
    return NULL;
}

/* As the game's strcmp: the difference of the first unequal bytes (as
 * unsigned), 0 if equal. Returns 0 in *ok if a string runs off RAM. */
static int cmp(u32 a, u32 b, int* ok) {
    u32 ra, rb, i, n;
    const uint8_t *s = str_at(a, &ra), *t = str_at(b, &rb);
    *ok = 0;
    if (!s || !t) return 0;
    n = ra < rb ? ra : rb;
    for (i = 0; i < n; i++) {
        int d = (int)s[i] - (int)t[i];
        if (d || !s[i]) { *ok = 1; return d; }
    }
    return 0;
}

/* int strcmp(const char* a, const char* b) */
WP_HOOK(0x80405E10, strcmp) {
    int ok, d;
    if (off()) return 0;
    d = cmp(c->gpr[3], c->gpr[4], &ok);
    if (!ok) return 0;
    c->gpr[3] = (u32)d;
    wp_return(c);
    return 1;
}

/* void* find(table* t, const char* name): the table starts with the offset
 * of its directory; the directory has a u16 count at +4 and entries from
 * +8, each a u16 name length, the name, a u32 offset (from the directory)
 * and 4 more bytes. Returns the directory plus the offset of the first
 * entry named name, or NULL. The game has two copies of it. */
static int find_by_name(CPUState* c) {
    u32 dir, n, i, at;
    const uint8_t* h;
    if (off() || !(h = wp_ram(c->gpr[3], 4))) return 0;
    dir = c->gpr[3] + ((u32)h[0] << 24 | (u32)h[1] << 16 | (u32)h[2] << 8 | h[3]);
    if (!(h = wp_ram(dir, 8))) return 0;
    n = (u32)h[4] << 8 | h[5];
    at = dir + 8;
    for (i = 0; i < n; i++) {
        u32 len, name, ofs;
        int ok, d;
        if (!(h = wp_ram(at, 2))) return 0;
        len = (u32)h[0] << 8 | h[1];
        name = at + 2;
        at = name + len;
        if (!(h = wp_ram(at, 4))) return 0;
        ofs = (u32)h[0] << 24 | (u32)h[1] << 16 | (u32)h[2] << 8 | h[3];
        at += 8;
        d = cmp(c->gpr[4], name, &ok);
        if (!ok) return 0;
        if (d == 0) {
            c->gpr[3] = dir + ofs;
            wp_return(c);
            return 1;
        }
    }
    c->gpr[3] = 0;
    wp_return(c);
    return 1;
}

WP_HOOK(0x802330F0, find_by_name_a) { return find_by_name(c); }
WP_HOOK(0x80233380, find_by_name_b) { return find_by_name(c); }
