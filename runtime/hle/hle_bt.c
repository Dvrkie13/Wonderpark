/* Bluetooth. There is no Bluetooth adapter (IOS refuses
 * /dev/usb/oh1/57e/305); Wii Remote input will come from an HLE of WPAD.
 * WUD still sets up its own state and alarms.
 * Same approach as the reference runtime's hle_bt.c (CC0). */
#include "hle.h"

/* void BTA_Init(void): opens the adapter and waits until it is ready. */
WP_HOOK(0x802F3E10, BTA_Init) {
    wp_return(c);
    return 1;
}

/* s32 WUDGetStatus(void), which WPADGetStatus forwards to: report the stack
 * as set up (WPAD_STATE_SETUP), with no remotes connected. */
WP_HOOK(0x802E4D30, WUDGetStatus) {
    c->gpr[3] = 3;
    wp_return(c);
    return 1;
}
