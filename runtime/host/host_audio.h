/* From MildRain479/PokePark-Wiicompiled recomp/runtime/host_audio.h (CC0). */
/* Sound output to the host, from the DSP HLE (hle_audio.c). The platform
 * layer (recomp/boot/audio_win32.c) sets the hook; without it the mix is
 * dropped. */
#pragma once

#include <stdint.h>

/* AX's output: 32 kHz, 16-bit stereo, interleaved left/right. */
#define HOST_AUDIO_RATE 32000

/* Queues frames (left/right pairs) for playback, as they are mixed. */
extern void (*g_host_audio_push)(const int16_t* lr, int frames);

/* A short mono sound of our own mixed over everything (audio_win32.c): a
 * cry in the Gallery, the sticker chime. gain 0-1 (the Master volume is
 * applied on top); a new one replaces the one playing. */
void audio_play_sound(const int16_t* pcm, int frames, int rate, float gain);
int audio_sound_playing(void);
