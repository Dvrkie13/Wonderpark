# Developing Wonderpark

## Where each kind of change goes

| Change | Goes in | Never in |
| --- | --- | --- |
| How the **game** behaves (frame rate, bloom, a bug in the game, a gameplay option) | `config/patches/*.c` (see its README) | the generated code, `runtime/` |
| How the **PC** runs it (graphics, sound, input, files, settings, UI, emulated hardware) | `runtime/` (one README per module) | `config/patches/` |
| Names for game/SDK functions | `config/S2LE01/symbols.txt` (from `tools/analysis/sigmatch.py`) | |
| Recompiler review notes (self-modifying-code flags) | `config/S2LE01/smc_review.toml` | |
| Disc, recompile and build steps | `tools/`, `builder/pipeline.py` | |
| The launcher | `builder/` | |

The generated code (`generated/`) is rebuilt from scratch by
`tools/recomp.py` and must stay reproducible: don't edit it. Hooks
(`WP_HOOK`) are inserted by `tools/recomp.py`, so after adding or moving one,
run it again.

Nothing game-derived goes into git (`game/`, `generated/`, `build/` are
ignored). See `CLAUDE.md`.

## Commands

```
python tools/extract_disc.py <disc.iso>      # verify (ID, revision, hash) and extract to game/
python tools/recomp.py                       # DolRecomp: game/sys/main.dol -> generated/
python tools/build_game.py                   # build build/game/Wonderpark.exe (clang-cl; --msvc for MSVC)
python tools/pgo_train.py                    # PGO: instrumented build, train on a park and a beach save, merge to build/pgo/ (~35 min)
python tools/build_game.py --no-pgo         # without the PGO profile (used by default when build/pgo/wonderpark.profdata exists)
python tools/build_game.py --no-lto         # without ThinLTO (on by default when lld-link.exe and llvm-lib.exe are beside clang-cl; ~13 min instead of ~18)
python tools/build_game.py --build-dir game-x --cflags "/arch:AVX2"   # a variant in build/game-x, for comparisons
python builder/pipeline.py <disc.iso>        # all of the above, with progress for the launcher
python builder/pipeline.py --rebuild         # recompile and compile again from the extracted disc in game/
python builder/pipeline.py --status          # @status ok|missing|outdated (is build/game current with the sources?)
python tools/build_launcher.py               # build build/launcher/WonderparkLauncher.exe (the launcher, MSVC)
.\build\launcher\WonderparkLauncher.exe      # the launcher: place the disc, build, play
.\build\game\Wonderpark.exe                  # run (from the repo root)
.\build\game\Wonderpark.exe --headless --exit-after 60   # automated run, prints a summary
```

## Command line

| Option | What it does |
| --- | --- |
| `[path]` | main.dol to run (default `game/sys/main.dol`) |
| `--headless` | no visible window, no sound device, no input |
| `--exit-after N` | stop after N real seconds: frames, average fps, retraces |
| `--prepare-shaders` | the build's last step ("Preparing shaders…", `builder/pipeline.py`): runs the boot hidden and silent at game speed x10 for 8 s, through the title, with a data folder of its own (`cache\prepare\` beside the exe), no mods and no preloading, then exits. The graphics driver compiles each new shader and draw state the first time it sees one (AMD: ~0.45 s for the ubershader, then ~0.4 s pauses at the first frame and at the title) and keeps them in its own cache; after this step the player's first start has none of those pauses. Areas past the title can still pause once the very first time (the driver meets new draw states there) |
| `--help` | usage |

## The launcher (`builder/launcher/`)

`WonderparkLauncher.exe` is a native Win32 window drawn with Direct2D and
DirectWrite (part of Windows; no runtime to install). It matches
`docs/launcher-mockup.html`; everything is drawn in code, and the only
files built into it are the Fredoka and Nunito fonts (SIL OFL,
`builder/launcher/fonts/`). It contains no game code or data, so the
executable can ship in a release.

- It finds the Wonderpark folder by looking upwards from itself for
  `builder/pipeline.py` (or `--root <dir>`, or `WONDERPARK_ROOT`).
- It runs `builder/pipeline.py` with Python (`WONDERPARK_PYTHON`, else
  `python.exe` on PATH, else `py -3`) on a worker thread, in a Windows job
  object (stopping the build or closing the launcher ends every process
  of it). Each build's output goes to `build/logs/build-<date>-<time>.log`
  ("Show log" after an error opens it).
- At start it asks `pipeline.py --status`: a current build shows Play, an
  out-of-date one (the sources changed) Rebuild, none Place ISO.
- The disc check on "Verifying…" (game ID S2LE01, revision 0) is done by
  the launcher itself from the image's header; the full image hash is
  part of the build's first stage.
- Esc, or closing the window, during a build asks whether to stop it.
- The Japanese disc (S2LJ01), dropped or picked, isn't built: its Pokémon
  voices are copied for Audio > Voice language (`tools/extract_voices.py`,
  output in `build/logs/voices-ja.log`; it needs dtk like the build), and the
  launcher says when they're ready. `--test-script` has `drop` for the
  `--test-iso`.
- Controller A, Enter and Space press the main button. With Windows'
  "Animation effects" off (or `--reduced-motion`), nothing moves.
- "Click the logo" is a label in the top-left corner (not clickable).
- While a controller is connected, its A ("Choose") and Y ("Mods") show in
  the top-right corner as that pad's own icons (`runtime/host/pad_icons_core.cpp`,
  shared with the game; the icons are built in from `runtime/host/pad_icons.rc`).
  `WONDERPARK_TEST_PAD=1` (tests) shows them without a pad.

### The Mods page

The MODS tab on the right edge (with the number of installed mods) opens
it; so do M, Tab then Enter, a controller's Y or d-pad right, or dragging a
file onto the window. X, Esc or the controller's B close it. Up/Down (or
Tab, or the d-pad) move between the drop zone, the close button and the
switches; Enter, Space or A choose. The mouse wheel scrolls the list.

- **Install from .zip** (click for a file picker, or drop a .zip anywhere
  on the window) runs on a worker thread with stages Reading zip…,
  Checking mod.json…, Copying files…, Done (`builder/launcher/mods.cpp`).
  The new mod goes to the top of the list, switched on.
- A mod is a .zip with `mod.json` `{id, name, summary, version, author,
  game: "S2LE01"}` at its root or in one top folder, `textures\` and
  `files\`. Only those are copied; nothing else in the zip is used (no code
  mods). Without a mod.json, the name comes from the file name and the
  summary is "No description included with this mod."; installing the same
  id again replaces the old copy.
- Refused, with a message: not a .zip, damaged or not a zip, paths that are
  absolute or contain `..`, encrypted entries, zip64, other compression than
  stored/deflate, a file over 512 MB, over 2 GB in all, over 20,000 entries,
  entries that unpack to over 1000x their size, a mod.json for another game.
- Installed to `%APPDATA%\Wonderpark\mods\<id>\` (through a
  `.install-<id>` folder, moved into place at the end); on/off and order in
  `mods.ini`. Switches apply the next time Play is pressed.
- The game (`runtime/host/mods.cpp`) reads `mods.ini` at start and uses only
  the mods switched on, the first listed winning where two change the same
  thing. `files\<path>` replaces the disc's `files/<path>` (the FST in
  memory gets its size; a larger file is placed past the end of the disc's
  data). `textures\<hash>.png` (or `<hash>_anything.png`; PNG, BMP or JPEG,
  up to 8192x8192) replaces the texture with that hash, the one
  `RECOMP_GX_DUMP_TEXTURES=<dir>` names its dumps by; it's read on a
  background thread and shown from the frame after it's ready. Replacing
  only: a mod can't add new disc files.

Test options: `--test-iso <file>` (instead of the file picker), `--test-zip
<file>` (instead of the Mods page's file picker) and
`--test-script "<seconds>:<action>,..."` with actions `logo`, `press`,
`cancel` (no question), `shot=<file.png>` (saves the next frame),
`wait-state=ready|built|noiso|building`, `wait-progress=<0-100>`, `mods`,
`mods-close`, `install=<file>`, `toggle=<n>` (the n-th card's switch, from
0), `wait-install=<0-100>` (101: until it ends), `quit`;
`+N` times are after the previous action. `WONDERPARK_DATA_DIR` moves
`mods\` and `mods.ini` too. `WONDERPARK_LAUNCHER_STATS=<file>`
writes the average time per frame spent drawing, in `EndDraw` and in
`Present`. It draws 30 frames a second while idle and 60 after input and
during transitions: about 4% of one core on the menu (0.2% with reduced
motion). Fixed shapes are drawn from geometry realizations; flattening
them every frame was most of its CPU time.

## Files

| File | Where | What |
| --- | --- | --- |
| `settings.ini` | `%APPDATA%\Wonderpark` | options |
| `bindings.ini` | `%APPDATA%\Wonderpark` | keyboard and controller bindings, two each per Wii Remote button |
| `nand\` | `%APPDATA%\Wonderpark` | the emulated NAND: saves (`title\00010000\53324c45\data`) and system settings |
| `mods.ini` | `%APPDATA%\Wonderpark` | the installed mods in order, `id = 1` on / `0` off (written by the launcher, read by the game at start) |
| `mods\<id>\` | `%APPDATA%\Wonderpark` | installed mods: `mod.json`, `textures\`, `files\` |
| `shadercache\gl.bin` | `%APPDATA%\Wonderpark` | only with `WONDERPARK_SHADERS=spec`: the TEV setups seen, compiled again in the background at each start (numbers, made on the player's PC; starts over when the shader source changes; safe to delete) |
| `cache\nandtmp\` | beside `Wonderpark.exe` | the NAND's `/tmp` (the game copies ~41 MB there at each start; emptied at start) |

## Settings

Each can be set in `settings.ini` or overridden by the environment
variable shown (the variable wins; an overridden value is never written to
the file).

| settings.ini | Environment | Values | Status |
| --- | --- | --- | --- |
| `[display] fullscreen` | `WONDERPARK_FULLSCREEN` | 0, 1 (borderless; Alt+Enter toggles) | works |
| `[display] scale` | `WONDERPARK_SCALE` | 0 (Auto: fits the window, at most 4x, the highest measured; a 1440p window gets 3x), 1-6 internal resolution | works |
| `[display] msaa` | `WONDERPARK_MSAA` | 1, 2, 4, 8 | works |
| `[display] aniso` | `WONDERPARK_ANISO` | 1, 2, 4, 8, 16 | works |
| `[display] renderer` | `WONDERPARK_RENDERER` | opengl, vulkan | only OpenGL is built |
| `[display] frame_rate` | `WONDERPARK_FRAME_RATE` | 30, 60, 120-240 | 30 and 60 work, at once (F1 or the Options page): at 60 the game's frame timer runs at 59.94 Hz instead of 29.97 and draws every 60 Hz logic step; movies stay at 30, and it drops to 30 for 15 s when frames keep coming late (`runtime/hle/hle_frame.c`). 120 and up work (F1 offers 120, 144, 240; the Options page too): the game still runs at 60 and the frames between are drawn by `runtime/gpu/gx_frames.c` (from the reference runtime): each of the game's frames is recorded and drawn again with each draw's matrices blended from the frame before, one game frame behind; drawn while the game waits (`runtime/core/idle.c`, which was missing: before it, recording showed nothing), never more than the display's refresh rate (`RECOMP_IGNORE_REFRESH=1` for measuring). Title screen, dev build: 120.0-120.9 frames shown a second. A game frame no in-between frame showed (the game too busy to wait) is drawn as the next one ends, so the picture never falls below the game's own rate |
| `[display] fov` | `WONDERPARK_FOV` | 70-150 | works, at once: the game's world cameras are widened where they're applied (`0x80019B20`) and where their frustum is worked out for culling (`0x8001A390`), so nothing pops in; in the park and its story scenes (`runtime/hle/hle_camera.c`, from MildRain479/PokePark-2-Wiicompiled, CC0) |
| `[display] fov_attractions` | | 0, 1 (default 0) | 1: the field of view in attractions too (their world cameras; their HUD is flat and stays) |
| `[gameplay] skip_movies` | `WONDERPARK_SKIP_MOVIES` | 0, 1 | not working yet (see "Skip movies" in `docs/recomp-status.md`) |
| `[gameplay] berries`, `prices` | `WONDERPARK_BERRIES`, `WONDERPARK_PRICES` | 50-500 %, 0-200 % | work, at once: the Berry count (save +0x1474) is watched each frame; a rise is scaled by `berries`, a drop by `prices` (shops still show and need their own price). A frame where much of the save changed (a load, a new game) isn't scaled (`runtime/hle/hle_balance.c`) |
| `[gameplay] damage_dealt`, `damage_taken` | `WONDERPARK_DAMAGE_DEALT`, `WONDERPARK_DAMAGE_TAKEN` | 25-400 %, 0-200 % | in battles, at once: the damage a Pokémon's LifeModuleImpl takes (`0x800B0200`) is scaled, by "taken" when it's the player's (`runtime/hle/hle_modifiers.c`, from MildRain479/PokePark-2-Wiicompiled, CC0). Not yet tried in a battle (none reachable by script so far) |
| `[gameplay] chase_speed` | `WONDERPARK_CHASE_SPEED` | 50-150 % | in chase games: the chased Pokémon's target and top speed and acceleration are scaled for its movement update (`0x800A9340`) while its chase action runs (`0x801E1210`). `WONDERPARK_CHASE_ALL=1` (tests) applies it to every Pokémon but the player |
| `[gameplay] text_speed` | `WONDERPARK_TEXT_SPEED` | | from the PokéPark 1 runtime; not used (Text speed is the game's own, in the F1 tab) |
| `[updates] check` | `WONDERPARK_CHECK_UPDATES` | 0, 1 | no update check built yet |
| `[mouse] sensitivity` | `WONDERPARK_SENSITIVITY` | 0.1-3 | the mouse's camera turning (0.3 degrees a pixel at 1) and pointer speed |
| `[mouse] invert_tilt` | `WONDERPARK_INVERT_TILT` | 0, 1 | the camera's up and down, mouse and stick |
| `[controller] rumble` | `WONDERPARK_RUMBLE` | 0 off, 1 light, 2 normal, 3 strong | |

The PC settings of the F1 tab (`runtime/host/settings.c`, table `kX`; each
also `WONDERPARK_<KEY>`, key in capitals, for example `WONDERPARK_WINDOW_MODE=1`):

| Setting | Values | State |
| --- | --- | --- |
| `[display] window_mode` | 0 windowed, 1 borderless, 2 fullscreen | works (replaces `fullscreen`, still read from old files). Fullscreen sets the screen to the resolution setting's size while the game is in front (Match screen: the screen's own, as borderless); Alt+Tab gives the desktop its size back and minimizes, and it's put back on exit (`runtime/gpu/window_win32.c`) |
| `[display] resolution` | window size: 0 1280x720, 1 1600x900, 2 1920x1080, 3 2560x1440, 4 match screen (maximized) | works, in a window and in fullscreen |
| `[display] vsync` | 0, 1 (default 0) | works, at once: presents wait for the screen's refresh (swap interval 1, or DXGI sync interval 1 without tearing). Off by default, as before: a window never tears, and waiting can hold up the game thread (`runtime/gpu/gl_backend.c`) |
| `[display] brightness` | 50-150 % | works, at once: a gamma curve on the shown picture (whites stay white) |
| `[graphics] scaling_filter` | 0 smooth, 1 sharp | works |
| `[graphics] widescreen` | 0, 1 | works, from the next start (`RECOMP_WIDESCREEN` still wins): the game's own 16:9 mode (it is told the TV is 16:9: `SCGetAspectRatio`, `runtime/hle/hle_os.c`). A window wider than 16:9 (up to 32:9) shows more of the world to the sides with menus and HUD kept in the middle; a narrower one (16:10) is cropped at the sides (`runtime/gpu/gx_fifo.c`, "Widescreen") |
| `[graphics] hud_edges` | 0, 1 | works, with widescreen: in game, HUD items at the sides (the minimap) move out to the screen's edges; menus stay as they are |
| `[graphics] fast_loading` | 0, 1 (default 1) | works, from the next start: a thread reads the disc's files into RAM at start, all but movies and streamed music and voices (949 files, 284 MB), the game's own reads first (`runtime/ios/dvd.c`, "Fast loading") |
| `[graphics] bloom` | 0, 1 (default 1) | works, at once: off skips the game's store of the effect's strength at `0x8011D91C`, as the known Gecko code does (`0411D91C 60000000`), and keeps the value it was made with (`runtime/hle/hle_effects.c`) |
| `[graphics] fog` | 0, 1 (default 1) | works, at once: off draws no GX fog (`runtime/gpu/gx_fifo.c`) |
| `[graphics] colors` | 0 original, 1 vivid | works, at once: more saturation (1.3x) and a little contrast on the shown picture |
| `[graphics] per_pixel_lighting`, `hdr` | | not yet |
| `[graphics] shader_mode` | 0 smooth, 1 fast, 2 precompile | from the next start: 1 (and 2, for now the same) turns on the specialised draw programs, as `WONDERPARK_SHADERS=spec` does (`gl_backend.c` spec_init; that variable still wins): less GPU time (Wish Park at 4x: 12 -> 5 ms a frame), but on AMD's driver the first draw with each new program blocks 50-300 ms. Precompile (waiting for the saved ones before the game starts) isn't built |
| `[controls] free_camera` | 0, 1 (default 1) | works (but see `docs/camera-and-extras.md`: frame rate dips while it keeps turning): in the park the mouse and right stick turn and tilt the follow camera (tilt -20 to 45 degrees); B (the game's look-around) eases it back behind the player; the eye is pulled in from walls and kept off the ground, for drawing only; another camera or scene starts over. Off eases back. Not in minigames (`runtime/hle/hle_camera.c`, adapted from MildRain479/PokePark-2-Wiicompiled's, CC0; the scene from `runtime/hle/hle_scene.c`). `WONDERPARK_MOUSE=<dx>,<dy>[,<updates>]` (tests) turns it by that many pixels an update (only for that many turned updates, when given) |
| `[controls] stick_sensitivity` | 10-300 % | works: the right stick's turning (600 mouse pixels a second at 100%) |
| `[controls] invert_x` | 0, 1 | works: the camera's left and right, mouse and stick (Mirror world flips it too) |
| `[controls] camera_recenter` | 0-30 s (0 never) | works: that long without turning, the camera eases back behind |
| `[controls] right_stick` | 0 camera, 1 pointer, 2 auto (default) | works: Auto moves the Wii Remote pointer with the right stick while the game has the pointer on (`hle_pointer_on`, its IR camera: aiming minigames, menus pointed at), else turns the camera; Pointer always (the camera then only by mouse). The push squared, so small pushes aim finely (`runtime/host/input_win32.c`). Not yet tried with a real controller |
| `[controls] aim_speed` | 25-200 % | works: the right stick's pointer speed (900 mouse pixels a second at full push, at 100%) |
| `[controls] aim_assist`, `steer_sensitivity`, `rhythm_offset` | | not yet |
| `[input] button_icons` (`WONDERPARK_BUTTON_ICONS`) | 0 auto, 1 Xbox, 2 PlayStation, 3 Nintendo, 4 keyboard only | works: the button icons in F1's Controls (pad columns), the game's prompts and the launcher. Auto: the connected pad's type, from the game controllers Windows lists (HID) by maker (Sony: PlayStation; Nintendo, and Hori/PowerA/PDP Switch pads: Nintendo; else Xbox), since XInput doesn't say; a PlayStation or Switch pad behind a mapper (Steam Input, DS4Windows) still lists itself. Looked at again when a pad connects or goes away. Face buttons go by position (XInput's A is the bottom one: a cross, or a Nintendo pad's B). The game's prompts show the device used last (any pad button or the stick past half; a bound key or mouse button), key caps for the keyboard; 4 never shows a pad's (`runtime/host/pad_icons.cpp`, `pad_icons_core.cpp`, `runtime/hle/hle_prompts.c`). The log says what was found ("pad icons: ...") |
| `[audio] volume_master` | 0-100 % | works |
| `[audio] audio_latency` | 0 low, 1 normal, 2 safe | works: 48, 84 (as before) or 132 ms queued before playback starts; lower at once, higher from the next refill (`runtime/host/audio_win32.c`) |
| `[audio] volume_music`, `volume_sfx`, `volume_voices` | 0-100 % | work, at once, through the game's own CRI categories (`runtime/hle/hle_volume.c`): music is BGM and JGL (jingles), voices PK_VOICE and PC_VOICE, effects the other five. The game's own category changes are kept and multiplied. Movies (the title's too) play their sound outside CRI and aren't affected |
| `[audio] voice_language` | 0 English, 1 Japanese | works, from the next start: with 1, the Japanese disc's voice banks in `game/voices/ja/Sound/` (copied once by `tools/extract_voices.py <Japanese disc>`, or by dropping that disc on the launcher) take the place of the US files the way a mod's do (they have other sizes, so the FST the game reads at boot is changed: `runtime/ios/dvd.c` voice_file; settings are now read before the disc opens, `main.c`). Copied: the 28 banks that differ from the US ones but have the same cue names, indexes and IDs (the starters' voices, the area and story banks), each with its `_streamfiles.awb`; the other Pokémon's shared bank `S_POKE_VOICE` stays English (its cues differ). The F1 row is greyed ("Needs your Japanese disc") until `voices/ja/voices.txt` exists. `WONDERPARK_LOG=mods` logs each Japanese file read. Checked: in Cove Town the game reads the Japanese `S_Fd0102`, `S_DEMO_COMMON` and the starters' voice banks |
| `[audio] mute_background` | 0, 1 | works: silent while another window is in front |
| `[gameplay] pause_background` | 0, 1 | works: the game pauses while another window is in front |
| `[gameplay] speed_up`, `speed_toggle` | 0 off, 1 2x, 2 3x; 0 hold, 1 toggle | works: Tab, or the controller's left trigger when no action is bound to it, runs the game's clock 2 or 3 times as fast (so it runs that many more of its fixed steps), only in the park and never in an attraction, a movie, or while an event script (story scene, talk) runs (`hle_event_running`, `runtime/hle/hle_scene.c`); silent meanwhile, as with F7. A speed set from the debug overlay is left alone (`runtime/host/input_win32.c`). How much faster it really gets depends on the CPU's headroom, since each extra step is a whole frame, drawing included: in Cove Town the optimised build is 65-69% busy at normal speed and went from 30 to 34 game frames a second at 2x (1.13x); the dev build is already 100% busy. A real 2x would need the in-between steps' drawing left out (not done). `WONDERPARK_SPEED_HELD=1` (tests) acts as if the key were held |
| `[gameplay] save_backups` | 0, 1 (default 1) | works: before the game writes a save slot (opened for writing, deleted, or replaced by a rename; `GD0`, `GD1`...), the file is copied to `backups\` in the data folder as `GD<n>-<date>-<time>.bin`, unless the newest backup of that slot is the same; the newest 10 per slot are kept. On the IOS thread (`runtime/ios/nand.c`). To restore one, copy it over `nand\title\00010000\53324c45\data\GD<n>` and `GD<n>M` with the game closed |
| `[gameplay] skip_logos` | 0, 1 | works: the Strap scene (the logos and the wrist strap warning, a fixed time) runs at 10 times the speed (our game speed, as F7), so the title menu comes about 12 s sooner (frame 952 instead of 1694). Not the "Loading..." screen after it (Hold): sped up, it took longer. Leaving Strap early once the park is loaded (`0x804BA89C` reaches 2; MildRain's `0x804BA8C8` stays 11 in ours) left a blank screen (`runtime/hle/hle_scene.c`) |
| `[silly] mirror_world` | 0, 1 | works, at once: the shown picture flipped left to right, with left and right (and the pointer's left and right) swapped to match (`runtime/host/input_win32.c`) |
| `[silly] voice_pitch` | -50..50 % of the frequency | works, from the next thing a Pokémon says: voices (sounds from the `*_VOICE` banks) get CRI pitch on top of the game's own (`runtime/hle/hle_voice_pitch.c`). The game's two voice pools are made with a 49,152 Hz limit instead of 32,768, so +50% isn't clamped. The attractions' own voice banks (`M_*_AT_VOICE`) aren't covered |
| `[silly] victory_confetti` | 0, 1 | works: a 4.5 s burst of paper confetti when the game draws its "Attraction Cleared!" banner (`AttractionMenu02.tpl`, found when bound by `runtime/hle/hle_prompts.c`, noticed when a draw samples it in `gx_fifo.c`), at most once in 10 s; drawn as small filled rectangles over the picture (`runtime/host/confetti.c`, `gl_backend.c` finish_present). `WONDERPARK_CONFETTI=<seconds>` (tests) throws some then. Not yet seen on a real win |
| `[silly] stereo_3d` | 0 off, 1 red-cyan, 2 side by side | red-cyan works (OpenGL): the recorded frame (`gx_frames.c`) drawn twice, the left eye into red alone and the right into green and blue (the device's `eye_mask`), each eye the camera moved sideways (the draw's position matrix and the first ten moved along view x) with the projection turned so things at the convergence distance stay put; the 2D layouts' camera is left alone. `WONDERPARK_3D_SEPARATION` (default 0.8) and `WONDERPARK_3D_DEPTH` (60) tune it. EFB copies made in the second pass carry the first eye's red. Side by side (2): each eye drawn in full colour and presented squeezed into its half of the window (`gl_present`), shown after the right one |
| `[silly] pokemon_size` | 0 mini, 1 normal, 2 giant | works (for looks): the scale (+0x20, normally 1 1 1) of every character's posture record (the words `80446070 80446090`, owned by an entity: the pointer at -8; the park's boxes and signs have none) set to 0.5 or 2 each frame; the records found by scanning MEM1 on each scene change and every 300 frames, about 3 ms, only while it isn't Normal (`runtime/hle/hle_size.c`). Cove Town: 41 found; Kecleon twice the size. Collision is unchanged. The player's own character isn't scaled (its record is another kind) |

## The F1 settings tab

`runtime/host/settings_ui.cpp`, matching `docs/settings/ingame-f1-settings.html`.

The Gameplay page ends with a **Friends** list (the friendship tracker): "N
of 194" and a row per Pokémon that can be befriended, not yet first, each
with its icon and English name. Those are read once, on a thread of their
own, from the player's extracted disc (`Archive/PokemonIcon.dac`,
`Archive/ResidentMessage.dac`'s `Pokemon.msbt`); whether each is a friend
(100 friendship points or more) is read live from the loaded save
(`runtime/hle/hle_friends.c`, from MildRain479/PokePark-2-Wiicompiled, CC0).
With no save loaded it says so. Not grouped by zone yet.

F1 or a controller's Back + Start opens it; F1, Esc, Close or the
controller's B close it. It's drawn by the runtime with Direct2D and
DirectWrite (software, into an image the overlay path lays over the frame;
redrawn only on change), no game assets; the fonts are the launcher's,
built into the exe (`runtime/host/wonderpark.rc`).

While it's open the game is paused: from the idle hook (between frames),
`gx_frames.c gxb_pause_loop` stops the game's clock (`wp_clock_pause`) and
shows the last whole picture again (`present_last`, gl_backend.c keeps a
copy each frame) with the tab over it, handling window messages and the
controller. Changes apply at once and save to `settings.ini` /
`bindings.ini`; internal resolution and anti-aliasing take effect when the
tab closes (they rebuild the EFB). The same pause holds the game while its
window is in the background, with "Pause in the background" on.

Settings not built yet are shown greyed with "Coming soon" (a choice not
available yet is a dimmed pill). Text speed is the game's own value, read
and written in its loaded save (`runtime/hle/hle_options.c`; greyed "Load
a save first" before one is loaded).

### The game's Options page: PC Settings

On the game's own Options page (main menu > save file > Options) the
runtime adds a fourth row, **PC Settings**, under "Delete a Saved Game",
in the game's own style: its row texture, icon frame and font, read at run
time from the player's extracted disc (`game/files/Archive/MenuStartMenu.dac`,
`PrepareMessageRes.dac`) on a thread of its own, never copied into the
repo. Down from the last game item moves to it (the game's own row is drawn
again unselected); pressing ② (or A) shows the eight PC settings in the
same rows (display mode, resolution, frame rate, anti-aliasing, graphics
quality, widescreen, V-Sync, button icons; left/right change them, up/down scroll), with
"In game, press F1 for every setting" under them; ① (or B) goes back. While
one of these rows has the focus the game gets no buttons
(`options_rows_filter`, called from `hle_wpad.c` WPADRead). The page, cursor
and dialog state come from the game's memory (`runtime/hle/hle_options.c`,
addresses in `docs/handoff/SETTINGS.md`). Values the game can't show yet
(frame rate 60, V-Sync, exclusive fullscreen) are skipped or dimmed.

Test hooks: `WONDERPARK_F1_SHOT=<prefix>` opens the tab
`WONDERPARK_F1_SHOT_AT` seconds after the first frame (default 1) and saves
`<prefix>-<n>[b]-<category>.bmp` (the frame as shown; `b`: scrolled to the
end), then closes it. `WONDERPARK_F1_SCRIPT="<seconds>:<key>,..."` presses
keys (f1 esc up down left right enter q e backspace, a letter or digit) or
`shot=<file.bmp>`, through the tab's own key handling (headless too).

## Environment variables (development)

| Variable | What it does |
| --- | --- |
| `WONDERPARK_DATA_DIR` | data folder instead of `%APPDATA%\Wonderpark` |
| `WONDERPARK_CACHE_DIR` | cache folder instead of `cache\` beside the program |
| `WONDERPARK_SETTINGS`, `WONDERPARK_BINDINGS` | other settings / bindings files (`RECOMP_SETTINGS` is still accepted) |
| `WONDERPARK_OVERLAY=1` | debug overlay open at start |
| `WONDERPARK_GAME_SPEED=N` | game speed 1, 2, 4 or 10 at start |
| `WONDERPARK_PAD_SCRIPT` | scripted Wii Remote input, timed from start: `40:2,40.4:,45:down,45.3:` (time in seconds, then buttons joined by `+`: up down left right a b 1 2 plus minus home shake shake2; empty = release). `f3000:2,f3010:` counts the game's finished frames instead (waits for the disc don't shift it) |
| `WONDERPARK_RAMDUMP=f<frame>:<file>,...` | at the game's n-th finished frame, save guest memory (MEM1 then MEM2, 88 MB) to a file, written by a thread of its own; compare dumps to find where the game keeps a value (`runtime/core/ramdump.c`). `t<seconds>:<file>` instead takes it that many seconds after start, lining up with `WONDERPARK_PAD_SCRIPT` |
| `WONDERPARK_POKE=<s>:<addr hex>:<float>,...;...` | tests: writes floats into guest memory for half a second from that time (`runtime/hle/hle_poke.c`) |
| `WONDERPARK_MOVE=<s>:<x>,<y>,<z>:<x>,<y>,<z>[,<facing>];<s>::<x>,<y>,<z>[,<facing>];...` | tests: moves the object whose position record is nearest the first point (within 20; the player at its spawn point) to the second, and later entries move it again; facing in degrees, atan2(dx, dz). Works for the player in Cove Town (spawn after loading the 100% save, slot 1: -226.6, 0, 148.7); at Seasong Beach which record is the player isn't known yet (`runtime/hle/hle_poke.c`) |
| `WONDERPARK_LOG` | debug log categories, comma-separated (below) |
| `WONDERPARK_MAX_SLICES` | stop after N dispatcher slices, printing where |
| `WONDERPARK_PEEK=addr,len` | with `WONDERPARK_MAX_SLICES`: dump guest memory at the stop |
| `WONDERPARK_NO_IDLE_SLEEP` | don't sleep in the game's idle loop |
| `WONDERPARK_RENDER_THREAD=1` | decode GX and draw on a render thread beside the game (`runtime/gpu/gx_thread.c`; off by default until measured) |
| `WONDERPARK_RT_DEBUG=1` | with the render thread: print its state and the GPU interrupt state at exit |
| `WONDERPARK_SYNC_IOS=1` | handle IOS requests (disc, NAND) on the game thread at once, not on the IOS thread (for comparisons) |
| `WONDERPARK_DIRECT_LOG=1` | write log lines straight to stderr instead of through the log thread |
| `WONDERPARK_NO_NATIVE_MTX=1` | run the SDK's matrix routines as recompiled instead of natively (`runtime/hle/hle_mtx.c`) |
| `WONDERPARK_NO_NATIVE_STRING=1` | run strcmp and the game's two resource-by-name lookups as recompiled instead of natively (`runtime/hle/hle_string.c`) |
| `WONDERPARK_STALL_MS` | with `WONDERPARK_LOG=stalls`: the frame length that counts as a stall (default 250) |
| `WONDERPARK_NO_HIDE` | don't hide GPU/DSP time from the game's clock (for comparisons) |
| `WONDERPARK_VI_HZ` | retrace rate (default 60) |
| `WONDERPARK_MQ=addr` | message queue for the `mqwatch` log |
| `WONDERPARK_WATCH=lo-hi` | physical range for `memcpywatch`, or the load and store watch: a build made with `python tools/build_game.py --build-dir game-watch --no-lto --no-pgo --watch` reports each guest block that loads from or stores into the range, once, with the value and the caller |
| `WONDERPARK_AUDIO_DUMP=file.wav` | also write the sound output to a file |
| `WONDERPARK_AX_NOAUX`, `WONDERPARK_AX_SNAP`, `WONDERPARK_AX_WB` | AX mixer debugging |
| `RECOMP_FPS=1` | print fps, the overlay's numbers (`stats:`), disc reads and how many came from RAM (`disc:`), the GPU's own time from timer queries and the share of draws by specialised shaders (`gputime:`) and GL timings every second |
| `RECOMP_AUDIO=0` | no sound |
| `RECOMP_GX_HIDDEN=1` | hidden window (`--headless` does this too) |
| `RECOMP_GX_DUMP=dir`, `RECOMP_GX_DUMP_EVERY=N`, `RECOMP_GX_DUMP_FROM=N` | save presented frames as BMPs |
| `RECOMP_GX_LISTS=0` | decode display lists at every call instead of keeping their vertices on the GPU (for comparisons) |
| `RECOMP_GX_CACHE_LOG=1` | every 30 frames: draws from kept display lists, lists decoded, other draws; and vertex arena growth |
| `RECOMP_GX_LOG`, `RECOMP_GX_DEBUG_FRAME`, `RECOMP_GX_COPY_LOG`, `RECOMP_GX_SKIP`, `RECOMP_GX_DUMP_TEX(TURES)` | GX decoder debugging |
| `RECOMP_GL_DEBUG`, `RECOMP_GL_PRESENT`, `RECOMP_GPU` | OpenGL debugging and presenting path |
| `WONDERPARK_SHADERS=spec` | also a fragment program specialised for each TEV setup, compiled by the driver in the background (`=thread`: on a second GL context) while the ubershader draws, and the setups kept in `shadercache\`. Off by default: on AMD's GL driver the first draw with each new program blocks for 50-300 ms (`runtime/gpu/gl_backend.c`, "Specialised draw programs") |
| `RECOMP_GL_SIMPLE_FS=1` | measurements only, the picture is wrong: a trivial fragment shader instead of TEV, to see what TEV costs the GPU |
| `RECOMP_WIDESCREEN` | 16:9 |
| `WONDERPARK_NO_MODS=1` | start without any mods, whatever `mods.ini` says |
| `WONDERPARK_DISC_UNCACHED=1` | tests: read the disc's files past Windows' file cache, every read from the drive, as on a cold start |
| `WONDERPARK_ATTRACTION=<n>,<seconds>` | tests: that many seconds after the park first shows, go to attraction n's lobby (1 Cake Contraption, 2 Dance Inferno, 3 Power Bash, 4 Flight of Fancy), as its gate does (`runtime/hle/hle_scene.c`, from MildRain479/PokePark-2-Wiicompiled, CC0) |
| `RECOMP_PROMPTS=controller` | tests: the game's button prompts as if the controller were used last (scripted input is no device's) |
| `RECOMP_WINDOW_SIZE=WxH` | tests: the window's inside at start (a hidden `--headless` window keeps it), e.g. `2128x912` for 21:9 |

(`RECOMP_*` names come from the reference runtime and still work.)

### `WONDERPARK_LOG` categories

`adxcheck audioq audiostats axramp balance bloom camera framerate axvoice axvoices dec dsp efbcopy exc gxhw gxirq hook gxpe hostprof hwmisc idle ios ipc irq kpad memcpywatch mmio mmiostat modifiers mods movie mqwatch nandio pi pointer prof prompts scene stalls steps threads voicepitch volume waits wpad wpadbtn`

The most useful: `waits` (where threads block), `steps` (the game's logic
steps per tick), `idle`, `efbcopy`, `gxirq`, `mmiostat` (slow memory
accesses), `axvoices`, `pointer`, `hostprof` (host profiler; see
`tools/analysis/hostprof.py`), `audioq` (the sound output queue's depth at each block, and dropped blocks), `stalls` (the game thread during any frame over 250 ms: its most common
host stacks, the host functions and source lines the time went to, and
the guest link registers, which name the game functions since the
generated chunks have no line info), `prof` (where slices end).

## Developer tools

| Key | What it does |
| --- | --- |
| backtick | debug overlay: fps and slowest frame; per-frame CPU / GPU / wait time; shader compiles, texture uploads, CPU readbacks, vertices, draw batches, EFB copies |
| F7 (overlay open) | game speed x1, x2, x4, x10 (the emulated clock runs faster; drawn frames stay capped by the display, the game catches up with more logic steps; sound is silent above x1) |
| Alt+Enter | fullscreen |

## Patches

None yet. Each patch in `config/patches/` is listed here with what it
changes.
