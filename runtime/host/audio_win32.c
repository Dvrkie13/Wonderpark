/* From MildRain479/PokePark-Wiicompiled recomp/boot/audio_win32.c (CC0),
 * rewritten for Wonderpark. */
/* Sound output through WASAPI (shared mode), on a thread and clock of its
 * own.
 *
 * The game (AX, hw_ax.c) and the Jukebox hand over 32 kHz stereo as they
 * make it; push() only copies it into a ring buffer, so the game thread
 * never waits on the sound device and nothing here happens per drawn frame.
 * The sound thread is woken by the device (its event, about every 10 ms)
 * and tops the device's buffer up to a few ms ahead from the ring: resampled
 * to 48 kHz by a windowed-sinc resampler whose ratio follows the ring's
 * level. The game makes sound by its own clock (the emulated audio DMA
 * timer), in bursts whenever it holds off interrupts, and the device plays
 * by its own crystal; the two never agree exactly. So the ring is kept near
 * a target level (the Audio delay setting) by playing up to 0.5% faster or
 * slower (inaudible), up to 3% after a big burst (loading), instead of
 * dropping or repeating samples. If the ring runs dry (the game stalled),
 * the sound fades out and waits until the ring is back at its level: one
 * gap, not a run of crackles. If the device goes away (headphones
 * unplugged), the default device is opened again.
 *
 * (waveOut, used before, starved unless about 60 ms was queued with it, and
 * couldn't say when it had.)
 *
 * RECOMP_AUDIO=0 turns sound off. WONDERPARK_LOG=audiostats prints, every
 * 5 s: underruns (the ring ran dry), device gaps (the device's buffer ran
 * dry), dropped input, the ring's level, the rate, and the sample rates in
 * and out. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_audio.h"
#include "host_settings.h"

#pragma comment(lib, "ole32.lib")

#define IN_RATE HOST_AUDIO_RATE /* 32 kHz */
#define OUT_RATE 48000
#define RING_FRAMES (1 << 16)   /* 2 s of input */

int wp_game_speed(void);   /* cpu_host.c */
int wp_clock_paused(void); /* cpu_host.c */
extern double wp_wall_seconds(void);
extern int win_has_focus(void);

/* The Audio delay setting (g_x[X_AUDIO_LATENCY]) Low / Normal / Safe: the
 * ring's target level and how far ahead the device's buffer is kept. About
 * 40, 75 and 140 ms in all. */
static int setting(void) {
    int l = g_x[X_AUDIO_LATENCY];
    return l < 0 ? 0 : l > 2 ? 2 : l;
}
static int target_ms(void) {
    static const int k[3] = {20, 45, 80};
    return k[setting()];
}
static int ahead_ms(void) {
    static const int k[3] = {20, 30, 60};
    return k[setting()];
}

/* ------------------------------------------------------------------------ */
/* The ring: written by push() (the game thread, or the Jukebox's), read by
 * the sound thread. Positions only grow; one writer at a time (sPushLock). */
static int16_t sRing[RING_FRAMES * 2];
static volatile LONG64 sWrite, sRead;
static CRITICAL_SECTION sPushLock;
static volatile LONG sDrops, sUnderruns, sPushedIn, sFramesOut, sDevGaps; /* since the last report */

static LONG64 ring_level(void) {
    return InterlockedCompareExchange64(&sWrite, 0, 0) - InterlockedCompareExchange64(&sRead, 0, 0);
}

/* Milliseconds of sound waiting to be heard: the ring and the device's
 * buffer (the Jukebox paces itself by it). */
static volatile LONG sDevQueuedFrames;
int audio_queued_ms(void) {
    return (int)(ring_level() * 1000 / IN_RATE) + (int)(sDevQueuedFrames * 1000 / OUT_RATE);
}
/* What audio_queued_ms() sits at in steady play: the Jukebox keeps it there. */
int audio_target_ms(void) { return target_ms() + ahead_ms(); }

static void push(const int16_t* lr, int frames) {
    LONG64 w, room;
    int first;
    if (wp_game_speed() != 1) return; /* sped up (F7, speed-up): silent rather than garbled */
    EnterCriticalSection(&sPushLock);
    InterlockedExchangeAdd(&sPushedIn, frames);
    w = sWrite;
    room = RING_FRAMES - (w - InterlockedCompareExchange64(&sRead, 0, 0));
    if (frames > room) {
        InterlockedExchangeAdd(&sDrops, (LONG)(frames - room));
        frames = (int)room;
    }
    first = (int)(RING_FRAMES - (w & (RING_FRAMES - 1)));
    if (first > frames) first = frames;
    memcpy(&sRing[(w & (RING_FRAMES - 1)) * 2], lr, (size_t)first * 4);
    if (frames > first) memcpy(sRing, lr + first * 2, (size_t)(frames - first) * 4);
    InterlockedExchange64(&sWrite, w + frames);
    LeaveCriticalSection(&sPushLock);
}

/* ------------------------------------------------------------------------ */
/* The resampler: windowed sinc, 32 taps, 512 phases (linear between two),
 * low-pass at 15 kHz, any ratio near 2/3. */
#define HALF 16
#define PHASES 512
static float sKernel[PHASES + 1][2 * HALF];
static float sHist[2][128]; /* the newest input samples, by input index & 127 */
static LONG64 sIn;          /* input samples taken from the ring */
static LONG64 sPosI;        /* the next output's position in the input: sPosI + sPosF */
static double sPosF;

static double bessel_i0(double x) {
    double sum = 1, term = 1;
    int k;
    for (k = 1; k < 30; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

static void kernel_init(void) {
    const double fc = 15000.0 / IN_RATE, beta = 8.0, pi = 3.14159265358979323846;
    int ph, j;
    for (ph = 0; ph <= PHASES; ph++) {
        double f = (double)ph / PHASES, sum = 0;
        for (j = 0; j < 2 * HALF; j++) {
            double t = f - (j - HALF + 1); /* tap j is input sample i + j - HALF + 1 */
            double r = t / HALF, w = fabs(r) < 1 ? bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta) : 0;
            double s = t == 0 ? 2 * fc : sin(2 * pi * fc * t) / (pi * t);
            sKernel[ph][j] = (float)(s * w);
            sum += s * w;
        }
        for (j = 0; j < 2 * HALF; j++) sKernel[ph][j] = (float)(sKernel[ph][j] / sum);
    }
}

/* One input sample from the ring into the history; 0 if the ring is empty. */
static int take(void) {
    LONG64 r = sRead;
    const int16_t* s;
    if (InterlockedCompareExchange64(&sWrite, 0, 0) <= r) return 0;
    s = &sRing[(r & (RING_FRAMES - 1)) * 2];
    sHist[0][sIn & 127] = s[0];
    sHist[1][sIn & 127] = s[1];
    sIn++;
    InterlockedExchange64(&sRead, r + 1);
    return 1;
}

static int sPriming = 1;
static double sLevel = -1; /* the ring's level, smoothed (input frames) */
static double sRate = 1;   /* playback speed now (1.005: 0.5% faster) */
static float sLast[2];     /* the last sample played, faded out on a gap */

/* The playback speed, from the ring's level (each wake, about every 10 ms). */
static void steer(void) {
    double target = target_ms() * IN_RATE / 1000.0, err, want, maxr;
    LONG64 level = ring_level();
    sLevel = sLevel < 0 ? (double)level : sLevel + 0.05 * ((double)level - sLevel); /* ~0.2 s */
    err = (sLevel - target) / IN_RATE * 1000.0;                                     /* ms over the target */
    maxr = err > 150 ? 0.03 : 0.005;
    want = 1 + (fabs(err) < 5 ? 0 : err * 0.00005); /* 100 ms over: 0.5% faster */
    if (want > 1 + maxr) want = 1 + maxr;
    if (want < 1 - 0.005) want = 1 - 0.005;
    sRate += 0.1 * (want - sRate); /* eased: no audible steps */
}

/* frames of 48 kHz stereo float into out. */
static void fill(float* out, int frames) {
    double step = (double)IN_RATE / OUT_RATE * sRate;
    float gain;
    int n, vol = g_x[X_VOLUME_MASTER];
    if (g_x[X_MUTE_BACKGROUND] && !win_has_focus()) vol = 0;
    gain = (float)(vol * vol) / (10000.0f * 32768.0f); /* squared, so the slider feels even */
    if (sPriming) {
        /* Waiting for the ring to reach its level (at start, after a gap). */
        if (ring_level() >= (LONG64)target_ms() * IN_RATE / 1000) {
            sPriming = 0;
            sLevel = -1;
            sRate = 1;
            /* Start from what's there: the history is the ring's next samples. */
            sPosI = sIn + HALF - 1;
            sPosF = 0;
            while (sIn < sPosI + HALF && take()) {}
        } else {
            memset(out, 0, (size_t)frames * 8);
            return;
        }
    }
    for (n = 0; n < frames; n++) {
        float ph, a, b, l = 0, r = 0;
        const float *k0, *k1;
        int p, j;
        LONG64 base;
        while (sIn < sPosI + HALF + 1) {
            if (!take()) break;
        }
        if (sIn < sPosI + HALF + 1) {
            /* Ran dry: fade the last sample out over the rest, then wait
             * for the ring to fill again. */
            int k;
            if (!wp_clock_paused() && wp_game_speed() == 1) InterlockedIncrement(&sUnderruns);
            for (k = n; k < frames; k++) {
                float f = k - n < 96 ? 1.0f - (k - n) / 96.0f : 0;
                out[2 * k] = sLast[0] * f;
                out[2 * k + 1] = sLast[1] * f;
            }
            sLast[0] = sLast[1] = 0;
            sPriming = 1;
            return;
        }
        ph = (float)(sPosF * PHASES);
        p = (int)ph;
        b = ph - p;
        a = 1 - b;
        k0 = sKernel[p];
        k1 = sKernel[p + 1];
        base = sPosI - HALF + 1;
        for (j = 0; j < 2 * HALF; j++) {
            float c = k0[j] * a + k1[j] * b;
            int idx = (int)((base + j) & 127);
            l += c * sHist[0][idx];
            r += c * sHist[1][idx];
        }
        l *= gain;
        r *= gain;
        sLast[0] = l = l > 1 ? 1 : l < -1 ? -1 : l;
        sLast[1] = r = r > 1 ? 1 : r < -1 ? -1 : r;
        out[2 * n] = l;
        out[2 * n + 1] = r;
        sPosF += step;
        while (sPosF >= 1) sPosF -= 1, sPosI++;
    }
}

/* ------------------------------------------------------------------------ */
/* The device */

DEFINE_GUID(WP_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(WP_IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(WP_IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(WP_IID_IAudioRenderClient, 0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

static IAudioClient* sClient;
static IAudioRenderClient* sRender;
static UINT32 sBufferFrames;
static HANDLE sWake;

static void device_close(void) {
    if (sClient) IAudioClient_Stop(sClient);
    if (sRender) IAudioRenderClient_Release(sRender);
    if (sClient) IAudioClient_Release(sClient);
    sRender = NULL;
    sClient = NULL;
}

/* Opens the default output device: 48 kHz stereo float, the system turning
 * it into the device's own format (its rate, 5.1 and so on). */
static int device_open(void) {
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    WAVEFORMATEX fmt;
    HRESULT hr;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = 3; /* WAVE_FORMAT_IEEE_FLOAT */
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = OUT_RATE;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = 8;
    fmt.nAvgBytesPerSec = OUT_RATE * 8;
    hr = CoCreateInstance(&WP_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &WP_IID_IMMDeviceEnumerator, (void**)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(dev, &WP_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&sClient);
    if (SUCCEEDED(hr))
        hr = IAudioClient_Initialize(sClient, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                         AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                     (REFERENCE_TIME)100 * 10000, 0, &fmt, NULL); /* a 100 ms buffer, kept part full */
    if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(sClient, sWake);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(sClient, &sBufferFrames);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(sClient, &WP_IID_IAudioRenderClient, (void**)&sRender);
    if (SUCCEEDED(hr)) hr = IAudioClient_Start(sClient);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    if (FAILED(hr)) {
        fprintf(stderr, "audio: no output device (0x%08lX)\n", (unsigned long)hr);
        device_close();
        return 0;
    }
    return 1;
}

static int log_on(void) {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("WONDERPARK_LOG");
        on = e && strstr(e, "audiostats") != NULL;
    }
    return on;
}

static DWORD WINAPI sound_thread(LPVOID arg) {
    double next_report = wp_wall_seconds() + 5, retry = 0;
    int started = 0;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    if (device_open())
        fprintf(stderr, "audio: WASAPI %d Hz stereo, its own thread, about %d ms delay\n", OUT_RATE, audio_target_ms());
    for (;;) {
        UINT32 padding = 0, want, room;
        BYTE* buf;
        HRESULT hr;
        WaitForSingleObject(sWake, 20);
        if (!sClient) {
            /* No device (unplugged, or none at all): try again every 2 s,
             * meanwhile the ring is let go (so the game never waits). */
            InterlockedExchange64(&sRead, InterlockedCompareExchange64(&sWrite, 0, 0));
            if (wp_wall_seconds() >= retry) {
                retry = wp_wall_seconds() + 2;
                if (device_open()) fprintf(stderr, "audio: output device opened again\n");
                started = 0;
            }
            continue;
        }
        hr = IAudioClient_GetCurrentPadding(sClient, &padding);
        if (FAILED(hr)) {
            fprintf(stderr, "audio: the output device went away (0x%08lX)\n", (unsigned long)hr);
            device_close();
            continue;
        }
        if (padding == 0 && started) InterlockedIncrement(&sDevGaps); /* it played everything it had */
        want = (UINT32)(OUT_RATE * ahead_ms() / 1000);
        if (want > sBufferFrames) want = sBufferFrames;
        room = want > padding ? want - padding : 0;
        if (room) {
            steer();
            if (SUCCEEDED(IAudioRenderClient_GetBuffer(sRender, room, &buf))) {
                fill((float*)buf, (int)room);
                IAudioRenderClient_ReleaseBuffer(sRender, room, 0);
                InterlockedExchangeAdd(&sFramesOut, (LONG)room);
                padding += room;
                started = 1;
            }
        }
        InterlockedExchange(&sDevQueuedFrames, (LONG)padding);
        if (log_on() && wp_wall_seconds() >= next_report) {
            next_report += 5;
            fprintf(stderr,
                    "[audiostats] 5 s: %ld underruns, %ld device gaps, %ld input frames dropped, ring %.0f ms (target %d), "
                    "rate %.4f, in %.0f Hz, out %.0f Hz%s\n",
                    InterlockedExchange(&sUnderruns, 0), InterlockedExchange(&sDevGaps, 0), InterlockedExchange(&sDrops, 0),
                    sLevel * 1000.0 / IN_RATE, target_ms(), sRate, InterlockedExchange(&sPushedIn, 0) / 5.0,
                    InterlockedExchange(&sFramesOut, 0) / 5.0, wp_clock_paused() ? ", paused" : "");
        }
    }
}

void audio_init(void) {
    const char* e = getenv("RECOMP_AUDIO");
    if (e && *e == '0') return;
    kernel_init();
    InitializeCriticalSection(&sPushLock);
    sWake = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_host_audio_push = push;
    if (!CreateThread(NULL, 0, sound_thread, NULL, 0, NULL)) {
        fprintf(stderr, "audio: no sound thread; running without sound\n");
        g_host_audio_push = NULL;
    }
}
