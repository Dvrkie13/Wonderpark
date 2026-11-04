/* Device setup and the per-slice tick. */
#include "../core/wp.h"

void wp_devices_init(void) {
    hw_pi_init();
}

void wp_devices_tick(uint64_t now) {
    (void)now;
}
