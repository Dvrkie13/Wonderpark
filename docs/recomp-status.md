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
- Short beeps in the first cutscene when the game lags (reported by ear).
  Likely cause: when AX's frame comes late, the AI DMA model plays the same
  3 ms buffer again, which repeats as a ~333 Hz tone. Fix: play silence (or
  fade) when no new frame has been mixed since the last DMA tick.
- AX's compressor (`0A`) itself isn't emulated (the limiter replaces it).
- Wii Remote speaker sound (`WM_OUTPUT`) is dropped.
- Not yet listened to by a person; cutscene audio/video sync not checked.

Debug switches: `WONDERPARK_AUDIO_DUMP=file.wav`, `WONDERPARK_AX_NOAUX=1`,
`WONDERPARK_AX_SNAP=<frame>` (voice buffers to build/), `WONDERPARK_LOG=`
`axvoice,axvoices,axlist,adxcheck,memcpywatch`; CMake `-DWP_STORE_WATCH=ON` with
`WONDERPARK_WATCH=lo-hi` reports which guest code stores into a range.

# Phase 2, step 5: first area

Played from save slot 1 through the opening cave, about 25 minutes of play,
with no crashes or emulation errors (the log holds only the startup MMIO
warnings for unused EXI/SI registers).

Covered:
- **Walking, camera, jumping**: the jump tutorial (ledges) and the cave path,
  with the minimap tracking progress.
- **Objects and items**: breaking the "?" box with ① gives a Berry.
- **A mini-game, start to finish**: Piplup's tag game (timer, ① dash, tagging
  him), won; the reward Berries and the friend entry are credited.
- **A battle**: the Timburr tutorial (Thunderbolt by holding ①, HP gauges,
  3-minute timer, effects). My scripted input lost the fight on skill (no
  dodging), and play returned to the field as it should.
- **Menus**: the PokéPark Pad (Pals, Friend List, Important Items, Collection,
  Photos, Save).
- **Save and load**: Pad > Save wrote `GD0`/`GD0M` in the emulated NAND. Then
  "quit the game" went back to the title screen (a soft reset in-game).
  Slot 1 then showed the new play time (0:16) and loaded back into the cave:
  no intro replay, 11 Berries, 1 Friend, the same objective ("Go to Seasong
  Beach with Piplup"), and the explored minimap kept.

Not reached yet: the hub's **attractions** (Wish Park). They come later in
the story than the cave, past Seasong Beach.

Controls notes: in menus, ② (Space/X) confirms (A/Enter does not), and the
d-pad (arrow keys) moves. Dialogue only takes a press once the text has
finished typing.

## Speed in 3D areas

Over the session: mean 26.6 fps; 54% of seconds at a full 30 fps, 16% at
24-29, 13% at 18-24, 15% below 18 (mostly exactly 15).

The 15 fps cluster was pacing, not raw speed; fixed below.

# Speed: the 15 fps drops

**Cause.** The main loop (`0x8002D224`) waits for a 30 Hz alarm, then runs
the frame function (`0x8002CCE0`). That function asks `0x800333A0` how many
fixed 60 Hz logic steps have elapsed. If more ran than two, it sets a flag
that skips drawing on the next tick, to catch up. A drawing tick does its
rendering *before* asking for the step count, while a skipping tick asks at
once. So after one long frame, the gap from the skip tick to the next drawing
tick is 33 ms plus the drawing tick's pre-step work. If that work is over
about 15 ms, the gap is three steps again, and the game stays locked at half
rate. In the PokePark Pad (about 9,600 draws a frame) and in battles, the
pre-step work was 15-20 ms. About half of that was our GPU decoding and
drawing on the game thread, which on a Wii runs in parallel and costs the
CPU nothing.

**Fix.** Time spent emulating hardware that runs in parallel on a Wii (GX
decoding and GL work, AX mixing) is hidden from the guest's clock
(`wp_parallel_time_hide`, `runtime/core/cpu_host.c`). The time base pauses
while that work runs, and the idle hook gives the time back the next time the
guest waits, so alarms and retraces still land on real time. At most 50 ms
is owed. `WONDERPARK_NO_HIDE=1` turns this off, for comparisons.

Same route (title, cave, Pad, close-up dialogue, Timburr battle), before and
after:

| Scene | Before | After |
| --- | --- | --- |
| PokePark Pad | 16.4 fps (15 most seconds) | 30.0 fps |
| Dialogue close-up | 26.9 fps (4 s under 24) | 30.0 fps |
| Timburr battle | 20.7 fps (30 s under 24) | 29.9 fps (none under 24) |

Over the whole run: mean 32.4 fps (the boot screens run at 60), 92% of
seconds at 29+. What's left under 24 is loading (half-second frames while
an area streams in), not pacing.

Also in this pass:
- GX write-gather pipe stores and locked-cache accesses
  (`0xE0000000`, ~28,000 a frame) now take the fast path in
  `wp_cpu_fast.h`, instead of the MMIO dispatcher. They were measurable but
  not the cause.
- NAND writes are no longer flushed after every write, only on close.
- An apparent boot slowdown seen while testing came from launching with
  PowerShell `Start-Process` redirection (blocking stderr writes), not the
  game; launched normally, boot runs at 60 then 30 fps.

Known, separate: the audio queue drops a few blocks (too much audio queued,
up to ~7 per second) around loading stalls and in battles, with an
occasional underrun. That happens the same with the fix turned off.

Debug switches added: `WONDERPARK_LOG=` `steps` (logic steps per tick),
`waits` (where threads block: OSReceiveMessage, OSSleepThread,
VIWaitForRetrace, GXDrawDone, VISetNextFrameBuffer), `idle` (idle sleeps
over 1 ms), `efbcopy` (every EFB copy), `gxirq` (PE interrupts per second),
`mmiostat` (slow-path memory accesses by region), `nandio` (NAND reads and
writes). `mqwatch` lines now carry timestamps.
