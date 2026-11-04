/* Lets code ported from the reference runtime (MildRain479/PokePark-Wiicompiled,
 * recomp/patches, CC0) compile against Wonderpark's core with few changes. */
#pragma once

#include <stdio.h>
#include <string.h>

#include "wp.h"

#define MA(a) wp_mem_ptr((uint32_t)(a), 1)
#define HLE_LOG(...) WP_LOG("ios", __VA_ARGS__)
#define HLE_WARN(...) WP_WARN(__VA_ARGS__)

static inline void hle_read_str(uint32_t addr, char* out, size_t n) {
    size_t i;
    for (i = 0; i + 1 < n; i++) {
        out[i] = (char)LD8(addr + (uint32_t)i);
        if (!out[i]) return;
    }
    out[i] = '\0';
}

/* Title ID of PokePark 2 (USA): 00010000-53324C45 ("S2LE"). */
#define WP_TITLE_HI 0x00010000u
#define WP_TITLE_LO 0x53324C45u
#define WP_TITLE_DIR "/title/00010000/53324c45"
