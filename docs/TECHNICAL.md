# Technical notes

How ff8interp.dll attaches to a stock FFNx, and what can break it. For users, see `../README.md`.
For building, see `BUILDING.md`.

## Layout

| Path | Role |
|---|---|
| `interp/` | the interpolation module (battle, world map, field) and its host interface `host.h`. This repo is its source of truth (`interp/SOURCE.md`). `interp/README.md` has the module architecture and host contract. |
| `src/dllmain.cpp` | `DllMain`: one instance per process, starts the watcher thread, installs nothing |
| `src/adapter_standalone.cpp` | the `InterpHost` (hooking, mode, frame counter, logs), FFNx detection, limiter re-hook, pacer, deferred install, diagnostics |
| `src/config.cpp` | `ff8interp.toml` reader (flat TOML subset, no library) + `ff8_fps_limiter` from the game's `FFNx.toml` |
| `src/log.cpp` | `ff8interp.log` in the game folder |
| `mod/` | Junction VIII mod folder: `mod.xml`, `ff8interp.toml` (+ the built `ff8interp.dll`) |
| `tools/pack_iro.py` | packs `mod/` into a J8 `.iro` (stored, uncompressed) and verifies it |

## How it works

### Start-up (deferred install)

1. **DllMain** installs nothing (loader lock; FFNx has not run `ff8_init_hooks` yet). It checks
   that the process is `FF8_*.exe`, takes a per-process mutex and starts the watcher thread.
2. **Watcher thread** (off the game thread):
   - opens `ff8interp.log`; checks the FF8 1.2 US signature (FFNx `get_version`: dwords at
     `0x401004` / `0x401404`);
   - finds `AF3DN.P`, logs its path range and version resource (FileVersion + ProductVersion);
     refuses to run if that AF3DN.P contains the string `ff8_battle_anim_interp` (the FFNx interp
     fork has the module built in; both together would hook every site twice);
   - reads the options;
   - polls the game's limiter entry `0x4020F0` every 1 ms until it holds `E9 rel32` (FFNx
     `replace_function(ff8_externals.fps_limiter, ff8_limit_fps)` in `ff8_init_hooks`), stable over
     20 ms and pointing into AF3DN.P. Vanilla prologue `83 EC 10 8D 44` = not yet. Other bytes, a
     jump outside AF3DN.P, or a graphics driver that exists for 10 s while the entry stays vanilla
     (FFNx `ff8_fps_limiter = 0`) -> log and stay out;
   - saves the jump target (FFNx `ff8_limit_fps`) and rewrites the rel32 at `0x4020F1` to the
     pacer (one locked 32-bit store, same cache line: the game thread sees the old or new target,
     both valid). This is the only write made off the game thread.
3. **First pacer call** (game thread; the limiter is only called by the credits, field, battle and world loops, so this is the end of the first boot-credits frame or of the first frame in one of those modules; all FFNx init is done by then):
   - verifies every engine constant the adapter uses (mode word, module loops, card-game flag, game
     object, driver offset, flip call, cross-fade timer operand);
   - reads `FF8_EN.exe` from disk as the byte reference;
   - hooks the frame counter (below);
   - builds the `InterpHost` and calls `ff8_interp::install()`;
   - logs how many call sites / function entries were patched and skipped. With
     `ff8interp_strict = true`, one skipped site rolls back every patch and the flip hook; the pacer
     then only forwards to FFNx.

### Host interface

- **`hook_call` / `hook_function`**: FFNx `replace_call` / `replace_function` semantics. Before
  patching, the bytes at the site (5, or 6 for `FF 15`) must equal FF8_EN.exe on disk, or be exactly
  what ff8interp wrote there earlier. Otherwise the site is skipped with a warning that shows memory
  vs disk bytes and, for E8/E9, the current target and the module that owns it (typically AF3DN.P).
  Every patch keeps its original bytes for the rollback.
- **`hook_call_chain`**: for a call site FFNx itself redirects. It is allowed when the bytes are
  still the game's, or when the E8/E9 now reaches into AF3DN.P. It returns the previous target, so
  the module calls FFNx's function, not the vanilla one. A redirect by anything else is refused.
- **Game mode**: a copy of FFNx `getmode()` over the FF8 mode table: the `_mode` WORD `0x1CD8FC6`,
  the running loop `game_obj(*0x1A79D88)+0xB40` (the `game_loop_obj` at +0xB30; NOT the +0xA1C that FFNx's `ff8.h` field names suggest, which made v0.1 miss every battle), the loops field `0x46FEE0`, world `0x53F0F0`,
  battle `0x47CF60`, swirl `0x559890`, credits `0x52DA20`, main menu `0x4A24B0`, and the card-game
  flag `0x1DCD798`. The result is cached per frame counter, like `getmode_cached()`. `mode_id()` is
  the FFNx driver-mode number.
- **Frame counter**: FFNx increments `frame_counter` in `common_flip`. The game calls the driver's
  flip through `0x41DF0C` (`call [driver+0x10]`, driver = `game_obj+0xA74`, the struct FFNx's
  `ff8_load_driver` allocated). ff8interp swaps that one data pointer for a wrapper that calls FFNx's
  `common_flip` and then increments its own counter, so the counter matches FFNx's exactly. No code
  byte is touched. The pacer re-checks the slot every frame and re-hooks if FFNx rebuilt the driver.
  If the slot cannot be hooked, the counter falls back to +1 per limiter call (logged).
- **`on_frame()` and frame pacing**: the pacer is what the game's 4 limiter call sites reach
  (field `0x470141`, battle `0x47D259`, credits `0x52DB6F`, world `0x53F2D7`). Like the fork, it
  calls `on_frame()` first, then `frame_rate(-1)`:
  - no override (menus, card game, swirl, or a module that is off): FFNx's `ff8_limit_fps` runs
    unchanged (its rate, speedhack and cross-fade timer);
  - override: a QPC busy-wait to `ff8_*_fps`, the same loop FFNx uses. It also writes FFNx's only
    other side effect: `*(double*)0x1A78BE0` (`time_volume_change_related_1A78BE0`, the vanilla
    music cross-fade step) = ms since the previous limiter exit. When pacing goes back to FFNx,
    FFNx's own value is stale (its `last_gametime` did not move while we paced), so ff8interp
    overwrites it once with the real frame time.

## Fragility (read before updating FFNx)

- **The limiter re-hook depends on FFNx internals**:
  - FFNx must implement its limiter as a 5-byte `E9` at the game's limiter entry
    (`replace_function`), written in `ff8_init_hooks`;
  - `ff8_limit_fps` must stay a no-argument cdecl function returning int;
  - its only side effects must be the busy-wait and the cross-fade timer.

  All three hold for 1.22.0 .. 1.24.3 and master (2026-09). If FFNx adds work to `ff8_limit_fps`,
  it still runs in the pass-through modes but is skipped in the interpolated ones. Nothing in the
  binary can detect that; re-read `ff8_limit_fps` in `src/ff8_opengl.cpp` on every FFNx update.
- **Flip slot**: FFNx's driver struct layout (`flip` at +0x10) and "`common_flip` increments
  `frame_counter`" are assumed.
- **Speedhack**: FFNx's speedhack multiplies only FFNx's own rate. In the interpolated modes the
  logic is paced on wall-clock time anyway, so the speedhack does nothing there (same as the fork).
- **Late FFNx patches**: sites FFNx patches after our install (at mode entry, for example) would
  overwrite ours without a check. None are known: FFNx only toggles vibration at run time.
- **Two limiters in one process**: another mod that also chains FFNx's limiter jump is detected
  only if it rewrote the jump before us (target outside AF3DN.P). If it writes after us, it wins.
- **What the byte check cannot see**: a foreign patch *inside* a function whose call site we hook
  changes behaviour without changing the site bytes.
- **Loader timing**: the watcher needs FFNx to hook the limiter within 10 minutes of process start.
  A DLL loaded after `ff8_init_hooks` also works: the jump is already there.

## Diagnostics line

With `ff8interp_diag = true` (or `trace_battle_animation = true`), the pacer logs one line per
second:

```
diag: mode World (FFNx driver_mode 2, _mode 2, loop 0053F0F0, card 0) rate 144 | 143.6 limiter calls/s: 144 paced, 0 to FFNx | 143.6 flips/s | frame 549
```

The fields are:
- the module's game mode, then FFNx's `driver_mode` number (field 0, battle 1, world 2);
- the game's raw `_mode` WORD, the running main loop, and the card-game flag;
- the rate the pacer used (`FFNx/0` = handed to FFNx's own limiter);
- limiter calls per second, split into paced and passed to FFNx;
- presented frames (flips) per second.

## Tested

- In game (2026-09-24): stock FFNx 1.24.3 via Junction VIII under Proton, `ff8_fps_limiter = 1`.
  Battle, field and world map were presented at 144 fps. Install log: "130 call sites and 38
  function entries patched, 0 sites skipped".
- Site `0x53FFA6` (world map) is redirected by stock FFNx. The module hooks it with
  `hook_call_chain`, which calls FFNx's target instead of the vanilla one.
