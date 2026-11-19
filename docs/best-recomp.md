# Best recomp: step 0 findings (2026-11-18)

From the "best recomp" plan, step 0 (research only, no
code changed). MildRain479/PokePark-2-Wiicompiled (CC0, version 0.1.2) is
cloned in `refs\PokePark-2-Wiicompiled` (the project keeps its clones in
`refs\`, not `D:\ref\`); read: its README, `recomp/README.md` and
`recomp/patches/` (shared and `S2LE01/`). eliasm4r's GPL-3.0 recomp was not
read (nothing in this step needed it).

## One correction to the roadmap

The minigame list in item 3 (Hurdle Bounce, Daring Dash, Speed Slam, Vine
Swing, Swing-Along, Boulder Bash, Aqua Dash, Snow Slide, Circle Circuit, Air
Race, Bumper Burn, Balloon Panic, Spooky Shoot-'em-Up, Panel Crush) is
**PokéPark 1's**. None of those names is in PokéPark 2's message files on the
US disc (Cake Contraption and Flight of Fancy are), and MildRain's PokéPark 2
table (`S2LE01/minigames.h`) has only the four attractions. Their
`hle_motion.c` mentions those games because it's shared with their PokéPark 1
port. So for us, item 3 means the four attractions (plus whatever motion the
park's own skill games use: not found to need any yet).

## Feature table

| # | Feature | Wonderpark | MildRain head start |
| --- | --- | --- | --- |
| 1 | Local multiplayer, 1-4 remotes | **No.** Remote 0 only (`hle_wpad.c` `connected()`: `chan == 0`), though its per-channel state is already 4 wide | **No** (single player) |
| 2 | English / Japanese voices | **Yes** (2026-11-18): from the player's own Japanese disc; the shared Pokémon voice bank stays English | No |
| 3a | Cake Contraption without motion | **Yes**: pointer by mouse or right stick (Auto), A shoots | Same (`hle_motion.c`) |
| 3b | Dance Inferno without motion | **Partial**: the shake keys dance whatever move is due; move icons show the shake button; timing offset setting | **Yes**: a key per move, drawn on its icon, held to the bar's end (`S2LE01/hle_dance.c`, `minigames.h`) |
| 3c | Power Bash without motion | **Yes** (2026-11-18): each shake-key press swings three times on all axes (`hle_wpad.c` bash_swing); a scripted round scored 8,355 (33 hits) where the old swing scored 0 | **Yes**: a swing burst per press, any direction (`hle_motion.c` `USE_ANY_WAY`). The "several bursts per press" finding is not in theirs |
| 3d | Flight of Fancy without motion | **Yes** (2026-11-18, adapted from theirs) + Steering sensitivity | Done (`hle_motion.c`) |
| 3e | Late pose presses still GREAT (Dance) | No | No |
| 4 | Optional gyro aiming | **No**: we read pads through XInput, which has no gyro | **No** (they use SDL3, which could read gyros, but don't) |
| 5 | Easiest setup, offline | **Partial**: one launcher, offline once installed, but the player needs Python 3, Visual Studio Build Tools (with Clang) and `dtk.exe`, all downloaded first, and a ~20 min compile | No (they ship a prebuilt program; our rule of never shipping game-derived code rules that out) |
| 6 | Mods | **Yes**: zip install and on/off manager, file and texture replacement (our own texture hash naming) | Partial: Dolphin-format texture packs (`boot/texpack.c`), which ours can't read |
| 7a | Interpolated 120/144/165/240 | **Yes** (120, 144, 240 in F1; 165 in settings.ini) | Same machinery (ours came from their PokéPark 1 runtime) |
| 7b | MSAA + anisotropic | **Yes** | Yes |
| 7c | Ultrawide | **Yes** (up to 32:9, HUD kept in the middle) | Yes |
| 7d | Save import/export (Dolphin / Wii) | **Partial**: same NAND layout, so folders copy over by hand; no in-game import/export, no Wii `data.bin` | **Yes** (`boot/save_import_win32.c`, `boot/wii_save.c`) |
| 7e | Button prompts in your keys | **Yes**, plus attraction icons and controller type | Yes; also rewrites the How to Play and move instructions to name your keys (`S2LE01/hle_text.c`), which we don't |
| 7f | Real Wii Remote | No | No |
| 7g | Dialogue speed-up | **Partial**: the game's Normal/Fast and the speed-up key | Yes: 1x-3x or a page at once |
| 7h | Cutscene skip | **No** (`skip_movies` not working) | **Yes**: hold + half a second (`S2LE01/hle_game.c` `skip_movie`) |
| 7i | Save backups | **Yes** (autosave shelved: it corrupted a test save) | Autosave yes, on friend made / area entered (`S2LE01/hle_autosave.c`); backups no |
| 8a | Native Linux / Steam Deck | No | No (Proton only) |
| 8b | EU / JP discs | No | No |
| 8c | Android | No | **Yes** (the whole Android side: `boot/*_android.c`, `gpu/window_android.c`, Vulkan) |

## Recommended first three

1. **Controllers through SDL3** (medium). We read pads with XInput, so a
   PlayStation or Switch pad only works through Steam Input or DS4Windows,
   a gyro can't be read at all, and the pad type is guessed from the
   device list. SDL3 (zlib license) fixes all three, and it's the base the
   next two need: one SDL gamepad per player, and gyro later. MildRain's
   `boot/input_win32.c` shows the SDL3 side. Needs your OK on the
   dependency: SDL3 shipped as source in `tools/` (built offline with the
   game) keeps setup offline.
2. **Local multiplayer, 1-4** (large). Nobody has it. Remotes 1-3 connected
   with their own reports and motion state, each fed by its own pad (or
   the keyboard), players assigned in the launcher and F1, prompts per
   player, and every attraction's no-motion controls per player. First a
   check of how the attractions pick players (their "multi" result screens
   exist: `MnAtResultMulti.brlyt`).
3. **The attraction and polish gaps MildRain already solved** (small to
   medium each, CC0, credited): Dance Inferno's key per move
   (`hle_dance.c`), Power Bash's burst per press (`hle_motion.c`, plus our
   own extra bursts so none is missed), the rewritten instructions
   (`hle_text.c`), hold + to skip movies (`hle_game.c`), and analog
   movement (below).

## In their code that fixes gaps we already have

- **Analog movement** (`patches/hle_stick.c`, 75 lines): the left stick
  moves Pikachu in any direction by replacing the d-pad's eight directions
  in its control module; we still turn the stick into eight d-pad
  directions. Small.
- **Movie skip** (`S2LE01/hle_game.c`): our Skip movies setting doesn't
  work; theirs skips a non-looping movie when + is held half a second.
- **Instructions naming your keys** (`S2LE01/hle_text.c`): the game still
  tells our players to shake, tilt or point the remote.
- **Wii save import/export** (`boot/wii_save.c`, `boot/save_import_win32.c`),
  including a Wii's `data.bin` without Dolphin.
- **Autosave** (`S2LE01/hle_autosave.c`): ours, ported from an earlier
  version of it, corrupted a test save; theirs saves on a friend made or an
  area entered, which may avoid the moment that broke ours. Worth a second
  look.
- **Dolphin texture packs** (`boot/texpack.c`).
- **Text speed 1x-3x / page at once**, in their game patches.
- Already matched, nothing to take: resource lookups (`hle_lookup.c`; ours
  is `hle_string.c`), the camera, friends list, battle and chase options,
  prompts, minimap marking.
