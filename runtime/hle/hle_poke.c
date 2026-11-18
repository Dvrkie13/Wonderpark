/* Developer tools for test runs (each frame on the game thread, from
 * hle_frame.c; times are seconds after program start, as
 * WONDERPARK_PAD_SCRIPT's):
 *
 * WONDERPARK_POKE="<seconds>:<address hex>:<float>,<float>...;..."
 *   writes the floats into guest memory, one after another from the
 *   address, every frame for half a second from that time.
 *
 * WONDERPARK_MOVE="<seconds>:<x>,<y>,<z>:<x>,<y>,<z>[,<facing>];<seconds>::<x>,<y>,<z>[,<facing>];..."
 *   moves an object: the first entry finds the position record nearest the
 *   first point (within 20 units: the player's spawn point, say) and puts
 *   it at the second; entries with no first point move the same object
 *   again. Position records (found 2026-11-17): 0x4C bytes, an owner
 *   pointer, 0, the words 0x80446070 0x80446090, the position (x, y, z
 *   floats), 0, a y rotation, 0, the scale, then the previous position at
 *   +0x24 from the first. They are in a heap pool (0x80BDB000 or so in Cove
 *   Town), and which object has which record changes from run to run.
 *   The move is written for half a second, both positions. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

double wp_wall_seconds(void);

#define POKES 32
#define POKE_FLOATS 12
#define SCAN_LO 0x80800000u
#define SCAN_HI 0x81800000u

static struct {
    double t;
    u32 addr;
    int n;
    float v[POKE_FLOATS];
} sPoke[POKES];
static int sPokes = -1;

#define MOVE_COPIES 64

static struct {
    double t;
    int find;
    float from[3], to[4];
    int turn; /* to[3]: the facing, in degrees */
    int copies_found, ncopies;
    u32 copy[MOVE_COPIES];
    float dy[MOVE_COPIES];
} sMove[POKES];
static int sMoves = -1;
static u32 sMoved; /* the record being moved */

static float ldf(u32 a) {
    union { float f; u32 u; } x;
    x.u = LD32(a);
    return x.f;
}

static void stf(u32 a, float v) {
    union { float f; u32 u; } x;
    x.f = v;
    ST32(a, x.u);
}

static const char* floats(const char* e, float* v, int n, int* got) {
    char* end;
    *got = 0;
    while (*got < n) {
        float f = strtof(e, &end);
        if (end == e) break;
        v[(*got)++] = f;
        e = end;
        if (*e != ',') break;
        e++;
    }
    return e;
}

static void parse(void) {
    const char* e = getenv("WONDERPARK_POKE");
    char* end;
    int got;
    sPokes = 0;
    while (e && *e && sPokes < POKES) {
        sPoke[sPokes].t = strtod(e, &end);
        if (*end != ':') break;
        sPoke[sPokes].addr = (u32)strtoul(end + 1, &end, 16);
        if (*end != ':') break;
        e = floats(end + 1, sPoke[sPokes].v, POKE_FLOATS, &got);
        sPoke[sPokes++].n = got;
        if (*e != ';') break;
        e++;
    }
    e = getenv("WONDERPARK_MOVE");
    sMoves = 0;
    while (e && *e && sMoves < POKES) {
        sMove[sMoves].t = strtod(e, &end);
        if (*end != ':') break;
        e = floats(end + 1, sMove[sMoves].from, 3, &got);
        sMove[sMoves].find = got == 3;
        if (*e != ':') break;
        e = floats(e + 1, sMove[sMoves].to, 4, &got);
        if (got < 3) break;
        sMove[sMoves].turn = got == 4;
        sMoves++;
        if (*e != ';') break;
        e++;
    }
}

/* The position record nearest p, within 20 units; 0 if none. */
static u32 sNearest; /* for the log: the nearest record at all */
static int sRecords;

static u32 find_record(const float* p) {
    u32 a, best = 0;
    float bestd = 1e30f, nearest = 1e30f;
    sRecords = 0;
    for (a = SCAN_LO; a < SCAN_HI; a += 4) {
        float dx, dy, dz, d;
        if (LD32(a) != 0x80446070u || LD32(a + 4) != 0x80446090u) continue;
        sRecords++;
        dx = ldf(a + 8) - p[0], dy = ldf(a + 12) - p[1], dz = ldf(a + 16) - p[2];
        d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < nearest) nearest = d, sNearest = a + 8;
        /* In Cove Town the player's partners stand (hidden) on the
         * player's spot, and the player's own record has no owner pointer
         * (0 at -8 from the words): one of those wins over any other
         * within 20. (At Seasong Beach the player's record has one.) */
        if (d >= 20.0f) continue;
        if (LD32(a - 8) != 0) d += 20.0f;
        if (d < bestd) bestd = d, best = a + 8;
    }
    return bestd < 40.0f ? best : 0; /* within 20 (an owned one: 20 added) */
}

void hle_poke_tick(void) {
    double now;
    int i, k;
    if (sPokes < 0) parse();
    if (!sPokes && !sMoves) return;
    now = wp_wall_seconds();
    for (i = 0; i < sPokes; i++) {
        if (now < sPoke[i].t || now > sPoke[i].t + 0.5) continue;
        if (!wp_mem_ptr(sPoke[i].addr, 4u * (u32)sPoke[i].n)) continue;
        for (k = 0; k < sPoke[i].n; k++) stf(sPoke[i].addr + 4u * (u32)k, sPoke[i].v[k]);
    }
    for (i = 0; i < sMoves; i++) {
        if (now < sMove[i].t) continue;
        if (sMove[i].find) {
            /* Looked for every frame (up to 30 s) until it's there: the
             * time it takes to get into the area varies. The move's half
             * second counts from when it's found. */
            u32 r = find_record(sMove[i].from);
            if (!r && wp_log_on("poke")) {
                static double logged;
                if (now - logged > 1) {
                    logged = now;
                    WP_LOG("poke", "%.1f s: nothing within 20 of (%.1f, %.1f, %.1f) yet (%d records; nearest %08X at %.1f, %.1f, %.1f)",
                           now, sMove[i].from[0], sMove[i].from[1], sMove[i].from[2], sRecords, sNearest,
                           sNearest ? ldf(sNearest) : 0, sNearest ? ldf(sNearest + 4) : 0, sNearest ? ldf(sNearest + 8) : 0);
                }
            }
            if (!r) {
                if (now > sMove[i].t + 30) sMove[i].find = 0;
                continue;
            }
            WP_LOG("poke", "moving the record at %08X, %.1f s", r, now);
            sMoved = r;
            sMove[i].find = 0;
            sMove[i].t = now;
        }
        if (now > sMove[i].t + 0.5) continue;
        if (!sMoved || !wp_mem_ptr(sMoved, 0x30)) continue;
        if (!sMove[i].copies_found) {
            /* The game keeps other copies of the position (physics,
             * camera target), some with another height: every x, z pair
             * in MEM1 equal to the record's (any y within 30 of it) is
             * moved too, keeping its height above the record's. Without
             * them the object slid back from its old place into walls. */
            u32 a;
            float x = ldf(sMoved), y = ldf(sMoved + 4), z = ldf(sMoved + 8);
            sMove[i].ncopies = 0;
            for (a = 0x80000000u; a + 12 < 0x81800000u && sMove[i].ncopies < MOVE_COPIES; a += 4) {
                if (a == sMoved || ldf(a) != x || ldf(a + 8) != z) continue;
                if (!(fabsf(ldf(a + 4) - y) < 30.0f)) continue;
                sMove[i].copy[sMove[i].ncopies] = a;
                sMove[i].dy[sMove[i].ncopies++] = ldf(a + 4) - y;
            }
            sMove[i].copies_found = 1;
            WP_LOG("poke", "%d other copies of the position", sMove[i].ncopies);
        }
        for (k = 0; k < 3; k++) {
            stf(sMoved + 4u * (u32)k, sMove[i].to[k]);
            stf(sMoved + 0x24 + 4u * (u32)k, sMove[i].to[k]);
        }
        if (sMove[i].turn) {
            /* The y rotation, degrees: atan2(dx, dz) of the way it faces. */
            stf(sMoved + 0x10, sMove[i].to[3]);
            stf(sMoved + 0x34, sMove[i].to[3]);
        }
        for (k = 0; k < sMove[i].ncopies; k++) {
            stf(sMove[i].copy[k], sMove[i].to[0]);
            stf(sMove[i].copy[k] + 4, sMove[i].to[1] + sMove[i].dy[k]);
            stf(sMove[i].copy[k] + 8, sMove[i].to[2]);
        }
    }
}
