# Releasing Wonderpark

A release zip holds only what we're allowed to give away, and never anything from the game.

## Making one

1. Raise `WONDERPARK_VERSION` in `runtime/host/version.h` (the release is tagged `v` + that, e.g. `v0.1.0-beta`); the launcher shows it and the game prints it as it starts.
2. Commit everything, then run `python tools/make_release.py`. It builds the launcher, copies the committed files a release needs (below; submodules included) into `build/release/Wonderpark-<version>/` with the launcher at the top, runs the check below on the folder and on the zip, and writes `Wonderpark-<version>.zip` and its SHA-256.
3. Test it: unzip it into a new folder, run the launcher, place the disc, Build, Play.
4. On GitHub, draft a release with tag `v<version>`, the zip, its SHA-256 and the release notes (`docs/release-notes/`). The owner reviews and publishes it.

## What a release zip contains

- **The launcher**: `WonderparkLauncher.exe` (our code, with SDL3 compiled in, zlib license) and its fonts' licenses.
- **Our runtime, tools and builder as source**: `runtime/`, `tools/` (the Python scripts), `builder/`, `config/`, `CMakeLists.txt`, so the player's PC can recompile and build the game from their own disc. The submodules' source as pinned: `tools/dolrecomp` (GPL-3.0, with its `LICENSE`) and `tools/sdl3` (zlib, with its `LICENSE.txt`).
- **Our own assets and their licenses**: `assets/icon/` (our art, with `OFL-Fredoka.txt`), `assets/ui/pad-icons/` (CC BY-SA 4.0, with `LICENSE.txt`), `builder/launcher/fonts/` (SIL OFL 1.1, with the `-OFL.txt` files).
- **The papers**: `README.md`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `CONTRIBUTING.md`.

If a release ever ships a prebuilt `dolrecomp.exe` or any prebuilt object that contains DolRecomp's `cpu.c`, that part is GPL-3.0: include its license and a link to its exact source commit.

## What it never contains

- The recompiled game (`generated/`), the built game program (`build/game/`, `Wonderpark.exe`, `.pdb`), or anything else built from the game's code.
- Anything from the disc, extracted or converted (`game/`, disc images, archives, textures, layouts, fonts, text, sound, movies, the game's executable), the Japanese voices (`game/voices/`), or decoded icons and shader caches.
- Saves (`nand/`), settings, logs, screenshots or dumps from anyone's PC.
- Anything the player downloads themselves (decomp-toolkit, Python, the compilers).

## Checking a release

Before uploading, run the checker on the release folder and on the zip:

```powershell
python tools/check_release.py path\to\release-folder
python tools/check_release.py path\to\Wonderpark.zip
```

It looks at every file's name, folder and first bytes (disc images, the game's file formats by their magic numbers, recompiled code, the built game, saves, logs) and fails (exit status 1) with a list if anything is there. Don't upload until it says "Clean".

Releases are made by the owner, by hand, after this check. Wonderpark is free: no paid downloads, donations or ads.
