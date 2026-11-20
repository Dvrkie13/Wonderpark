# Wonderpark

Unofficial static recompilation of PokéPark 2: Wonders Beyond (Wii) into a native Windows PC port. Target disc: US only, game ID **S2LE01**. Built from scratch (not a fork). Follow-up to the owner's SparkPark project (PokéPark 1).

The full plan and its phases live in `docs/handoff/` (a git-ignored copy of the owner's handoff kit: on the owner's PC only, never committed). Confirmed findings for contributors are in `docs/research.md`. Read `docs/handoff/WONDERPARK-FULL-PROMPT.md` (the current plan; it supersedes `WONDERPARK-MASTER-PROMPT.md`) at the start of any session that isn't a small, obvious fix. Progress so far is in `docs/recomp-status.md`.

## Hard rules

- **Never commit game data.** No ISOs, extracted disc files, recompiled output, game textures, audio, fonts, text or decompiled game code. Everything game-derived is produced on the player's PC and git-ignored. If a change would put anything game-derived into git, stop and ask.
- **No game art in our UI.** The launcher and the F1 settings tab are drawn in code. The only exception is the extra rows in the game's own Options page, which use the game's art loaded at run time from the player's disc and never copied into the repo.
- **The owner's PC.** Where things go on this machine (drives, caches, reference clones) is in `CLAUDE.local.md` (git-ignored). Follow it when it's there.
- **Stop at checkpoints.** When a plan doc says STOP, report and wait for the owner's go-ahead. Also ask before anything hard to undo: deleting files, rewriting git history, changing the license, publishing releases, pushing to GitHub.
- **Unofficial and clear about it.** Keep the README notice: "Wonderpark: an unofficial PokéPark 2 recomp", not affiliated with Nintendo, The Pokémon Company or Creatures. Never link to ISOs or ROMs. Warn players not to share their built game folder.
- **Credit references.** Runtime code reused from MildRain479/PokePark-Wiicompiled (CC0) gets credited in the README. Never copy its `src/` (decompiled game code). Wonderpark is GPL-3.0-or-later (DolRecomp's CPU code is compiled into the exe); only copy code under GPL-3.0-compatible licenses.
- **No heredocs.** On this machine, never write files or scripts through shell heredocs or inline echo; always use the Write/Edit tools.
- **No blocking disk I/O on the game or render thread, ever.** Players may have spinning hard drives.

## Friend Book rules

- **Portraits are the Pokémon's real in-game icons**, the same ones the game's own PokéPark Pad shows. Read them from the player's disc at run time (MildRain479/PokePark-2-Wiicompiled's hle_pad_art.c, CC0, shows how) and keep them in memory only. Never draw letter or placeholder portraits in the shipped game, and never commit, cache to disk or ship the icons.
- **All 194 befriendable Pokémon** (partners excluded) must appear, in six areas: Cove Area (Port Town, Seasong Beach), Arbor Area (Windmill Way, Verdant Courts), Crag Area (Rockridge Fort, Colosseum), Tech Area (Train Depot, Unawarehouse, Scientorium), Arcane Area (Lakeshore) and Wish Park (Cake, Dance, Power and Flight Zones). Names, areas, places and zones come from the game's data. The roster checklist (roster-checklist.csv) is a local test checklist only: keep it in a git-ignored folder, never commit it.

## Game sound rule

- **Nothing from the game plays before the game is fully recompiled.** No music, cry or sound from the game anywhere (launcher, F1 books, anything) until the player's own disc is verified AND the build has finished successfully. Before that, and after a failed, cancelled or deleted build, the launcher is silent.

## Shelved for later (do not build until the owner says so)

- **Multiplayer, local and online.** Player 1 only for now: no Multiplayer tab, no networking. The Wii Remotes 2-4 code stays in the repo but is off by default behind a disabled setting. The game's own "Multiplayer Attractions" menu stays as the game made it. The research is kept in the owner's handoff kit (`later/`) for when it comes back.

## Removed features (do not build)

- **3D glasses / stereo 3D** (red-cyan, side by side) was removed from the Silly settings on the owner's request. Don't add it back, don't add a `stereo_3d` setting, and remove any leftover code, settings.ini key, env var or doc mention of it.

## Key facts

- Disc: S2LE01, v1.00. Clean ISO SHA-1 `e429a232431ea7075ae959b030a7675846357c19` (from GameTDB). A mismatch is a warning, not an error.
- Developer Creatures, internal name "PokePark Wii2". Likely shares engine and SDK code with PokéPark 1, but it is a different executable.
- Recompiler: DolRecomp (CPU only) plus the reference runtime. NWiiRecomp is reference only. See `docs/handoff/RECOMPILER.md`.
- Audio: CRI (.acb/.awb). Probably outputs through Nintendo AX; confirm first. See `docs/handoff/AUDIO-VIDEO.md`.
- Frame timing: 0x804B9C0C holds the frame length (0x1EEE10 = 29.97 FPS). See `docs/handoff/FRAMERATE.md`.
- Bloom: the Gecko code nops 0x8011D91C.
- Controls: sideways Wii Remote. The draft mapping is in `docs/handoff/CONTROLS-DRAFT.md`.
- Settings: basics go in the game's own Options page; everything goes in the F1 tab. See `docs/handoff/SETTINGS.md` and the mockups in `docs/settings/`.
- Launcher: match `docs/launcher-mockup.html` (starry night sky, compass-star emblem, one main button).
- No motion controls, ever required: keyboard + mouse and controller only (`docs/handoff/CONTROLS-DRAFT.md`).
- Performance plan: `docs/handoff/PERFORMANCE.md`. Voice switch: `docs/handoff/VOICE-LANGUAGE.md`.

## Where things go

- `tools/`: disc check, extraction, recompile and build scripts.
- `config/`: recompiler config, function names, and **patches to game behavior** (fixes to how the game itself acts).
- `runtime/`: **PC platform code** (graphics, audio, input, file I/O, settings, UI). Each module gets its own README.
- `builder/`: the launcher and one-time builder.
- `docs/`: `DEVELOPMENT.md` (where each kind of fix belongs, every setting and environment variable), `research.md` (confirmed findings), the launcher mockup. (`docs/handoff/` is local only.)
- Git-ignored: `game/` (extracted disc), `generated/` (recompiled code), `build/`.

A fix to game behavior goes in `config/` patches, never as edits to generated code. Generated code is rebuilt from scratch and must stay reproducible.

## Conventions

- 64-bit only. Emulated Wii memory is one base + offset block; never hand the game raw host pointers.
- Options in `settings.ini`, controls in `bindings.ini`, saves in `%APPDATA%\Wonderpark\`. Every setting can be overridden by the environment variable `WONDERPARK_<NAME>`.
- Developer tools: backtick for the debug overlay, F7 for game speed (with the overlay open), `--headless` for automated tests, a skip-movies option.
- Commit in small steps with clear messages. Update `docs/DEVELOPMENT.md` whenever you add a setting, environment variable or patch.
- When something is unknown, check the game's code or ask. Don't guess and present it as fact. Put confirmed findings in `docs/research.md` (plain, no personal notes or machine paths) so later sessions don't re-investigate.

## Commands

Fill these in during Phase 1, once the scripts exist, and keep them current:

- Verify and extract a disc: `python tools/extract_disc.py <disc image>` (into `game/`)
- Japanese voices (optional, the player's own S2LJ01 disc): `python tools/extract_voices.py <iso>` (into git-ignored `game/voices/ja/`; the launcher does it when that disc is dropped)
- Recompile: `python tools/recomp.py` (DolRecomp, into `generated/`)
- Build (dev): `python tools/build_game.py` (clang-cl + Ninja + ThinLTO, into `build/game/`; `--no-lto` faster to build, `--msvc` to use MSVC)
- PGO profile: `python tools/pgo_train.py --save <nand dir> ...` (into git-ignored `build/pgo/`; `build_game.py` then uses it, `--no-pgo` to skip)
- Build (release): not yet separate from the dev build
- Run: `.\build\game\Wonderpark.exe` from the repo root (`RECOMP_FPS=1` prints frame rates; debug logs via `WONDERPARK_LOG=...`, listed in `docs/recomp-status.md`)
- Full pipeline: `python builder/pipeline.py <iso>` (verify, extract, recompile, build); `--rebuild` from game/, `--status`
- Launcher: `python tools/build_launcher.py`, then `.\build\launcher\WonderparkLauncher.exe` (test hooks in `docs/DEVELOPMENT.md`)
- Headless test: `--headless` (no window; `--exit-after <seconds>`), with the test hooks in `docs/DEVELOPMENT.md`
- Banner: `python tools/make_banner.py` (needs fontTools and brotli; rebuilds `docs/banner.svg`'s fonts and `docs/banner.png`)
- Release check: `python tools/check_release.py <folder>` (fails if a release folder holds game code or disc files; `docs/RELEASING.md`)

## Owner context

The owner makes decisions at the STOP checkpoints. Keep reports short and in plain language: what was found, what you recommend, what you need from them. (More in `CLAUDE.local.md`.)
