/* Placeholders for parts of the reference runtime's host layer that
 * Wonderpark hasn't ported yet. Each one is replaced by a PokePark 2
 * version later (see docs/recomp-status.md):
 *   - the reference's gameplay sliders (berries, prices, damage, chase
 *     speed, text speed) are PokePark 1 game patches; Wonderpark's own come
 *     with Phase 6;
 *   - frame rate and FOV need PokePark 2 patches (Phase 6);
 *   - the in-window settings menu and the update check come with the
 *     launcher and Options work;
 *   - the Vulkan device needs the shader build step (OpenGL is used meanwhile). */
#include <stddef.h>
#include <stdint.h>

#include "gx_device.h"

void hle_set_berry_rate(int percent) { (void)percent; }
void hle_set_prices(int percent) { (void)percent; }
void hle_set_damage(int dealt, int taken) { (void)dealt; (void)taken; }
void hle_set_chase_speed(int percent) { (void)percent; }
void hle_set_text_speed(int speed) { (void)speed; }
void hle_set_fov(int percent) { (void)percent; }
void hle_set_frame_rate(int fps) { (void)fps; }
void update_check(void) {}
void menu_draw(void) {}
int menu_window_message(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp) {
    (void)hwnd; (void)msg; (void)wp; (void)lp;
    return 0;
}

static int vk_unavailable(void) { return 0; }
const GXDevice g_vk_device = {"Vulkan (not built yet)", vk_unavailable};

/* Read by the reference's camera and minigame patches (Phase 3); the input
 * layer sets them. */
void (*g_host_mouse_read)(float* dx, float* dy);
void (*g_host_pointer_read)(float* dx, float* dy);
void (*g_host_sticks_read)(float* lx, float* ly, float* rx, float* ry);

/* The reference's in-game Options menu and button-prompt icons (Phase 3):
 * closed, and no scripted menu input. */
int menu_is_open(void) { return 0; }
void menu_controller(uint32_t pressed, uint32_t held) { (void)pressed; (void)held; }
int options_script(uint32_t held) { (void)held; return 0; }
void prompts_init(void) {}
