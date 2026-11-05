/* The AX ucode at a high level: what the DSP does with each frame's command
 * list, and the AI DMA that plays the result.
 *
 * Command list (Wii AX of PokePark 2's SDK; u16 commands, arguments follow;
 * names as Dolphin's AXWii HLE has them):
 *   00 SETUP a32            ramp start values             (ignored)
 *   01 ADD_TO_LR a32        surround feedback             (ignored)
 *   04 PROCESS a32          the voice list: parameter blocks linked by +0x00
 *   0A COMPRESSOR u16 u16 a32                             (ignored)
 *   0B OUTPUT vol a32 a32   surround buffer, then the stereo output buffer
 *   0D WM_OUTPUT a32 x4     Wii Remote speakers           (dropped)
 *   0E END
 *   05-07 MIX_AUXA-C vol a32 a32  the bus's sends out, its effect's output back in
 * Others (08/09 aux upload with LRSC mix, DPL2 output 0C) haven't been seen in this game;
 * they are logged once and skipped.
 *
 * The output buffer is then "played" by the AI DMA (hw_dsp.c), which hands it
 * to the host here. The voice decoder (DSP-ADPCM, PCM16, PCM8, linear
 * resampling, envelope, one-pole low-pass) is the reference runtime's
 * (MildRain479/PokePark-Wiicompiled recomp/patches/hle_audio.c, CC0).
 *
 * WONDERPARK_AUDIO_DUMP=<file.wav> also writes what is played to a WAV file. */
#include <stdlib.h>
#include <string.h>

#include "../core/wp.h"
#include "host_audio.h"

extern void (*g_dsp_ax_frame)(u32 cmdlist, u32 size);
extern void (*g_dsp_ai_buffer)(u32 addr, u32 bytes);
void (*g_host_audio_push)(const int16_t* lr, int frames);

#define FRAME 96 /* samples per AX frame: 3 ms at 32 kHz */
static int32_t s_l[FRAME], s_r[FRAME]; /* the main mix of the frame being run */

enum {
    PB_NEXT = 0x00, PB_SRC_TYPE = 0x08, PB_STATE = 0x10, PB_IS_STREAM = 0x12,
    PB_MIX_L = 0x14, PB_MIX_R = 0x18, PB_MIX_S = 0x34,
    PB_VE_VOLUME = 0x6A, PB_LOOP_FLAG = 0x6E, PB_FORMAT = 0x70,
    PB_LOOP_ADDR = 0x72, PB_END_ADDR = 0x76, PB_CUR_ADDR = 0x7A,
    PB_ADPCM_COEFS = 0x7E, PB_ADPCM_PS = 0xA0, PB_ADPCM_YN1 = 0xA2, PB_ADPCM_YN2 = 0xA4,
    PB_SRC_RATIO = 0xA6, PB_SRC_FRAC = 0xAA, PB_SRC_LAST = 0xAC,
    PB_LOOP_PS = 0xB4, PB_LOOP_YN1 = 0xB6, PB_LOOP_YN2 = 0xB8,
    PB_LPF_ON = 0xBA, PB_LPF_YN1 = 0xBC, PB_LPF_A0 = 0xBE, PB_LPF_B0 = 0xC0,
};
enum { FMT_ADPCM = 0x00, FMT_PCM16 = 0x0A, FMT_PCM8 = 0x19 };

static u32 ld32_pair(u32 a) { return LD16(a) << 16 | LD16(a + 2); }
static void st32_pair(u32 a, u32 v) { ST16(a, v >> 16); ST16(a + 2, v & 0xFFFF); }
static int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* ---- the voice decoder (reference runtime) */

typedef struct {
    u32 pb, cur, end, loop;
    int format, looping, stream, running;
    int32_t ps, yn1, yn2;
} Voice;

static int32_t read_sample(Voice* v) {
    int32_t s;
    if (!v->running) return 0;
    switch (v->format) {
    case FMT_ADPCM: {
        int32_t nibble, c1, c2;
        u8 b;
        if ((v->cur & 15) < 2) v->cur = (v->cur & ~15u) + 2;
        if ((v->cur & 15) == 2) v->ps = LD8((v->cur & ~15u) >> 1);
        b = (u8)LD8(v->cur >> 1);
        nibble = (v->cur & 1) ? (b & 15) : (b >> 4);
        if (nibble >= 8) nibble -= 16;
        c1 = (int16_t)LD16(v->pb + PB_ADPCM_COEFS + (v->ps >> 4 & 7) * 4);
        c2 = (int16_t)LD16(v->pb + PB_ADPCM_COEFS + (v->ps >> 4 & 7) * 4 + 2);
        s = clamp16((c1 * v->yn1 + c2 * v->yn2 + ((nibble << (v->ps & 15)) << 11) + 1024) >> 11);
        v->yn2 = v->yn1;
        v->yn1 = s;
        break;
    }
    case FMT_PCM16: s = (int16_t)LD16(v->cur * 2); break;
    case FMT_PCM8: s = (int8_t)LD8(v->cur) * 256; break;
    default: s = 0; break;
    }
    if (v->cur == v->end) {
        v->cur = v->loop;
        if (!v->looping) {
            v->running = 0;
        } else if (v->format == FMT_ADPCM && !v->stream) {
            v->ps = LD16(v->pb + PB_LOOP_PS);
            v->yn1 = (int16_t)LD16(v->pb + PB_LOOP_YN1);
            v->yn2 = (int16_t)LD16(v->pb + PB_LOOP_YN2);
        }
    } else {
        v->cur++;
    }
    return s;
}

/* Aux buses A-C: each voice's sends (left, right, surround) for this frame,
 * uploaded to the game's effect callback with MIX_AUXn, whose processed
 * output (from the frame before) comes back into the main mix. */
#define AUX_BUSES 3
static int32_t s_aux[AUX_BUSES][3][FRAME];
static int s_aux_used[AUX_BUSES];

static void mix_voice(u32 pb, int32_t* outL, int32_t* outR) {
    Voice v;
    u32 ratio, frac;
    int32_t last0, last1, ve, volL, volR, lpfOn, lpfY, lpfA, lpfB;
    int i;
    v.pb = pb;
    v.cur = ld32_pair(pb + PB_CUR_ADDR);
    v.end = ld32_pair(pb + PB_END_ADDR);
    v.loop = ld32_pair(pb + PB_LOOP_ADDR);
    v.format = (int)LD16(pb + PB_FORMAT);
    v.looping = LD16(pb + PB_LOOP_FLAG) != 0;
    v.stream = LD16(pb + PB_IS_STREAM) != 0;
    v.running = 1;
    v.ps = (int32_t)LD16(pb + PB_ADPCM_PS);
    v.yn1 = (int16_t)LD16(pb + PB_ADPCM_YN1);
    v.yn2 = (int16_t)LD16(pb + PB_ADPCM_YN2);
    ratio = LD16(pb + PB_SRC_TYPE) == 2 ? 0x10000 : ld32_pair(pb + PB_SRC_RATIO);
    frac = LD16(pb + PB_SRC_FRAC);
    last0 = (int16_t)LD16(pb + PB_SRC_LAST);
    last1 = (int16_t)LD16(pb + PB_SRC_LAST + 2);
    ve = (int32_t)LD16(pb + PB_VE_VOLUME);
    /* Surround is folded into both sides at half volume. */
    volL = (int32_t)(LD16(pb + PB_MIX_L) + LD16(pb + PB_MIX_S) / 2);
    volR = (int32_t)(LD16(pb + PB_MIX_R) + LD16(pb + PB_MIX_S) / 2);
    int32_t auxv[AUX_BUSES][3];
    int b, sends = 0;
    for (b = 0; b < AUX_BUSES; b++) {
        auxv[b][0] = (int32_t)LD16(pb + 0x1C + 8 * b);     /* left */
        auxv[b][1] = (int32_t)LD16(pb + 0x1C + 8 * b + 4); /* right */
        auxv[b][2] = (int32_t)LD16(pb + 0x38 + 4 * b);     /* surround */
        sends |= auxv[b][0] | auxv[b][1] | auxv[b][2];
    }
    lpfOn = (int32_t)LD16(pb + PB_LPF_ON);
    lpfY = (int16_t)LD16(pb + PB_LPF_YN1);
    lpfA = (int32_t)LD16(pb + PB_LPF_A0);
    lpfB = (int32_t)LD16(pb + PB_LPF_B0);
    {
        /* Wonderpark: 4-tap cubic (Catmull-Rom) interpolation over the PB's
         * four-sample history, as AX's own 4-tap polyphase filter works on,
         * instead of linear; and the volume ramps (each volume's delta,
         * applied per sample and written back, as the DSP does), so volume
         * changes don't step every 3 ms. */
        int32_t h[4];
        int32_t mL = (int32_t)LD16(pb + PB_MIX_L), dL = (int16_t)LD16(pb + PB_MIX_L + 2);
        int32_t mR = (int32_t)LD16(pb + PB_MIX_R), dR = (int16_t)LD16(pb + PB_MIX_R + 2);
        int32_t mS = (int32_t)LD16(pb + PB_MIX_S), dS = (int16_t)LD16(pb + PB_MIX_S + 2);
        int32_t dve = (int16_t)LD16(pb + PB_VE_VOLUME + 2);
        int k;
        for (k = 0; k < 4; k++) h[k] = (int16_t)LD16(pb + PB_SRC_LAST + 2 * k);
        if (wp_log_on("axramp")) {
            extern u32 g_ax_frame_no;
            if (g_ax_frame_no % 333 == 0)
                WP_LOG("axramp", "pb %08X L %04X d %d  R %04X d %d  S %04X d %d  ve %04X d %d  last %d %d %d %d", pb, mL, dL, mR, dR, mS, dS,
                       ve, dve, h[0], h[1], h[2], h[3]);
        }
        (void)last0; (void)last1; (void)volL; (void)volR;
        for (i = 0; i < FRAME; i++) {
            int32_t t = (int32_t)(frac >> 1); /* 0..32767 */
            int64_t a0 = -h[0] + 3 * h[1] - 3 * h[2] + h[3];
            int64_t a1 = 2 * h[0] - 5 * h[1] + 4 * h[2] - h[3];
            int64_t a2 = -h[0] + h[2];
            int64_t y = ((((a0 * t >> 15) + a1) * t >> 15) + a2) * t >> 15;
            int32_t s = clamp16((int32_t)(h[1] + (y >> 1)));
            int32_t vl = mL + mS / 2, vr = mR + mS / 2;
            s = (int32_t)(((int64_t)s * ve) >> 15);
            if (lpfOn) s = lpfY = clamp16((lpfA * s + lpfB * lpfY) >> 15);
            outL[i] += (s * vl) >> 15;
            outR[i] += (s * vr) >> 15;
            for (b = 0; sends && b < AUX_BUSES; b++) {
                s_aux[b][0][i] += (s * auxv[b][0]) >> 15;
                s_aux[b][1][i] += (s * auxv[b][1]) >> 15;
                s_aux[b][2][i] += (s * auxv[b][2]) >> 15;
            }
            mL += dL; mR += dR; mS += dS; ve += dve;
            if (mL < 0) mL = 0; if (mR < 0) mR = 0; if (mS < 0) mS = 0; if (ve < 0) ve = 0;
            if (mL > 0xFFFF) mL = 0xFFFF; if (mR > 0xFFFF) mR = 0xFFFF; if (mS > 0xFFFF) mS = 0xFFFF; if (ve > 0xFFFF) ve = 0xFFFF;
            for (frac += ratio; frac >= 0x10000; frac -= 0x10000) {
                h[0] = h[1]; h[1] = h[2]; h[2] = h[3];
                h[3] = read_sample(&v);
            }
        }
        {
            static int wb = -1;
            if (wb < 0) { const char* e = getenv("WONDERPARK_AX_WB"); wb = e ? atoi(e) : 3; }
            if (wb & 1) { /* ramped volumes */
                if (dL) ST16(pb + PB_MIX_L, (u16)mL);
                if (dR) ST16(pb + PB_MIX_R, (u16)mR);
                if (dS) ST16(pb + PB_MIX_S, (u16)mS);
                if (dve) ST16(pb + PB_VE_VOLUME, (u16)ve);
            }
            if (wb & 2) for (k = 0; k < 4; k++) ST16(pb + PB_SRC_LAST + 2 * k, (u16)h[k]);
            else { ST16(pb + PB_SRC_LAST, (u16)h[1]); ST16(pb + PB_SRC_LAST + 2, (u16)h[2]); }
        }
    }
    st32_pair(pb + PB_CUR_ADDR, v.cur);
    ST16(pb + PB_SRC_FRAC, frac & 0xFFFF);
    ST16(pb + PB_ADPCM_PS, (u16)v.ps);
    ST16(pb + PB_ADPCM_YN1, (u16)v.yn1);
    ST16(pb + PB_ADPCM_YN2, (u16)v.yn2);
    ST16(pb + PB_LPF_YN1, (u16)lpfY);
    if (!v.running) ST16(pb + PB_STATE, 0);
    if (wp_log_on("axvoice")) {
        extern u32 g_ax_frame_no;
        if (g_ax_frame_no % 333 == 0)
            WP_LOG("axvoice", "pb %08X fmt %02X cur %08X end %08X loop %08X ratio %08X ve %04X L %04X R %04X S %04X auxA %04X",
                   pb, v.format, v.cur, v.end, v.loop, ratio, ve, LD16(pb + PB_MIX_L), LD16(pb + PB_MIX_R), LD16(pb + PB_MIX_S),
                   LD16(pb + 0x1C));
    }
}

/* MIX_AUXn: hand this frame's sends to the effect callback (write) and add
 * its processed output (read) into the main mix at volume (0x8000 = 1). Each
 * buffer is three 96-sample s32 channels: left, right, surround. */
static void mix_aux(int b, u32 write, u32 read, u32 volume) {
    static int off = -1;
    int i, ch;
    if (off < 0) off = getenv("WONDERPARK_AX_NOAUX") != NULL;
    if (off) return;
    for (ch = 0; ch < 3; ch++)
        for (i = 0; i < FRAME; i++) ST32(write + 0x180 * ch + 4 * i, (u32)s_aux[b][ch][i]);
    for (i = 0; i < FRAME; i++) {
        int64_t l = (int32_t)LD32(read + 4 * i), r = (int32_t)LD32(read + 0x180 + 4 * i), sur = (int32_t)LD32(read + 0x300 + 4 * i);
        s_l[i] += (int32_t)(((l + sur / 2) * (int64_t)volume) >> 15);
        s_r[i] += (int32_t)(((r + sur / 2) * (int64_t)volume) >> 15);
    }
}

/* ---- command lists */

u32 g_ax_voices_last; /* running voices in the last frame (for the log) */
u32 g_ax_frame_no;

/* WONDERPARK_AX_SNAP=<frame>: at that AX frame, write every running PCM16
 * voice's whole buffer (loop to end) to build/voice_<pb>.raw (big-endian),
 * to tell the producer's data from the mixer's handling of it. */
static void snap(u32 pb) {
    static long at = -2;
    char path[64];
    FILE* f;
    u32 a, lo, hi;
    if (at == -2) { const char* e = getenv("WONDERPARK_AX_SNAP"); at = e ? atol(e) : -1; }
    if (at < 0 || (long)g_ax_frame_no != at || LD16(pb + PB_FORMAT) != FMT_PCM16) return;
    lo = ld32_pair(pb + PB_LOOP_ADDR);
    hi = ld32_pair(pb + PB_END_ADDR);
    if (hi <= lo || hi - lo > 0x100000) return;
    snprintf(path, sizeof(path), "build/voice_%08X.raw", pb);
    if (!(f = fopen(path, "wb"))) return;
    for (a = lo; a <= hi; a++) { u16 v = (u16)LD16(a * 2); fputc(v >> 8, f); fputc(v & 0xFF, f); }
    fclose(f);
    WP_LOG("axvoice", "snapshot %s: %u samples, playing at %u", path, hi - lo + 1, ld32_pair(pb + PB_CUR_ADDR) - lo);
}

static void process(u32 pb) {
    int n = 0;
    g_ax_voices_last = 0;
    while (pb && n++ < 256) {
        if (LD16(pb + PB_STATE) == 1) {
            snap(pb);
            mix_voice(pb, s_l, s_r);
            g_ax_voices_last++;
        }
        pb = ld32_pair(pb + PB_NEXT);
    }
}

/* OUTPUT, with a limiter standing in for AX's compressor (command 0A, not
 * emulated): when a frame would clip, the gain drops to fit it at once and
 * recovers over about 50 ms, ramped across each frame, instead of the mix
 * hard-clipping. */
static void output(u32 volume, u32 lr) {
    static double gain = 1.0;
    double peak = 0, target, g0 = gain, step;
    int i;
    for (i = 0; i < FRAME; i++) {
        double l = (double)s_l[i] * volume / 32768.0, r = (double)s_r[i] * volume / 32768.0;
        if (l < 0) l = -l;
        if (r < 0) r = -r;
        if (l > peak) peak = l;
        if (r > peak) peak = r;
    }
    target = peak * gain > 32000.0 ? 32000.0 / peak : gain + (1.0 - gain) * 0.06; /* ~50 ms release */
    if (target > 1.0) target = 1.0;
    step = (target - g0) / FRAME;
    for (i = 0; i < FRAME; i++) {
        double g = g0 + step * (i + 1);
        ST16(lr + 4 * i, (u16)clamp16((int32_t)((double)s_l[i] * volume / 32768.0 * g)));
        ST16(lr + 4 * i + 2, (u16)clamp16((int32_t)((double)s_r[i] * volume / 32768.0 * g)));
    }
    gain = target;
}

static void ax_frame(u32 list, u32 size) {
    static int unknown_logged;
    g_ax_frame_no++;
    u32 p = list, end = list + 2 * (size ? size : 0x80);
    memset(s_l, 0, sizeof(s_l));
    memset(s_r, 0, sizeof(s_r));
    memset(s_aux, 0, sizeof(s_aux));
    while (p < end) {
        u32 cmd = LD16(p);
        p += 2;
        switch (cmd) {
        case 0x00: case 0x01: case 0x02: case 0x03: p += 4; break;
        case 0x04: process(LD16(p) << 16 | LD16(p + 2)); p += 4; break;
        case 0x05: case 0x06: case 0x07: /* MIX_AUXA-C: volume, write, read */
            mix_aux((int)(cmd - 5), ld32_pair(p + 2), ld32_pair(p + 6), LD16(p));
            p += 10;
            break;
        case 0x0A: p += 8; break;
        case 0x0B: {
            u32 vol = LD16(p), lr = ld32_pair(p + 6);
            output(vol, lr);
            p += 10;
            break;
        }
        case 0x0D: p += 16; break;
        case 0x0E:
            if (wp_log_on("axvoices")) {
                static u32 frames;
                if (frames++ % 333 == 0) WP_LOG("axvoices", "%u running voices", g_ax_voices_last);
            }
            return;
        default:
            if (!unknown_logged++) {
                u32 i;
                WP_WARN("AX command list: unknown command %02X at %08X; rest of the list skipped", cmd, p - 2);
                fprintf(stderr, "  list %08X:", list);
                for (i = 0; i < 48; i++) fprintf(stderr, "%s%04X", i % 16 ? " " : "\n    ", LD16(list + 2 * i));
                fputc('\n', stderr);
            }
            return;
        }
    }
}

/* ---- AI DMA: the buffers AX filled, to the host */

static FILE* s_dump;
static u32 s_dump_frames;

static void dump_header(void) {
    u8 h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
    u32 rate = HOST_AUDIO_RATE, bytes = s_dump_frames * 4;
    memcpy(h + 24, &rate, 4);
    rate *= 4;
    memcpy(h + 28, &rate, 4);
    h[32] = 4;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    bytes += 36;
    memcpy(h + 4, &bytes, 4);
    fseek(s_dump, 0, SEEK_SET);
    fwrite(h, 1, 44, s_dump);
    fseek(s_dump, 0, SEEK_END);
}

static void ai_buffer(u32 addr, u32 bytes) {
    static int16_t lr[2048];
    u32 frames = bytes / 4, i;
    static int opened;
    if (frames > 1024) frames = 1024;
    for (i = 0; i < frames; i++) {
        lr[2 * i] = (int16_t)LD16(addr + 4 * i);
        lr[2 * i + 1] = (int16_t)LD16(addr + 4 * i + 2);
    }
    if (g_host_audio_push) g_host_audio_push(lr, (int)frames);
    if (!opened) {
        const char* path = getenv("WONDERPARK_AUDIO_DUMP");
        opened = 1;
        if (path && *path && (s_dump = fopen(path, "wb")) != NULL) dump_header();
    }
    if (s_dump) {
        fwrite(lr, 4, frames, s_dump);
        s_dump_frames += frames;
        if (s_dump_frames % (FRAME * 333) < frames) { dump_header(); fflush(s_dump); }
    }
}

/* On a Wii the DSP mixes in parallel with the CPU; here it runs on the game
 * thread, so its time is hidden from the guest's clock like the GPU's
 * (cpu_host.c). */
static void ax_frame_parallel(u32 list, u32 size) {
    u64 t0;
    wp_clock_tick();
    t0 = wp_host_time();
    ax_frame(list, size);
    wp_clock_tick();
    wp_parallel_time_hide(wp_host_time() - t0);
}

void hw_ax_init(void) {
    g_dsp_ax_frame = ax_frame_parallel;
    g_dsp_ai_buffer = ai_buffer;
}
