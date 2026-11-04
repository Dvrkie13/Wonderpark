# Recompile status (Phase 2, step 2)

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
