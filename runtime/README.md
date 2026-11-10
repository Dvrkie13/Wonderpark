# runtime/

PC platform code: everything the recompiled game runs on. Fixes to how the
game itself behaves don't belong here; they go in `config/patches/` (see
`docs/DEVELOPMENT.md`).

| Folder | What it is |
| --- | --- |
| `core/` | Start-up, the dispatcher that runs the generated code, memory, clock, interrupts, hooks, idle sleep, paths, debugging aids |
| `hw/` | Models of the Wii's hardware registers: PI, VI, AI, DSP and the AX mixer, GX command processor, IPC, locked cache |
| `ios/` | IOS (the Wii's I/O processor): disc reads, NAND file system (saves) |
| `hle/` | High-level replacements for SDK functions: OS, Bluetooth, caches, WPAD/KPAD (Wii Remote), debug hooks |
| `gpu/` | The GX decoder and the OpenGL renderer, the window |
| `host/` | Windows input, sound output, settings files, the debug overlay |

Code taken from MildRain479/PokePark-Wiicompiled (CC0) says so in its
first line.
