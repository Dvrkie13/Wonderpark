# Wonderpark: an unofficial PokéPark 2 recomp

Wonderpark is an unofficial, work-in-progress static recompilation of
*PokéPark 2: Wonders Beyond* (Wii, US disc, game ID `S2LE01`) into a native
Windows PC program.

> **Wonderpark is not affiliated with, endorsed by or sponsored by Nintendo,
> The Pokémon Company, Creatures Inc. or Game Freak.** It is a fan project.
> It is also unrelated to the 2019 film *Wonder Park*.
> The project uses no official logos or game art.

## Bring your own disc

This repository contains **no game code, game assets or recompiled output**,
and it never will. To use Wonderpark you need your own legally obtained US
disc image (`S2LE01`). Everything game-derived (extracting the disc,
recompiling the executable and compiling the PC program) happens on your own
PC, and all of it stays out of git. Please don't open issues or PRs that
contain game files, and don't ask for disc images.

## Status

Work in progress (development preview). Played so far from the start of the
story through the first areas and the first attraction; a full playthrough
hasn't been done yet.

| Part | State |
| --- | --- |
| Disc check, extraction, CPU recompile (DolRecomp) | done |
| Boot, title, saves | works; saves in `%APPDATA%\Wonderpark`, survive restarts |
| Sound (CRI Atom over AX) | music and effects play |
| First area (walk, mini-game, battle, save/load) | plays at 30 fps |
| Wish Park, Seasong Beach | 30 fps; the game's own work is 10-14 ms of the 33 ms frame (PGO build) |
| First attraction (Cake Contraption, pointer) | plays to the end (mouse as pointer) |
| Movies (Mobiclip) | detected and played; picture and sound still to be confirmed; skipping doesn't work yet |
| Other attractions, chases, full playthrough | not checked yet |
| Launcher (one-time builder) | works: place the disc, build, play |
| Settings menu (F1), 60 fps | not built yet |

## Playing it (development preview)

You build the game on your own PC from your own disc, with the launcher.
Nothing from the game is downloaded or included here.

You need:

- Your own *PokéPark 2: Wonders Beyond* disc image, US version (`S2LE01`).
- Windows 10 or 11, 64-bit, and about 6 GB free disk space.
- [Python 3](https://www.python.org/) (3.10 or newer).
- [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/)
  with the "Desktop development with C++" workload (CMake and Ninja come
  with it), plus the "C++ Clang tools for Windows" component. Clang makes
  the game run about twice as fast as Microsoft's compiler; an
  [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) install also works.
- [decomp-toolkit](https://github.com/encounter/decomp-toolkit) (`dtk.exe`)
  on your PATH or in `tools\bin`, for reading a plain `.iso`, `.wbfs` or
  `.rvz` (NKit images are read without it).

Then, in the folder you unpacked (or cloned with
`git clone --recursive`), build the launcher once and open it:

```
python tools/build_launcher.py
.\build\launcher\WonderparkLauncher.exe
```

Click the logo, then the one button: **Place ISO** (pick your disc image),
**Build** (checks and extracts the disc, recompiles the game and compiles
`build\game\Wonderpark.exe`: 20-30 minutes, the window shows each stage),
then **Play**. Next time the launcher goes straight to Play, or offers
Rebuild after an update. Esc or closing the window stops a build. Without
the launcher: `python builder/pipeline.py path\to\your\disc.iso`, then
`.\build\game\Wonderpark.exe`.

Optional, for about a third less CPU time per frame: once you have a save
in a busy area,
`python tools/pgo_train.py`, then `python tools/build_game.py` again.

Controls: keyboard and mouse or a controller (`bindings.ini` in
`%APPDATA%\Wonderpark`); the mouse is the Wii Remote pointer. Alt+Enter
toggles fullscreen; backtick opens the debug overlay.

**Don't share your built game folder** (`game\`, `generated\`, `build\`):
it contains the game's code and files from your disc.

## Developers

`docs/DEVELOPMENT.md` lists the commands, settings, environment variables
and where each kind of change goes. `docs/recomp-status.md` has the progress
notes. Fetch the submodule first: `git submodule update --init --recursive`.
Disc extraction alone: `python tools/extract_disc.py path\to\your\disc.iso`
(checks the game ID and revision; NKit images are accepted).

## Repository layout

| Folder | Holds | In git |
| --- | --- | --- |
| `builder/` | Launcher and one-time builder | yes |
| `tools/` | Disc check, extraction, recompile scripts | yes |
| `config/` | Recompiler config, function names, patches | yes |
| `runtime/` | Graphics, audio, input, file I/O | yes |
| `docs/` | Design notes, launcher mockup | yes |
| `assets/` | The app icons (our own art) | yes |
| `game/` | Your extracted disc | no |
| `generated/` | Recompiled code | no |
| `build/` | Compiled program | no |

## Credits

- **MildRain479, [PokePark-Wiicompiled](https://github.com/MildRain479/PokePark-Wiicompiled)**
  (a recomp of PokéPark Wii: Pikachu's Adventure, CC0 1.0). Wonderpark's
  runtime is based on its `recomp/` runtime code. None of that project's
  decompiled game sources are used here.
- **MildRain479, [PokePark-2-Wiicompiled](https://github.com/MildRain479/PokePark-2-Wiicompiled)**
  (a recomp of PokéPark 2: Wonders Beyond, CC0 1.0). Wonderpark's free
  camera and field of view (`hle_camera.c`), scene tracking (`hle_game.c`),
  battle damage and chase speed options (`hle_modifiers.c`) and friends list
  (`hle_friends.c`, `hle_pad_art.c`) and in-game button prompts
  (`hle_prompts.c`, `boot/prompts.c`) are adapted from its code.
- Controller icons: Flat Gamepad Icons by RobTheFiveNine
  (https://robthefivenine.itch.io/flat-gamepad-icons), CC BY-SA 4.0. Home icon
  adapted in the same style. They're in `assets/ui/pad-icons/` with the
  license (`LICENSE.txt`); the CC BY-SA license covers those icons only, not
  Wonderpark's code. The pack's platform-logo icons are not included.
- **ExpansionPak, [DolRecomp](https://github.com/ExpansionPak/DolRecomp)**
  (GPL-3.0-or-later), the PowerPC-to-C recompiler, used as a git submodule.
  A `Wonderpark.exe` you build contains GPL-3.0 code from DolRecomp (its CPU
  helpers and the code it generates).
- **encounter, [decomp-toolkit](https://github.com/encounter/decomp-toolkit)**
  for disc extraction.
- Fonts: [Fredoka](https://fonts.google.com/specimen/Fredoka) and
  [Nunito](https://fonts.google.com/specimen/Nunito) (SIL Open Font License).
- The icons (`assets/icon/`) are part of the Wonderpark project: our own
  art, drawn in code, with no game assets. Windows uses the compass-star
  emblem (`wonderpark.ico`, `png-emblem/`). The rounded space tile with the
  "Wonderpark" name is kept for a later Android version; its wordmark is
  Fredoka Bold converted to outlines (SIL Open Font License,
  `assets/icon/OFL-Fredoka.txt`).

## License

Wonderpark's own code is released under CC0 1.0 Universal (see `LICENSE`),
matching the upstream runtime. This may change if a recompiler under a
different license ends up compiled into the program.

*PokéPark* and *Pokémon* are trademarks of Nintendo, The Pokémon Company,
Creatures Inc. and Game Freak. They are used here only to say which game
this project works with.
