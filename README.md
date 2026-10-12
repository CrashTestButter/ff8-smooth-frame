# FF8 Smooth Frames

Smooth, high-frame-rate battles, world map and fields for **Final Fantasy VIII** (Steam 2013),
without speeding the game up.

> _Video / GIF placeholder: 15 fps vanilla vs 144 fps side by side._

FF8 normally moves at 15 frames per second in battle and 30 on the world map and in towns and
dungeons. This mod draws the frames in between, so characters, the camera and spell effects move
smoothly at 144 fps (or any rate you choose), while the game itself runs at its normal speed.

It is a Junction VIII mod and works with the FFNx that Junction VIII installs.

## Requirements

- Final Fantasy VIII, **Steam 2013 edition, English** (`FF8_EN.exe`). Other languages, the 2000 CD
  release and the Remastered edition are not supported; the mod stays off there.
- **Junction VIII** with FFNx 1.24.3 or newer.
- Recommended: a display faster than 60 Hz. With vsync on (FFNx's default), the frame rate is
  capped at your display's refresh rate.

## Install

1. Download [`FF8SmoothFrames.iroj`](https://github.com/CrashTestButter/ff8-smooth-frame/releases/latest/download/FF8SmoothFrames.iroj)
   from the [latest release](https://github.com/CrashTestButter/ff8-smooth-frame/releases/latest).
2. In Junction VIII, click **Import mod** and pick the file.
3. Activate **FF8 Smooth Frames** in your profile and start the game from Junction VIII. J8 may
   warn that the mod contains code (a DLL); that is expected.

To update, import the new release the same way.

**Steam Deck / Linux**: Junction VIII has to run inside the game's Proton prefix. The Linux J8
installer [MateriaForge](https://github.com/dotaxis/MateriaForge-rs) sets this up for you. Mods only
apply when the game is started from Junction VIII.

## Settings

To change the frame rate, download `FF8SmoothFrames.zip` from the release, take `ff8interp.toml`
out of it, edit it, and put it next to `FF8_EN.exe` in the game folder.

| Setting | Default | |
|---|---|---|
| `ff8_battle_fps` | `144` | Battle frame rate (30 to 1000) |
| `ff8_world_fps` | `144` | World map frame rate |
| `ff8_field_fps` | `144` | Field frame rate (towns, dungeons) |
| `ff8_battle_anim_interp`, `ff8_world_interp`, `ff8_field_interp` | `true` | Set to `false` to turn smoothing off in that part of the game |

## What is smooth so far

- Battles: characters, enemies, weapons, the camera and the Renzokuken bar.
- Spell effects run at the right speed; some particles still move in steps.
- Summons: Quezacotl and Shiva run at the right speed. Other summons still play too fast.
- World map and fields: characters and the camera.
- Not changed yet: menus, the card game, the results screen, movies.

Everything is still work in progress. Effects can appear slightly behind their sound, and vehicles
and chocobos on the world map are untested.

## Compatibility

- Texture and model mods work (tested with AxlRose's WIP).
- Do not combine it with other frame-rate or interpolation mods. If another mod changes the same
  game code, Smooth Frames turns itself off and the game runs as normal.

## Reporting a problem

The mod writes `ff8interp.log` next to `FF8_EN.exe`. If something looks wrong or a place is still
choppy, set `ff8interp_diag = true` in `ff8interp.toml`, play for a few seconds there, and attach
the log to an [issue](https://github.com/CrashTestButter/ff8-smooth-frame/issues).

## Credits

- [FFNx](https://github.com/julianxhokaxhiu/FFNx) by Julian Xhokaxhiu and the FFNx team.
- [Junction VIII](https://github.com/tsunamods-codes/Junction-VIII) and
  [Tsunamods](https://www.tsunamods.com/).
- The [FF8 Modding Wiki](https://hobbitdur.github.io/FF8ModdingWiki/) (HobbitDur and contributors).

## License

GPL-3.0; see [`LICENSE`](LICENSE). Building from source: [`docs/BUILDING.md`](docs/BUILDING.md).
