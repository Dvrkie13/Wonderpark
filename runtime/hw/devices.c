/* Device setup and the per-slice tick. */
#include "../core/wp.h"

void hw_ipc_init(void);
void hw_ipc_tick(void);
void hw_gx_tick(void);
extern void (*g_gx_game_tick)(void); /* runtime/gpu/gx_thread.c */
void hw_misc_init(void);
void hw_vi_init(void);
void hw_vi_tick(uint64_t now);
void hw_lc_init(void);
void hw_ai_init(void);
void hw_gx_init(void);
void hw_dsp_init(void);
void hw_dsp_tick(uint64_t now);
void hw_ax_init(void);

void wp_devices_init(void) {
    hw_pi_init();
    hw_ipc_init();
    hw_misc_init();
    hw_vi_init();
    hw_lc_init();
    hw_ai_init();
    hw_gx_init();
    hw_dsp_init();
    hw_ax_init();
}

void wp_devices_tick(uint64_t now) {
    hw_ipc_tick();
    hw_gx_tick();
    if (g_gx_game_tick) g_gx_game_tick();
    hw_vi_tick(now);
    hw_dsp_tick(now);
}
