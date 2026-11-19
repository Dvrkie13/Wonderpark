# Books and Extras: findings and state (2026-11-19)

From the "books and extras" plan. What each step 0
found, what's built, and what still shows "Coming soon" (never faked).

## A0. Bookmarks (built)

The F1 notebook has five books as bookmarks on its right edge: Settings
(F1), Friend Book (F2, friends/total), Journal (F3), Jukebox (F4), Gallery
(F5, friends on display). LT / RT go round the books, LB / RB round the
left tabs, Y opens or closes the Friend Book; in a book, F1 (or the footer's
"F1 Back to Settings") goes back to Settings. The notebook sits 62 units
further left so the bookmarks fit.

## A1. Journal (built, partly "Coming soon")

From the save (the loaded GD copy in memory, pointer at `0x804BA694`):

| What | Where | State |
| --- | --- | --- |
| Play time | u32 seconds at `+0x1470` (the game's save screen shows 465 as 0:07) | shown |
| Berries | u32 at `+0x1474` | shown |
| Friends | friendship u16 at `+0x28` of each Pokémon's entry (`hle_friends.c`) | shown |
| Where | area, zone at `+0x14A6` (Cove Town `01 02`; named by `PlaceName.msbt` `Fd<area><zone>`) | milestones |
| Attraction best scores, medals | not found | "Coming soon" |

Counted by Wonderpark from now on (`runtime/hle/hle_journal.c`, in
`%APPDATA%\Wonderpark\journal.json`, per save file): play time per day
(game time, not paused), attractions played (the AttractionId the scene
was entered with), milestones with their day (first friend and who, each
area first reached, 50/100/150/194 friends). The save file loaded is the
last `GD<n>` the game opened (a save written is certain; `ios/nand.c`).
Friends and areas a save already had when Wonderpark first saw it are taken
as they are (no milestones for them).

Not found yet ("Coming soon" on their stickers): jumps, Thunderbolts,
photos taken, distance. The game's Pad has no records screen, so there are
no names for the counters at `+0x1194`..`+0x11B8` (they grow as one plays,
even before any attraction: likely game stats, unidentified). Ways in: the
player's state machine for jumps and Thunderbolts (not found), or the CRI
sound cue each plays (the cue starts at `0x8039955C`, `hle_voice_pitch.c`;
cue names would need the ACB's cue table). Distance: the follow camera's
point (`hle_camera.c`) moves with the player, but the game's unit isn't
known, so no distance is shown.

## A2. Jukebox (not built: "Coming soon")

Step 0 not done yet. To find: the music cues (49 BGM banks, `S_BGM_Fd*`,
`S_Fd_BGM_*`, `S_Field_BGM`, `S_AT_*_BGM`; `docs/disc-inventory.md`), how a
cue starts (CRI AtomEx, `criAtomExPlayer_Start` `0x8039955C`) so "heard"
can be recorded, and how to play one from the F1 menu while the game is
paused (a CRI player of our own on the game's thread, or decoding the
streams ourselves: the game's clock is stopped while F1 is open).

## A3. Gallery (built with the fallback)

A grid of every befriendable Pokémon per area; one not befriended is a
black circle with "???" (no name, can't be focused or clicked). A friend
opens the viewer: its icon at large size on a pedestal, backgrounds Park /
Studio / Night drawn by us, "3D view coming soon". The 3D model (loading a
Pokémon's model and animations from the disc into an offscreen view while
the game is paused) wasn't attempted: the game's renderer runs on its
paused thread, and our GX replay would need the model's own display lists.
Animation and "Save a picture" are "Coming soon".

## B1. Get mods ("Coming soon")

The page says "The mod list isn't online yet"; its address is `[mods]
index_url` in `launcher.ini`. The checklist for adding a mod to the list is
`docs/MODS-INDEX.md`.

## B2. Saves (built)

See `docs/DEVELOPMENT.md` (the launcher). Saves stay where the game keeps
them, `%APPDATA%\Wonderpark\nand\title\00010000\53324c45\data` (the game's
two files, GD0 and GD1, not three); backups in
`%APPDATA%\Wonderpark\backups\saves\`.

## B3. HD Pack ("Coming soon")

- Texture names: a replacement is found by the hash of the texture's bytes
  as the game hands them to the GPU (`gx_fifo.c` `hash_bytes` over the
  image, XORed with the palette's hash for C4/C8/C14X2), so a pack can be
  made from the disc's own TPL and BRRES textures without playing, if they
  are hashed the same way (their bytes are the same as in memory).
- Still needed: an extractor for every texture on the disc (TPL files and
  BRRES TEX0/PLT0 in the `.dac` archives), the GX format decoders (the
  runtime's `gx_decode_texture`), alpha upscaled separately, small and
  dithered textures and fonts skipped, mipmaps rebuilt.
- Upscaler: Real-ESRGAN's `realesrgan-ncnn-vulkan`; its licenses (the
  program and the model files) still have to be checked before it's
  downloaded, so nothing was downloaded or tested yet. No 50-texture test.
