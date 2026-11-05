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

Early development. The opening area plays from start to save.

| Step | State |
| --- | --- |
| Repo, disc check and extraction | done |
| CPU recompile (PowerPC to C, DolRecomp) | done |
| Boot to title | reaches the title screen at full speed (30 fps) |
| Sound (CRI Atom over AX) | plays (music, effects) |
| First area (walk, mini-game, battle, save/load) | plays at 30 fps (dips only while loading) |
| First attraction (Wish Park, pointer) | Cake Contraption plays to the end (mouse as pointer) |
| Full playthrough, launcher | not started |

## Extracting your disc (developers)

Requires Python 3 and [decomp-toolkit](https://github.com/encounter/decomp-toolkit) (`dtk`).

```
python tools/extract_disc.py path\to\your\disc.iso
```

The script checks that the disc's game ID is `S2LE01`, then extracts
`main.dol`, any `.rel` modules and the file tree into `game/` (git-ignored).
NKit-processed `.iso` images are also accepted.

## Building (developers)

Also needs CMake, Ninja and Visual Studio Build Tools (C++). clang-cl is
strongly recommended (the game runs about twice as fast built with it): the
Build Tools' "C++ Clang tools" component, or an llvm-mingw install. Fetch the submodule
first: `git submodule update --init --recursive`.

```
python builder/pipeline.py path\to\your\disc.iso
```

This verifies and extracts the disc, recompiles `main.dol` with DolRecomp
(`tools/recomp.py`), and compiles `build/game/Release/Wonderpark.exe`.
It reaches the title screen but isn't playable yet.

## Repository layout

| Folder | Holds | In git |
| --- | --- | --- |
| `builder/` | Launcher and one-time builder | yes |
| `tools/` | Disc check, extraction, recompile scripts | yes |
| `config/` | Recompiler config, function names, patches | yes |
| `runtime/` | Graphics, audio, input, file I/O | yes |
| `docs/` | Design notes, launcher mockup | yes |
| `game/` | Your extracted disc | no |
| `generated/` | Recompiled code | no |
| `build/` | Compiled program | no |

## Credits

- **MildRain479, [PokePark-Wiicompiled](https://github.com/MildRain479/PokePark-Wiicompiled)**
  (a recomp of PokéPark Wii: Pikachu's Adventure, CC0 1.0). Wonderpark's
  runtime is based on its `recomp/` runtime code. None of that project's
  decompiled game sources are used here.
- **ExpansionPak, [DolRecomp](https://github.com/ExpansionPak/DolRecomp)**
  (GPL-3.0-or-later), the PowerPC-to-C recompiler, used as a git submodule.
  A `Wonderpark.exe` you build contains GPL-3.0 code from DolRecomp (its CPU
  helpers and the code it generates).
- **encounter, [decomp-toolkit](https://github.com/encounter/decomp-toolkit)**
  for disc extraction.
- Fonts: [Fredoka](https://fonts.google.com/specimen/Fredoka) and
  [Nunito](https://fonts.google.com/specimen/Nunito) (SIL Open Font License).

## License

Wonderpark's own code is released under CC0 1.0 Universal (see `LICENSE`),
matching the upstream runtime. This may change if a recompiler under a
different license ends up compiled into the program.

*PokéPark* and *Pokémon* are trademarks of Nintendo, The Pokémon Company,
Creatures Inc. and Game Freak. They are used here only to say which game
this project works with.
