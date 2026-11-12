# runtime/host

The Windows side.

- `input_win32.c`: keyboard, mouse and XInput controller to Wii Remote buttons and pointer; mouse capture.
- `audio_win32.c`: sound output (waveOut, 48 kHz, upsampled from the game's 32 kHz).
- `settings.c`: `settings.ini` and `bindings.ini`, `WONDERPARK_<KEY>` overrides.
- `overlay_win32.c`: the debug overlay (backtick) and game speed (F7); routes the window's messages and the overlay image to the F1 tab while it's open.
- `settings_ui.cpp`: the F1 settings tab (Direct2D/DirectWrite, drawn in code; pauses the game, applies and saves at once), and the PC Settings rows on the game's own Options page (drawn with the game's art and font, loaded from the player's disc at run time). `wonderpark.rc` builds the tab's fonts in.
- `host_stubs.c`: placeholders for reference-runtime features not built yet (Options menu, button prompts, Vulkan).
