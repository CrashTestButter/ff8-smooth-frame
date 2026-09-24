# FF8 Smooth Frames

High-frame-rate battles, world map and fields for **Final Fantasy VIII** (Steam 2013) on top of
**FFNx**. It does not speed the game up.

It is a small DLL (`ff8interp.dll`) loaded next to an **unmodified** FFNx, packaged as a Junction
VIII mod.

## Requirements

- Final Fantasy VIII, **Steam 2013 edition, English executable** (`FF8_EN.exe`). Other languages,
  the 2000 CD release and the Remastered edition are not supported; the mod switches itself off
  there.
- **Stock FFNx 1.24.3 or newer** (Steam edition, `AF3DN.P`), for example as installed by Junction
  VIII. It was tested with 1.24.3.
- FFNx's `ff8_fps_limiter` at 1 (the default), 2 or 3. With 0 the mod has nothing to attach to and
  stays off.
- Recommended: a display that refreshes faster than 60 Hz. With FFNx's `enable_vsync` on (the
  default), the frame rate is capped at your display's refresh rate.

## Install

### Junction VIII (recommended)

1. Download `FF8SmoothFrames-0.2.iro` or `FF8SmoothFrames-0.2.zip` from the releases.
2. Import it:
   - `.iro`: in Junction VIII, **Import mod**, then pick the file;
   - `.zip`: unpack it into J8's mods library folder (it contains the folder
     `FF8SmoothFrames`), or unpack it anywhere and use **Import mod** on that folder.
3. Activate **FF8 Smooth Frames** in your profile and start the game from Junction VIII. J8 may
   warn that the mod contains code (a DLL); that is expected.
4. Settings:
   - folder install: edit `ff8interp.toml` inside the mod folder;
   - `.iro` install: put an `ff8interp.toml` next to `FF8_EN.exe`, because J8 does not unpack the
     settings file from the archive.

### Manual install with an ASI loader (untested)

1. Put [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (32-bit) into the
   game folder and name it `winmm.dll`.
2. Copy `ff8interp.dll` into the game folder renamed to `ff8interp.asi`, and `ff8interp.toml`
   next to it.
3. Keep your normal FFNx install. Do not name anything `dinput.dll` (Junction VIII uses that name).

### Linux / Steam Deck (Proton)

- Junction VIII must run **inside the game's own Proton prefix** (`compatdata/39150`), with the
  environment variable `WINEDLLOVERRIDES="dinput=n,b"`. Otherwise Wine uses its built-in dinput,
  J8's loader never starts, and no mod is applied, this one included. The Linux J8 installers
  ([MateriaForge](https://github.com/dotaxis/MateriaForge-rs), formerly 8thDeck) set this up for
  you.
- Mods only apply when the game is started from Junction VIII. A plain Steam launch runs FFNx
  alone.
- For the manual ASI install, add the Steam launch option `WINEDLLOVERRIDES="winmm=n,b" %command%`.
- The mod was developed and tested under Proton.

## Configuration (`ff8interp.toml`)

| Key | Default | Meaning |
|---|---|---|
| `ff8_battle_anim_interp` | `true` | Smooth battles |
| `ff8_battle_fps` | `144` | Battle frame rate, 30..1000. `0` = use FFNx's `ff8_fps_limiter` (2 = 30, 3 = 60, anything else = off) |
| `ff8_battle_interp_debug` | `0` | Battle kill switches, bits below (add them up) |
| `ff8_world_interp` | `true` | Smooth the world map |
| `ff8_world_fps` | `144` | World map frame rate, 30..1000 (`0` = off) |
| `ff8_world_interp_debug` | `0` | 1 = no camera blend, 2 = no position blend, 4 = no pose blend, 8 = play clock at frame rate, 16 = old one-frame-lead blend |
| `ff8_field_interp` | `true` | Smooth fields |
| `ff8_field_fps` | `144` | Field frame rate, 30..1000 (`0` = 60) |
| `ff8_field_interp_debug` | `0` | 2 = no character blend, 4 = no pose blend, 8 = play clock at frame rate, 16 = old one-frame-lead blend |
| `trace_battle_animation` | `false` | Very verbose per-frame log lines (bug reports only) |
| `ff8interp_diag` | `false` | One status line per second in the log: mode, frame rate, frames per second |
| `ff8interp_strict` | `true` | If another mod already changed a spot this mod needs, undo everything and stay off (see Troubleshooting) |

The debug bits exist to find the cause of a glitch. Leave them at 0 for normal play.

Battle debug bits (`ff8_battle_interp_debug`, names from `interp/battle/internal.h`):

| Bit | Name | Effect when set |
|---|---|---|
| 1 | `DBG_BLINK_OFF` | blink / UI clock (arrow, cursor flash, ready blink) at frame rate |
| 2 | `DBG_POSE_CUT_OFF` | always cross-fade a new animation, never cut |
| 4 | `DBG_RESULTS_GATE_ON` | opt in: pace the after-battle results screens at 60 Hz |
| 8 | `DBG_PAD_REPEAT_OFF` | menu cursor auto-repeat at frame rate |
| 16 | `DBG_GF_BOOST_OFF` | no GF boost-gauge settle before the summon result |
| 32 | `DBG_IMPACT_FX_OFF` | hit / impact effects at frame rate |
| 64 | `DBG_DEATH_FADE_SKIP` | old death fade (corpse may strobe) |
| 128 | `DBG_GF_PACE_OFF` | no GF cinematic pacing |
| 256 | `DBG_QUEUE_PACE_OFF` | no battle task-queue pacing |
| 512 | `DBG_FX_BLEND_OFF` | no magic effect blending |
| 1024 | `DBG_HUD_BLEND_OFF` | no Renzokuken trigger-bar blending |
| 2048 | `DBG_FX_DEFER_OFF` | effect blending without holding a logic step's primitives back |
| 4096 | `DBG_FX_RECT_OFF` | effect blending without sprite / tile primitives |
| 8192 | `DBG_EDGE_ACC_OFF` | no button-press accumulation for Renzokuken / boost |
| 16384 | `DBG_RENZ_SYNC_OFF` | new spell effects at frame rate until their second step |
| 32768 | `DBG_PLAY_CLOCK_OFF` | play clock (play time, countdowns) at frame rate |
| 65536 | `DBG_BOOST_ANY_GF_OFF` | GF boost settle only for Quezacotl |
| 131072 | `DBG_ALPHA_LEAD` | old one-frame-lead blend |
| 262144 | `DBG_DUEL_EDGE_OFF` | Zell's Duel inputs read every frame |
| 524288 | `DBG_APPEAR_RAMP_OFF` | battle-entry fade-in at frame rate (enemies pop in early) |
| 1048576 | `DBG_INTRO_PACE_OFF` | battle intro chain and fade at frame rate |

## What is smoothed

| Area | Status |
|---|---|
| Battle: character and enemy models, weapons, positions | **smoothed** (blended between logic steps) |
| Battle camera | **smoothed** |
| Battle spell effects | **paced**: correct speed, drawn every frame; particle motion still steps at 15 fps |
| GF summons Quezacotl and Shiva | **paced** at the right speed |
| Other GF summons | **not yet**: play about 10x too fast |
| Battle HUD (ATB, menus, damage numbers, blink) | **paced** at the original rate |
| Renzokuken trigger bar | **smoothed** |
| Status-effect glow, flipbook sprites | **still stepped** at 15 fps |
| World map (player, camera, terrain, parallax) | **smoothed** |
| Fields (characters: positions, facing, poses) | **smoothed** |
| Menus, card game, results screens, credits, movies | unchanged (FFNx's normal rate) |

Known limits:
- Battle effects appear about 60 ms behind their sound.
- Vehicles, chocobos and the Ragnarok on the world map are untested.
- FFNx's speedhack has no effect in the smoothed modes.

## Compatibility

- **Texture and model mods**: fine. Tested with AxlRose's WIP (fields, UI, Squall's field model).
- **Code mods that patch the same game code**: the mod notices before it changes anything. By
  default (`ff8interp_strict = true`) it then undoes all its changes, stays off, and writes which
  addresses conflicted to the log. The game runs exactly as with FFNx alone.
- **The FFNx "interp" fork** (an FFNx build with this interpolation built in): do not combine it
  with this mod. The mod detects the fork and stays off.
- **FFNx updates**: the mod attaches to FFNx's frame limiter from outside. If a future FFNx changes
  that code, the mod switches itself off and says so in the log. See `docs/TECHNICAL.md`.

## Troubleshooting

All messages go to **`ff8interp.log`** in the game folder (next to `FF8_EN.exe`). The log is
rewritten on every start.

- **Working**: the log shows a line ending in `... now reaches the ff8interp pacer ...`, then the
  battle / world map / field `interpolation vNN enabled` lines, then
  `install: N call sites and M function entries patched, 0 sites skipped`.
- **"rolled back" / "ff8interp_strict = true"**: another mod (or a different FFNx) changed code the
  mod needs. The warnings above that line name the addresses and who owns them. You can set
  `ff8interp_strict = false` to keep the parts that could be installed, but the affected mode may
  then run too fast.
- **"did not hook the frame limiter" / "ff8_fps_limiter"**: set `ff8_fps_limiter = 1` in
  `FFNx.toml`.
- **"already contains the interpolation module"**: you are running the FFNx interp fork. Use a
  stock FFNx.
- **Still 15 / 30 fps somewhere**: set `ff8interp_diag = true`, play for a few seconds in that
  place, and look at the `diag:` lines. They show the detected mode, the frame rate used and the
  real frames per second. Please attach them to a bug report.
- **Nothing in the log at all**: the DLL was not loaded. Under Proton, check the `dinput=n,b`
  override for Junction VIII.

## Building

See [`docs/BUILDING.md`](docs/BUILDING.md). How it attaches to FFNx, and what can break it:
[`docs/TECHNICAL.md`](docs/TECHNICAL.md).

## Credits

- [FFNx](https://github.com/julianxhokaxhiu/FFNx) by Julian Xhokaxhiu and the FFNx team. This mod
  runs on top of it and mirrors its mode detection and frame limiter.
- [Junction VIII](https://github.com/tsunamods-codes/Junction-VIII) and
  [Tsunamods](https://www.tsunamods.com/), for the mod manager and its mod format.
- The [FF8 Modding Wiki](https://hobbitdur.github.io/FF8ModdingWiki/) (HobbitDur and contributors),
  for engine documentation.

## License

GPL-3.0; see [`LICENSE`](LICENSE). The interpolation module started inside FFNx (GPL-3.0).

Project page: https://github.com/CrashTestButter/ff8-smooth-frame
