/* OS-level hooks. Addresses are S2LE01 (config/S2LE01/symbols.txt). */
#include "hle.h"

/* void __OSInitAudioSystem(void): uploads a small init program to the DSP
 * and waits for it to report back. There is no DSP; AX is emulated at the
 * parameter-block level (as in the reference runtime's hle_audio.c). */
WP_HOOK(0x8028DD40, __OSInitAudioSystem) {
    wp_return(c);
    return 1;
}

/* u8 SCGetAspectRatio(void): the system menu's TV shape, 1 for 16:9. The
 * game reads it once at start (0x800300FC) into its 16:9 flag. Answered
 * from the Widescreen setting rather than SYSCONF, which says 4:3. */
WP_HOOK(0x802C9A70, SCGetAspectRatio) {
    extern int hle_widescreen(void);
    c->gpr[3] = hle_widescreen() ? 1u : 0u;
    wp_return(c);
    return 1;
}

/* static void __AI_SRC_INIT(void): times how long the audio interface's
 * sample-rate converter takes to start, by watching its sample counter, and
 * retries until the timing looks like hardware's. There is no converter to
 * time (the reference runtime skips it too). */
WP_HOOK(0x802B4970, __AI_SRC_INIT) {
    wp_return(c);
    return 1;
}

/* Interrupt enables. Implemented here (they only flip MSR[EE]) so that they
 * return to the dispatcher, which takes any pending interrupt right away
 * when they turn interrupts back on, as the CPU would. */
#define MSR_EE 0x8000u

/* BOOL OSEnableInterrupts(void) */
WP_HOOK(0x80292E30, OSEnableInterrupts) {
    c->gpr[3] = (c->msr & MSR_EE) ? 1 : 0;
    c->msr |= MSR_EE;
    wp_return(c);
    return 1;
}

/* BOOL OSRestoreInterrupts(BOOL level) */
WP_HOOK(0x80292E50, OSRestoreInterrupts) {
    u32 level = c->gpr[3];
    c->gpr[3] = (c->msr & MSR_EE) ? 1 : 0;
    if (level) c->msr |= MSR_EE;
    else c->msr &= ~MSR_EE;
    wp_return(c);
    return 1;
}
