# runtime/hle

High-level replacements for Nintendo SDK functions (`WP_HOOK`), where
emulating the hardware underneath isn't worth it.

- `hle_os.c`: audio-system and interrupt-enable functions.
- `hle_bt.c`: Bluetooth stack start-up (no real Bluetooth).
- `hle_cache.c`: cache flush and invalidate (no-ops: there is no cache to keep coherent).
- `hle_wpad.c`: the Wii Remote (WPAD): buttons from the keyboard and controller, the pointer from the mouse.
- `hle_kpad.c`: KPAD logging.
- `hle_debug.c`: debugging hooks, active only with their `WONDERPARK_LOG` category.

Game-specific behaviour changes don't go here; they go in `config/patches/`.
