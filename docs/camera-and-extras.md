# Camera and extras: step 0 findings (2026-11-17)

From the "camera and extras" plan, step 0: what we have,
what MildRain479/PokePark-2-Wiicompiled (CC0, cloned to
`refs\PokePark-2-Wiicompiled`, never committed) gives us, and sizes. No code
changed. (The prompt says to clone to `D:\ref\`; the project keeps its clones in
`refs\`.)

Read: `recomp/patches/S2LE01/` `hle_camera.c` (297 lines), `hle_friends.c` (148),
`hle_minimap.c` (37), `hle_autosave.c` (126), `hle_modifiers.c` (173),
`hooks.txt`, plus `hle_game.c` (scene tracking, which several of them need).

## Do the addresses match our build?

Yes. Same disc (S2LE01 v1.00), same main.dol. Checked against our recompiled
code: each is a function entry (`stwu r1` prologue), and the details agree.

| Address | What (MildRain's name) | Checked |
| --- | --- | --- |
| `0x80024EE0` | CameraFollow_Update | entry; the follow camera's vtable `0x80434938` +0x24 holds it |
| `0x80024C50` | CameraFollow_EasePitch | entry |
| `0x8001EFD0` | CameraManager_Update | entry |
| `0x80019B20` | Camera_ApplyToG3dCamera (fovy in f1) | entry |
| `0x8001A390` | FrustumForCamera | entry |
| `0x800B0200` | LifeModuleImpl virt0x38 (damage in f1) | first lines are `left = left - damage` |
| `0x801E1210` | FieldAIActionChallengeChase virt0x14 | entry |
| `0x800A9340` | KineticEnergyControl virt0x4_0xC | entry |
| `0x8013CAC0` | a layout's draw (minimap hook) | entry |
| `0x8044A850` | ModuleAccessorPlayer vtable | its type info names it |
| `0x80076EE0`, `0x8012DD80` | CollisionManager Get, Ray (camera wall checks) | from their `addrs.h` |

None of these is hooked by us yet.

## Part 1: free camera

**What we have now:** nothing that turns the camera. `input_win32.c` already
captures the mouse and has a `read_mouse` (mouse plus right stick, sensitivity,
invert Y) for `g_host_mouse_read`, but nothing calls it: the "Camera speed" and
"Invert up / down" rows in F1 only reach the pointer today. Field of view works
by scaling the world's projection in `gx_fifo.c` (widescreen only), which can't
change the game's culling, so things pop in at the sides above 100%.

**Reuse:** MildRain's `hle_camera.c` is the whole Level 1 design the prompt
describes (yaw by swinging the eye round the followed point, pitch as an offset
only while placed, wall pull-in and ground clearance applied only for drawing, B
eases back) and its FOV hooks widen fovy in both the camera and the frustum, so
culling matches. It can't be copied as-is: it's written for their runtime
(`ppc_t`, `F_x_orig` wrappers, `ld_f32`/`st_f32`, `ppc_icall` with 8 arguments
for the collision ray, `hle_in_minigame` from their `hle_game.c`). Porting means:
our `WP_HOOK` with `wp_call_original` for the before-and-after hooks, a
`wp_call_guest` that passes up to 8 arguments, and a small scene tracker
(their `hle_game.c`: next-scene hook `0x80210660`, `AddSceneParameter`
`0x80206DC0`). Credit in the README as for the rest of their code.

What the prompt asks beyond theirs: aim at Pikachu's real position rather than
the trailing target, stick sensitivity and invert X, auto-recenter, scene/warp
resets. Small additions on top.

| Level | Size | Notes |
| --- | --- | --- |
| 1. Orbit camera | **Medium** | port of `hle_camera.c` (+ scene tracker, 8-argument guest call), F1 rows (most exist), our FOV moved to their hooks so culling matches |
| 2. Photo mode | **Large** | nobody has one. The hard part: while paused, our runtime only shows the last picture again (`gl_present_last`); a free camera needs new pictures. Either let the game draw with its logic held (needs the game's update/draw split found) or redraw the last frame's recorded draws with a changed view (world matrices are model x view, so each would be corrected by the new view). Then flying, HUD hide (layout draws are already told apart in `gx_fifo.c`), high-resolution capture, PNG out |
| 3. Cutscene camera | **Medium, after 2** | in-engine scenes only (movies are Mobiclip pictures). Mostly testing which scenes are safe |

## Part 2: extras

| | Extra | Size | CC0 head start | Risk | Notes |
| --- | --- | --- | --- | --- | --- |
| A | Wii Remote speaker through the PC | Medium | none (theirs is silent too) | Medium | the game already reaches our `WPADSendStreamData`, where the data is dropped today; it needs decoding (the Wii Remote's 4-bit ADPCM) and mixing in |
| B | Controller rumble with strength | **Done** | | | F1's Rumble (Off / Light / Normal / Strong), from the game's own motor calls |
| C | Speed-up toggle | Small-medium | `hle_game.c` (which scene) | Low | we already have a game-speed multiplier (debug overlay, F7) that runs extra steps; needs a player key, off in attractions and cutscenes (scene tracker), and sound (it goes silent when sped up) |
| D | Friendship tracker in F1 | Medium | `hle_friends.c` (+ `hle_pad_art.c` for names and icons from the disc) | Low | the data reading ports directly; the work is an F1 page |
| E | Second-monitor map | Medium-large | `hle_minimap.c` (marks the minimap's draws in the FIFO) | Medium | a second window and drawing only the marked draws there |
| F | Accessibility | Mixed | none | Low-medium | hold-to-toggle and one-handed layouts are small (input layer); colour-blind dance icons can use our texture mods; text scale and screen shake/blur off need the game's code found |
| G | Speedrun timer + LiveSplit | Medium | none | Low | timer overlay is small; an auto-splitter needs a fixed place for LiveSplit to find guest memory and the load/scene addresses |
| H | Difficulty and assists | **Small** | `hle_modifiers.c` | Low | berries and prices are already ours (`hle_balance.c`, the same approach); their damage (`0x800B0200`) and chase speed (`0x801E1210`, `0x800A9340`) hooks port directly and are exactly what F1's two rows still need. "Forgiving minigame timing" is separate and unknown |
| I | Draw distance / LOD | Medium-large | `hle_camera.c` (far plane at camera +0x44) | Medium | far plane is known; LOD switches aren't |
| J | Achievements (local) | Medium | `hle_friends.c` for friends | Low | per-minigame results still to be found |
| K | Autosave + backups | Small-medium | `hle_autosave.c` | Medium (a save at a bad moment) | theirs waits until no event script has run for half a second; rotating backups are easy in our NAND layer |
| L | Quick restart of attractions | Medium-large | `hle_game.c` (entering an attraction from the park) | High | leaving and re-entering a scene cleanly is untested |
| M | Discord Rich Presence | Small-medium | `hle_autosave.c` (area and zone), `hle_friends.c` (count) | Low | Discord's local pipe, no library; opt-in, off by default |

**Top 5 (recommended order):**

1. **H, difficulty**: small, CC0 code ready, and it finishes the F1 tab's last
   two balance rows (damage dealt and taken, chase speed).
2. **Level 1 camera**: the most noticeable improvement while playing; medium,
   with CC0 code to port.
3. **D, friendship tracker**: CC0 data reading, low risk, useful.
4. **K, autosave + backups**: CC0 code, protects players' saves.
5. **C, speed-up**: mostly there already (game speed), small once scenes are
   tracked (which Level 1 brings).

Photo mode (Level 2) is the big unique one; I'd do it after these, since it
needs the "redraw while paused" piece worked out first.

## Progress

- **H (2026-11-17):** damage dealt and taken, and chase speed, ported to
  `runtime/hle/hle_modifiers.c`; the F1 rows work. Chase speed checked with
  `WONDERPARK_CHASE_ALL=1` (speeds scaled 1.5x at 150%; at 50% the park's
  Pokémon covered half the ground). Damage not yet tried in a battle.
- **Level 1 camera (2026-11-17):** ported to `runtime/hle/hle_camera.c`
  (with `hle_scene.c` and `wp_call_guest8` for the collision ray). Settings:
  Free camera, Camera speed (mouse), Stick speed, Invert left / right and
  up / down, Recenter after, Field of view (now the game's cameras, so
  culling matches; the projection scaling in `gx_fifo.c` is gone). Checked
  in Cove Town: turning and tilting by a scripted mouse, B's look-around
  bringing it back, field of view 130% wider. Not yet: aiming at the
  player's real position instead of the trailing point. Frame rate
  (optimised build, Cove Town): the camera turned and held at 90, 180 or 270
  degrees stays at 30 fps (70-82% busy, against 65-69% unturned), but while
  it keeps turning (a test mouse at about 36 degrees a second, without stop)
  it dips to about 10 fps for a few seconds each turn and comes back. Not
  from the camera's collision rays (about 5 ms a second in all); the cause
  is still to be found (the game reacting to a camera that moves every
  frame?). Short, ordinary turns are what players will mostly do, but this
  needs looking at. **Fixed 2026-11-18** (docs/handoff/PERFORMANCE.md, "The
  camera turning dip"): the display-list cache was too small and the vertex
  arena was sent to the GPU in one huge range; turning nonstop now averages
  29.3 fps (2 of 30 seconds under 25, was 21 of 30).
- **D, friendship tracker (2026-11-17):** a Friends list at the end of F1's
  Gameplay page (`runtime/hle/hle_friends.c`, `settings_ui.cpp`): "194 of
  194" with the 100% save, "2 of 194" with the Wish Park save (its Pad says
  Friends 2), icons and English names from the disc. Not grouped by zone.
- **K (2026-11-18): save backups done, autosave shelved.** Backups: before a
  save slot is written, a copy goes to `backups\` (last 10 per slot, no
  duplicates); checked with a real save through the PokéPark Pad on the
  100% save ("You saved the game!", reloads with play time 23:55, no
  "corrupted" message). Autosave was ported from MildRain's
  `hle_autosave.c` (save started through the Pad's own routine,
  `0x802165A0`, with the game held by `0x8002EAB0`), but **the slot it wrote
  was corrupted**: on reload the game said "Saved Game 1 is corrupted". The
  written GD0 and GD0M differed in 13 bytes (around +0x253, +0x28A, +0x12B6,
  +0x14C0), so the game's data kept changing while the save was written:
  the hold doesn't freeze everything the Pad's own pause does (play time,
  at least). Tested with `WONDERPARK_AUTOSAVE_NOW` (a save as soon as the
  field is up), on a test copy only. It's out of the build until it can
  save from a paused state; the port is kept outside the repo
  (`build/re/shelved/hle_autosave.c`, git-ignored) for that.
- **C, speed-up (2026-11-18):** Tab / the left trigger, hold or toggle, 2x or
  3x, park only (not attractions, movies or event scripts), silent while on
  (`runtime/host/input_win32.c`). It switches on and off as it should, but
  the gain is small: each extra step is a whole frame, drawing included,
  and in Cove Town the optimised build is already 65-69% busy at 30 fps, so
  2x gave 34 game frames a second (1.13x). A real 2x needs the in-between
  steps' drawing left out: the next thing to do for C. Not yet either: sound
  while sped up, an on-screen marker.

## Notes for the battle and chase work (F1 balance rows)

- MildRain's `hle_modifiers.c` has the exact hooks the damage and chase rows
  need (above), so the code tracing in `docs/handoff/SETTINGS.md` can stop.
- Their test entry `RECOMP_ATTRACTION=<n>,<seconds>` enters an attraction
  (1-4) from the park; it doesn't start battles or chases, so testing those
  still needs a save next to one.
- Their berry modifier is the same approach as ours (watch the count each
  frame, scale changes), but only while in the park scene, which avoids
  scaling a minigame's prize before it's shown.
