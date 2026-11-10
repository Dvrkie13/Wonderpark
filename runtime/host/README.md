# runtime/host

The Windows side.

- `input_win32.c`: keyboard, mouse and XInput controller to Wii Remote buttons and pointer; mouse capture.
- `audio_win32.c`: sound output (waveOut, 48 kHz, upsampled from the game's 32 kHz).
- `settings.c`: `settings.ini` and `bindings.ini`, `WONDERPARK_<KEY>` overrides.
- `overlay_win32.c`: the debug overlay (backtick) and game speed (F7).
- `host_stubs.c`: placeholders for reference-runtime features not built yet (Options menu, button prompts, Vulkan).
