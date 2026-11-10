# runtime/hw

Register-level models of the Wii hardware the game and SDK talk to. Each
file registers its address range with `mmio.c` (`devices.c` starts them).

- `hw_pi.c`: processor interface, interrupt cause and mask.
- `hw_vi.c`: video interface, the 60 Hz retrace interrupt.
- `hw_ai.c`, `hw_dsp.c`: audio interface and DSP mailboxes (boot handshake, AX protocol), the AI DMA timer that plays sound.
- `hw_ax.c`: runs AX's command lists (voice mixing, aux buses, output), the part a Wii's DSP does.
- `hw_gx.c`: the GPU's CPU side: write-gather pipe, command processor, pixel engine tokens.
- `hw_ipc.c`: the IPC registers to IOS (`runtime/ios`).
- `hw_lc.c`: the locked cache and its DMA.
- `hw_misc.c`: Hollywood registers at `0x0D800000`.
