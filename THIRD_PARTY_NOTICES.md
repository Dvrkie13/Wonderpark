# Third-party notices

Everything in Wonderpark that wasn't written for it, with its license. Our own code is under the license in [LICENSE](LICENSE). Nothing from the game is in this repository: the game's code and files are read and built on each player's PC from their own disc.

## Code reused or adapted

### MildRain479/PokePark-Wiicompiled (CC0 1.0)

<https://github.com/MildRain479/PokePark-Wiicompiled>, a recomp of PokéPark Wii: Pikachu's Adventure. Released into the public domain under CC0 1.0 Universal; credited here as thanks.

Wonderpark's runtime started from its `recomp/` runtime code (not its decompiled game sources, which are never used). Adapted from it:

- `runtime/core/main.c`, `runtime/core/ref_compat.h`
- `runtime/gpu/`: `gl_backend.c`, `gx.h`, `gx_device.h`, `gx_fifo.c`, `gx_frames.c`, `gx_texture.c`, `window_win32.c` (the GX FIFO decoder and OpenGL renderer)
- `runtime/host/`: `audio_win32.c`, `host_audio.h`, `host_font.h`, `host_input.h`, `host_settings.h`, `input_win32.c`, `settings.c`, `version.h`
- `runtime/hw/hw_ax.c`
- `runtime/ios/`: `dvd.c`, `ios.c`, `nand.c`
- `runtime/hle/hle_wpad.c` (the WPAD layer, re-addressed for this game)

### MildRain479/PokePark-2-Wiicompiled (CC0 1.0)

<https://github.com/MildRain479/PokePark-2-Wiicompiled>, a recomp of PokéPark 2: Wonders Beyond. CC0 1.0 Universal. Adapted from its `recomp/patches/` (findings, addresses and code; its game sources are never used):

| Wonderpark file | Adapted from |
| --- | --- |
| `runtime/hle/hle_camera.c` | `S2LE01/hle_camera.c` (free camera, field of view) |
| `runtime/hle/hle_scene.c` | `S2LE01/hle_game.c` (scene tracking, the attraction id, the attraction test hook), `S2LE01/hle_autosave.c` (whether an event script is running) |
| `runtime/hle/hle_modifiers.c` | `S2LE01/hle_modifiers.c` (battle damage, chase speed) |
| `runtime/hle/hle_friends.c` | `S2LE01/hle_friends.c` (friendship data, name and character tables) |
| `runtime/host/settings_ui.cpp` (Friend Book) | `S2LE01/hle_pad_art.c` (reading the names and icons from the disc) |
| `runtime/hle/hle_prompts.c` | `S2LE01/hle_prompts.c`, `boot/prompts.c` (button prompts) |
| `runtime/hle/hle_wpad.c` | `hle_motion.c` (Flight of Fancy's steering) |
| `runtime/hle/hle_dance.c` | `S2LE01/hle_dance.c` (Dance Inferno timing hooks) |
| `runtime/core/idle.c` | `hle_irq.c` (idle handling) |
| `builder/launcher/wii_save.c`, `wii_save.h` | `boot/wii_save.c`, `boot/wii_save.h`, with the data.bin reading (AES and the Wii's key) taken out (save import and export: Dolphin's folder, zips) |

### DolRecomp (GPL-3.0-or-later)

<https://github.com/ExpansionPak/DolRecomp> by ExpansionPak. A git submodule in `tools/dolrecomp` (its own `LICENSE`, GNU GPL 3.0), not copied into this repository.

- Used as a tool: it translates the game's PowerPC code into C on the player's PC (`tools/recomp.py`, into git-ignored `generated/`).
- Compiled into the game: its CPU helpers `src/cpu/cpu.c` and `src/cpu/cpu.h` (built as the `wp_dolrecomp_cpu` library, `CMakeLists.txt`), and the helper templates it writes into every generated file. This is why Wonderpark itself is GPL-3.0-or-later. Wonderpark doesn't distribute built programs.

### SDL3 (zlib license)

<https://github.com/libsdl-org/SDL>, a git submodule in `tools/sdl3` pinned to release 3.4.20 (its own `LICENSE.txt`), compiled into the game and the launcher (joystick, gamepad, haptic, HIDAPI and sensor parts only) for controllers.

> Copyright (C) 1997-2026 Sam Lantinga \<slouken@libsdl.org\>. This software is provided 'as-is', without any express or implied warranty. (zlib license; full text in `tools/sdl3/LICENSE.txt`.)

### zlib's puff.c (zlib license)

The launcher's `.zip` reader (`builder/launcher/mods.cpp`) inflates DEFLATE data with our own code written after Mark Adler's `puff.c` from zlib (by Mark Adler, zlib license), following RFC 1951. No zlib code is included as-is.

## Tools run but not included

- **decomp-toolkit** (`dtk.exe`) by encounter, <https://github.com/encounter/decomp-toolkit> (MIT or Apache-2.0, per its repository): reads the player's disc image (`tools/extract_disc.py`). The player downloads it; it isn't in this repository.
- **Python 3**, **CMake**, **Ninja**, **Clang / MSVC**: the build tools on the player's PC.
- **fontTools** (MIT) and **Brotli** (MIT), only to make `docs/banner.svg`'s embedded fonts (`tools/make_banner.py`).

## Fonts (SIL Open Font License 1.1)

- **Fredoka**, Copyright 2016 The Fredoka Project Authors. **Nunito**, Copyright 2014 The Nunito Project Authors.
- In `builder/launcher/fonts/` with their licenses (`Fredoka-OFL.txt`, `Nunito-OFL.txt`); built into the launcher and the game (`runtime/host/wonderpark.rc`). `assets/icon/OFL-Fredoka.txt` covers the icon's wordmark (Fredoka Bold turned into outlines), and `docs/banner.svg` embeds small subsets of both fonts.

## Art

- **Flat Gamepad Icons** by RobTheFiveNine, <https://robthefivenine.itch.io/flat-gamepad-icons>, CC BY-SA 4.0: the controller and keyboard button icons in `assets/ui/pad-icons/` (license in `assets/ui/pad-icons/LICENSE.txt`). The home icon (`home.*.png`) is ours, adapted in the same style (drawn by `tools/make_home_icon.py`) and shared under the same CC BY-SA 4.0 license. The pack's three platform-logo icons (`ps_super`, `xb_super`, `switch_super`) are not included. CC BY-SA covers these icons only, not Wonderpark's code.
- Everything else is our own, drawn in code with no game assets: the Wonderpark icon and compass-star emblem (`assets/icon/`), the banner (`docs/banner.svg`, `docs/banner.png`), the launcher, and the F1 notebook (cover, gold corners, paw prints, stickers).

## Trademarks

*Pokémon* and *PokéPark* are trademarks of Nintendo, The Pokémon Company, Creatures Inc. and Game Freak. Wonderpark is an unofficial, non-commercial fan project, not affiliated with or endorsed by them; the names are used only to say which game it works with.
