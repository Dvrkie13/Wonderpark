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

Jumps and Thunderbolts (2026-11-21) are counted from the Wii Remote
buttons the game gets (`g_wpad_held`), in the park outside story scenes and
talks: a press of 2 is a jump, 1 held half a second or more and let go is a
Thunderbolt (a quick press is a dash attack). Counted as pressed: a press
the game refused (mid-air) still counts. Saved as `"moves": [jumps,
thunderbolts]` per save file; `WONDERPARK_LOG=journal` logs button 1.
Checked: a 1.5 s hold counted one, a 0.1 s press none.

Not found yet ("Coming soon" on their stickers): photos taken, distance. The game's Pad has no records screen, so there are
no names for the counters at `+0x1194`..`+0x11B8` (they grow as one plays,
even before any attraction: likely game stats, unidentified). Ways in: the
player's state machine for jumps and Thunderbolts (not found), or the CRI
sound cue each plays (the cue starts at `0x8039955C`, `hle_voice_pitch.c`;
cue names would need the ACB's cue table). Distance: the follow camera's
point (`hle_camera.c`) moves with the player, but the game's unit isn't
known, so no distance is shown.

## A1b. Stickers (built, partly "Coming soon")

The Journal's fourth tab (`runtime/host/settings_ui.cpp`, `kStickers`): 33
round stickers in our own words, in groups. Places (the ten places with
their own names from the disc: befriend everyone there), Areas (Cove,
Arbor, Crag, Tech, Wish Park), Friends (50, 100, 150, all 194), Attractions
(gold in each, gold in all four), Collector (10,000 and 100,000 berries,
100 photos, 42 km, 1,000 Thunderbolts, 7 days played, every song in the
Jukebox), and two secrets ("???" until earned: playing in the park after
midnight, and before 6 in the morning). Earned: full colour with its day;
locked: a dashed outline with its goal. The attraction golds, photos and
the marathon say "Coming soon" and can't be earned (their numbers aren't
found in the game yet).

Worked out about once a second while the game runs, from what the Journal
already has (friends and berries from the save, Thunderbolts and days
counted, songs heard). Kept per save file in `journal.json`
(`"stickers": [[day, number], ...]`, by number: never renumbered). The
first look at a save in a run takes what it already has quietly; a sticker
earned after that slides in at the top of the screen for 3 s ("Sticker
earned!") with a soft two-note chime made in code (mixed over the game's
sound, at the Sound effects volume) and is added to Milestones. Checked:
with 999 Thunderbolts counted, one held press in the park earned "Power
surge", its card and chime (the chime's note found in the sound output at
that moment) and its milestone. Local only.

## A2b. Cries in the Gallery (built)

The viewer's "Play cry" plays the Pokémon's own cry from the player's disc
(`runtime/host/cries.cpp`; the bank is described in `docs/research.md`,
"Pokémon cries"), at the Pokémon voices volume, over whatever plays. 195
characters have one; for the others the button isn't shown. The Japanese
voices don't replace this bank, so it's the same in both languages.
Checked: the cries decode to sound (Piplup: 0.83 s, RMS about 8,600);
not yet listened to by a person.

## A2. Jukebox (built)

`runtime/host/jukebox.cpp`. The music is CRI ADX (type 3, 4-bit, all 32 kHz
stereo) inside the 24 music stream archives
(`files/Sound/*BGM*_streamfiles.awb`, CPK containers): 94 songs (a 95th,
mono with no samples, is left out). Each song is found by its ADX header
(`0x80 0x00`, the copyright offset pointing at "(c)CRI"), and the next one
just past its data: a few small reads per file, on a thread of its own,
when the Jukebox first opens. The game's sound engine is stopped while the
notebook is open (the game is paused), so the Jukebox decodes ADX itself
(the usual ADPCM: coefficients from the header's high-pass frequency) on a
thread of its own and queues it for the sound device at the Music volume,
about 150 ms ahead; closing the notebook stops it and the game's music goes
on. Checked offline: a decoded song has an RMS of about 3,500, nothing
clipped, centred on 0 (music, not noise); not yet listened to by a person.

Heard: the game streams a song from its archive as it plays it, so a disc
read inside a song's data (`ios/dvd.c` read_regions) marks it heard
(`jukebox_heard.ini` in the data folder: the 2 KB-rounded offsets read in
each music file). Songs not heard yet are "???" and can't be played.

Titles are our own, from where each plays: the place name from the disc
(`PlaceName.msbt`, "Cove Town 2"), the attraction by its AttractionId
(`S_AT_00` Cake Contraption, `02` Power Bash, `03` Flight of Fancy; Dance
Inferno's music is in a bank without a stream, not listed), and "Park
moments N" for the shared `S_Field_BGM` (events, chases, battles; not told
apart yet). No titles from soundtrack sites. Tabs: All, Park, Wish Park
(areas 06 and 61), Attractions, Story (`S_Field_BGM`, area 11).

Controls: previous, play / pause, next (among heard songs), shuffle, repeat
(round again from the song's own loop start).

"♥ Launcher" (2026-11-21): each heard song has a heart (click it, or Right
then Enter on the song's line); marked songs are the only ones the
launcher's music plays (`jukebox_launcher.ini`, songs as `<bank>:<index>`).
The game also writes the songs' titles once a run to `jukebox_titles.ini`
for the launcher's "now playing" line. Both in the data folder.

## A3. Gallery (built, with the 3D view)

A grid of every befriendable Pokémon per area; one not befriended is a
black circle with "???" (no name, can't be focused or clicked). A friend
opens the viewer: backgrounds Park / Studio / Night drawn by us, and the
Pokémon's own 3D model on a pedestal, turning slowly (drag on the stage
to turn it; the mouse wheel zooms, 0.6x to 1.8x), playing Idle, Happy,
Walk or Random (those it has). "Save a picture" writes the stage as shown
to `Pictures\Wonderpark\<Pokémon> <date> <time>.png`: the renderer reads
that square back after drawing a frame (`gl_backend.c`
gl_picture_capture) and a thread of its own writes the PNG (stored, no
compressor; never on the game's or the renderer's thread). The button
says "Saved ✓" (or "Couldn't save") for 3 s. The picture is the player's
own and stays on their PC; it shows the game's model, so it isn't for
the repo.

The 3D view (`runtime/host/gallery3d.cpp`, drawn by `gl_backend.c`
draw_gallery3d) doesn't use the game's renderer (paused with the game):
it reads the model itself, on a thread of its own, from the extracted
disc:
- Where: `files/Pokemon/<Name>.brres`, or `<Name>.brres` in
  `files/Pokemon/Res00-03.dan` (U8 archives; only their file tables and the
  model's bytes are read). `<Name>` is the character name the icon has
  (`Ic<Name>.tpl`). 238 models; 5 icons are named a little differently
  from their model (Bassrao: BassraoR/B, Dustdas: Dustdus, Minomucchi:
  Minomucchi000, Onondo: Onond, Pururill: PururillF/M), so the nearest name
  is taken (one that starts with it, not the FR or WP variants, else the
  longest shared start).
- What (checked 2026-11-19 on every one of the 238: all parse, all have
  NorIdle0; 226 Joy0, 204 Walk0, 212 Random00): MDL0 v11 bones (bind SRT,
  parent, inverse bind at +0xA0), vertex/normal/colour/UV arrays (each
  array's own index at +0x10), materials (cull at +0x18, the first layer's
  texture name and wrap), the Definitions' draw lists (op 4: material,
  object) and envelopes (op 3: node, weights), objects (VCD at +0x0C/+0x10,
  the primitive display list at +0x24/+0x28/+0x2C, array ids at +0x48;
  LOAD_INDX_A (0x20) commands map PNMTXIDX slots to nodes). Single-bound
  vertices are in their bone's space; weighted ones in model space
  (skinned with world x inverse bind). TEX0 decoded with gx_texture.c.
  CHR0 v5: per bone scale / rotation / translation flags (has 22-24,
  isotropic 4-6, fixed 13-21, formats at 25 / 27 / 30), i4 / i6 / i12
  Hermite keys and linear rotations (4, 5, 6). Rotations X, then Y, then Z.
- The face: the eye and mouth textures are sheets of expressions; the
  default face is frame 0 of the SRT0 FaceDef, which (in the models
  checked) shifts by 0, so the model's own UVs are used. Expressions in
  the animations (SRT0) aren't applied: the face stays the default.
- Walk carries the model forward (Piplup: 1.76 units a cycle); in the
  Gallery the top two bones' animated X and Z translation are left at
  rest, so it walks on the spot. Rotations are blended the short way.
- Drawn on the CPU (skinned, lit by one light, projected), into a
  framebuffer the stage's size, laid over the notebook's picture there,
  every present while paused. Framed so the whole Pokémon fits, its feet on
  the pedestal's ring.

## B1. Get mods ("Coming soon")

The page says "The mod list isn't online yet"; its address is `[mods]
index_url` in `launcher.ini`. The checklist for adding a mod to the list is
`docs/MODS-INDEX.md`.

## B2. Saves (built)

See `docs/DEVELOPMENT.md` (the launcher). Saves stay where the game keeps
them, `%APPDATA%\Wonderpark\nand\title\00010000\53324c45\data` (the game's
two files, GD0 and GD1, not three); backups in
`%APPDATA%\Wonderpark\backups\saves\`.

## B3. HD Pack (built)

`builder/launcher/hdpack.cpp`; how it works is in `docs/DEVELOPMENT.md`
(HD Pack). Decisions (2026-11-19, the owner):

- Real-ESRGAN's models were dropped: their weights state no license. The
  owner asked for a model with an explicit permissive license; neither
  4x-PBRify_RPLKSRd_V3 (CC0, RealPLKSR, `.pth` only) nor
  4xNomosUni_span_multijpg (CC BY 4.0, SPAN) runs on realesrgan-ncnn-vulkan
  (no ncnn files from their authors). Chosen: **ONNX Runtime + DirectML
  with 4xNomosUni_span_multijpg**, whose author publishes an ONNX file.
  Neither release lists a SHA-256, so the hashes were taken from the
  downloads of 2026-11-19 and pinned.
- DirectML: the NuGet package Microsoft.AI.DirectML is under Microsoft's
  own license (not open source, 193 MB), so it isn't downloaded; ONNX
  Runtime uses the DirectML that ships with Windows (it worked on the test
  PC's Windows 11), else the processor.
- Hashes from the disc, checked against the game: of the 55 textures the
  game drew in its first 90 s (title and menus), 50 match a texture on the
  disc exactly; the other 5 are C8 title-screen TPLs whose hash isn't
  understood yet. To make palette textures predictable, the renderer now
  hashes only as much palette as was loaded (C8 textures with fewer than
  256 colours used to include leftover palette memory).
- Not done: mipmaps for the replacements (they're drawn with one level,
  as every mod texture is); dithered textures aren't told apart.
