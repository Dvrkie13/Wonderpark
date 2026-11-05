/* Debugging hooks, active only with their log category. */
#include <stdlib.h>

#include "hle.h"

/* memcpy: WONDERPARK_LOG=memcpywatch with WONDERPARK_WATCH=lo-hi logs copies
 * into that physical range (source, length, caller). */
WP_HOOK(0x800043C4, memcpy_watch) {
    static int on = -1;
    static unsigned lo, hi, n;
    if (on < 0) {
        const char* e = getenv("WONDERPARK_WATCH");
        on = wp_log_on("memcpywatch") && e && sscanf(e, "%x-%x", &lo, &hi) == 2;
    }
    if (on) {
        u32 d = c->gpr[3] & 0x3FFFFFFFu;
        if (d < hi && d + c->gpr[5] > lo && n++ < 40) {
            u32 dst = c->gpr[3], src = c->gpr[4], len = c->gpr[5], i, bad = 0, first = 0;
            u32 lr = c->lr;
            wp_call_original(c, 0x800043C4u);
            for (i = 0; i < len; i++)
                if (LD8(dst + i) != LD8(src + i)) { if (!bad++) first = i; }
            WP_LOG("memcpywatch", "memcpy(%08X, %08X, %X) from %08X: %u bytes differ (first at +%X: %02X vs %02X)", dst, src,
                   len, lr, bad, first, bad ? LD8(dst + first) : 0, bad ? LD8(src + first) : 0);
            return 1;
        }
    }
    return 0;
}

/* CRI's ADX decoder (S2LE01 0x803BCB4C): decode(state r3, ?, in r5, ?, out_used r7,
 * ?, ?, ?, chan_ptrs r9, samples r10). WONDERPARK_LOG=adxcheck runs it, then
 * decodes the same input in C and reports the first difference. state: u8
 * channels; u16 key xor/mult/inc at +2/+4/+6; s16 coef1/coef2 at +8/+10;
 * s16 hist1/hist2 per channel at +12 + 4 ch. */
WP_HOOK(0x803BCB4C, adx_decode_check) {
    static unsigned calls;
    u32 st = c->gpr[3], in = c->gpr[5], tab = c->gpr[9], n = c->gpr[10], nch, frames, ch, f, i;
    s16 hist[8][2], c1, c2;
    u16 x, mult, inc;
    if (!wp_log_on("adxcheck") || calls++ > 200) return 0;
    nch = LD8(st);
    if (nch < 1 || nch > 8) return 0;
    x = (u16)LD16(st + 2); mult = (u16)LD16(st + 4); inc = (u16)LD16(st + 6);
    c1 = (s16)LD16(st + 8); c2 = (s16)LD16(st + 10);
    for (ch = 0; ch < nch; ch++) { hist[ch][0] = (s16)LD16(st + 12 + 4 * ch); hist[ch][1] = (s16)LD16(st + 14 + 4 * ch); }
    frames = n >> 5;
    /* As the code does: also capped by the input bytes (r6) and samples wanted (r4). */
    if (c->gpr[6] / (18 * nch) < frames) frames = c->gpr[6] / (18 * nch);
    if ((c->gpr[4] + 31) >> 5 < frames) frames = (c->gpr[4] + 31) >> 5;
    {
        static s16 want[8][4096];
        static u8 inb[8 * 18 * 128];
        u32 inlen = frames * 18 * nch, outp[8], bad = 0, first = 0, firstch = 0;
        if (frames > 128) frames = 128, inlen = frames * 18 * nch;
        for (i = 0; i < inlen; i++) inb[i] = (u8)LD8(in + i);
        if (calls == 40) { FILE* df = fopen("build/adx_in.bin", "wb"); if (df) { fwrite(inb, 1, inlen, df); fclose(df); } WP_LOG("adxcheck", "saved input of call 40 (%u bytes from %08X)", inlen, in); }
        for (ch = 0; ch < nch; ch++) outp[ch] = LD32(tab + 4 * ch);
        {
            /* Determinism: run the recompiled decoder twice from the same state. */
            static u8 st0[64], first_out[8][1024 * 2];
            CPUState saved = *c;
            u32 k, diff = 0;
            for (k = 0; k < 12 + 4 * nch; k++) st0[k] = (u8)LD8(st + k);
            wp_call_original(c, 0x803BCB4Cu);
            for (ch = 0; ch < nch; ch++) for (k = 0; k < 2 * n && k < sizeof(first_out[0]); k++) first_out[ch][k] = (u8)LD8(outp[ch] + k);
            for (k = 0; k < 12 + 4 * nch; k++) ST8(st + k, st0[k]);
            *c = saved;
            wp_call_original(c, 0x803BCB4Cu);
            for (ch = 0; ch < nch; ch++) for (k = 0; k < 2 * n && k < sizeof(first_out[0]); k++) diff += first_out[ch][k] != (u8)LD8(outp[ch] + k);
            if (diff) WP_LOG("adxcheck", "  NONDETERMINISTIC: the two runs differ in %u bytes", diff);
        }
        for (f = 0; f < frames; f++) {
            for (ch = 0; ch < nch; ch++) {
                const u8* fr = inb + (f * nch + ch) * 18;
                u16 raw = (u16)(fr[0] << 8 | fr[1]);
                s32 scale;
                if (raw & 0x8000) goto done;
                scale = ((raw ^ x) & 0x1FFF) + 1;
                if (ch == nch - 1 || 1) { /* the key steps once per frame per channel, as the code does */ }
                for (i = 0; i < 32; i++) {
                    int nib = (i & 1) ? (fr[2 + i / 2] & 15) : (fr[2 + i / 2] >> 4);
                    s32 s;
                    if (nib >= 8) nib -= 16;
                    s = nib * scale + ((c1 * hist[ch][0] + c2 * hist[ch][1]) >> 12);
                    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
                    hist[ch][1] = hist[ch][0];
                    hist[ch][0] = (s16)s;
                    want[ch][f * 32 + i] = (s16)s;
                }
                x = (u16)((x * mult + inc) & 0x7FFF);
            }
        }
    done:
        for (ch = 0; ch < nch; ch++)
            for (i = 0; i < f * 32; i++)
                if ((s16)LD16(outp[ch] + 2 * i) != want[ch][i]) { if (!bad++) { first = i; firstch = ch; } }
        if (bad) {
            u32 k;
            WP_LOG("adxcheck", "  input %08X-%08X (%u bytes), outputs:", in, in + inlen, inlen);
            for (k = 0; k < nch; k++) WP_LOG("adxcheck", "    ch %u %08X-%08X", k, outp[k], outp[k] + 2 * n);
            for (k = 0; k < inlen; k++) if ((u8)LD8(in + k) != inb[k]) { WP_LOG("adxcheck", "  input changed during the decode, first at +%X", k); break; }
        }
        if (bad || calls < 4)
            WP_LOG("adxcheck", "decode %u ch, %u frames: %u samples differ (first ch %u #%u: got %d want %d)", nch, f, bad, firstch,
                   first, bad ? (s16)LD16(outp[firstch] + 2 * first) : 0, bad ? want[firstch][first] : 0);
    }
    return 1;
}

/* WONDERPARK_LOG=mqwatch with WONDERPARK_MQ=<queue>: who posts to that
 * message queue, and when (frame pacing investigations). */
static int mq_on(u32* q) {
    static int on = -1;
    static unsigned mq;
    if (on < 0) {
        const char* e = getenv("WONDERPARK_MQ");
        on = wp_log_on("mqwatch") && e && sscanf(e, "%x", &mq) == 1;
    }
    *q = mq;
    return on;
}
static void mq_note(CPUState* c, const char* what) {
    u32 q;
    if (mq_on(&q) && c->gpr[3] == q) {
        static uint64_t last;
        uint64_t now = wp_host_time();
        WP_LOG("mqwatch", "%s(%08X, msg %08X) from %08X lr-chain %08X, %.1f ms since the last", what, q, c->gpr[4], c->lr,
               LD32(LD32(c->gpr[1]) + 4), (double)(now - last) * 1000.0 / TB_HZ);
        last = now;
    }
}
WP_HOOK(0x80293660, OSSendMessage_watch) { mq_note(c, "OSSendMessage"); return 0; }
WP_HOOK(0x80293810, OSJamMessage_watch) { mq_note(c, "OSJamMessage"); return 0; }
