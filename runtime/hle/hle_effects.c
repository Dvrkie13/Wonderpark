/* The Bloom setting (g_x[X_BLOOM]; the F1 tab's Graphics page).
 *
 * The known Gecko code that turns bloom off (0411D91C 60000000) replaces
 * the instruction at 0x8011D91C, "stfs f1, 1076(r30)", with a nop: the
 * effect's strength is never stored, so it stays at the value its object
 * was made with. This hook does the same while the setting is off: the
 * store is skipped (on to 0x8011D920, as after the nop). So that turning
 * it off in the middle of a game works too (the Gecko code is on from the
 * start), the value the object held before its first store here is kept,
 * and while off that is written back instead of the new one. */
#include "hle.h"
#include "host_settings.h"

#define BLOOM_STORE 0x8011D91Cu
#define BLOOM_FIELD 1076u

#define BLOOM_OBJECTS 8

WP_HOOK(0x8011D91C, BloomStrength) {
    static uint32_t obj[BLOOM_OBJECTS], base[BLOOM_OBJECTS];
    static unsigned next;
    uint32_t o = c->gpr[30];
    unsigned i;
    if (!wp_mem_ptr(o + BLOOM_FIELD, 4)) return 0;
    for (i = 0; i < BLOOM_OBJECTS && obj[i] != o; i++) {}
    if (i == BLOOM_OBJECTS) {
        /* A new object: what it was made with. */
        i = next++ % BLOOM_OBJECTS;
        obj[i] = o;
        base[i] = LD32(o + BLOOM_FIELD);
        WP_LOG("bloom", "object %08X, made with %08X; bloom %s", o, base[i], g_x[X_BLOOM] ? "on" : "off");
    }
    if (g_x[X_BLOOM]) return 0; /* the game's own store */
    ST32(o + BLOOM_FIELD, base[i]);
    c->pc = BLOOM_STORE + 4;
    return 1;
}
