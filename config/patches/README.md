# Game-behaviour patches

Fixes and changes to how **the game itself** behaves go here, never into the
generated code (which is rebuilt from scratch and must stay reproducible) and
not into `runtime/` (which is PC platform code: graphics, audio, input, files,
settings, UI).

Each patch is a C file in this folder. CMake compiles every `*.c` here into
`Wonderpark.exe`. A patch uses the same tools as the runtime:

- `WP_HOOK(0xADDR, name)` (`runtime/core/hle.h`) to replace or wrap a game
  function: `tools/recomp.py` inserts the call at that address in the
  generated code. Return 1 when the hook handled the call (set `c->pc`,
  usually with `wp_return(c)`), 0 to let the original run;
  `wp_call_original(c, addr)` runs the original from inside a hook.
- `LD32`/`ST32` and friends (`runtime/core/wp.h`) to read or write game
  memory, e.g. a value such as the frame length at `0x804B9C0C`.
- `g_settings` (`runtime/host/host_settings.h`) when a patch follows a
  setting.

Rules:

- One topic per file, named after it (`frame_rate.c`, `bloom.c`), with a
  comment saying what the game does, what the patch changes and how it was
  confirmed (address, function, test).
- Name addresses as in `config/S2LE01/symbols.txt` where possible.
- Never include or copy decompiled game code.
- After adding a hook, run `python tools/recomp.py` (hooks are patched into
  the generated code), then build.
- List each patch in `docs/DEVELOPMENT.md`.

There are no patches yet; the first planned ones are the frame-rate setting
(section 12 of the plan) and the bloom toggle.
