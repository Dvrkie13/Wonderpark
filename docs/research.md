# Research notes

What has been confirmed about PokéPark 2 (S2LE01) while building
Wonderpark: read in the game's code or measured, with dates. Only
facts and addresses are here, never the game's code or files. Where
each kind of fix goes, and every setting, is in
[DEVELOPMENT.md](DEVELOPMENT.md).

## Audio and movies

### Confirmed (Phase 1, from the US disc and main.dol)

- **Video format:** Mobiclip (`Movie/*.mo`, `MOC5` magic, 640×448, separate PCM-style audio track), 20 files, 304 MB. Not Sofdec/USM and not THP. The Mobiclip SDK (`RVL_MWM - MOBICLIP_MO`) is linked into main.dol and decodes in software.
- **CRI version:** Atom/WII 1.17.02c, AtomEx/WII 1.17.01c, Atom Sound 1.25.00, AtomEx Sequencer 1.00.03 (built Sep 21 2011); ADX, DSP-ADPCM and HCA-MX codec plugins.
- **AX usage:** CRI code (about `0x80388DA8–0x803D6784`) calls 97 functions in the SDK's AX/AXFX/MIX range, among them `AXAcquireVoice`, the aux-bus callbacks, AXFX reverb, `MIXInitChannel`/`MIXUpdateSettings`. Only 17 AX functions have names so far (signature match against PokéPark 1, a different SDK build); `AXInit`, `AXSetVoice*` and `AXRegisterCallback` are present in that range but not yet named.
- **How CRI uses AX (measured at run time):** 2–6 AX voices run at once (7 distinct over boot, title and field), nearly all PCM16 plus a few DSP-ADPCM. CRI decodes and mixes most audio on the CPU (ADX/HCA, HCA-MX mixer) and outputs through a handful of PCM16 AX voices. No custom DSP microcode.
- **Direct DSP access:** none outside the SDK. Every function touching the DSP (`0xCC005000+`) or AI registers is in the SDK's AI/DSP/AX block (`0x8028DD40–0x802C44F0`).
- **Movie player** (`0x80185800–0x80188C00`, built from Mobiclip's sample code): video frames are converted YUV→RGB on the GPU with a multi-stage TEV setup and drawn as an orthographic quad (no EFB copies to textures). Audio: the player takes its own two AX voices (`AXAcquireVoice` ×2, `MIXInitChannel`) and streams the movie's PCM packets into them, not through CRI. A decode thread paces itself with `OSSleepTicks`; it doesn't call `OSGetTime`, `VIGetRetraceCount` or `VIWaitForRetrace` itself. **Unconfirmed:** whether the frame shown follows the game's frame count or a clock. The community report that only movies speed up at 60 FPS suggests frame count; check in the frame-rate work.
- **Sound banks:** see `docs/disc-inventory.md`. One English voice set only (no per-language banks on the US disc).
- **CRI categories (2026-11-17):** `Sound/nile_sound.acf` has nine, id = index: PK_VOICE 0, BGM 1, PC_VOICE 2, PK_ACT 3, PC_ACT 4, ENV 5, ALWAYS_SE 6, AT_SE 7, JGL 8 (category groups: 0 = {0, 2, 3, 4}, 1 = BGM, 2 = {5, 6, 7}, 3 = JGL). Global AISAC controls: AT_BATTLE_MUTE_VOL, CHARACHANGE_VOL, EVENT_VOL, FIELD_MUTE_VOL, PAUSE_VOL, REVERB, TRIAL_VOL. `criAtomExCategory_SetVolumeById(id, f1 volume)` is at `0x80396244` (records of 76 bytes, volume at +16); the game calls it from its sound-script commands (`0x800697F0`). The F1 volume sliders use it (`runtime/hle/hle_volume.c`). Checked by recording the game's output in Wish Park: with Music at 0 the park's level fell from about 2,600 to 900 RMS.
- **CRI players and pitch (2026-11-17):** a category's parameters other than volume are ignored. A player's pitch is parameter 3 (cents) of its parameter set (word +92); `criAtomExPlayer_SetPitch` is `0x8039A7C0`, `_Start` `0x8039955C` (the bank handle at player +32), `_ResetParameters` `0x8039A6D4`, the parameter setter `0x803A1480`. The frequency ratio is `pow(2, cents / 1200)` (`0x803A0018`), passed to `criAtomPlayer_SetFrequencyRatio` (`0x8038678C`), which clamps it to the player's max sampling rate. The game's voice pools (`0x803948B8`, from `0x80066338`/`0x8006635C`) were made for 32,768 Hz; voices are 32 kHz ADX streams, sound effects mostly 22,050 Hz. Banks with a stream archive load through `0x80393C60` (r5 the archive handle, its word +8 the path, e.g. `Sound/S_PIKA_VOICE_streamfiles.awb`); banks without one through `0x80393A64`. Used by `runtime/hle/hle_voice_pitch.c`.

## Frame rate

### What the 60 FPS cheat tells us

The community Gecko code for the US version (NTSC-U):

```
204B9C0C 001EEE10
044B9C0C 000F7708
E0000000 80008000
```

It checks that the value at 0x804B9C0C is 0x1EEE10 and, if so, replaces it with 0xF7708 (exactly half).

The Wii's timebase ticks 60,750,000 times a second, so:

- 0x1EEE10 = 2,027,024 ticks = one frame at 29.97 FPS
- 0xF7708 = 1,013,512 ticks = one frame at 59.94 FPS

With the cheat on, people report that gameplay runs correctly and only the pre-rendered cutscenes speed up. That suggests the game scales movement by this frame-length value rather than assuming a fixed step per frame. It's a guess from the cheat's behavior and must be confirmed in the code.

#### Confirmed (2026-11-12, Level 1 step 1)

Found with a load/store watch on `0x804B9C0C` (`build_game.py --watch`,
`WONDERPARK_WATCH=004B9C0C-004B9C10`) and the disassembly:

- `0x804B9C0C` is not a constant the game reads every frame. It is the low
  word of the **requested period** of the second of two frame timers (an
  array of two 112-byte objects; this one's +0x40/+0x44 is the requested
  period as a 64-bit time-base count, +0x38/+0x3C the period in use, +0x30
  the alarm callback, +0x48 a message queue, +0x6C "running").
  - `0x8000F030` (via `0x8000F170`) starts a timer: sets both periods and
    an OS periodic alarm (`0x8028D160`) whose callback posts a message.
  - `0x8000F160` -> `0x8000EF70` is **the main loop's wait**: it receives
    that message (`0x80293730`, blocking). The main loop (`0x8002D224`) is
    just: wait for timer 1, run the frame function `0x8002CCE0`. The game
    does not wait on VIWaitForRetrace for its frame.
  - `0x8000EFA0` (both timers, via `0x8000F190`, each frame) is the only
    reader: when the requested period differs from the one in use, it
    cancels the alarm and sets it again with the new period.
  - `0x8000F200` requests a period. Its callers: `0x80205494`, from a
    per-scene table (`0x80469260 + 104 * scene`, words +16/+20), so each
    scene picks 30 or 60 Hz; `0x80053890` (back to the default); and
    `0x80205C10` for timer 0, from a float.
  - At start the main loop starts timer 1 at `0xF7708` (60 Hz, from the
    globals at r13-14032/-14028): the boot screens run at 60. Scenes then
    ask for `0x1EEE10` (30 Hz). The Gecko code turns that request back
    into 60 Hz each time it appears.
- **Gameplay does not run per drawn frame.** The frame function asks
  `0x800333A0` how many entries are queued at +9980 of the input manager
  (controller samples, added at 60 Hz) and runs one logic step for each
  (`0x80033400` takes them one at a time): at 30 fps, two steps a frame
  (`WONDERPARK_LOG=steps`). With a 60 Hz timer it would be one step a frame,
  the same speed, drawn twice as often. That is why the cheat "just works"
  for gameplay.
- Still to find out by testing at 60: anything advanced once per *drawn*
  frame rather than per step (the cheat's reports name pre-rendered
  cutscenes; also menus, HUD and effect animations, the movie player),
  and the "skip drawing after more than two steps" rule in `0x8002CCE0`
  (docs/recomp-status.md, "the 15 fps drops"), which would read one long
  frame differently at 60.

#### Done (2026-11-12): Frame rate 60

`runtime/hle/hle_frame.c` hooks `0x8000F190` (applies the timers' periods,
each frame): with Frame rate 60, timer 1's request of `0x1EEE10` becomes
`0xF7708` just before the game applies it, and back at 30, so a change
from F1 or the Options page takes effect at once. Other periods are left
alone. Tested:
- Gameplay speed: one logic step a frame at 60 (6,548 ticks with one,
  125 with two after a slow frame, 120 with none), 59.6 steps a second
  over a 114 s park run: the same game speed as at 30.
- Movies play one picture a drawn frame: the title movie was read 1.8x as
  fast at 60. So while a movie file is being read (and 5 s after) the
  timer stays at 30; with that, it was read exactly as at 30 (14.375 MB
  in 75 s both ways) and the title runs at 30.
- Menu animation (the sparkles along the main menu's top): 3.57 s a
  repeat at 30, ~3.56 s at 60 (203 frames at ~57 fps): the menus also
  move by logic step.
- Speed, optimised build, park and beach walks (~72 s in the area each):

  | | fps | s under 57 | game thread busy | CPU ms | GPU ms |
  | --- | --- | --- | --- | --- | --- |
  | Wish Park 2x | 60.0 | 1 | 70% | 8.1 | 6.2 |
  | Wish Park 3x | 60.0 | 0 | 69% | 8.1 | 8.6 |
  | Seasong Beach 2x | 60.0 | 4 | 82% | 10.6 | 6.6 |
  | Seasong Beach 3x | 60.0 | 3 | 82% | 10.7 | 8.7 |

At 2560x1440 (2026-11-17): 3x and 4x both hold 60 in the park and at the
beach, GPU ~12 ms a frame at 4x (table below).

A play-test at 60 (2026-11-12): chases, the Cake Contraption and
text speed fine; a couple of sound glitches just before a battle's win
screen; a story movie "slow sometimes"; many sound glitches and slowdown
at Seasong Beach. Followed up:
- Seasong Beach at 60 on the test PC alone holds 60 (191 s, 4 s under 57, in a
  maximised 1440p window at 3x) with the game thread ~83% busy, and its
  sound has as many clicks at 60 as at 30 (23 in 65 s, the game's own
  effects). With other programs busy on every CPU thread it fell to ~46
  fps and the game behind its 60 Hz steps. Raising the game thread's
  priority didn't help measurably (not kept). So Frame rate 60 now falls
  back to 30 by itself when over a third of ~2 s of frames take over
  20 ms (loading frames over 60 ms don't count), for 15 s, then tries 60
  again (`hle_frame.c`; `WONDERPARK_LOG=framerate`). Idle, it never
  triggered in the park or at the beach.
- The one long frame with dropped sound at the beach (267 ms) was a load:
  the game thread waiting in the GL driver for a copy read-back while the
  GPU took a load's texture uploads; sound is mixed on that thread. A
  battle's win screen is a load too; likely the same, not confirmed.
- The story movie: not reached by script.

Not tested yet (no way to reach them by script; the owner's saves are
there): battles, chases, attraction timers and rhythm games, a story
movie, multiplayer attractions, text speed in a real conversation.
The "skip a draw after more than two steps" rule now means a frame over
~33 ms costs one draw at 60.

## Settings

### Confirmed (2026-11-11)

- The game's own Options page (main menu > save file > Options) has three
  items: **Text Speed** (Fast / Normal; Normal by default), **Rumble Setting**,
  and **Delete a Saved Game**. There is no volume option in the game, so the
  F1 tab's volume sliders are ours alone.
- Text Speed lives in the loaded save in memory: the pointer at
  `0x804BA694` (also `0x804BA698`, `0x804BA6A0`, `0x804BA6B0`) points to a
  copy of the 5,344-byte `GD0` save (at `0x809CBFE0` in every run so far);
  byte `0x14A5` is Text Speed, 0 Normal, 1 Fast. It's written to the save
  file with the game's next save. Found by RAM snapshots before and after
  changing it in the menu (`WONDERPARK_RAMDUMP`), checked in three runs.
  The F1 tab reads and writes it there (`runtime/hle/hle_options.c`).
- Not confirmed yet: whether dialogue uses a change made from F1 at once
  (a byte at `0x804CEC8B` also went 0 -> 1 -> 0 with the setting, maybe the
  message window's own copy), and where Rumble Setting is kept (likely a
  neighbouring byte).
- The start menu: its manager object is at the pointer at `0x804BCDA8`.
  Word +0x34, high half: the page (2 the save file's menu, 3 Options).
  Word +0x3C: the Options page object (its vtable is `0x80457D50`), whose
  word +0x114 is the cursor (0 Text Speed, 1 Rumble Setting, 2 Delete a
  Saved Game). Manager word +0x15C is 5 while one of Options' dialogs is
  open, 0 with the list. Rumble Setting's dialog offers "Rumble ON" and
  "Rumble OFF". (RAM snapshots, 2026-11-11.)
- The Options list rows (`MnStStartSystem.brlyt`, layout units 720x480,
  centred, y up): List00-02 at (8, 50), (4, -15), (0, -80), 340x55, each an
  `ListBg03.tpl` background (the orange selected look; the green one is a
  material tint), an icon (`ListIcon05/06/08.tpl`, 44x44 at -128, 1) and a
  pre-rendered label (`ListText03/04/06.tpl`, 253x28 at 26, -1; white on
  black, tinted by the material). On screen the layout is 877 units across
  in the game's 16:9 mode (640 in 4:3; about 659 when the 4:3 picture was
  stretched to 16:9, before 2026-11-12) and 480 down. The game's font is
  `Archive/PrepareMessageRes.dac` `Font/Standerd_US_EU.brfnt` (I4 sheets).
- Widescreen (2026-11-12): the game has its own 16:9 mode. It calls
  `SCGetAspectRatio` (`0x802C9A70`) once at start (from `0x800300FC`) and
  keeps "16:9" in byte +93 of a system object; SYSCONF said 4:3, so until
  now it ran in 4:3 and the picture was stretched. Answered from the
  Widescreen setting, it uses a 16:9 world camera (y scale 2.006, x 1.128
  in Wish Park) and lays out menus and HUD for 16:9 itself, not stretched.
  All its 2D (menus, HUD, dialogue boxes) is drawn with one layout camera:
  perspective, 45 degrees (y scale 2.41421), aspect 1.829 (x 1.31984), plus
  full-screen orthographic quads 833 units across; effects (bloom) use an
  orthographic projection of the EFB's 640 pixels. That is how the runtime
  tells the layout from the world for wider windows and "HUD at screen
  edges" (`runtime/gpu/gx_fifo.c`, "Widescreen").
- Berries (2026-11-17): the count is the u32 at +0x1474 of the loaded save
  (the GD0 copy above), capped at 99,999; the PokéPark Pad showed "Berries
  1" for build/nand_wish_park's save, which holds 1 there (and 21 at
  +0x11EC, likely all ever collected). The game changes it in about ten
  places (0x8014E9DC, 0x801D71F8, 0x801D78B0, 0x802174F0, 0x802232E8,
  0x802233F8, 0x80223508, 0x802238C8, 0x802239D0, 0x80223AD8), each as
  clamp(count + change, 0, 99999), then adds the change to a collected or
  spent tally (stat 23 or 24, through a virtual call).
- Battle and chase tuning is in `Archive/PrepareResidentDat.dac`:
  `Event/EventChallenge/EventBattleData.rlb` (24,979 bytes) and
  `EventChaseData.rlb` (7,959): relocatable structs (header: size,
  fix-up table offset, fix-up count, 3 symbols `paramIndividualSet`,
  `vaParam`, `vaParamInfo`). Their fields are only known from the code that
  reads them: not traced yet (2026-11-17).
- Damage, partly traced (2026-11-17): `PlayerDamageSpecializer`'s vtable
  is at `0x8044C188` (own methods `0x800E1F20`, `0x800E2A10`, `0x800E2FE0`,
  `0x800E3080`), `PokemonDamageSpecializer`'s at `0x80450318`
  (`0x800FF940`, `0x80100120`, `0x80100BC0`, `0x80100DE0`, `0x800FB7B0`).
  `0x800E3080(this, player, f1)` looks like the player's HP change: it gets
  a status object (player virtual +140), reads a float from it (virtual
  +80, kept in f31), writes f1 to it (virtual +88), then reacts. Not
  confirmed which value is HP, or whether +88 sets or adds. Each
  Pokémon's attack and hit numbers are in its own data (`attackDataSet`,
  `hitDataSet`, `attackTackleParamPack`), so scaling data files would mean
  many files; a hook where HP changes is the better place. Nothing can be
  checked without reaching a battle: scripted input hasn't started one
  (see docs/recomp-status.md, "Coverage check"). `EventBattleData.rlb`
  has `COLOSSEUM_ST_` entries: a menu-driven battle mode, reachable with
  a further-along save, would be scriptable.
- The menus are standard NW4R layouts: `Archive/MenuStartMenu.dac` (LZ11 +
  U8) holds 13 `.brlyt`, 82 `.brlan`, 208 `.tpl`; the Options page is
  `MnStStartSystem.brlyt`; text in `.msbt` (`StartMenuRes.dac`).


## Voice language

### Confirmed (2026-11-18, investigation only)

A Japanese disc (S2LJ01) was extracted with dtk to a temporary folder
(outside the repo; nothing from it is kept in git) and its `files/Sound`
compared with the US disc's:

- Both have the same 365 sound files, same names. 318 are byte-identical
  (Pikachu's voices `S_PIKA_VOICE`/`M_PIKA_AT_VOICE`, all music, the
  per-Pokémon cry banks `M_<name>.acb`).
- 47 differ: the starters' voices (`S_MIJU_VOICE`, `S_POKA_VOICE`,
  `S_TUTA_VOICE` and their `_streamfiles.awb`; `M_MIJU/POKA/TUTA_AT_VOICE`),
  the other Pokémon's talking voices (`S_POKE_VOICE` + `.awb`, ~20 MB), the
  area banks (`S_Fd*`, `S_DEMO_COMMON`, `S_RICH_COMMON`), `M_AT_01` and
  `SoundCommonData.rlb` (same size, different bytes).
- Cue tables (`CueNameTable`, `CueTable`): in **every differing bank but
  one, the cue names, indexes and IDs are identical**; only the waveforms
  differ. So Method 1 (serving the Japanese file under the same name, the
  .acb with its .awb) fits all of them.
- The exception is `S_POKE_VOICE.acb`: US 1,990 cues, Japan 1,096, all of
  Japan's names also in the US one (the US added cues such as
  `SND_ID_004_EX1`). Swapping it whole would leave the US-only cues
  missing, so it needs Method 2 (play the Japanese cue by name, the US one
  where Japan has none), or stays English.

**Built (2026-11-18, the owner's go-ahead): Method 1, with `S_POKE_VOICE`
left English.** `tools/extract_voices.py` (or dropping the Japanese disc on
the launcher) copies the 28 matching banks into `game/voices/ja/` (72 MB,
on D: with the extracted disc, not `%APPDATA%`: C: is nearly full), and
`runtime/ios/dvd.c` serves them in place of the US files when Voice
language is Japanese, from the next start (the FST is read at boot). Seen
working: the game reads the Japanese banks in Cove Town. Not checked by
ear yet. `SoundCommonData.rlb` is left as the US one.

Not yet known: whether `SoundCommonData.rlb` must be swapped with the
banks, and what the area banks' differing waveforms are (voices in story
scenes, or other sounds). Next step (the owner's call): build Method 1 for
the starters' banks and the area banks, with `S_POKE_VOICE` left English or
done by Method 2.

## Performance

### Confirmed (first measurement, 2026-11-10)

Build: clang-cl Release (/O2), no LTO, no AVX2, single game thread (GX
decode and GL calls run on it). Ryzen 7 5800X; other programs were using
about half the CPU, so treat the stalls below with care. Run: headless,
Wish Park save, load, then walk for about 70 s (`RECOMP_FPS=1` prints a
`stats:` line each second; `WONDERPARK_LOG=hostprof` for the profile).

In Wish Park (72 seconds sampled, medians):

- 29.9 fps (30 is the game's rate); 17 of 72 seconds under 29 fps;
  slowest frame typically 40 ms, worst 1.5 s.
- Per frame: game code 19.5 ms, GX decode + GL 8.9 ms, waiting 5.5 ms
  (84% busy; the 33.3 ms budget has ~5 ms left).
- 33,700 GX draw commands and 208,000 vertices per frame (about 6
  vertices each), merged into ~420 GL draw calls.
- 20 EFB copies per frame, all on the GPU; 0 CPU readbacks; 0 texture
  uploads per second once in the park; shader compiles only at start
  (one ubershader). Shader cache, GPU-side EFB copies and the texture
  cache are already effectively in place.
- Host profile: ~50% recompiled game code, ~19% GX decode (`gx_fifo.c`:
  `read_floats`, `fifo_append`, `draw`), ~5% GL driver, ~16% waiting.

Stalls (found and fixed the same day, `WONDERPARK_LOG=stalls`): every
long frame was the game thread waiting on file access, on the test PC's
spinning hard drive: writing log lines to stderr (0.4-1.1 s;
the "present" timing includes the per-second stats print, which is why
presents looked slow), and IOS disc/NAND requests (creating a NAND
directory took 2.4 s once). Fixed: stderr goes through a pipe emptied by
its own thread (`runtime/core/logpipe.c`), and IOS runs on its own thread
(`runtime/hw/hw_ipc.c`), so the game goes on while a request waits, as on
a Wii. After: no frame over 200 ms in a 120 s run except the game's own
~680 ms of work when a save loads; seconds under 29 fps in the park went
from 17 of 72 to 6-8 of ~78 (those are CPU-bound: 84% busy).
Disc reads during play: a few per second in bursts, 0.2-0.6 ms each when
cached, 35 ms when not.

Render thread (2026-11-10, `runtime/gpu/gx_thread.c`, off by default:
`WONDERPARK_RENDER_THREAD=1`). Built and working: boots, title, save
load and the park draw correctly. It needed two fixes that also apply
without it:
- EFB copies the CPU reads are now written to memory before the next
  token or draw-done interrupt (`gxb_sync_point`), as on a Wii. Written
  at the next present, they landed in memory the game had already freed
  at the switch into the park, and corrupted its heap.
- PE token and draw-done interrupts are raised one at a time, in order
  (`hw_gx.c`). The game's token handler acknowledges by writing back the
  register as read, which also cleared a draw-done raised beside it, and
  the game waited in `GXDrawDone` for ever (about 1 start in 3).
- The game thread publishes what it wrote to the ring in 16 KB batches
  (one shared store per write made the cores trade a cache line millions
  of times a second).
Measured on a quiet machine (same day, Wish Park walk, per frame):
single thread: game 21.9-23.4 ms + GPU 10.7-11.2 ms on the same thread,
27-30 fps; render thread: game thread 26-28 ms, render thread 17.5-18.4
ms, 23-25 fps. Splitting the work makes both halves slower: the game
thread hands ~2 MB of commands a frame to another core, and the render
thread reads the game's vertex arrays, textures and display lists from
another core's cache (a 2 MB ring instead of 16 MB changed nothing). And
the game waits for the GPU itself each frame (a token every frame, draw
done every other), so little of the work overlaps. Left off; not worth
pursuing as is. Faster GX decoding on the game thread is the better path. One run (with the stall
catcher on) stayed on a 3-draw screen from the start; not seen again in
22 boots.

GX decoding (2026-11-10):
- Windows 11 power throttling ("efficiency mode") ran every headless run,
  and the game whenever its window isn't in front, at lower clocks with
  coarse timers: the same park walk ran at 15 fps with it, 30 without.
  `main.c` now opts out (SetProcessInformation, ProcessPowerThrottling).
  It also made earlier measurements on this day noisy, the render
  thread's included; those may be worth repeating.
- The display-list cache (vertices decoded once, kept on the GPU) only
  worked with the Vulkan device, which isn't built: with OpenGL every
  vertex was decoded and sent every frame. 99.8% of the park's vertices
  are in display lists. `gl_backend.c` now has the vertex arena. Lists
  are kept 5 s after their last call (20 s filled 216 MB while walking,
  and growing the arena that far stalled the driver for 300 ms); the
  arena starts at 1M vertices (108 MB) and grows on the GPU.
- Wish Park walk, per frame: GPU work 9.4 -> 3.7 ms; game thread 87% ->
  68% busy; seconds under 29 fps 13 -> 2 of 76. Seasong Beach: GPU 6.7
  -> 3.0 ms, 97% -> 86% busy, seconds under 29 fps 22 -> 3 of 70. The
  game's own code (19-26 ms a frame) is now nearly all of the time.

Compiler options (2026-11-10, same park walk and Seasong Beach, game
thread's work per frame):
- /arch:AVX2: no difference (19.8-20.1 vs 19.8-21.2 ms). Not worth
  leaving out older CPUs for.
- ThinLTO (`--lto`, lld-link): park 18.4 vs 18.7-19.2 ms, beach 25.1 vs
  26.3 ms (3-5% less); beach seconds under 29 fps 14 -> 7. The build
  takes 18.5 min instead of 13. Now on by default (`--no-lto` to
  build without). llvm-mingw has no
  lld-link.exe: a hard link to its ld.lld.exe named lld-link.exe works.
- Not tried yet: profile-guided optimisation (llvm-profdata is in
  llvm-mingw), the likeliest bigger win for the recompiled code's big
  switch-based chunks.

CPU, step 2 (2026-11-11). Game-thread work per frame (not counting GX
decoding), two runs each, ThinLTO build:
- Baseline: park 17.5-17.7 ms, Seasong Beach 23.9-24.1 ms.
- The guest profile is flat: no function above ~1.5% (the "Vec3Normalized"
  entry in hostprof.py's list was ~300 KB of unnamed engine code after it;
  `tools/analysis/hostprof_addrs.py` shows where samples really are). All
  PSMTX/PSVEC routines together ~3%, memcpy not in the top 30. Memory-access
  statements are 21% of samples and floating point 13%, spread everywhere.
- Native PSMTXIdentity/Copy/Concat/MultVec (`runtime/hle/hle_mtx.c`) and
  hooks called directly from the generated code (no table lookup): park
  16.3-16.8 ms, beach 22.5-22.8 ms (about 6% less).
- Found while checking pictures: the display-list cache (step before) drew
  a list's vertices before they were in the arena when a batch was flushed
  partway through a list being decoded (Pikachu's body missing or streaked
  in some frames). Fixed: each draw's vertices go to the arena as decoded.
- PGO (instrumented build, trained headless for 150 s on a Wish Park and
  a Seasong Beach save, then ThinLTO + profile): park 10.5-10.8 ms, beach
  14.0-14.1 ms (about 36% less than before it). Both now under the 16.7 ms
  60 fps needs; game thread 42% (park) and 51% (beach) busy at 30 fps.
  The profile is game-derived (made from the recompiled code), so it stays
  in build/pgo/ and every player has to make their own: an open question
  for the builder (it needs a save in a busy area to train on).
  llvm-mingw's profile runtime is MinGW-only: linked with
  legacy_stdio_definitions.lib and a ___chkstk_ms alias, and main.c
  initialises it and writes the profile at exit itself (WP_PGO_GENERATE).
- Idle-loop skipping: nothing more to gain. The OS idle loop already sleeps
  until the next event (idle.c), and the guest profile shows no spinning
  wait loops in the game's code.

Render thread, re-tested (2026-11-11, step 3). PGO build, power
throttling off, same park walk and Seasong Beach walk, two runs each,
alternating off/on. Per frame:
- Wish Park, off: game 10.5-10.8 + GX/GPU 3.3 ms, both on the game
  thread (41-42% busy). On: game thread 10.7 ms (32% busy), render
  thread 4.9-5.3 ms. Both runs with it on **crashed** in the park after
  10-50 s: the guest scheduler jumped to a thread whose saved context
  held FFFFFFFF ("no recompiled code at FFFFFFFF", lr in SelectThread),
  i.e. guest memory was overwritten. Not seen with it off.
- Seasong Beach, off: game 13.6-13.7 + 2.6 ms (49% busy). On: game
  thread 14.0-14.2 ms (41-42% busy), render thread 2.6-2.7 ms. Seconds
  under 29 fps: 2 and 2 off; 4 and 20 on.
- So the measurements made under power throttling were wrong in
  direction: the game thread now does about 3 ms (park) and 2.3 ms
  (beach) less a frame with it, as the GPU work moves off it, while the
  render thread itself is ~50% slower than the same work done inline
  (another core's cache). At the 30 fps cap this changes nothing visible:
  both modes already hold 30 fps.
- Verdict: not worth finishing now. It would matter only for 60 fps, and
  single-threaded the park needs ~13.8 ms and the beach ~16.3 ms of the
  16.7 ms a 60 fps frame has; the render thread would bring those to
  ~10.7 and ~14 ms. Finishing it means finding the memory corruption
  (likely another write-back the game reads before the render thread
  has done it) and the worse frame pacing. Revisit once 60 fps exists
  and if busy areas miss 16.7 ms single-threaded.

Vulkan vs OpenGL (2026-11-11, step 5; not switched).

What there is: the Vulkan SDK 1.4.363.1 was installed on the test PC
(headers, glslc). The reference project has a finished Vulkan device
(`recomp/gpu/vk_backend.c`, 3,100 lines, and six shaders in
`vk_shaders/`) behind the same `GXDevice` interface as our
`gl_backend.c`; `gx_device.h` already names `g_vk_device`. Porting it
means copying those (CC0), adding the shader build step (glslc -> C
arrays) to CMake/build_game.py, and giving it our changes since
(vertex arena hooks match its own; EFB sync points, copy_read).

GPU cost measured (Wish Park walk, RX 9060 XT, OpenGL, PGO build):

| Internal resolution | fps | frame time | game thread busy |
| --- | --- | --- | --- |
| 1x (811x528) | 30.0 | 33 ms (capped) | 43% |
| 2x, default (1621x1056) | 30.0 | 33 ms (capped) | 41% |
| 3x (2432x1584) | 23.7 | 42 ms | 93% |
| 4x | 14.0 | 71 ms | 95% |
| 6x | 6.6 | 151 ms | 98% |

Frame time grows with pixels (3x is 2.25 times 2x's pixels, 4x 4 times,
6x 9 times), which puts the GPU's own work at the default 2x at about
17-19 ms a frame: hidden at 30 fps, but more than a 60 fps frame has, on a
mid-to-high-end card. That is per-pixel work: one "uber" fragment shader
runs every TEV stage setup through loops and branches on uniforms, for
every pixel. Not yet confirmed which part (TEV shader, EFB copies at the
internal resolution, overdraw); GL timer queries would say.

What Vulkan would give:
- **Specialised shaders** per TEV setup, built on a background thread
  while the general one draws, and kept on disk (vk_pipelines.bin) so a
  second start doesn't build them again: the likeliest big cut in GPU
  time per pixel, without shader-compile stutter. The single biggest
  reason to have it.
- Less CPU per draw (no driver state checks, descriptor indexing, push
  constants, two frames in flight): small here, as GL calls are about
  0.7 ms of the game thread's 3.3 ms of GX work a frame.
- Better frame pacing and presenting (mailbox/FIFO swapchain, no
  SwapBuffers quirks per vendor), and validation layers for debugging.

What it costs: a second renderer to keep in step (every GX fix done
twice, as the arena and the sync points were), Vulkan 1.3 required
(old iGPUs fall back to GL anyway), and the shader build step in the
player's build (the SDK, or glslc bundled with the builder).

Recommendation: keep OpenGL as the working path. Before 60 fps, measure
GPU time with timer queries; if the TEV shader is the cost, either port
the reference's Vulkan device (it already has specialised pipelines and
the disk cache) or add the same to GL (specialised programs compiled on a
shared context, glProgramBinary cache). Vulkan is the better long-term
home for it; GL is the smaller first step.

Shader speed, confirmed (2026-11-12, RX 9060 XT, driver 26.10, OpenGL).
GPU time measured with timer queries (`RECOMP_FPS=1` prints `gputime:`),
median per game frame over the walk, same save and route each run:

| | 2x before | 2x after | 3x before | 3x after | 4x before | 4x after |
| --- | --- | --- | --- | --- | --- | --- |
| Wish Park GPU ms | 19.5 | 6.7 | 37.3 | 9.2 | 63.1 | 11.9 |
| Wish Park fps (s under 29 of ~70) | 30 (4) | 30 (0) | 25.2 (55) | 30 (1) | 15.8 (64) | 30 (0) |
| Seasong Beach GPU ms | 19.6 | 7.0 | 38.4 | 9.4 | 63.9 | 11.9 |
| Seasong Beach fps (s under 29) | 30 (4) | 30 (2) | 22.8 (66) | 30 (3) | 15.6 (67) | 30 (5) |

- The cost was not TEV's arithmetic but how the ubershader did it: local
  arrays indexed with values known only at run time (the 16 color inputs,
  8 alpha inputs, 4 registers, the indirect texels, the texture
  coordinates). GPUs keep such arrays in scratch memory, read and written
  for every pixel. Rewritten as selects and branches on the draw's
  (uniform) setup, with the same results (frame dumps compared: under 1/255
  mean difference, the rest animation timing), it is 3-5x faster. One
  program, so no compile stutter anywhere. Auto may now pick up to 4x.
- Specialised programs per TEV setup (the plan's layers 2 and 3) were
  built and measured: 5 ms at 4x in the park, but on this AMD driver the
  first draw with each new program blocks the thread drawing for 50-300 ms
  (75 ms for a one-stage setup; ~60% of it the shared vertex shader). The
  same whether compiled with KHR_parallel_shader_compile, loaded with
  glProgramBinary (~300 ms), or compiled and drawn once on a second shared
  context (which blocks the main context meanwhile, up to 1.4 s). A park
  has ~90 setups, so that is ~18 s of hitches. Kept, off, behind
  `WONDERPARK_SHADERS=spec` (with a cache of the setups seen, recompiled in
  the background at start) for drivers where it may behave; not needed
  for 30 fps. Vulkan pipelines compiled on threads would avoid the AMD GL
  problem, if 60 fps at 4x ever needs the last few ms.

Shader warm-up at build time, confirmed (2026-11-17, AMD GL): a shader
the driver hasn't seen (forced by changing the source) costs 0.41-0.73 s to
compile and link, plus ~0.4 s at the first frame and ~0.36 s again at the
title (new draw states). The driver caches both, so a second start takes
4 ms and shows no pauses. One draw of a test triangle cached the compile
but not the first-frame pauses; running the game's own boot did. So the
build ends with `Wonderpark.exe --prepare-shaders` (the boot at x10 for 8
s, hidden): measured afterwards, the first start had no pause over 116 ms
(77 ms when cached). There's nothing else to precompile: the ubershader is
the one draw program (specialised programs are off).

60 fps at 2560x1440, confirmed (2026-11-17, same PC, optimised build,
Frame rate 60, a 2560x1440 window, the owner's park and beach saves; median
per second over the time in the area):

| | fps | s under 57 | GPU ms | CPU ms | game thread busy |
| --- | --- | --- | --- | --- | --- |
| Wish Park 3x | 59.9 | 19 of 82 | 9.0 | 8.0 | 68% |
| Wish Park 4x | 60.0 | 6 of 75 | 12.2 | 7.8 | 67% |
| Seasong Beach 3x | 59.9 | 31 of 126 | 9.1 | 10.1 | 79% |
| Seasong Beach 4x | 60.0 | 11 of 116 | 11.8 | 10.1 | 79% |

- 3x and 4x both hold 60. At 4x the GPU takes ~12 ms of a 60 fps frame's
  16.7, with the ubershader alone; the specialised programs aren't needed.
- The seconds under 57 are not the GPU: 3x had more than 4x. Another
  program was busy on D: (up to 47 MB/s, ~150-340% disk time with
  Wonderpark closed) and the game thread is ~80% busy at the beach, so
  dips come from the CPU and the drive. On an idle PC (2026-11-12) the park
  at 3x had none.
- Found on the way: with the drive that busy, fast loading took 93 s
  instead of 19 and single game reads during start-up took 3-11 s (the
  preloader yields only between its 1 MB reads, and on a spinning drive
  each of its seeks delays the game's). Start-up to the save menu took up
  to ~95 s. Worth a look later: pause the preloader entirely while the game
  is loading, or read smaller chunks.

### The camera turning dip (2026-11-18)

Turning the free camera without stop (Cove Town, a test mouse at about 36
degrees a second) dropped the optimised build to 10-20 fps for seconds at
a time. The game's own code only rose from 18 to ~25 ms a frame; our side
("gpu" in the stats: GX decoding and GL calls) went from 9 to 35-66 ms,
while the GPU's own time (timer queries) stayed at 6-9 ms. Two causes:

- The display-list cache held 16,384 lists (4,096 sets x 4), but Cove
  Town calls about 26,000 a frame: turning pushed lists out, and a third
  of each frame's lists were decoded again (~200,000 per 30 frames,
  against ~70,000 when still). Now 65,536 (`LIST_SETS` 16,384 in
  `gx_fifo.c`): ~66,000 per 30 frames while turning; GL draw calls down
  from 11,000-13,000 a frame to 1,000-4,000.
- The bigger one: the vertex arena was brought up to date with one range
  from its lowest write to its highest. Lists decoded while turning land
  in freed spans anywhere in a 100+ MB arena, so each flush sent tens of
  MB (the time showed up inside the draw calls). Now separate ranges, up
  to 64 (`arena_upload`, `gl_backend.c`).

Dev build, turning: time in draw calls 184 -> 32 ms a second, our side
20.8 -> 4.6 ms a frame, 14 -> 25 fps (the dev build is CPU-bound).

Weak-PC measurements (2026-11-20, Part 6 step 0). The build players get
(the pipeline's optimised build: llvm-mingw `-O2`, no ThinLTO, no PGO),
standing in the Wish Park hub after loading slot 1, 20 s each, RX 9060 XT
with a 16-thread CPU, 2560x1440 window, anti-aliasing off. "Two cores":
the process limited to CPUs 0-1. Game = the game thread's own work, GX =
turning GX into OpenGL (same thread), GPU = timer queries, per frame.

| | fps | busy | game | GX | GPU |
| --- | --- | --- | --- | --- | --- |
| 30 fps, Native | 30.0 | 59% | 16.1 ms | 3.6 ms | 4.9 ms |
| 30 fps, 3x | 30.0 | 59% | 16.1 ms | 3.6 ms | 9.0 ms |
| 60 fps, Native | 59.9 | 96% | 12.5 ms | 3.6 ms | 4.8 ms |
| 60 fps, 3x | 59.9 | 96% | 12.5 ms | 3.6 ms | 8.6 ms |
| Two cores, 30 fps, Native | 29.9 | 72% | 18.8 ms | 5.4 ms | 4.8 ms |
| Two cores, 30 fps, 3x | 30.0 | 71% | 18.5 ms | 5.3 ms | 8.9 ms |
| Two cores, 60 fps, 3x | 30 (Frame rate fell back; worst second 15) | 72% | 18.6 ms | 5.3 ms | 9.1 ms |
| ThinLTO + PGO build, 60 fps, 3x | 60.0 | 68% | 7.8 ms | 3.5 ms | 8.7 ms |

(At 30 fps a game frame is two logic steps, hence more game ms than at 60.)

- In every run: 20 EFB copies a frame (19 to textures), **0 readbacks**
  (Effect copies Accurate), no shader compiles (Smooth shaders), sound
  thread 0.2-0.27% busy, 0 underruns.
- The player's build barely holds 60 here (96% busy); PGO takes the game's
  own work from 12.5 to 7.8 ms (38%). On two of these cores 60 isn't held
  and 30 is 72% busy: a laptop core about half as fast wouldn't hold 30 in
  busy areas without a faster build.
- Frame pacing already sleeps (high-resolution waitable timer in
  `idle.c`); no busy-waiting to remove.
- Attractions: `WONDERPARK_ATTRACTION` reaches each lobby, but scripted
  presses of 2 don't get past "Choose a Pokemon" (probably the pointer),
  so the attractions themselves aren't measured yet. The lobby: 41-43%
  busy, game 6 ms, GPU 5.4 ms at 3x.
- Not measurable on this PC: an Intel integrated GPU.
