# FF8 runtime interpolation module

Renders FF8 (2000 PC / Steam 2013, English 1.2 executable) at any frame rate while every engine
logic loop keeps its native rate: 15 Hz in battle, 30 Hz on the world map and in fields. Between two
logic ticks the module blends model poses, positions, the camera, magic primitives and some HUD
elements; it gates the engine's logic calls so they still run at the native rate on a 144 Hz display.

The module is self-contained. It talks to the program it lives in only through a small host
interface (`host.h`), so it can run inside FFNx (this tree, `adapter_ffnx.cpp`) or later as a
standalone DLL with a different adapter.

## Architecture

```
host program (FFNx)                       src/ff8/interp/
------------------------------------      ---------------------------------------------------
cfg.cpp  read_cfg()           ------->    adapter_ffnx.cpp  read_cfg(toml)   options -> InterpConfig
ff8_init_hooks()              ------->    adapter_ffnx.cpp  init()           builds InterpHost
                                                              |
                                          core.cpp          install(host) -> battle / world / field *_hook_init()
ff8_limit_fps() (per frame)   ------->    core.cpp          on_frame()    -> *_on_frame()  (mode-change resets, traces)
ff8_limit_fps() frame cap     ------->    core.cpp          frame_rate()  -> ff8_*_fps while that mode's module is installed
```

Every sub-module keeps its own wall-clock accumulator (`common/tick.h`: seconds since the previous
presented frame, cached per host frame number) and fires a logic tick every 1/15 s or 1/30 s. The
`ff8_*_fps` options only cap the render rate; the logic rate never depends on them.

The hooks go in once at startup. Each one checks the current game mode itself (`game_mode()`) and
passes straight through outside its mode.

## Host interface (`host.h`)

The adapter fills one `ff8_interp::InterpHost` and passes it to `ff8_interp::install()`. The module
copies it into `ff8_interp::g_host`. Module code reads it only through the inline accessors
(`ff8_interp::hook_call()`, `cfg()`, `frame_counter()` and the others).

| Field | Contract |
|---|---|
| `hook_call(site, fn)` | `site` holds an `E8` call, an `E9` jmp or an `FF 15` indirect call. Rewrite its rel32 operand so the instruction reaches `fn` (`fn - site - 5`, or `- 6` for `FF 15`) and leave the opcode alone. Make the page writable first (`VirtualProtect`). This is FFNx `replace_call`. |
| `hook_function(addr, fn)` | Write `E9 rel32` to `fn` over the first 5 bytes at `addr`. The module has already copied the prologue into its own trampoline (`make_trampoline` in `battle/animations.cpp`: byte-checked, position-independent prologue plus `jmp addr+len`, `VirtualAlloc` RWX). So the host must patch exactly 5 bytes and must not relocate anything. The return value is an opaque handle that the module ignores. This is FFNx `replace_function`. |
| `log_info / log_warning / log_trace(fmt, ...)` | printf-style. The format strings already end in `\n`. |
| `game_mode()` | `Field`, `World`, `Battle` or `Other`. Must stay stable within one presented frame (see below). |
| `mode_id()` | The host's own mode number, used only to detect a mode change (any change resets the sub-modules, including a change between two `Other` modes) and in trace lines. |
| `frame_counter` | Pointer to a `uint32_t` that the host increments once per presented frame. All per-frame caches key on it. It is read through a pointer rather than a call because the hot hooks read it often. |
| `is_us_exe()` | True on the English 1.2 executable. Every sub-module skips itself on anything else. All addresses in the module are US addresses. |
| `config` | `InterpConfig`: `battle_enabled/fps/debug`, `world_enabled/fps/debug`, `field_enabled/fps/debug`, `host_limiter_fps` (30 or 60 when the host caps at that rate, otherwise 0; it is the battle rate when `battle_fps` is 0) and `trace`. |

Besides the struct, a host must:

1. **Call `install()` once**, early, before the first game frame. FFNx does it at the end of
   `ff8_init_hooks`, after its own engine patches. The module's call-site hooks assume that nothing
   else rewrites the same call sites later.
2. **Call `on_frame()` once per presented frame**, before its own frame pacing waits. The call also
   has to happen while the game sits in a mode the module does not handle, so that mode changes
   reset the sub-modules.
3. **Cap the frame rate at `frame_rate(host_rate)`.** The engine only runs one main-loop pass per
   presented frame. The module needs the host to present frames at `ff8_*_fps` in its modes, and
   the uncapped logic is safe only because the gates are installed. FFNx keeps its busy-wait limiter
   `ff8_limit_fps` and asks the module for the rate. A standalone DLL has to supply its own limiter:
   hook the game's frame pacing, or run with a present hook plus vsync.
4. **Detect the mode.** FFNx `getmode()` (`src/common.cpp`, mode table `src/ff8_data.h`) uses two
   game variables: the module WORD `_mode`, whose address is the operand at `main_loop + 0x115`
   (`ff8_data.cpp`; `main_loop` is found from the main-menu loop), and the running loop function
   `game_obj->game_loop_obj.main_loop`. That function is compared with the per-mode loops read from
   `main_loop + 0x144` (field), `+ 0x2D0` (world map) and `+ 0x340` (battle); the JP executable uses
   different offsets. Field is `_mode` 1 (`FF8_MODE_FIELD`) with the field loop running. World map
   is `_mode` 2 (`FF8_MODE_WORLDMAP`) with the world loop running. Battle has no `_mode` value of its
   own: `_mode` is 8, which it shares with the card game (`*is_card_game == 1` means card game), and
   FFNx recognises battle by the battle main loop alone (its pseudo-mode 999, `FF8_MODE_BATTLE`). A
   standalone adapter can read the same WORD and loop pointer. It should cache the result once per
   presented frame, as `getmode_cached()` does.

## FFNx integration (the only edits to upstream files)

- `src/cfg.cpp`: one forward declaration and the call `ff8_interp::read_cfg(config)`. The toml keys
  and defaults are unchanged (`ff8_battle_anim_interp`, `ff8_battle_fps`, `ff8_battle_interp_debug`,
  `ff8_world_interp`, `ff8_world_fps`, `ff8_world_interp_debug`, `ff8_field_interp`, `ff8_field_fps`,
  `ff8_field_interp_debug`). `trace_battle_animation` and `ff8_fps_limiter` are existing FFNx options
  that the adapter copies at `init()`.
- `src/ff8_opengl.cpp`: the `#include "ff8/interp/interp.h"`, `ff8_interp::init()` in
  `ff8_init_hooks`, `ff8_interp::on_frame()` at the top of `ff8_limit_fps`, and
  `framerate = ff8_interp::frame_rate(framerate);` before the speedhack multiplier. The last one has
  to stay in FFNx because the limiter's wait loop and its local frame rate belong to FFNx.
- `misc/FFNx.toml`: documentation of the options.
- No CMake change. `CMakeLists.txt` collects `src/*.cpp` with `GLOB_RECURSE`, so re-run the CMake
  configure step once after adding or moving files.

## Files

| File | Role |
|---|---|
| `host.h` | `InterpHost`, `InterpConfig`, `GameMode`, the inline accessors |
| `interp.h` | entry points: `init()` (adapter), `install()`, `on_frame()`, `frame_rate()` (core) |
| `core.cpp` | host-independent entry points. Installs, runs and frame-caps the three sub-modules |
| `adapter_ffnx.cpp` | FFNx host. It is the ONLY file here that includes FFNx headers |
| (outside this tree) | `~/ff8/ff8interp/src/adapter_standalone.cpp`: standalone host for `ff8interp.dll` next to a stock FFNx; compiles every file here except `adapter_ffnx.cpp` |
| `common/tick.h` | wall-clock frame delta shared by all sub-modules (`ff8_tick`) |
| `common/play_clock.{h,cpp}` | 60 Hz pacing of the play-time clock `0x4701B0` (`ff8_play_clock`) |
| `common/chara_pose.{h,cpp}` | world / field skeleton pose blender, quaternion slerp (`ff8_chara`) |
| `battle/animations.{h,cpp}` | battle module (`ff8_battle_anim`): model poses, entity positions, camera, effect / ATB / UI gates, pad pacing, trampolines |
| `battle/internal.h` | battle-internal glue: shared tick accessors, `DBG_*` kill bits, sibling entry points |
| `battle/gf_pacing.cpp` | GF summon cinematic pacing and camera ownership |
| `battle/fx_queues.cpp` | battle task-queue pacing |
| `battle/fx_blend.cpp` | magic primitive blending (display-list record / replay) |
| `battle/hud_blend.cpp` | Renzokuken trigger bar blending |
| `world/interp.{h,cpp}` | world map module (`ff8_world`) |
| `field/interp.{h,cpp}` | field module (`ff8_field`) |

Namespaces: the module keeps `ff8_battle_anim`, `ff8_world`, `ff8_field`, `ff8_tick`,
`ff8_play_clock` and `ff8_chara`, plus `ff8_interp` for the host layer. A namespace called `ff8`
cannot be used, because FFNx has a global `uint32_t ff8`.

## Version strings

Each sub-module logs its version at install (`ff8_interp::log_info`). Grep the log for these to see
which build is running:

- battle: `battle animation interpolation v73 enabled (...)`
- world map: `world map interpolation v21 enabled (...)`
- field: `field interpolation v5 enabled (...)`

Bump the number in the `log_info` line of `*_hook_init()` whenever that sub-module changes.

## Kill bits

`ff8_battle_interp_debug`, `ff8_world_interp_debug` and `ff8_field_interp_debug` are bit masks that
switch individual gates or blends off, for bisecting in game. The battle bits are the `DBG_*`
constants in `battle/internal.h`. The world and field bits sit at the top of their `interp.cpp`.
Each bit, the symptom it covers and its test are documented in `~/ff8/notes/verify-checklist.md`
(§3.3 battle bits, and the world and field sections) and in `~/ff8/notes/ffnx/interp-module.md`.
