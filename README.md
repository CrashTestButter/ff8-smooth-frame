# FF8 Smooth Frames

Smooth, high-frame-rate battles, world map and fields for **Final Fantasy VIII** (Steam 2013, or
Remastered through Junction VIII), without speeding the game up.

https://github.com/user-attachments/assets/52ea8750-5e22-49be-bf56-cdd4d54ad73e

https://github.com/user-attachments/assets/96d77b3d-b08e-42d2-9f55-bc1cf1462e8e

FF8 normally moves at 15 frames per second in battle and 30 on the world map and in towns and
dungeons. This mod draws the frames in between, so characters, the camera and spell effects move
smoothly at 144 fps (or any rate you choose), while the game itself runs at its normal speed.

It is a Junction VIII mod and works with the FFNx that Junction VIII installs.

## Requirements

- Final Fantasy VIII in **English**, one of:
  - the **Steam 2013 edition** (`FF8_EN.exe`);
  - the **Remastered edition** set up in Junction VIII (J8 runs it with `ff8.exe` and FFNx as
    `eax.dll`).

  Other languages are not supported; the mod stays off there.
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

In Junction VIII, select **FF8 Smooth Frames** in your profile and click **Configure Mod**:

- **Frame rate**: 60, 90, 120, 144 (default), 165 or 240 fps. Pick your display's refresh rate.
- **Diagnostic log**: turn on when reporting a problem.

## What is smooth so far

- Battles: characters, enemies, weapons, the camera and the Renzokuken bar.
- Spell effects run at the right speed; some particles still move in steps.
- Summons run at the right speed (Odin is still being worked on).
- World map and fields: characters, the camera, background scrolling and draw point sparkles.
  Dialogue windows, timers and the play-time clock keep their normal speed.
- Not changed yet: menus, the card game, the results screen, movies.

Everything is still work in progress. Effects can appear slightly behind their sound, and vehicles
and chocobos on the world map are untested.

## Compatibility

- Texture and model mods work (tested with AxlRose's WIP).
- Do not combine it with other frame-rate or interpolation mods. If another mod changes the same
  game code, Smooth Frames turns itself off and the game runs as normal.

## Reporting a problem

The mod writes `ff8interp.log` in the game folder (next to `FF8_EN.exe`, or `ff8.exe` on
Remastered). If something looks wrong or a place is still
choppy, turn on **Diagnostic log** in Configure Mod, play for a few seconds there, and attach the
log to an [issue](https://github.com/CrashTestButter/ff8-smooth-frame/issues).

## Credits

- [FFNx](https://github.com/julianxhokaxhiu/FFNx) by Julian Xhokaxhiu and the FFNx team.
- [Junction VIII](https://github.com/tsunamods-codes/Junction-VIII) and
  [Tsunamods](https://www.tsunamods.com/).
- The [FF8 Modding Wiki](https://hobbitdur.github.io/FF8ModdingWiki/) (HobbitDur and contributors).

## License

GPL-3.0; see [`LICENSE`](LICENSE). Building from source: [`docs/BUILDING.md`](docs/BUILDING.md).
