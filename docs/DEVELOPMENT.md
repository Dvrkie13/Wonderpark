# Developing Wonderpark

## Where each kind of change goes

| Change | Goes in | Never in |
| --- | --- | --- |
| How the **game** behaves (frame rate, bloom, a bug in the game, a gameplay option) | `config/patches/*.c` (see its README) | the generated code, `runtime/` |
| How the **PC** runs it (graphics, sound, input, files, settings, UI, emulated hardware) | `runtime/` (one README per module) | `config/patches/` |
| Names for game/SDK functions | `config/S2LE01/symbols.txt` (from `tools/analysis/sigmatch.py`) | |
| Recompiler review notes (self-modifying-code flags) | `config/S2LE01/smc_review.toml` | |
| Disc, recompile and build steps | `tools/`, `builder/pipeline.py` | The player's build is two-step: Build compiles the recompiled chunks at `-O1` with no ThinLTO into `build/game` (measured 2026-11-19 on a 16-thread PC: 4 min of compiling instead of 16, the game about 7% busier per frame than `-O2`); then the launcher runs `pipeline.py --optimise` (`-O2`, no ThinLTO, `build/game-opt`, about 11 min) in the background at below-normal priority while it's open and the game isn't running, stops it when the game starts (ninja resumes later; its log is `build/optimise.log`), and plays `build/game-opt` once it's current. A cold first build from the release folder (2026-11-19, 16 threads, the disc image on a hard drive but read recently): verify 10 s, extract 3 s, recompile 17 s, compile 4.3 min, preparing shaders 8 s, 5.0 min in all; the optimised build after it 10.5 min. ThinLTO measured only ~2% for 5 more minutes, so the pipeline leaves it out; `tools/build_game.py` alone still uses it. A profile from `tools/pgo_train.py` (`build/pgo/`) helps most (park: 70% busy vs 87-89% without) and is used by both steps when present |
| The launcher | `builder/` | |

The generated code (`generated/`) is rebuilt from scratch by
`tools/recomp.py` and must stay reproducible: don't edit it. Hooks
(`WP_HOOK`) are inserted by `tools/recomp.py`, so after adding or moving one,
run it again.

Nothing game-derived goes into git (`game/`, `generated/`, `build/` are
ignored). See `CLAUDE.md`.

## Commands

```
python tools/fetch_tools.py                  # the build tools players get: llvm-mingw and dtk, into tools/ (pinned SHA-256; --status, --from FILE)
python tools/make_tools.py                   # the ready-built tools a release ships: tools/python, tools/bin/ninja.exe and dolrecomp.exe, tools/prebuilt/sdl3-mingw
python tools/extract_disc.py <disc.iso>      # verify (ID, revision, hash) and extract to game/ (--verify-only --hashes F keeps the hashes for the extract run)
python tools/recomp.py                       # DolRecomp: game/sys/main.dol -> generated/ (tools/bin/dolrecomp.exe when there, else built with CMake)
python tools/build_game.py --mingw           # build build/game/Wonderpark.exe with llvm-mingw: what players get, no Visual Studio and no CMake (build.ninja written by the script)
python tools/build_game.py                   # the same with Visual Studio's clang-cl and CMake when it's installed (--msvc for MSVC; llvm-mingw when it isn't)
python tools/pgo_train.py                    # PGO: instrumented build, train on a park and a beach save, merge to build/pgo/ (~35 min)
python tools/build_game.py --no-pgo         # without the PGO profile (used by default when build/pgo/wonderpark.profdata exists)
python tools/build_game.py --no-lto         # without ThinLTO (on by default when lld-link.exe and llvm-lib.exe are beside clang-cl; ~13 min instead of ~18)
python tools/build_game.py --build-dir game-x --cflags "/arch:AVX2"   # a variant in build/game-x, for comparisons
python builder/pipeline.py <disc.iso>        # all of the above, with progress for the launcher
python builder/pipeline.py --rebuild         # recompile and compile again from the extracted disc in game/
python builder/pipeline.py --status          # @status ok|missing|outdated|building (is build/game current with the sources? building: @pid, @log of the build running), @exe, @optimise needed|done
python builder/pipeline.py --optimise        # the fully optimised build, into build/game-opt (the launcher runs it in the background)
python tools/build_game.py --chunk-opt 1     # the recompiled chunks at -O1 (the pipeline's quick first build)
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
| `--prepare-shaders` | the build's last step ("Testing this PC's speed…", `builder/pipeline.py`), 10 s. It is also the speed test: from the third second on it sums the game thread's busy time per game frame and the GPU's time per frame (at 2x internal resolution on every PC, `WONDERPARK_SCALE=2`) and prints `@perf cpu_ms=.. gpu_ms=..` at exit (`gx_frames.c`, `WONDERPARK_PERF_TEST=1`); the pipeline scales the title's figures to the park's (`PARK_CPU_FACTOR` 1.33, `PARK_GPU_FACTOR` 7.7, measured 2026-11-20 on a 16-thread PC; to check on weak PCs in Part 6), picks the best preset with a fifth spare (Ultra 4x/60, High 3x/60, Medium 2x/60, Low 1x/30, else Potato), writes `build/perf-test.json` and prints `@suggest <preset>`; the launcher shows it when the build is done (the presets themselves: coming soon). It runs the boot hidden and silent at game speed x10, through the title, with a data folder of its own (`cache\prepare\` beside the exe), no mods and no preloading, then exits. The graphics driver compiles each new shader and draw state the first time it sees one (AMD: ~0.45 s for the ubershader, then ~0.4 s pauses at the first frame and at the title) and keeps them in its own cache; after this step the player's first start has none of those pauses. Areas past the title can still pause once the very first time (the driver meets new draw states there) |
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
- It runs `builder/pipeline.py` with Python (`WONDERPARK_PYTHON`, else the
  embeddable Python a release ships in `tools\python`, else `python.exe` on
  PATH, else `py -3`). The build runs on its own, outside the launcher's job
  object: its output goes straight to `build/logs/build-<date>-<time>.log`
  ("Show log" after an error opens it), which the launcher reads as it
  grows. Closing the launcher leaves the build running; the pipeline notes
  itself in `build/build-running.json`, and a launcher started later finds
  it (`--status` says `building`) and shows it again ("Still building
  Wonderpark"). Cancel (or Esc) asks first, then creates `build/build.cancel`;
  the pipeline sees it within half a second and ends its running step and
  everything that step started.
- The build screen (bottom left while building): the five stages (Checking
  your disc, Getting the build tools with its download size, Reading the
  game, Recompiling, Compiling) and Ready, each ticked off; overall progress
  with the time left (from the end of the download, by the progress since;
  updated every 3 s); "You can leave this running"; a tip every 9 s (our own
  words, `BUILD_TIPS`); Cancel.
- At start it asks `pipeline.py --status`: a current build shows Play, an
  out-of-date one (the sources changed) Rebuild, none Place ISO. Play
  starts the optimised build (`build/game-opt`) only when it was made from
  the same sources and is no older than the quick one (`build/game`), so a
  newer quick build is never passed over.
- Play writes the game's log to `build/logs/game-<date>-<time>.log` (the
  newest 10 kept). Its first lines name the program that ran, when it was
  built, and the settings file it used; F1 > Display shows the same two
  things small at the bottom. There is one settings file, `settings.ini` in
  the data folder (`%APPDATA%\Wonderpark`); it's written by a thread of its
  own the moment a setting changes (a temporary file, then renamed over
  it), so F1 never waits for the drive.
- The disc check on "Verifying…" (game ID S2LE01, revision 0) is done by
  the launcher itself from the image's header; the full image hash is
  part of the build's first stage.
- Esc during a build asks whether to stop it; closing the window doesn't
  stop it.
- The Japanese disc (S2LJ01), dropped or picked, isn't built: its Pokémon
  voices are copied for Audio > Voice language (`tools/extract_voices.py`,
  output in `build/logs/voices-ja.log`; it needs dtk like the build), and the
  launcher says when they're ready. `--test-script` has `drop` for the
  `--test-iso`.
- Controller A (any pad, through SDL3: `pad_sdl.c`), Enter and Space press the main button. With Windows'
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
  only: a mod can't add new disc files. `textures\aliases.txt` (lines
  `<hash> <hash>`) draws the first hash with the second's picture (the HD
  Pack uses it for a texture's other mipmap counts). A palette texture's
  hash includes only as much of its palette as the game loaded (up to 16
  colours for C4, 256 for C8), so it can be worked out from the disc.

**Extras** (the right-edge tab, EXTRAS, with the installed mods' count;
`M` or Y opens it) has three pages: **Mods** (Installed: the above; Get
mods: the curated list, "Coming soon" until the wonderpark-mods-index list
exists, its address `[mods] index_url` in `launcher.ini`), **Saves** and
**HD Pack**.

**HD Pack** (`builder/launcher/hdpack.cpp`): the textures in the extracted
disc (`game\files`: TPL, and BRRES TEX0 with PLT0 palettes, inside LZ11 U8
`.dac` archives, `.brres` and `.efb`), each named by the hash the game
gives it (all its mipmap counts: the picture by all its levels, the rest in
`aliases.txt`), decoded with `runtime/gpu/gx_texture.c`, upscaled 4x (alpha
on its own as grey) and saved as the mod `hd-pack` (on in `mods.ini`).
Textures under 16 or over 512 pixels on a side are left as they are. The
upscaler is downloaded once into `hd\upscaler\` in the Wonderpark folder (the pictures go in `hd\textures\`, about 2.7 GB, beside the build rather than in `%APPDATA%`; the mod folder `mods\hd-pack\` has only `mod.json`, `pack.ini` and `textures.txt`, the folder the game reads them from), each
file checked by a SHA-256 pinned in `hdpack.cpp`: ONNX Runtime 1.24.4
(`onnxruntime.dll` from the Microsoft.ML.OnnxRuntime.DirectML NuGet package,
MIT), run on the graphics card through the DirectML that comes with Windows
(Microsoft's separate DirectML package, under its own license, isn't
downloaded), else on the processor; and the model 4xNomosUni_span_multijpg
(Philip Hofmann, CC BY 4.0, the fp32 ONNX from the author's release). A
stopped build goes on where it stopped (pictures already made are kept).
`WonderparkLauncher.exe --build-hd-pack [N]` builds it with no window (the
first N textures only, if given), logging to `hd-build.log` in the data
folder; exit code 0 when done. Known gap: 5 title-screen layout textures
(C8 TPLs in `MenuStartMenu.dac`) don't match the hash the game gives them
yet, so they aren't replaced.

**Saves** (`builder/launcher/saves.cpp`): the game's two save files (`GD0`
and `GD1`, each with its mirror `GD<n>M` and photos `Pic<n>`) in
`%APPDATA%\Wonderpark\nand\title\00010000\53324c45\data`, each shown with
where it was saved, friends, berries, play time and when it was last
written. From the save file (5,344 bytes, big-endian): play time in seconds
at `+0x1470` (checked: 465 is the game's "0:07"), berries at `+0x1474`,
area and zone at `+0x14A6` (named by the disc's `PlaceName.msbt`,
`Fd<area><zone>`), friendship as in `hle_friends.c` (the entry table read
from `game\sys\main.dol`). A file with no play time and no berries (a new
game's) shows "Empty". Per file: **Back up** and **Export** (a .zip of its
files in Dolphin's layout). **Import a save…**: a .zip, or any file in
Dolphin's save folder (the folder is taken); the files brought in replace
theirs and the other file is kept. A real Wii's `data.bin` isn't read
(Wonderpark doesn't ship the Wii's SD key): the page tells the player to
import it into Dolphin first (Tools > Import Wii Save), then import the
Dolphin folder. Reading and zipping is MildRain479/PokePark-2-Wiicompiled's
`wii_save.c` (CC0), with its data.bin and AES code taken out. **Backups**: folders in
`%APPDATA%\Wonderpark\backups\saves\` (an `about.ini` with the label),
each restorable; "Back up before every Play" (on by default, `[saves]
backup_before_play` in `launcher.ini`) keeps the newest 10 automatic ones.
Restoring and importing always back up first. Nothing can be changed while
`Wonderpark.exe` runs (checked every 2 s; the page says so). The game's
own per-save backups (Save backups in F1) stay in `backups\` beside these.

Test options: `--test-iso <file>` (instead of the file picker), `--test-zip
<file>` (instead of the Mods page's file picker), `--test-save <file>`
(instead of the Saves page's import and export pickers) and
`--test-script "<seconds>:<action>,..."` with actions `logo`, `press`,
`cancel` (no question), `shot=<file.png>` (saves the next frame),
`wait-state=ready|built|noiso|building`, `wait-progress=<0-100>`, `mods`,
`mods-close`, `install=<file>`, `toggle=<n>` (the n-th card's switch, from
0), `wait-install=<0-100>` (101: until it ends), `page=<0-2>` (an Extras
page), `sub=<0-1>` (Installed / Get mods), `click=<code>` (anything on the
panel by its code: 300 + 2n back up file n, 301 + 2n export it, 320 import,
322 the auto-backup switch, 330 + n restore backup n), `quit`;
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
| `[display] frame_rate` | `WONDERPARK_FRAME_RATE` | 30, 60, 120-240 | 30 and 60 work, at once (F1 or the Options page): at 60 the game's frame timer runs at 59.94 Hz instead of 29.97 and draws every 60 Hz logic step; movies stay at 30, and it drops to 30 for 15 s when frames keep coming late (`runtime/hle/hle_frame.c`). 120 and up work (F1 offers 120, 144, 240; the Options page too): the game still runs at 60 and the frames between are drawn by `runtime/gpu/gx_frames.c` (from the reference runtime): each of the game's frames is recorded and drawn again with each draw's matrices blended from the frame before, one game frame behind; drawn while the game waits (`runtime/core/idle.c`, which was missing: before it, recording showed nothing), never more than the display's refresh rate (`RECOMP_IGNORE_REFRESH=1` for measuring). Title screen, dev build: 120.0-120.9 frames shown a second. A game frame no in-between frame showed (the game too busy to wait) is drawn as the next one ends, so the picture never falls below the game's own rate. A frame between is left out while the GPU still hasn't finished the frame before the last one shown (a fence per frame, `gl_gpu_busy`): queued further, the game's next frame waited for all of them (its EFB readbacks), the game thread missed its 3 ms audio DMAs (`WONDERPARK_LOG=aidma`: at 240, about 85 every 5 s over a whole buffer late, up to 6.8 ms) and its own mix popped (clicks counted in a recording: 1,700-2,700 a minute at 240, 130-370 at 60). With it: none over a buffer late, clicks as at 60. On a GPU that needs 8.4 ms a frame (3x, RX 9060 XT) that means about 118 frames shown a second at 144 and 240 instead of 137-143 |
| `[display] fov` | `WONDERPARK_FOV` | 70-150 | works, at once: the game's world cameras are widened where they're applied (`0x80019B20`) and where their frustum is worked out for culling (`0x8001A390`), so nothing pops in; in the park and its story scenes (`runtime/hle/hle_camera.c`, from MildRain479/PokePark-2-Wiicompiled, CC0) |
| `[display] fov_attractions` | | 0, 1 (default 0) | 1: the field of view in attractions too (their world cameras; their HUD is flat and stays) |
| `[overlay] show_fps` | 0 off, 1 compact, 2 detailed | works: the performance overlay (`runtime/host/perf_overlay.cpp`), a piece of the PokéPark Pad (brown leather, a dashed stitch, gold corners, drawn by us). Compact: a status dot, the frame rate, "FPS". Detailed: the frame rate, the 1% and 0.1% lows of the last 10 s, a frame-time graph of the last 2 s (a point per frame shown, the target dashed), Wonderpark's Game (game thread per game frame), Graphics (GX to OpenGL) and GPU (timer queries, per frame shown) ms against the frame's budget, and This PC: CPU and GPU use with graphs (GetSystemTimes; the "GPU Engine" performance counter, as Task Manager), VRAM (the "GPU Process Memory" counter; hidden if Windows doesn't give it) and RAM (working set) with bars. Green at 95% of the target or more, yellow at 80%, red below; the target is Frame rate, or the screen's refresh rate if lower (30 while a movie plays). Drawn on a thread of its own (below normal priority) with software Direct2D, 8 times a second (Compact 4), at the overlay's own scale (below); the renderer only notes each frame's time and lays the picture over the window (0.011 ms a frame). Standing in Cove Town at Frame rate 240 (180 Hz screen), three runs each: off 133.5 fps, Detailed 133.3. `WONDERPARK_LOG=overlay` logs its costs; `WONDERPARK_OVERLAY_NOPDH=1` (tests) leaves out the GPU and VRAM counters. Hidden while F1 is open. The overlay key (`[overlay] overlay_key`, F7) cycles it. An old `[display] show_fps` (1 top left, 2 top right) is read as Compact in that corner |
| `[overlay] overlay_corner`, `overlay_size`, `overlay_opacity` | 0 top left, 1 top right, 2 bottom left, 3 bottom right; 75-150 %; 40-100 % | works: 14 units from the edges. At 100% the overlay is 1 unit a pixel at 1350 lines (1.07 at 1440p, 0.8 at 1080p, at least 0.6): about half the notebook's scale. The gold corners lie over the plaque's dark edge |
| `[overlay] overlay_screenshots` | 0, 1 | "Coming soon": Wonderpark has no screenshot key yet (the game's own photos never show the overlay) |
| `[overlay] overlay_key` | a Windows virtual-key code (118, F7) | works: F1 > Controls > Performance overlay (one key); with the debug overlay open F7 is still its game speed |
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
| `[display] brightness` | 50-150 % | works, at once: the black level of the shown picture: above 100% the shadows are lifted toward white (up to 15% of the way), below it everything is dimmed (to 70%). Before `gamma` existed this was the gamma curve: a file with `brightness` and no `gamma` has its value moved to `gamma` once (`settings.c`) |
| `[display] gamma` | 50-150 % | works, at once: a gamma curve on the shown picture (whites stay white; `gl_backend.c` look pass) |
| `[display] colorblind_filter` | 0 off, 1 deuteranopia, 2 protanopia, 3 tritanopia | works, at once: "daltonizing" in the look pass: the picture as that kind of color blindness sees it (Machado, Oliveira and Fernandes 2009, full strength, in linear light), and the difference from the real one shifted into the channels still seen (green and blue for red-green, red and green for blue-yellow) |
| `[display] monitor` | 0 the window's own screen, 1-16 that screen (1 the main one, then Windows' order) | works: fullscreen and borderless cover that screen; a window opens centered on it (Match screen: maximized on it). F1 lists the screens each time it opens (`runtime/gpu/window_win32.c`) |
| `[display] refresh_rate` | 0 the screen's own, else Hz | works in Fullscreen (window mode 2) only: the screen is switched to that rate with the resolution (Match screen: its desktop size); a rate it can't do leaves its own (logged). F1 lists the rates the chosen screen offers at that size, and greys the row ("Fullscreen only") otherwise |
| `[graphics] scaling_filter` | 0 smooth, 1 sharp | works |
| `[graphics] widescreen` | 0, 1 | works, from the next start (`RECOMP_WIDESCREEN` still wins): the game's own 16:9 mode (it is told the TV is 16:9: `SCGetAspectRatio`, `runtime/hle/hle_os.c`). A window wider than 16:9 (up to 32:9) shows more of the world to the sides with menus and HUD kept in the middle; a narrower one (16:10) is cropped at the sides (`runtime/gpu/gx_fifo.c`, "Widescreen") |
| `[graphics] hud_edges` | 0, 1 | works, with widescreen: in game, HUD items at the sides (the minimap) move out to the screen's edges; menus stay as they are |
| `[graphics] fast_loading` | 0, 1 (default 1) | works, from the next start: a thread reads the disc's files into RAM at start, all but movies and streamed music and voices (949 files, 284 MB), the game's own reads first (`runtime/ios/dvd.c`, "Fast loading") |
| `[graphics] bloom` | 0, 1 (default 1) | works, at once: off skips the game's store of the effect's strength at `0x8011D91C`, as the known Gecko code does (`0411D91C 60000000`), and keeps the value it was made with (`runtime/hle/hle_effects.c`) |
| `[graphics] fog` | 0, 1 (default 1) | works, at once: off draws no GX fog (`runtime/gpu/gx_fifo.c`) |
| `[graphics] colors` | 0 original, 1 vivid | works, at once: more saturation (1.3x) and a little contrast on the shown picture |
| `[graphics] per_pixel_lighting` | 0, 1 | works, at once: the vertex shader also passes the view-space position and normal and the vertex colors, and the fragment shader works out the two color channels' lighting again for each pixel (the same hardware model, `GLSL_XF_LIGHTING` shared by both; `u_ppl`), so shading on round things is smoother. Park, dev build: no measurable GPU cost |
| `[graphics] hdr` | | not yet |
| `[graphics] preset` | 0 Potato, 1 Low, 2 Medium, 3 High, 4 Ultra, 5 Custom (default) | works: choosing one sets internal resolution, frame rate, anti-aliasing, texture filtering, per-pixel lighting, bloom and effect copies (Potato 1x, 30, off, 1, off, off, fast; Low 1x, 30, off, 1, off, on, accurate; Medium 2x, 60, off, 4x, off, on, accurate; High 3x, 60, 2x, 16x, on, on, accurate; Ultra 4x, 60, 4x, 16x, on, on, accurate: `kPresets` in `settings.c`). Changing any of those by hand (F1, the Options page) makes it Custom at the next save, unless the environment overrides one of them (a test run). The very first start (no `settings.ini` yet) uses the one the build's speed test suggested (`"suggested"` in `build/perf-test.json`, beside the game's `build/game/` folder; `builder/pipeline.py` suggest_preset), and F1's description says which it was |
| `[graphics] effect_copies` | 0 fast, 1 accurate (default) | works: the game copies the picture into textures for some effects (EFB copies). Both keep the copy on the GPU for draws. Accurate also reads back each copy not remade every frame and writes it into the game's memory, as a Wii does (that waits for the GPU); Fast doesn't, except a copy the game asks for itself (its photos, `gxb_copy_sync`) (`runtime/gpu/gx_frames.c` gxb_copy_to_texture). Part 6 measures it |
| `[graphics] dynamic_resolution`, `dynamic_resolution_target` | 0, 1; 30 or 60 | not yet ("Coming soon"): Part 6 of the plan |
| `[graphics] shader_mode` | 0 smooth, 1 fast (2, the old precompile, reads as fast; F1 offers Smooth / Fast only) | from the next start: 1 (and 2) turns on the specialised draw programs, as `WONDERPARK_SHADERS=spec` does (`gl_backend.c` spec_init; that variable still wins): less GPU time (Wish Park at 4x: 12 -> 5 ms a frame), but on AMD's driver the first draw with each new program blocks 50-300 ms. Precompile (waiting for the saved ones before the game starts) isn't built. Tried 2026-11-18 on the RX 9060 XT: one throwaway draw with each program as it's made, to take the driver's first-draw cost then; in the park Fast had 1 slow second of 34 and that 3 of 31 (noise), as the saved programs are already made and first drawn during the start-up screens. Left out |
| `[controls] free_camera` | 0, 1 (default 1) | works (but see `docs/camera-and-extras.md`: frame rate dips while it keeps turning): in the park the mouse and right stick turn and tilt the follow camera (tilt -20 to 45 degrees); B (the game's look-around) eases it back behind the player; the eye is pulled in from walls and kept off the ground, for drawing only; another camera or scene starts over. Off eases back. Not in minigames (`runtime/hle/hle_camera.c`, adapted from MildRain479/PokePark-2-Wiicompiled's, CC0; the scene from `runtime/hle/hle_scene.c`). `WONDERPARK_MOUSE=<dx>,<dy>[,<updates>]` (tests) turns it by that many pixels an update (only for that many turned updates, when given) |
| `[controls] stick_sensitivity` | 10-300 % | works: the right stick's turning (600 mouse pixels a second at 100%) |
| `[controls] invert_x` | 0, 1 | works: the camera's left and right, mouse and stick (Mirror world flips it too) |
| `[controls] camera_recenter` | 0-30 s (0 never) | works: that long without turning, the camera eases back behind |
| `[controls] recenter_speed` | 0 slow, 1 normal (default), 2 fast | works: the camera eases back behind (B, or `camera_recenter`) over 60, 30 (the game's own) or 12 updates |
| `[controls] camera_distance` | 100-200 % | works, in the park: the follow camera's eye is placed that much further back along its line, for drawing only (the game goes on from its own; checked in Wish Park: the camera drawn is the follow camera with the eye at 48.8 units at 200%, the game's 24.4); walls and the ground bring it in as for a turn, but while the camera isn't turned never nearer than the game's own (at the Wish Park spawn a wall behind pulled 200% in to 0.4 of it, nearer than the game's) (`runtime/hle/hle_camera.c`) |
| `[controls] camera_smoothing` | 0-90 % | works: the mouse's and right stick's turn eases in: each update takes (100 - smoothing)% of what's still to come |
| `[controls] deadzone_left`, `deadzone_right` | 0-50 % (default 24, XInput's suggestion) | works: that much of each stick's travel does nothing, per axis; the left stick's d-pad directions need half its travel or the dead zone, whichever is more (`runtime/host/input_win32.c`) |
| `[controls] trigger_sensitivity` | 10-90 % (default 50) | works: a trigger counts as pressed past (100 - this)% of its travel |
| `[controls] charge_toggle` | 0 hold, 1 toggle | works: with 1, a press of the Wii Remote's 1 held 0.4 s keeps 1 held after it's let go (the charged move keeps charging) until the next press, which lets it go; shorter presses are taps as before |
| `[controls] right_stick` | 0 camera, 1 pointer, 2 auto (default) | works: Auto moves the Wii Remote pointer with the right stick while the game has the pointer on (`hle_pointer_on`, its IR camera: aiming minigames, menus pointed at), else turns the camera; Pointer always (the camera then only by mouse). The push squared, so small pushes aim finely (`runtime/host/input_win32.c`). Not yet tried with a real controller |
| `[controls] aim_speed` | 25-200 % | works: the right stick's pointer speed (900 mouse pixels a second at full push, at 100%) |
| `[controls] rhythm_offset` | -100..100 ms | works: Dance Inferno's song tick (which its judging and the scrolling move icons follow; the music streams on its own) moved back by the offset (positive: presses feel late, so the judging waits longer) once per part of the song, in whole frames (17 ms; +100 ms = 6). `runtime/hle/hle_dance.c`, from MildRain479/PokePark-2-Wiicompiled's (CC0); `WONDERPARK_LOG=dance` |
| `[controls] steer_sensitivity` | 25-200 % | works: Flight of Fancy (attraction 4, `hle_minigame`, from the game's "AttractionId" scene parameter) is flown by the remote's tilt: the left stick tilts it in proportion and the d-pad keys ease it over (left/right steer all the way over, up/down climb and dive to 50 degrees); the setting scales how far a push tilts it and how quickly. Adapted from MildRain479/PokePark-2-Wiicompiled's `hle_motion.c` (CC0) into `hle_wpad.c` flight_tilt. Checked: holding right swung Pikachu right, up climbed |
| `[controls] aim_assist` | | not yet: needs the targets' places on screen |
| `[input] button_icons` (`WONDERPARK_BUTTON_ICONS`) | 0 auto, 1 Xbox, 2 PlayStation, 3 Nintendo, 4 keyboard only | works: the button icons in F1's Controls (pad columns), the game's prompts and the launcher. Auto: the pad's own type from SDL3 (`pad_sdl_type`), or without one, from the game controllers Windows lists (HID) by maker (Sony: PlayStation; Nintendo, and Hori/PowerA/PDP Switch pads: Nintendo; else Xbox); a PlayStation or Switch pad behind a mapper (Steam Input, DS4Windows) still lists itself. Looked at again when a pad connects or goes away. Face buttons go by position (the bottom one is A: a cross, or a Nintendo pad's B). The game's prompts show the device used last (any pad button or the stick past half; a bound key or mouse button), key caps for the keyboard; 4 never shows a pad's (`runtime/host/pad_icons.cpp`, `pad_icons_core.cpp`, `runtime/hle/hle_prompts.c`). The log says what was found ("pad icons: ...") |
| `[input] extra_remotes` (`WONDERPARK_EXTRA_REMOTES`) | 0 (default), 1 | multiplayer is shelved: off, player 1 only (the keyboard and controller 1), and `keyboard_player` does nothing. Not in F1 (its Multiplayer tab is hidden, `kMultiplayerTab` in `settings_ui.cpp`). 1 turns the code below back on, untested. Test scripts `WONDERPARK_PAD2_SCRIPT`-`_PAD4_SCRIPT` still connect their remotes |
| `[input] keyboard_player` | 0 shared, 1 keyboard alone | shelved, only with `extra_remotes` 1: local multiplayer. Wii Remotes 2-4 connect while players 2-4 have a controller (SDL3, in the order they connected) and go away with it, told to the game through its connect callback (`hle_wpad.c` report()); each gets its own buttons (player 1's bindings), rumble, pointer (its right stick, at Aim speed) and Flight of Fancy tilt. 0: the keyboard and controller 1 are player 1, controller 2 player 2; 1: the keyboard alone is player 1, controller 1 player 2. In the game: Multiplayer Attractions > Add Wii Remote Controllers (its Add Wii Remotes screen lights each connected remote) > Done (+), then Two to Four Players opens. Checked to Pick an Attraction with a scripted player 2; a whole two-player attraction not yet |
| `[audio] volume_master` | 0-100 % | works |
| `[audio] audio_latency` | 0 low, 1 normal, 2 safe | works: about 40, 75 or 140 ms from the game to the speakers (the sound thread's ring kept at 20 / 45 / 80 ms, the device's buffer 20 / 30 / 60 ms ahead), at once (`runtime/host/audio_win32.c`). Sound plays on a thread of its own through WASAPI, by the device's clock; the game only copies its 32 kHz mix into a ring, and the ring's level steers a windowed-sinc resampler (±0.5%, 3% after a loading burst) so nothing is dropped or repeated. 45 s at 60 fps in Cove Town, each setting: 0 underruns and 0 device gaps after start-up, 48,000 Hz out, 32,000 in |
| `[audio] volume_music`, `volume_sfx`, `volume_voices` | 0-100 % | work, at once, through the game's own CRI categories (`runtime/hle/hle_volume.c`): music is BGM and JGL (jingles), voices PK_VOICE and PC_VOICE, effects the other five. The game's own category changes are kept and multiplied. Movies (the title's too) play their sound outside CRI and aren't affected |
| `[audio] voice_language` | 0 English, 1 Japanese | works, from the next start: with 1, the Japanese disc's voice banks in `game/voices/ja/Sound/` (copied once by `tools/extract_voices.py <Japanese disc>`, or by dropping that disc on the launcher) take the place of the US files the way a mod's do (they have other sizes, so the FST the game reads at boot is changed: `runtime/ios/dvd.c` voice_file; settings are now read before the disc opens, `main.c`). Copied: the 28 banks that differ from the US ones but have the same cue names, indexes and IDs (the starters' voices, the area and story banks), each with its `_streamfiles.awb`; the other Pokémon's shared bank `S_POKE_VOICE` stays English (its cues differ). The F1 row is greyed ("Needs your Japanese disc") until `voices/ja/voices.txt` exists. `WONDERPARK_LOG=mods` logs each Japanese file read. Checked: in Cove Town the game reads the Japanese `S_Fd0102`, `S_DEMO_COMMON` and the starters' voice banks |
| `[audio] mute_background` | 0, 1 | works: silent while another window is in front |
| `[audio] output_device` | 0 Windows' default, else a number made from the device's id (FNV-1a of Windows' id string) | works, at once: the sound thread opens that device (one that's gone: the default). F1 lists the active output devices by name, read on the sound thread when the tab opens (`runtime/host/audio_win32.c`) |
| `[audio] mono` | 0, 1 | works, at once: left and right averaged, after the mix |
| `[audio] night_mode` | 0, 1 | works, at once: a compressor on the output: above about -24 dB, 3:1, then 9 dB louder and held under full scale; the level rises in about 5 ms and falls over about 300 ms |
| `[gameplay] pause_background` | 0, 1 | works: the game pauses while another window is in front |
| `[gameplay] speed_up`, `speed_toggle` | 0 off (the default since 0.1.1-beta: on, an unbound left trigger fast-forwarded the park with the sound muted while held, which played as choppy sound), 1 2x, 2 3x; 0 hold, 1 toggle | works: Tab, or the controller's left trigger when no action is bound to it, runs the game's clock 2 or 3 times as fast (so it runs that many more of its fixed steps), only in the park and never in an attraction, a movie, or while an event script (story scene, talk) runs (`hle_event_running`, `runtime/hle/hle_scene.c`); silent meanwhile, as with F7. A speed set from the debug overlay is left alone (`runtime/host/input_win32.c`). How much faster it really gets depends on the CPU's headroom, since each extra step is a whole frame, drawing included: in Cove Town the optimised build is 65-69% busy at normal speed and went from 30 to 34 game frames a second at 2x (1.13x); the dev build is already 100% busy. A real 2x would need the in-between steps' drawing left out (not done). `WONDERPARK_SPEED_HELD=1` (tests) acts as if the key were held |
| `[gameplay] save_backups` | 0, 1 (default 1) | works: before the game writes a save slot (opened for writing, deleted, or replaced by a rename; `GD0`, `GD1`...), the file is copied to `backups\` in the data folder as `GD<n>-<date>-<time>.bin`, unless the newest backup of that slot is the same; the newest 10 per slot are kept. On the IOS thread (`runtime/ios/nand.c`). To restore one, copy it over `nand\title\00010000\53324c45\data\GD<n>` and `GD<n>M` with the game closed |
| `[gameplay] skip_logos` | 0, 1 | works: the Strap scene (the logos and the wrist strap warning, a fixed time) runs at 10 times the speed (our game speed, as F7), so the title menu comes about 12 s sooner (frame 952 instead of 1694). Not the "Loading..." screen after it (Hold): sped up, it took longer. Leaving Strap early once the park is loaded (`0x804BA89C` reaches 2; MildRain's `0x804BA8C8` stays 11 in ours) left a blank screen (`runtime/hle/hle_scene.c`) |
| `[gameplay] hud_key` | a Windows virtual-key code (117, F6) | works: hides and shows the HUD (F1 > Controls > Buttons > Hide HUD; F1 > Gameplay > Hide HUD does the same; not saved). Hidden: in frames that drew the world, the game's layout draws narrower than 90% of the screen aren't drawn (the HUD, the minimap, conversation windows); menus, fades and backgrounds are (`runtime/gpu/gx_fifo.c` wide_for_draw) |
| `[gameplay] auto_advance`, `hud_size`, `hud_opacity` | 0, 1; 50-150 %; 0-100 % | not yet ("Coming soon"). Auto-advance needs the game's conversation code (when a page is shown in full); HUD size and opacity need a per-item transform and alpha for the layout draws. "Skip intro movies" is also "Coming soon" (`skip_movies` above) |
| `[silly] mirror_world` | 0, 1 | works, at once: the shown picture flipped left to right, with left and right (and the pointer's left and right) swapped to match (`runtime/host/input_win32.c`) |
| `[silly] voice_pitch` | -50..50 % of the frequency | works, from the next thing a Pokémon says: voices (sounds from the `*_VOICE` banks) get CRI pitch on top of the game's own (`runtime/hle/hle_voice_pitch.c`). The game's two voice pools are made with a 49,152 Hz limit instead of 32,768, so +50% isn't clamped. The attractions' own voice banks (`M_*_AT_VOICE`) aren't covered |
| `[silly] victory_confetti` | 0, 1 | works: a 4.5 s burst of paper confetti when the game draws its "Attraction Cleared!" banner (`AttractionMenu02.tpl`, found when bound by `runtime/hle/hle_prompts.c`, noticed when a draw samples it in `gx_fifo.c`), at most once in 10 s; drawn as small filled rectangles over the picture (`runtime/host/confetti.c`, `gl_backend.c` finish_present). `WONDERPARK_CONFETTI=<seconds>` (tests) throws some then. Not yet seen on a real win |
| `[silly] pokemon_size` | 0 mini, 1 normal, 2 giant | works (for looks): the scale (+0x20, normally 1 1 1) of every character's posture record (the words `80446070 80446090`, owned by an entity: the pointer at -8; the park's boxes and signs have none) set to 0.5 or 2 each frame; the records found by scanning MEM1 on each scene change and every 300 frames, about 3 ms, only while it isn't Normal (`runtime/hle/hle_size.c`). Cove Town: 41 found; Kecleon twice the size. Collision is unchanged. The player's own character isn't scaled (its record is another kind) |

## The F1 settings tab

`runtime/host/settings_ui.cpp`, matching `docs/settings/ingame-f1-settings.html`
(the "Explorer's Notebook" look: a green cover with gold corners, a brown
leather page with paw prints, colored index tabs, washi-tape section titles,
sticker buttons with navy outlines and hard shadows, small tilts done with
one rotation matrix per element; all drawn in code). Six tabs: Display,
Graphics (View: field of view, draw distance), Controls, Audio, Gameplay,
Silly. Every tab has its Reset button. (A Multiplayer tab, the four player
slots and "Coming soon" Online rows, is kept in the code but hidden while
multiplayer is shelved.)

The notebook has five books, bookmarks on its right edge: Settings (F1),
Friend Book (F2), Journal (F3), Jukebox (F4) and Gallery (F5); LT / RT go
round them, Y opens the Friend Book, and in a book F1 goes back to
Settings. The Journal (`runtime/hle/hle_journal.c`, `journal.json` in the
data folder), the Jukebox (`runtime/host/jukebox.cpp`, `jukebox_heard.ini`) and the Gallery are described in
`docs/books-and-extras.md`. `WONDERPARK_LOG=journal` logs milestones.

The **Friend Book** (F2, or Y on a controller)
is a view of its own: the index tabs become the park's areas (All, Cove,
Arbor, Crag, Tech, Arcane, Wish Park), with a ring of friends made, an
All / Friends / Not yet filter, the area's places as chips, and a polaroid
card per Pokémon (a gold star on friends, a dashed "???" with no friendship
points yet). A card opens a hint card: friendship points, and the Pad's own
note on what that Pokémon wants (`Flavor_Pokemon.msbt` `FL_<NAME>_020`,
"show more" for `_040`; `_100` once a friend). Names, icons, notes and place
names are read once, on a thread of their own, from the player's extracted
disc (`Archive/PokemonIcon.dac`; `Archive/ResidentMessage.dac`'s
`Pokemon.msbt`, `Flavor_Pokemon.msbt`, `PlaceName.msbt`); friendship is read
live from the loaded save (`runtime/hle/hle_friends.c`, from
MildRain479/PokePark-2-Wiicompiled, CC0). Each Pokémon's area and place is a
table of letters in the Pad's order (`kWhere`, from the US game's area list,
whose order matches the game's name table at `0x80435C10`); which Wish Park
zone each of its Pokémon is in isn't known, so Wish Park has no place chips.
"Not met yet" means no friendship points yet: the save's own "met" flag
hasn't been found. With no save loaded the book says so.

Fixed with it: the LZ11 decoder stopped a few bytes early when the data
ended with a back-reference, cutting off an archive's last file
(`Flavor_Pokemon.msbt`).

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

Settings not built yet stay visible, dimmed, with a red "Coming soon" rubber
stamp (a section title can carry it: Online); a choice not available yet
(Vulkan) is a dashed pill marked "soon". Text speed is the game's own value, read
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
addresses in `docs/research.md, Settings`). Values the game can't show yet
(frame rate 60, V-Sync, exclusive fullscreen) are skipped or dimmed.

Test hooks: `WONDERPARK_F1_SHOT=<prefix>` opens the tab
`WONDERPARK_F1_SHOT_AT` seconds after the first frame (default 1) and saves
`<prefix>-<n>[b]-<category>.bmp` (the frame as shown; `b`: scrolled to the
end) for the six tabs, then the Friend Book (`8-friendbook`, `9-cove`,
`10-hint` with a hint card open, `10b-hint` after its "show more" when it
has one), the Journal (`11-journal`, `11b-journal-attractions`,
`11c-journal-milestones`), `12-jukebox`, the Gallery (`13-gallery`,
`13b-gallery-viewer`), then closes it. `WONDERPARK_F1_SCRIPT="<seconds>:<key>,..."` presses
keys (f1-f5 esc up down left right enter q e backspace, a letter or digit),
`shot=<file.bmp>` or `anim=<0-3>` (the Gallery viewer's animation), through
the tab's own key handling (headless too). The Gallery's 3D view:
`WONDERPARK_G3_CHARACTER=<name>,<name>,...` shows those models instead of
the friend's, a new one every 1.5 s (when the viewer is drawn again);
`WONDERPARK_G3_DUMP=<frame>` prints each bone's position once the shown
animation reaches that frame (`runtime/host/gallery3d.cpp`).

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
| `WONDERPARK_EXIT_AFTER=N` | tests: as `--exit-after N`, for a game started by the launcher's Play |
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
| `WONDERPARK_AUDIO_DUMP=file.wav` | also write the game's own mix (32 kHz, as AX made it) to a file; written on the game thread, so tests only |
| `WONDERPARK_AUDIO_OUT_DUMP=file.raw` | tests: what the sound device gets (48 kHz stereo 16-bit raw), from the sound thread |
| `WONDERPARK_AX_NOAUX`, `WONDERPARK_AX_SNAP`, `WONDERPARK_AX_WB` | AX mixer debugging |
| `RECOMP_FPS=1` | print fps (`fps:` frames shown, the slowest, how busy the game thread was, the game's own frames, the 60 Hz retraces its logic steps by, and the cap: the Frame rate setting, or the screen's refresh rate if lower), the overlay's numbers (`stats:`), disc reads and how many came from RAM (`disc:`), the GPU's own time from timer queries and the share of draws by specialised shaders (`gputime:`) and GL timings every second |
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
| `WONDERPARK_ATTRACTION=<n>,<seconds>[,<players>]` | tests: that many seconds after the park first shows, go to attraction n's lobby (with players: also the "PlayerNum" and "FromMulti" parameters, but from the park the game still plays it alone) (1 Cake Contraption, 2 Dance Inferno, 3 Power Bash, 4 Flight of Fancy), as its gate does (`runtime/hle/hle_scene.c`, from MildRain479/PokePark-2-Wiicompiled, CC0) |
| `WONDERPARK_PAD2_SCRIPT` .. `_PAD4_SCRIPT` | tests: players 2-4's scripted input (as `WONDERPARK_PAD_SCRIPT`), which also connects their remotes. In any of these scripts `s<scene>+<seconds>:...` is timed from when that scene first started (6 the start menu, 0 the park, 5 a lobby, 4 an attraction), whatever loading took |
| `RECOMP_PROMPTS=controller` | tests: the game's button prompts as if the controller were used last (scripted input is no device's) |
| `RECOMP_WINDOW_SIZE=WxH` | tests: the window's inside at start (a hidden `--headless` window keeps it), e.g. `2128x912` for 21:9 |

(`RECOMP_*` names come from the reference runtime and still work.)

### `WONDERPARK_LOG` categories

`adxcheck aidma audiostats axramp overlay balance bloom camera charge dance framerate wpadflags axvoice axvoices dec dsp efbcopy exc gxhw gxirq hook gxpe hostprof hwmisc idle ios ipc irq kpad memcpywatch mmio mmiostat modifiers mods movie mqwatch nandio pi pointer prof prompts scene stalls steps threads voicepitch volume waits wpad wpadbtn`

The most useful: `waits` (where threads block), `steps` (the game's logic
steps per tick), `idle`, `efbcopy`, `gxirq`, `mmiostat` (slow memory
accesses), `axvoices`, `pointer`, `hostprof` (host profiler; see
`tools/analysis/hostprof.py`), `audiostats` (every 5 s: underruns, device gaps, input dropped, the ring's level, the resampler's rate, the sample rates in and out), `framerate` (Frame rate 60 falling back to 30), `stalls` (the game thread during any frame over 250 ms: its most common
host stacks, the host functions and source lines the time went to, and
the guest link registers, which name the game functions since the
generated chunks have no line info), `prof` (where slices end),
`camera` (the camera on screen; every 120 updates where the free camera
placed the eye: Camera distance, the wall limit, how far in it's pulled),
`charge` (what the game gets of the Wii Remote's 1, as Charge (1): Toggle
changes it).

## Developer tools

| Key | What it does |
| --- | --- |
| backtick | debug overlay: fps and slowest frame; per-frame CPU / GPU / wait time; shader compiles, texture uploads, CPU readbacks, vertices, draw batches, EFB copies |
| F7 (overlay open) | game speed x1, x2, x4, x10 (the emulated clock runs faster; drawn frames stay capped by the display, the game catches up with more logic steps; sound is silent above x1) |
| Alt+Enter | fullscreen |

## Patches

None yet. Each patch in `config/patches/` is listed here with what it
changes.
