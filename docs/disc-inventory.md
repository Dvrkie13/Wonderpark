# Disc inventory: PokéPark 2 (US, S2LE01)

Findings from Phase 1. Facts about the disc layout and the executable only;
no game data is stored in this repo.

## Disc

| | |
| --- | --- |
| Game ID / title | `S2LE01`, internal title "PokePark Wii2", disc 1, revision 0 |
| Data partition | 875 MiB, 1,022 files |
| Code modules | **None.** No `.rel`/`.rso`/`.sel` files: all code is in `main.dol` |

NKit-processed images (hashes stripped, partition stored decrypted) can't be
read by dtk 1.8.4 (it fails reading the apploader). `tools/extract_disc.py`
falls back to a built-in reader for those, and uses dtk for normal ISOs.

## main.dol

4,915,616 bytes. Entry `0x80004050`. BSS `0x804B0400`, size `0xFFCD0`.

| Section | Address | Size |
| --- | --- | --- |
| text0 | 0x80004000 | 0x2720 |
| text1 | 0x80009B20 | 0x408620 (4.03 MiB of code) |
| data0 | 0x80006720 | 0x14E0 |
| data1 | 0x80007C00 | 0x1F20 |
| data2 | 0x80412140 | 0x120 |
| data3 | 0x80412260 | 0x20 |
| data4 | 0x80412280 | 0x20480 |
| data5 | 0x80432700 | 0x79400 |
| data6 | 0x804ABB00 | 0x4900 |
| data7 | 0x804B19A0 | 0x3CA0 |

~9,600 functions found as `bl` targets (rough count).

### Libraries linked in (from version strings)

| Library | Version |
| --- | --- |
| RVL SDK (OS, GX, DVD, AI, AX, DSP, PAD, WPAD, KPAD, HBM, NAND, SC, EXI, SI, VI, FA) | release build Aug 23 2010, `0x4302_145` |
| NW4R G3D, EF, LYT, UT | (no NW4R SND: audio is CRI) |
| CRI Atom/WII 1.17.02c, AtomEx/WII 1.17.01c, Atom Sound 1.25.00, AtomEx Sequencer 1.00.03 | built Sep 21 2011 |
| CRI codec plugins: ADX, "RVL DSP ADPCM", HCA-MX mixer; ACB/ACF table support | |
| RVL_MWM MOBICLIP_MO (Mobiclip video) | |
| RVL_MWM TMC_JPEG090323 | |
| Creatures engine, namespace `nsNile` (RTTI strings present) | |

PokéPark 1 (`R8AE01`) for comparison: RVL SDK Feb 27 2009 (same compiler
`0x4302_145`), NW4R SND audio, THP video, and **no** `nsNile` strings. So the
SDK is a different revision and engine overlap is unconfirmed. Signature
matching should start with the SDK and expect partial matches only (e.g.
`AXAcquireVoice` differs between the two builds).

## Top-level folders

| Folder | Files | Size | Notes |
| --- | --- | --- | --- |
| Archive | 29 | 7.9 MB | |
| Attraction | 12 | 17 MB | |
| Common | 1 | 0.3 MB | |
| Field | 64 | 102 MB | map `.dac` pairs |
| Gadget | 37 | 0.5 MB | |
| Gimmick | 343 | 21 MB | |
| Hbm | 3 | 1.4 MB | Home Button Menu |
| Item | 7 | 0.2 MB | |
| Message | 36 | 1.2 MB | |
| Movie | 20 | 304 MB | cutscenes, `.mo` |
| Player | 4 | 0.1 MB | |
| Pokemon | 90 | 73 MB | |
| Scene | 1 | 0.5 MB | |
| Shadow | 1 | 0.1 MB | |
| Shot | 5 | 0.1 MB | |
| Sound | 365 | 345 MB | 310 `.acb`, 53 `.awb`, 1 `.acf`, 1 `.rlb` |
| Weed | 3 | 0.1 MB | |
| opening.bnr | | | disc banner |

## Cutscene video: Mobiclip

`Movie/*.mo` files start with the `MOC5` magic: **Mobiclip** (Actimagine's
codec, Nintendo's Wii "MO" format), not THP and not CRI USM/Sofdec. Video
stream `V2`, 640×448; there is a separate audio track (tag `pc`, probably PCM).

The Mobiclip decoder (`RVL_MWM - MOBICLIP_MO`, `MOCreateInstance`,
`MOClose`, ...) is **inside main.dol**, so it is recompiled with the game and
decodes in software. No external decoder is needed. The reference runtime's
THP-specific HLE (`hle_thp.c`) doesn't apply; what carries over is its
audio-streaming and frame-timing approach. Native-rate video at 60 FPS will
need the movie player's clock decoupled from the game's frame rate.

## Audio: CRI goes through AX, not the DSP

Evidence from main.dol:

1. **Strings.** CRI's own messages refer to AX: "Can not aquire AxVoice",
   "AxVoice has droped", `AXFXReverbHiInit`/`AXFXChorusInit`/`AXFXDelayInit`
   (and their `Exp`/`Dpl2` variants) "function returned FALSE", "Invalid AxFx ID".
   Its ADPCM path is a "RVL DSP ADPCM Codec Plugin", i.e. AX voices
   in DSP-ADPCM format.
2. **Code.** CRI code occupies roughly `0x80388DA8–0x803D6784`. None of its
   functions read or write hardware registers. Every function that touches
   the DSP (`0xCC005000+`) or AI DMA registers sits in one block,
   `0x8028DD40–0x802C44F0`, which is the SDK's AI/DSP/AX code (it calls
   `__AIClockInit`, references `<< RVL_SDK - DSP`, and contains
   `__AXSetPBDefault`/`__AXGetCurrentProfile`, matched against PokéPark 1).
3. **Calls.** CRI functions call about 26 different functions in the SDK's
   AX/AXFX range (`0x802B4xxx–0x802C3xxx`).

**Conclusion:** CRI Atom is a client of Nintendo's AX library. It creates AX
voices (DSP-ADPCM and PCM streams; HCA-MX mixes in software and plays through
AX voices) and AXFX effects. That is the same layer the reference runtime
already emulates in `recomp/patches/hle_audio.c`: it stubs the DSP boot,
answers DSP mail at once, and mixes AX's voice parameter blocks (AXPB) itself
on a timer standing in for the AI interrupt. For Wonderpark that code needs:

- PokéPark 2 addresses for the hooked AX functions and AX globals (different
  SDK build);
- checking that every voice format CRI uses (DSP-ADPCM, PCM16, PCM8) and AX
  features (SRC types, ramps, aux buses A/B/C, possibly Dolby Pro Logic II)
  are covered by its mixer.

No DSP mailbox/microcode emulation (NWiiRecomp-style) should be needed.

Not yet verified: how the Mobiclip player outputs movie audio (likely an AX
streaming voice or CRI's streamer; to be checked in Phase 2 step 4).
