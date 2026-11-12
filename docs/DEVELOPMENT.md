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
- Controller A, Enter and Space press the main button. With Windows'
  "Animation effects" off (or `--reduced-motion`), nothing moves.

Test options: `--test-iso <file>` (instead of the file picker) and
`--test-script "<seconds>:<action>,..."` with actions `logo`, `press`,
`cancel` (no question), `shot=<file.png>` (saves the next frame),
`wait-state=ready|built|noiso|building`, `wait-progress=<0-100>`, `quit`;
`+N` times are after the previous action. `WONDERPARK_LAUNCHER_STATS=<file>`
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
| `cache\nandtmp\` | beside `Wonderpark.exe` | the NAND's `/tmp` (the game copies ~41 MB there at each start; emptied at start) |

## Settings

Each can be set in `settings.ini` or overridden by the environment
variable shown (the variable wins; an overridden value is never written to
the file).

| settings.ini | Environment | Values | Status |
| --- | --- | --- | --- |
| `[display] fullscreen` | `WONDERPARK_FULLSCREEN` | 0, 1 (borderless; Alt+Enter toggles) | works |
| `[display] scale` | `WONDERPARK_SCALE` | 0 (Auto: fits the window, at most 2x for now: higher costs too much GPU time until the TEV shader is faster), 1-6 internal resolution | works |
| `[display] msaa` | `WONDERPARK_MSAA` | 1, 2, 4, 8 | works |
| `[display] aniso` | `WONDERPARK_ANISO` | 1, 2, 4, 8, 16 | works |
| `[display] renderer` | `WONDERPARK_RENDERER` | opengl, vulkan | only OpenGL is built |
| `[display] frame_rate` | `WONDERPARK_FRAME_RATE` | 30, 60, 120-240 | not hooked up yet (plan section 12) |
| `[display] fov` | `WONDERPARK_FOV` | 80-150 | not hooked up yet |
| `[gameplay] skip_movies` | `WONDERPARK_SKIP_MOVIES` | 0, 1 | not working yet (see "Skip movies" in `docs/recomp-status.md`) |
| `[gameplay] text_speed`, `berries`, `prices`, `damage_dealt`, `damage_taken`, `chase_speed` | `WONDERPARK_<KEY>` | percent | from the PokéPark 1 runtime; not hooked up for PokéPark 2 yet |
| `[updates] check` | `WONDERPARK_CHECK_UPDATES` | 0, 1 | no update check built yet |
| `[mouse] sensitivity` | `WONDERPARK_SENSITIVITY` | 0.1-3 | camera and pointer speed |
| `[mouse] invert_tilt` | `WONDERPARK_INVERT_TILT` | 0, 1 | |
| `[controller] rumble` | `WONDERPARK_RUMBLE` | 0 off, 1 light, 2 normal, 3 strong | |

The PC settings of the F1 tab (`runtime/host/settings.c`, table `kX`; each
also `WONDERPARK_<KEY>`, key in capitals, for example `WONDERPARK_WINDOW_MODE=1`):

| Setting | Values | State |
| --- | --- | --- |
| `[display] window_mode` | 0 windowed, 1 borderless, 2 fullscreen | 0 and 1 work (replaces `fullscreen`, still read from old files); exclusive fullscreen not yet |
| `[display] resolution` | window size: 0 1280x720, 1 1600x900, 2 1920x1080, 3 2560x1440, 4 match screen (maximized) | works, in a window |
| `[display] vsync`, `brightness` | 0/1; 50-150 % | not yet |
| `[graphics] scaling_filter` | 0 smooth, 1 sharp | works |
| `[graphics] widescreen` | 0, 1 | works, from the next start (`RECOMP_WIDESCREEN` still wins) |
| `[graphics] hud_edges`, `bloom`, `fog`, `per_pixel_lighting`, `hdr`, `colors`, `shader_mode`, `fast_loading` | | not yet |
| `[controls] invert_x`, `right_stick`, `aim_assist`, `steer_sensitivity`, `aim_speed`, `rhythm_offset` | | not yet |
| `[audio] volume_master` | 0-100 % | works |
| `[audio] volume_music`, `volume_sfx`, `volume_voices`, `voice_language`, `audio_latency` | | not yet |
| `[audio] mute_background` | 0, 1 | works: silent while another window is in front |
| `[gameplay] pause_background` | 0, 1 | works: the game pauses while another window is in front |
| `[gameplay] skip_logos` | | not yet |
| `[silly] pokemon_size`, `voice_pitch`, `mirror_world`, `victory_confetti`, `stereo_3d` | | not yet |

## The F1 settings tab

`runtime/host/settings_ui.cpp`, matching `docs/settings/ingame-f1-settings.html`.
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
again unselected); pressing ② (or A) shows the seven PC settings in the
same rows (display mode, resolution, frame rate, anti-aliasing, graphics
quality, widescreen, V-Sync; left/right change them, up/down scroll), with
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
| `WONDERPARK_RAMDUMP=f<frame>:<file>,...` | at the game's n-th finished frame, save guest memory (MEM1 then MEM2, 88 MB) to a file, written by a thread of its own; compare dumps to find where the game keeps a value (`runtime/core/ramdump.c`) |
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
| `WONDERPARK_WATCH=lo-hi` | physical range for `memcpywatch`, or the store watch (CMake `-DWP_STORE_WATCH=ON`) |
| `WONDERPARK_AUDIO_DUMP=file.wav` | also write the sound output to a file |
| `WONDERPARK_AX_NOAUX`, `WONDERPARK_AX_SNAP`, `WONDERPARK_AX_WB` | AX mixer debugging |
| `RECOMP_FPS=1` | print fps, the overlay's numbers (`stats:`), disc reads (`disc:`) and GL timings every second |
| `RECOMP_AUDIO=0` | no sound |
| `RECOMP_GX_HIDDEN=1` | hidden window (`--headless` does this too) |
| `RECOMP_GX_DUMP=dir`, `RECOMP_GX_DUMP_EVERY=N`, `RECOMP_GX_DUMP_FROM=N` | save presented frames as BMPs |
| `RECOMP_GX_LISTS=0` | decode display lists at every call instead of keeping their vertices on the GPU (for comparisons) |
| `RECOMP_GX_CACHE_LOG=1` | every 30 frames: draws from kept display lists, lists decoded, other draws; and vertex arena growth |
| `RECOMP_GX_LOG`, `RECOMP_GX_DEBUG_FRAME`, `RECOMP_GX_COPY_LOG`, `RECOMP_GX_SKIP`, `RECOMP_GX_DUMP_TEX(TURES)` | GX decoder debugging |
| `RECOMP_GL_DEBUG`, `RECOMP_GL_PRESENT`, `RECOMP_GPU` | OpenGL debugging and presenting path |
| `RECOMP_WIDESCREEN` | 16:9 |

(`RECOMP_*` names come from the reference runtime and still work.)

### `WONDERPARK_LOG` categories

`adxcheck audioq audiostats axramp axvoice axvoices dec dsp efbcopy exc gxhw gxirq hook gxpe hostprof hwmisc idle ios ipc irq kpad memcpywatch mmio mmiostat movie mqwatch nandio pi pointer prof stalls steps threads waits wpad wpadbtn`

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
