/* OS-level hooks. Addresses are S2LE01 (config/S2LE01/symbols.txt). */
#include "hle.h"

/* void __OSInitAudioSystem(void): uploads a small init program to the DSP
 * and waits for it to report back. There is no DSP; AX is emulated at the
 * parameter-block level (as in the reference runtime's hle_audio.c). */
WP_HOOK(0x8028DD40, __OSInitAudioSystem) {
    wp_return(c);
    return 1;
}
