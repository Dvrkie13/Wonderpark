/* Pokémon cries from the player's disc, for the Gallery (cries.cpp). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void cries_load(void);            /* reads the voice bank's tables, once, on a thread of its own */
int cry_available(int character); /* the game's character number (hle_character_name) */
int cry_state_pending(void);      /* still reading */
void cry_play(int character);     /* over everything, at the Pokémon voices volume */

#ifdef __cplusplus
}
#endif
