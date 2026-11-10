# runtime/core

- `main.c`: start-up. Command line (`--headless`, `--exit-after`), memory, loading main.dol, the Wii's low-memory boot values, then the run loop.
- `dispatch.c`: runs DolRecomp's generated code slice by slice, delivers exceptions and interrupts between slices, and implements hooks (`WP_HOOK`, `wp_call_original`, `wp_call_guest`).
- `cpu_host.c`: the clock (host time to the Wii's 60.75 MHz time base, game speed, hidden GPU/DSP time), special registers, exceptions, logging.
- `hostirq.c`: runs guest interrupt callbacks the way the OS does, and host timers.
- `idle.c`: sleeps while the game's idle loop has nothing to run.
- `mmio.c`: hardware register dispatch by physical address.
- `paths.c`: the data folder (`%APPDATA%\Wonderpark`) and cache folder.
- `wp_cpu_fast.h`, `wp_fast_fp.h`: inline fast paths compiled into the generated code (RAM, locked cache, GX pipe, floating point).
- `debug.c`, `hostprof.c`, `watch.c`: symbols and backtraces, a host profiler, a store watch.
