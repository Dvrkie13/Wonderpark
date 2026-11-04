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

- **Speed: about 10 fps in 3D scenes** (60 fps on 2D screens). The CPU
  side is the bottleneck: every memory access and floating-point op goes
  through a DolRecomp helper call, and every instruction is a `switch` label.
  First things to try: an inline fast path for RAM accesses (own CPU header
  via `DOLRECOMP_CPU_HEADER`), inlined FP helpers, then DolRecomp's LLVM
  backend.
- **No sound yet**: AX frames run, but voices aren't mixed (step 4).
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
