# runtime/hle

High-level replacements for Nintendo SDK functions (`WP_HOOK`), where
emulating the hardware underneath isn't worth it.

- `hle_os.c`: audio-system and interrupt-enable functions; `SCGetAspectRatio`, which tells the game the TV is 16:9 when the Widescreen setting is on (the game then runs in its own 16:9 mode).
- `hle_bt.c`: Bluetooth stack start-up (no real Bluetooth).
- `hle_cache.c`: cache flush and invalidate (no-ops: there is no cache to keep coherent).
- `hle_frame.c`: the Frame rate setting: at 60, the game's frame timer's 30 Hz request becomes 60 Hz each frame, except while a movie plays.
- `hle_wpad.c`: the Wii Remote (WPAD): buttons from the keyboard and controller, the pointer from the mouse.
- `hle_prompts.c`: button prompts: the font's Wii Remote button glyphs, the field's action bubble and the title's "Please Press" redrawn as the bound pad button's icon or key cap (`runtime/host/pad_icons.cpp`); adapted from MildRain479/PokePark-2-Wiicompiled (CC0).
- `hle_kpad.c`: KPAD logging.
- `hle_debug.c`: debugging hooks, active only with their `WONDERPARK_LOG` category.

Game-specific behaviour changes don't go here; they go in `config/patches/`.
- `hle_string.c`: native strcmp and the game's resource-by-name lookups (0x802330F0, 0x80233380). A scene's loading looks up every resource by name; recompiled, that was a 650 ms frame in the Cake Contraption's character select.
- `hle_mtx.c`: native PSMTXIdentity, PSMTXCopy, PSMTXConcat and PSMTXMultVec (the SDK's paired-single matrix code, called hundreds of thousands of times a second).
