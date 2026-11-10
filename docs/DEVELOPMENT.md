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
python builder/pipeline.py <disc.iso>        # all of the above, with progress for the launcher
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
| `[display] scale` | `WONDERPARK_SCALE` | 0 (fit window), 1-6 internal resolution | works |
| `[display] msaa` | `WONDERPARK_MSAA` | 1, 2, 4, 8 | works |
| `[display] aniso` | `WONDERPARK_ANISO` | 1, 2, 4, 8, 16 | works |
| `[display] renderer` | `WONDERPARK_RENDERER` | opengl, vulkan | only OpenGL is built |
| `[display] frame_rate` | `WONDERPARK_FRAME_RATE` | 30, 60, 120-240 | not hooked up yet (plan section 12) |
| `[display] fov` | `WONDERPARK_FOV` | 80-150 | not hooked up yet |
| `[gameplay] skip_movies` | `WONDERPARK_SKIP_MOVIES` | 0, 1 | see "Skip movies" in `docs/recomp-status.md` |
| `[gameplay] text_speed`, `berries`, `prices`, `damage_dealt`, `damage_taken`, `chase_speed` | `WONDERPARK_<KEY>` | percent | from the PokéPark 1 runtime; not hooked up for PokéPark 2 yet |
| `[updates] check` | `WONDERPARK_CHECK_UPDATES` | 0, 1 | no update check built yet |
| `[mouse] sensitivity` | `WONDERPARK_SENSITIVITY` | 0.1-3 | camera and pointer speed |
| `[mouse] invert_tilt` | `WONDERPARK_INVERT_TILT` | 0, 1 | |
| `[controller] rumble` | `WONDERPARK_RUMBLE` | 0 off, 1 light, 2 normal, 3 strong | |

## Environment variables (development)

| Variable | What it does |
| --- | --- |
| `WONDERPARK_DATA_DIR` | data folder instead of `%APPDATA%\Wonderpark` |
| `WONDERPARK_CACHE_DIR` | cache folder instead of `cache\` beside the program |
| `WONDERPARK_SETTINGS`, `WONDERPARK_BINDINGS` | other settings / bindings files (`RECOMP_SETTINGS` is still accepted) |
| `WONDERPARK_OVERLAY=1` | debug overlay open at start |
| `WONDERPARK_GAME_SPEED=N` | game speed 1, 2, 4 or 10 at start |
| `WONDERPARK_PAD_SCRIPT` | scripted Wii Remote input, timed from start: `40:2,40.4:,45:down,45.3:` (time in seconds, then buttons joined by `+`: up down left right a b 1 2 plus minus home shake shake2; empty = release) |
| `WONDERPARK_LOG` | debug log categories, comma-separated (below) |
| `WONDERPARK_MAX_SLICES` | stop after N dispatcher slices, printing where |
| `WONDERPARK_PEEK=addr,len` | with `WONDERPARK_MAX_SLICES`: dump guest memory at the stop |
| `WONDERPARK_NO_IDLE_SLEEP` | don't sleep in the game's idle loop |
| `WONDERPARK_NO_HIDE` | don't hide GPU/DSP time from the game's clock (for comparisons) |
| `WONDERPARK_VI_HZ` | retrace rate (default 60) |
| `WONDERPARK_MQ=addr` | message queue for the `mqwatch` log |
| `WONDERPARK_WATCH=lo-hi` | physical range for `memcpywatch`, or the store watch (CMake `-DWP_STORE_WATCH=ON`) |
| `WONDERPARK_AUDIO_DUMP=file.wav` | also write the sound output to a file |
| `WONDERPARK_AX_NOAUX`, `WONDERPARK_AX_SNAP`, `WONDERPARK_AX_WB` | AX mixer debugging |
| `RECOMP_FPS=1` | print fps and GL timings every second |
| `RECOMP_AUDIO=0` | no sound |
| `RECOMP_GX_HIDDEN=1` | hidden window (`--headless` does this too) |
| `RECOMP_GX_DUMP=dir`, `RECOMP_GX_DUMP_EVERY=N`, `RECOMP_GX_DUMP_FROM=N` | save presented frames as BMPs |
| `RECOMP_GX_LOG`, `RECOMP_GX_DEBUG_FRAME`, `RECOMP_GX_COPY_LOG`, `RECOMP_GX_SKIP`, `RECOMP_GX_DUMP_TEX(TURES)` | GX decoder debugging |
| `RECOMP_GL_DEBUG`, `RECOMP_GL_PRESENT`, `RECOMP_GPU` | OpenGL debugging and presenting path |
| `RECOMP_WIDESCREEN` | 16:9 |

(`RECOMP_*` names come from the reference runtime and still work.)

### `WONDERPARK_LOG` categories

`adxcheck axramp axvoice axvoices dec dsp efbcopy exc gxhw gxirq hook hostprof hwmisc idle ios ipc irq kpad memcpywatch mmio mmiostat mqwatch nandio pi pointer prof steps threads waits wpad wpadbtn`

The most useful: `waits` (where threads block), `steps` (the game's logic
steps per tick), `idle`, `efbcopy`, `gxirq`, `mmiostat` (slow memory
accesses), `axvoices`, `pointer`, `hostprof` (host profiler; see
`tools/analysis/hostprof.py`), `prof` (where slices end).

## Developer tools

| Key | What it does |
| --- | --- |
| backtick | debug overlay: fps and slowest frame; per-frame CPU / GPU / wait time; shader compiles, texture uploads, CPU readbacks, vertices, draw batches, EFB copies |
| F7 (overlay open) | game speed x1, x2, x4, x10 (the emulated clock runs faster; drawn frames stay capped by the display, the game catches up with more logic steps; sound is silent above x1) |
| Alt+Enter | fullscreen |

## Patches

None yet. Each patch in `config/patches/` is listed here with what it
changes.
