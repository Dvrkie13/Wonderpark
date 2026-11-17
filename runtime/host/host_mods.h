/* Mods (mods.cpp): the mods the player switched on in the launcher's Mods
 * page, read once at start. A mod is a folder in <data>\mods\<id>\ with
 *   files\     disc files to use instead of the disc's, at the same paths
 *              (files\Common\x.arc replaces the disc's files/Common/x.arc)
 *   textures\  replacement textures, named by the hash the game's texture
 *              has (RECOMP_GX_DUMP_TEXTURES saves them as <hash>_<w>x<h>_<fmt>.bmp;
 *              a replacement is <hash>.png, or any name starting <hash>_)
 * Which are on, and their order, is in <data>\mods.ini; where two mods
 * replace the same thing, the one listed first wins. No code mods. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads mods.ini and indexes the enabled mods' files and textures (main.c,
 * before the disc is opened). WONDERPARK_NO_MODS=1 skips all mods. */
void wp_mods_init(void);

/* A replacement for the disc file at path (relative to the disc's files/,
 * "Common/x.arc"): its host path and size; 0 if no mod replaces it. */
int wp_mod_file(const char* path, char* out, size_t n, uint64_t* size);

/* Whether any enabled mod has textures (so the renderer can skip looking). */
int wp_mods_have_textures(void);

/* The replacement for a texture with this hash: 0 none, 1 being loaded (on
 * a background thread; ask again later), 2 ready (RGBA8, w x h, kept for
 * the whole run). */
int wp_mod_texture(uint32_t hash, const uint8_t** rgba, int* w, int* h);

#ifdef __cplusplus
}
#endif
