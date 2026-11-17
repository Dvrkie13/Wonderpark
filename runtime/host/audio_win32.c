/* From MildRain479/PokePark-Wiicompiled recomp/boot/audio_win32.c (CC0). */
/* Sound output through waveOut. The DSP HLE hands over 3 ms frames as AX
 * mixes them; they are gathered into 12 ms blocks and queued to the
 * device, which plays them back to back.
 *
 * The game's audio timer runs on host time, so on average it keeps pace
 * with the device, but its ticks arrive in bursts whenever the game holds
 * off interrupts. Playback therefore starts only once PRIME_BLOCKS are
 * queued, which absorbs that much jitter; if the queue still runs dry (a
 * long stall), the device pauses and primes again, so a stall costs one
 * gap instead of a run of crackles. Frames that arrive with the queue full
 * are dropped, which bounds the latency at BLOCKS blocks.
 *
 * RECOMP_AUDIO=0 turns sound off. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "host_audio.h"
#include "host_settings.h"

#pragma comment(lib, "winmm.lib")

/* Wonderpark: the device runs at 48 kHz; AX's 32 kHz mix is upsampled here
 * with a windowed-sinc filter (below) rather than left to Windows. */
#define OUT_RATE 48000
#define BLOCK_FRAMES 576 /* 12 ms at 48 kHz */
/* Wonderpark: playback starts with PRIME_BLOCKS queued and the queue then
 * sits about there (production keeps pace with the device), so BLOCKS is
 * headroom: windows reports finished blocks in clumps, and after a stall
 * the DMA timer catches up in a burst. With 10 the queue sat at 9 of 10
 * and either overflowed (dropped blocks: clicks, e.g. at the Cake
 * Contraption's character select); 16 absorbs them, and the latency stays
 * that of PRIME_BLOCKS. */
#define BLOCKS 16 /* at most 192 ms queued */
/* The Audio delay setting (g_x[X_AUDIO_LATENCY]): low 4 blocks (48 ms),
 * normal 7 (84 ms, as before there was a setting), safe 11 (132 ms). A
 * lower one takes effect at once (the queue is trimmed down to it); a
 * higher one at the next refill (after a pause or a stall). */
static int prime_blocks(void) {
    static const int kPrime[3] = {4, 7, 11};
    int l = g_x[X_AUDIO_LATENCY];
    return kPrime[l < 0 ? 0 : l > 2 ? 2 : l];
}
#define PRIME_BLOCKS prime_blocks()
#define TRIM_ABOVE 2
#define TRIM_EVERY 128

static HWAVEOUT sOut;
static WAVEHDR sHdr[BLOCKS];
static int16_t sData[BLOCKS][BLOCK_FRAMES * 2];
static int sFill;   /* block being filled */
static int sFrames; /* frames in it */
static int sPriming = 1;
/* Wonderpark: dropped blocks (queue full) and underruns (queue ran dry),
 * printed every 5 s with WONDERPARK_LOG=audiostats. */
static unsigned sDrops, sUnderruns, sPushed;

static int busy(const WAVEHDR* h) { return (h->dwFlags & WHDR_INQUEUE) && !(h->dwFlags & WHDR_DONE); }

static int queued(void) {
    int i, n = 0;
    for (i = 0; i < BLOCKS; i++) n += busy(&sHdr[i]);
    return n;
}

static int log_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("WONDERPARK_LOG"); on = e && strstr(e, "audiostats") != NULL; }
    return on;
}

/* WONDERPARK_LOG=audioq: the queue's depth at each block written, and each
 * block dropped, with the time. */
static int qlog_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("WONDERPARK_LOG"); on = e && strstr(e, "audioq") != NULL; }
    return on;
}
extern double wp_wall_seconds(void);

static void push_out(const int16_t* lr, int frames) {
    static double energy; /* sum of squares since the last report (before the master volume) */
    sPushed += (unsigned)frames;
    if (log_on()) {
        int k;
        for (k = 0; k < frames * 2; k++) energy += (double)lr[k] * lr[k];
    }
    if (log_on() && sPushed >= (unsigned)OUT_RATE * 5) {
        fprintf(stderr, "[audiostats] 5 s: %u blocks dropped, %u underruns, %d queued, level %.0f (RMS)\n", sDrops,
                sUnderruns, queued(), sqrt(energy / (sPushed * 2.0)));
        sPushed = 0;
        energy = 0;
        sDrops = sUnderruns = 0;
    }
    while (frames > 0) {
        WAVEHDR* h = &sHdr[sFill];
        int n;
        if (sFrames == 0) {
            if (busy(h)) {
                if (qlog_on()) fprintf(stderr, "audioq %.3f s: dropped\n", wp_wall_seconds());
                sDrops++;
                return;
            } /* queue full: drop */
            if (!sPriming && queued() == 0) {
                /* Ran dry: hold the device until the queue is primed. */
                waveOutPause(sOut);
                sPriming = 1;
                sUnderruns++;
            }
        }
        n = BLOCK_FRAMES - sFrames;
        if (n > frames) n = frames;
        memcpy(&sData[sFill][sFrames * 2], lr, (size_t)n * 4);
        sFrames += n;
        lr += n * 2;
        frames -= n;
        if (sFrames == BLOCK_FRAMES) {
            /* Master volume (squared, so the slider feels even) and
             * silence while another window is in front (settings). */
            extern int win_has_focus(void);
            int vol = g_x[X_VOLUME_MASTER];
            if (g_x[X_MUTE_BACKGROUND] && !win_has_focus()) vol = 0;
            if (vol < 100) {
                int g = vol * vol * 65536 / 10000, k;
                for (k = 0; k < BLOCK_FRAMES * 2; k++) sData[sFill][k] = (int16_t)((sData[sFill][k] * g) >> 16);
            }
            h->dwFlags &= ~WHDR_DONE;
            waveOutWrite(sOut, h, sizeof(*h));
            if (qlog_on()) fprintf(stderr, "audioq %.3f s: %d queued\n", wp_wall_seconds(), queued());
            sFill = (sFill + 1) % BLOCKS;
            sFrames = 0;
            if (sPriming && queued() >= PRIME_BLOCKS) {
                sPriming = 0;
                waveOutRestart(sOut);
            }
        }
    }
}

/* 32 -> 48 kHz: upsample by 3 (zero stuffing) to 96 kHz, low-pass at 15 kHz
 * with a 72-tap Kaiser-windowed sinc, take every second sample. Each output
 * sample needs 24 input samples per channel. */
#define TAPS 72
static double sFir[TAPS];
static double sHist[2][64];
static unsigned long long sIn, sOutN; /* input samples seen, output samples made */

static double bessel_i0(double x) {
    double sum = 1, term = 1;
    int k;
    for (k = 1; k < 30; k++) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; }
    return sum;
}

static void fir_init(void) {
    const double fc = 15000.0 / 96000.0, beta = 8.0, pi = 3.14159265358979323846;
    double sum = 0;
    int k;
    for (k = 0; k < TAPS; k++) {
        double m = k - (TAPS - 1) / 2.0, r = 2.0 * k / (TAPS - 1) - 1.0;
        double sinc = m == 0 ? 2 * fc : sin(2 * pi * fc * m) / (pi * m);
        sFir[k] = sinc * bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta);
        sum += sFir[k];
    }
    for (k = 0; k < TAPS; k++) sFir[k] *= 3.0 / sum; /* unity gain after zero stuffing */
}

int wp_game_speed(void); /* cpu_host.c */

static void push(const int16_t* lr, int frames) {
    static int16_t out[2048 * 2];
    static unsigned trim;
    int i, n = 0, over;
    if (wp_game_speed() != 1) return; /* sped up (debug overlay, F7): silent rather than garbled */
    /* Wonderpark: a burst (catching up after a stall, start-up) can leave
     * the queue well above where playback started, and as production then
     * keeps pace with the device it would stay there, as latency. While it
     * is more than TRIM_ABOVE blocks over PRIME_BLOCKS, one input sample in
     * TRIM_EVERY is left out (0.8% faster, inaudible; before the low-pass
     * filter, so no click) until it is back. */
    over = !sPriming && queued() > PRIME_BLOCKS + TRIM_ABOVE;
    for (i = 0; i < frames; i++) {
        if (over && ++trim % TRIM_EVERY == 0) continue;
        sHist[0][sIn & 63] = lr[2 * i];
        sHist[1][sIn & 63] = lr[2 * i + 1];
        sIn++;
        /* outputs whose 96 kHz position 2*sOutN is covered by the inputs so far */
        while (2 * sOutN <= 3 * (sIn - 1) && n < 2048) {
            long long pos = (long long)(2 * sOutN), m;
            double y[2] = {0, 0};
            long long m0 = pos / 3, m1 = (pos - (TAPS - 1) + 2) / 3;
            if (m1 < 0) m1 = 0;
            for (m = m0; m >= m1 && m >= (long long)sIn - 64; m--) {
                long long k = pos - 3 * m;
                if (k < 0 || k >= TAPS) continue;
                y[0] += sFir[k] * sHist[0][m & 63];
                y[1] += sFir[k] * sHist[1][m & 63];
            }
            out[2 * n] = (int16_t)(y[0] > 32767 ? 32767 : y[0] < -32768 ? -32768 : y[0]);
            out[2 * n + 1] = (int16_t)(y[1] > 32767 ? 32767 : y[1] < -32768 ? -32768 : y[1]);
            n++;
            sOutN++;
        }
    }
    push_out(out, n);
}

void audio_init(void) {
    WAVEFORMATEX fmt;
    const char* e = getenv("RECOMP_AUDIO");
    int i;
    if (e && *e == '0') return;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = OUT_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = OUT_RATE * 4;
    fir_init();
    if (waveOutOpen(&sOut, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "audio: no output device; running without sound\n");
        return;
    }
    fprintf(stderr, "audio: waveOut, %d Hz stereo, %d ms buffered\n", HOST_AUDIO_RATE,
            PRIME_BLOCKS * BLOCK_FRAMES * 1000 / HOST_AUDIO_RATE);
    waveOutPause(sOut);
    for (i = 0; i < BLOCKS; i++) {
        sHdr[i].lpData = (LPSTR)sData[i];
        sHdr[i].dwBufferLength = sizeof(sData[i]);
        waveOutPrepareHeader(sOut, &sHdr[i], sizeof(sHdr[i]));
        sHdr[i].dwFlags |= WHDR_DONE;
    }
    g_host_audio_push = push;
}
