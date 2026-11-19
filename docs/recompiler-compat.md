# Recompiler choice: DolRecomp vs the reference recompiler

RECOMPILER.md steps 2 and 3. Checked against DolRecomp `02e27ac` (2026-10-10,
built locally with MSVC / VS Build Tools 2026, all 20 tests pass) and
PokePark-Wiicompiled `62a9759` (2026-11-03).

## What the reference project uses

**Not DolRecomp.** `tools/recomp/recomp.py` plus `translate.py` (~1,000 lines of
Python) are its own recompiler. It:

- takes function boundaries from `config/R8AE01/symbols.txt` (from the
  PokéPark 1 decomp, made with dtk). **PokéPark 2 has no symbols file**, so the
  reference recompiler can't be pointed at our DOL as it stands;
- emits one C function per real guest function, `void f_<ADDR>(ppc_t* c)`;
  `bl` is a plain C call and `blr` is a C `return`;
- for every address in `recomp/patches/hooks.txt` (79 hooks), renames the
  recompiled body to `f_<ADDR>_orig`, so the hand-written `f_<ADDR>` in
  `recomp/patches/*.c` is what every caller reaches, and it can still call the
  original;
- does indirect calls through `ppc_icall` and a sorted `ppc_func_table`.

## How DolRecomp's C output differs

| | Reference (`ppc.h`) | DolRecomp C backend (`cpu/cpu.h`) | Impact |
| --- | --- | --- | --- |
| Unit of code | one C function per guest function | fixed **4,096-instruction chunks** tiling `.text` (`func_<chunk start>`), each a `switch (ctx->pc)` | Function names no longer mean guest functions |
| Calls and returns | `bl` = C call, `blr` = C return | Mostly `ctx->pc = target; return;` to a host loop ("chassis"). Direct cross-chunk calls nest at most 24 deep, then fall back to the loop. Backward branches yield on a cycle `downcount` | The runtime needs a dispatcher loop, plus a "run guest function until it returns" helper (set `lr` to a sentinel, loop until `pc` equals it) for every place the HLE code calls guest code |
| State struct | `ppc_t`: `r[]`, `f[]`, `ps1[]`, `cr/lr/ctr`, XER split into `xer_ca/xer_so/xer_low`, `spr[1024]` | `CPUState`: `gpr[]`, `fpr[]`, `ps1[]`, `pc`, packed `xer`, named SPRs, `downcount`, exception fields, host callbacks | Mechanical rewrite of every patch (`c->r[3]` → `ctx->gpr[3]`, …); XER handling differs |
| Memory | inline `LD32/ST32` on `g_mem` with a `0x3FFFFFFF` mask; write-gather pipe caught inline | calls to `mem_read32(ctx, ea)` etc., implemented in DolRecomp's `cpu.c` with external-read/write callbacks | We either link DolRecomp's `cpu.c` (GPL, see below) or provide our own `mem_*` that implements the reference's fast path and WGPIPE check |
| Hooks | compile-time rename, so every call reaches the patch | `dolrecomp_dispatch_replacement(ctx, addr)`, consulted **only when the host loop dispatches**. Local `bl` inside a chunk is a `goto`, and direct cross-chunk calls go straight in, so **neither path checks replacements**. (Its `--config` patches are for the LLVM/ModernGekko runtime only) | **Biggest gap.** The reference's 79 HLE hooks would be skipped on most calls. Fixes: (a) before recompiling, overwrite the first instruction of each hooked function with a trap or `sc` that DolRecomp turns into a host call; (b) run with replacement checks at chunk entry plus `DOLRECOMP_C_LOCAL_RETURNS=0` (slower); (c) add a "hook these addresses" option to DolRecomp upstream |
| Function discovery | needs a symbols file | none needed: it tiles all of `.text` | DolRecomp's main advantage for PokéPark 2 |
| Floating point / paired singles | inline helpers in `ppc.h` | helpers in `cpu.c`, `--semantics exact` by default | Fine either way |
| Threads / interrupts | guest threads on host fibers (`hle_thread.c`), timer-driven IRQs | chunks yield via `downcount`; the host loop can take exceptions | The fiber model still works (each fiber runs its own loop with its own `CPUState`) |

**Summary:** switching to DolRecomp means a new execution core for the reference
runtime: a dispatcher loop, a call-into-guest helper, a hook mechanism that
actually catches every call, and a port of the ~20 `hle_*.c` patch files to
`CPUState`. Graphics, audio mixing, input, settings and the host layer carry
over unchanged. The real `f_ADDR` names in the reference's patches become
plain guest addresses, which we need to re-find for PokéPark 2 anyway.

Performance risk: chunk-switch dispatch is slower than direct calls. Movie
decoding (Mobiclip runs on the CPU) and audio are the hot paths to watch.

## License impact (GPL-3.0-or-later; no exception for generated output)

What ends up in `Wonderpark.exe` if we use the C backend:

1. **`cpu/cpu.h`** (GPL): included by every generated file. It can be swapped
   via `DOLRECOMP_CPU_HEADER`, but a replacement must expose the same
   `CPUState` layout and function names.
2. **`cpu/cpu.c`** (GPL, ~1,700 lines): `mem_read*/mem_write*`, FP and
   paired-single helpers, `ppc_mfspr/mtspr`, exceptions. The generated code
   calls these, so it must be linked unless we write our own.
3. **The generated `.c` files**: besides the translated game code, every file
   carries helper functions copied verbatim from `emitter.c`'s templates
   (`dolrecomp_f32_from_bits`, call-depth guards, dispatch tables). Program
   output is normally not covered by the tool's license, but output that
   reproduces parts of the program's own source is. DolRecomp grants no
   exception for this, so treat the generated code as GPL-3.0 too.

**Decided 2026-11-19: Wonderpark is GPL-3.0-or-later** (the owner's choice at
the GitHub audit), so the code compiled into the exe and the repo's license
match. The analysis below was written while it was still CC0.

**Can Wonderpark stay CC0? Yes, with conditions.**

- GPL obligations start when a work is *distributed*. Wonderpark never
  distributes `Wonderpark.exe` or the generated code: they are built on the
  player's PC from the player's disc. What we distribute is our own source and
  the builder.
- Our own runtime, tools and launcher can stay CC0 (CC0 code can be combined
  into a GPL work). DolRecomp stays outside the repo (a submodule or a pinned
  download), keeping its own GPL license, never copied in under a CC0 label.
- If a release zip includes `dolrecomp.exe` or any prebuilt object containing
  `cpu.c`, that part is GPL-3.0: ship its license and point to its exact
  source commit. Simply bundling it alongside our CC0 builder is fine.
- If we ever ship a prebuilt runtime binary that links `cpu.c`, that binary
  as a whole is GPL-3.0 (its source is already public, so in practice that
  means including the license text and a source offer).
- The alternative is to write our own `cpu.h`/`cpu.c` equivalent from scratch
  (no copied text) and keep everything compiled into the exe CC0, except the
  emitted helper templates, which we can't avoid with the C backend.

**Recommendation:** keep Wonderpark CC0. Pin DolRecomp as a git submodule under
`tools/dolrecomp/` (GPL, with its own license). State in the README that the
built `Wonderpark.exe` contains GPL-3.0 code from DolRecomp, and don't
distribute builds ourselves.

## NWiiRecomp

Cloned to `refs\NWiiRecomp` (`595f176`) for reference. Its license is
custom ("Modified MIT with Commercial Restrictions"), not open source in the
OSI sense, so **no code from it may be copied**; read it for ideas only. Per the
Phase 1 findings, its DSP mailbox emulation shouldn't be needed (CRI uses AX).
