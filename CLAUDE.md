# Wonderpark

Unofficial static recompilation of PokéPark 2: Wonders Beyond (Wii) into a native Windows PC port. Target disc: US only, game ID **S2LE01**. Built from scratch (not a fork). Follow-up to the owner's SparkPark project (PokéPark 1).

The full plan and its phases live in `docs/handoff/` (copied from the owner's handoff kit). Read `docs/handoff/WONDERPARK-FULL-PROMPT.md` (the current plan; it supersedes `WONDERPARK-MASTER-PROMPT.md`) at the start of any session that isn't a small, obvious fix. Progress so far is in `docs/recomp-status.md`.

## Hard rules

- **Never commit game data.** No ISOs, extracted disc files, recompiled output, game textures, audio, fonts, text or decompiled game code. Everything game-derived is produced on the player's PC and git-ignored. If a change would put anything game-derived into git, stop and ask.
- **No game art in our UI.** The launcher and the F1 settings tab are drawn in code. The only exception is the extra rows in the game's own Options page, which use the game's art loaded at run time from the player's disc and never copied into the repo.
- **Everything stays on D:.** Repo at the repo folder, reference clones at refs\. The owner's C: drive is nearly full: never install to, cache on or build on C: when D: works. Point tool caches (pip, CMake, vcpkg, temp build dirs) at D:.
- **Stop at checkpoints.** When a plan doc says STOP, report and wait for the owner's go-ahead. Also ask before anything hard to undo: deleting files, rewriting git history, changing the license, publishing releases, pushing to GitHub.
- **Unofficial and clear about it.** Keep the README notice: "Wonderpark: an unofficial PokéPark 2 recomp", not affiliated with Nintendo, The Pokémon Company or Creatures. Never link to ISOs or ROMs. Warn players not to share their built game folder.
- **Credit references.** Runtime code reused from MildRain479/PokePark-Wiicompiled (CC0) gets credited in the README. Never copy its `src/` (decompiled game code). Check DolRecomp's GPL-3.0 terms before compiling any of its files into our executable.
- **No heredocs.** On this machine, never write files or scripts through shell heredocs or inline echo; always use the Write/Edit tools.
- **D: is a spinning hard drive.** No blocking disk I/O on the game or render thread, ever.

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
- `docs/`: `DEVELOPMENT.md` (where each kind of fix belongs, every setting and environment variable), the launcher mockup, the handoff docs.
- Git-ignored: `game/` (extracted disc), `generated/` (recompiled code), `build/`.

A fix to game behavior goes in `config/` patches, never as edits to generated code. Generated code is rebuilt from scratch and must stay reproducible.

## Conventions

- 64-bit only. Emulated Wii memory is one base + offset block; never hand the game raw host pointers.
- Options in `settings.ini`, controls in `bindings.ini`, saves in `%APPDATA%\Wonderpark\`. Every setting can be overridden by the environment variable `WONDERPARK_<NAME>`.
- Developer tools: backtick for the debug overlay, F7 for game speed (with the overlay open), `--headless` for automated tests, a skip-movies option.
- Commit in small steps with clear messages. Update `docs/DEVELOPMENT.md` whenever you add a setting, environment variable or patch.
- When something is unknown, check the game's code or ask. Don't guess and present it as fact. Put confirmed findings in the relevant handoff doc's "confirmed" notes so later sessions don't re-investigate.

## Commands

Fill these in during Phase 1, once the scripts exist, and keep them current:

- Verify and extract a disc: `python tools/extract_disc.py <disc image>` (into `game/`)
- Recompile: `python tools/recomp.py` (DolRecomp, into `generated/`)
- Build (dev): `python tools/build_game.py` (clang-cl + Ninja + ThinLTO, into `build/game/`; `--no-lto` faster to build, `--msvc` to use MSVC)
- Build (release): not yet separate from the dev build
- Run: `.\build\game\Wonderpark.exe` from the repo root (`RECOMP_FPS=1` prints frame rates; debug logs via `WONDERPARK_LOG=...`, listed in `docs/recomp-status.md`)
- Full pipeline: `python builder/pipeline.py <iso>` (verify, extract, recompile, build)
- Headless test: not yet (planned `--headless`)

## Owner context

The owner works on Windows and isn't always at their PC. They make decisions at the STOP checkpoints. Keep reports short and in plain language: what was found, what you recommend, what you need from them.
