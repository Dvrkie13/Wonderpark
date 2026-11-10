# runtime/gpu

The GX graphics pipeline (ported from the reference runtime).

- `gx_fifo.c`: decodes the GX command stream (BP/CP/XF registers, vertices, display lists, EFB copies) as the game sends it.
- `gx_frames.c`: frames, presentation pacing, the fps report and the debug overlay's counters.
- `gx_texture.c`: decodes Wii texture formats.
- `gl_backend.c`: draws with OpenGL (one shader for all GX states), EFB copies, presenting, frame dumps.
- `window_win32.c`: the window (hidden with `--headless`).
