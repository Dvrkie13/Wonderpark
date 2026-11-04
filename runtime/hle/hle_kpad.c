/* KPAD (above WPAD). Debugging aid for now: WONDERPARK_LOG=kpad logs what
 * KPADRead hands the game. KPADStatus starts with u32 hold, trig, release. */
#include "hle.h"

/* s32 KPADRead(s32 chan, KPADStatus* samples, u32 length) (S2LE01, found as
 * the KPAD-region function the game's input code calls; size 0x818). */
WP_HOOK(0x802EAB70, KPADRead) {
    u32 chan, buf, len, ret;
    if (!wp_log_on("kpad")) return 0;
    chan = c->gpr[3];
    buf = c->gpr[4];
    len = c->gpr[5];
    wp_call_original(c, 0x802EAB70u);
    ret = c->gpr[3];
    {
        static u32 n, last_hold = 0xFFFFFFFFu;
        u32 hold = (int32_t)ret > 0 ? LD32(buf) : 0, trig = (int32_t)ret > 0 ? LD32(buf + 4) : 0;
        if (n++ < 5 || hold != last_hold || trig)
            WP_LOG("kpad", "KPADRead(chan %u, len %u) = %d, hold %08X trig %08X (lr %08X)", chan, len, (int)ret, hold, trig, c->lr);
        last_hold = hold;
    }
    return 1; /* wp_call_original already returned to the caller */
}
