# Recompile status

DolRecomp `02e27ac`, C backend, Broadway profile, run on the S2LE01 `main.dol`.

## DolRecomp run

| | |
| --- | --- |
| Mode | Wii / Broadway (needs `database/titles.txt`; `tools/recomp.py` creates an empty one in `build/recomp-work`, otherwise DolRecomp silently uses GameCube mode) |
| Time | ~0.6 s with `-j14` |
| text0 | 2,504 words: 850 instructions, 1,654 embedded data, 0 unknown |
| text1 | 1,057,160 words: 1,032,268 instructions, 24,892 embedded data, 0 unknown |
| Output | 260 chunks of up to 4,096 instructions (~265 MB of C) |
| Functions | DolRecomp doesn't split into functions (it tiles `.text`). Our own `bl`-target scan finds ~9,600 functions |
| Unknown / unsupported opcodes | none |
| Functions it couldn't find | n/a (no function discovery; every word of `.text` is covered) |

### Self-modifying-code flags: 48, none real

Full list with classification: `config/S2LE01/smc_review.toml`. `tools/recomp.py`
fails if a flag shows up that isn't listed there.

- 44 stores (`stw`/`sth`/`stb`/`stfs`/`psq_st`): ordinary stores through
  pointer arguments. DolRecomp's constant tracker carried a stale base
  register across a `blr` into the next function, so it believed the target
  was in `.text`.
- 4 `icbi`: SDK cache flush/invalidate loops (`0x800043AC`, `0x8028E13C`,
  `0x8040D5A0`) and the exception stub (`0x8000494C`). They change no code.

No code patches are needed. Code the OS copies at runtime (exception
handlers in low memory) is outside `.text` and was never recompiled; the
runtime has to handle exceptions itself (see below).

### Instructions left to the host

DolRecomp emits a fallback call (then returns to the dispatcher) for 387
sites: supervisor SPR moves (`mtspr`/`mfspr` of BATs, HID0/1, SPRG0–3, DEC,
L2CR, PMCs, WPAR, …) and `dcbf`/`dcbst`/`dcbi`/`icbi`. `runtime/core/main.c`
emulates them (SPRs in a table, cache operations as no-ops) and resumes at
`cia + 4`.

## Build

`cmake` builds `Wonderpark.exe` from the 260 chunks, DolRecomp's `cpu.c`
(from the submodule) and `runtime/core/main.c`. MSVC (VS Build Tools 2026),
`/O2`: ~1.5 minutes on this PC with parallel builds; the exe is ~141 MB.
No errors.

## First launch

`build/game/Release/Wonderpark.exe [game/sys/main.dol]`. Environment:
`WONDERPARK_MAX_SLICES=N` stops after N dispatcher slices;
`WONDERPARK_MMIO_LOG=1` logs every hardware register access.

It runs `__start` and the OS init (BATs, caches, L2, EXI, VI, PI, DSP/AI
registers: ~70 MMIO accesses, all ignored for now) until:

```
exception 00000020 at 00000800, srr0 80004584 lr 8028DD78
```

That's a floating-point-unavailable exception. The Wii OS runs with MSR[FP]
off and turns it on lazily from its own exception handler, which it copies
into low memory at runtime, so that handler isn't recompiled. This is the
first item for step 3 (boot to title), together with:

- exceptions and interrupts handled by the runtime (the reference's
  `hle_irq.c`/`hle_thread.c` approach: guest threads on host fibers);
- the time base paced on host time (currently it advances by guest cycles);
- the trap-based hook mechanism, then the reference's HLE patches ported to
  `CPUState` with PokéPark 2 addresses (EXI, IOS, DVD, AX/AI, GX, WPAD).


# Phase 2, step 3: boots to the title screen

The recompiled game runs from `__start` through the OS, IOS, CRI Atom and
GX setup, shows the wrist-strap screen, the logo and the title screen (Wish
Park's fountain, "Please Press 2"), and takes Wii Remote input from the
keyboard, mouse or an XInput controller.

## How the runtime is put together

| Part | Approach | Code |
| --- | --- | --- |
| CPU | DolRecomp C output, run from a dispatcher loop in slices | `runtime/core/dispatch.c` |
| Hooks | `WP_HOOK(0xADDR, name)` in C; `tools/recomp.py` inserts a check at that address's label in the generated code and writes the table | `runtime/core/hle.h`, `tools/recomp.py` |
| Exceptions | The OS's own handlers: the runtime does what the copied `__OSEVStart` stub would (it isn't recompiled), then continues in the handler | `runtime/core/cpu_host.c` |
| Interrupts | Real exception entries between slices (PI external, decrementer); the OS's dispatcher and handlers run unchanged | `cpu_host.c`, `runtime/hw/hw_pi.c` |
| Host callbacks | "Host interrupts": an external-interrupt entry, then the callbacks inside an OS-style scratch context, then `__OSReschedule` + `OSLoadContext`; no nested host calls, so thread switches work as on hardware | `runtime/core/hostirq.c` |
| Threads | Nothing special: guest context switches are just state changes in the dispatcher model | |
| Time | Time base on host time (game-settable); devices on a monotonic host clock | `cpu_host.c` |
| IOS | IPC hardware registers; requests answered by IOS device code ported from the reference (`/dev/di` from the extracted files, `/dev/fs` NAND in `nand/`, `/dev/es`, `/dev/stm`) | `runtime/hw/hw_ipc.c`, `runtime/ios/` |
| VI | Registers plus a 60 Hz retrace interrupt | `runtime/hw/hw_vi.c` |
| DSP / AX | Mailboxes, the boot-ROM handshake and the AX ucode's mail protocol at a high level; AI DMA timer drives AX frames | `runtime/hw/hw_dsp.c`, `hw_ai.c` |
| GPU | Write-gather pipe into the PI FIFO; CP/PE registers and token/finish interrupts; the reference's FIFO decoder and OpenGL renderer | `runtime/hw/hw_gx.c`, `runtime/gpu/` |
| Wii Remote | The reference's WPAD layer, re-addressed for S2LE01, on host interrupts and timers; keyboard/XInput from the reference's input layer | `runtime/hle/hle_wpad.c`, `runtime/host/input_win32.c` |
| Locked cache | 16 KB scratch with its DMA | `runtime/hw/hw_lc.c` |

28 hooks so far (Bluetooth, WPAD, cache maintenance, `__OSInitAudioSystem`).
Function names come from `tools/analysis/sigmatch.py` (2,502 named,
`config/S2LE01/symbols.txt`).

## Known gaps

- Wii Remote: buttons and shake only; no pointer (WPADIsDpdEnabled /
  WPADGetDpdFormat aren't hooked yet), no motion mapping (Phase 3).
- Vulkan renderer not built (needs the shader build step); OpenGL is used.
- The reference's Options menu, update check and PokePark 1 gameplay sliders
  are stubbed (`runtime/host/host_stubs.c`); `settings.ini` still has
  PokePark 1 fields.
- The movie player (Mobiclip) hasn't been reached yet.

Dev switches: `WONDERPARK_LOG=` (`exc,dec,ipc,ios,dsp,wpad,irq,hook,prof,threads,mmio`),
`WONDERPARK_MAX_SLICES=N`, `WONDERPARK_PAD_SCRIPT="8:a 8.2: 12:2"`,
`RECOMP_GX_LOG=1` (GPU decoder).


# Speed pass

Title screen (Wish Park plaza behind the logo, a Mobiclip video decoded by
the game on the CPU), Radeon RX 9060 XT, OpenGL:

| Change | Title fps |
| --- | --- |
| Start (MSVC, every access through cpu.c) | 12.3 |
| Inline RAM fast path (`runtime/core/wp_cpu_fast.h`), one clock read per slice, slices of many dispatches | 12.9 |
| Recompiled code built with clang-cl instead of MSVC | 26.6 |
| SwapBuffers instead of the DXGI interop present on non-NVIDIA GPUs | **30.0** (the game's own rate) |
| Idle loop sleeps until the next event | 30.0, CPU busy ~50-65% |

2D screens run at 60. The title bar's "busy %" is now real headroom.

Notes:
- `tools/build_game.py` picks clang-cl (VS's own, `$WONDERPARK_CLANG_CL`,
  PATH, or an llvm-mingw install with a `clang-cl.exe` hard link next to
  `clang-23.exe`) and builds with Ninja; `--msvc` forces MSVC. clang takes
  about 8 minutes for the recompiled code (MSVC: 1.5) but the result runs
  twice as fast.
- Slices now run block after block until the budget is spent; so that
  interrupts aren't starved, `OSEnableInterrupts`/`OSRestoreInterrupts` are
  hooks and a slice ends as soon as interrupts are on and one is due.
  `__AI_SRC_INIT` is skipped (its timing loop can't pass at slice granularity;
  the reference skips it too).
- `WONDERPARK_LOG=hostprof` samples the host (needs
  `tools/build_game.py --debug-info` for names in the recompiled code);
  `tools/analysis/hostprof.py` maps samples back to guest functions.

## Input

Works: the title takes 2 (controller A, keyboard Space or X by default) and
the save-select and main menus respond. What was wrong:

- `WPADIsDpdEnabled` wasn't hooked, so KPAD asked for the IR camera on every
  sample (now hooked, `0x802D6900`).
- The test script's presses landed during the logo intro or the attract-mode
  cutscene; script times now count from program start.

`WONDERPARK_LOG=kpad` logs what `KPADRead` (`0x802EAB70`) hands the game,
`WONDERPARK_LOG=wpadbtn` what `WPADRead` reports.


# Phase 2, step 4: sound

Sound plays: title music, menu sounds and effects, through the attract-mode
cutscene, at 30 fps with no measurable cost.

How (all hardware-level, no game addresses needed):
- **AX command lists** (`runtime/hw/hw_ax.c`): each frame's list (mailed to
  the DSP model) is run: `PROCESS` walks the voice list and mixes every
  running voice (the reference runtime's decoder: DSP-ADPCM, PCM16, PCM8,
  resampling, envelope, low-pass), `MIX_AUXA-C` hands the aux sends to the
  game's effect callbacks and mixes their output back, `OUTPUT` writes the
  stereo mix into the buffer AX gave.
- **AI DMA** (`runtime/hw/hw_dsp.c`) "plays" the buffer the game points it at,
  to the host: `waveOut` at 32 kHz (the reference runtime's backend).
- CRI Atom runs unchanged on top: ADX streams are read from the disc and
  decoded by CRI's own (recompiled) code, then copied into AX voices.

Checked along the way (the menu's stream sounded like noise in an automatic
test; it turned out to be noise-like by nature): the recompiled ADX decoder
gives exactly what an independent decode gives, from the right file bytes,
frame-aligned, and memcpy copies it exactly.

Quality (after "sounds a bit grainy"):
- Voices are resampled with 4-tap cubic interpolation over the PB's sample
  history (was linear), and their volume ramps are applied per sample
  (written back only while ramping: writing unchanged volumes back silenced
  voices).
- A limiter stands in for AX's compressor, so loud passages don't clip.
- The device runs at 48 kHz; the 32 kHz mix is upsampled by a 72-tap
  windowed-sinc filter (flat to 8 kHz, -2 dB at 14 kHz, artifacts ~90 dB down).
- Output had no dropouts or underruns (`WONDERPARK_LOG=audiostats`).

Known gaps:
- AX's compressor (`0A`) itself isn't emulated (the limiter replaces it).
- Wii Remote speaker sound (`WM_OUTPUT`) is dropped.
- Not yet listened to by a person; cutscene audio/video sync not checked.

Debug switches: `WONDERPARK_AUDIO_DUMP=file.wav`, `WONDERPARK_AX_NOAUX=1`,
`WONDERPARK_AX_SNAP=<frame>` (voice buffers to build/), `WONDERPARK_LOG=`
`axvoice,axvoices,axlist,adxcheck,memcpywatch`; CMake `-DWP_STORE_WATCH=ON` with
`WONDERPARK_WATCH=lo-hi` reports which guest code stores into a range.
