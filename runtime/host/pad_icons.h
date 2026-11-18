/* Controller button icons (pad_icons.cpp): which set to show (the pad's own
 * type, found from its maker, or the Button icons setting), and the icons
 * themselves (Flat Gamepad Icons by RobTheFiveNine, CC BY-SA 4.0, built into
 * the program from assets/ui/pad-icons/). Used by the F1 tab, the Options
 * page's PC rows and the in-game button prompts (hle_prompts.c). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PADSET_KEYS, PADSET_XB, PADSET_PS, PADSET_NS };

/* The game: starts reading the icons and looking for the pad's type, on a thread of
 * its own (sets the prompt hooks too). */
void pad_icons_init(void);
/* The shared part alone (the launcher): the icons and the pad's type. */
void pad_icons_core_init(void);
/* Changes when the icons are read or the pad's type is looked at again. */
uint32_t pad_icons_core_generation(void);
/* A pad connected or went away: look at its type again (input_win32.c). */
void pad_icons_hotplug(void);
/* The connected pad's type (PADSET_XB, _PS or _NS; XB when none or unknown). */
int pad_icons_detected(void);
/* What the pad's buttons are shown as: the setting, or the pad's type. */
int pad_icons_pad_set(void);
/* What the game's prompts show: PADSET_KEYS when the keyboard and mouse were
 * used last (or the setting says keyboard only), else pad_icons_pad_set(). */
int pad_icons_prompt_set(void);
/* The icon's name for a controller button (XPAD_*) in a set ("xb_a",
 * "ps_x", "switch_b", "generic_l3"); NULL if none. The face buttons go by
 * position: XInput's A is the bottom one, a Nintendo pad's B. */
const char* pad_icons_name(uint32_t xpad, int set);
/* The button's name as printed on a pad of the set ("Cross", "ZL"). */
const char* pad_icons_label(uint32_t xpad, int set);
/* Icon name drawn h pixels high (its width keeps its shape): straight RGBA,
 * top row first, into out (room for maxw x h). Returns its width, 0 if the
 * icon isn't there (or not read yet). Scaled down from the nearest size at
 * or above h. */
int pad_icons_rgba(const char* name, int h, uint8_t* out, int maxw);
/* Its width at height h; 0 if there is no such icon. */
int pad_icons_width(const char* name, int h);
/* Changes whenever what the icons show may have. */
uint32_t pad_icons_generation(void);
/* For the log and tests: what was found ("PlayStation (054C:09CC)"). */
const char* pad_icons_found(void);

#ifdef __cplusplus
}
#endif
