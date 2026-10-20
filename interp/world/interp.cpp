/****************************************************************************/
//    Copyright (C) 2026 Julian Xhokaxhiu                                   //
//    Copyright (C) 2026 Todi                                               //
//                                                                          //
//    This file is part of FFNx                                             //
//                                                                          //
//    FFNx is free software: you can redistribute it and/or modify          //
//    it under the terms of the GNU General Public License as published by  //
//    the Free Software Foundation, either version 3 of the License         //
//                                                                          //
//    FFNx is distributed in the hope that it will be useful,               //
//    but WITHOUT ANY WARRANTY; without even the implied warranty of        //
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         //
//    GNU General Public License for more details.                          //
/****************************************************************************/

// FF8 world map interpolation.
//
// The world map runs logic and rendering at 30 fps in one director function
// (FFWorldDirector 0x53FAC0) that is called once per host frame. Logic and
// rendering are interleaved inside it, so instead of splitting the director
// we keep it running every host frame and gate the individual logic calls
// (pad polling, movement, camera, docking, encounter rolls, model animation
// stepping) to 30 Hz logic ticks through the host's hook_call (FFNx replace_call) on the director's
// call sites. On the host frames in between, player position/facing and the
// camera state are replaced by values interpolated between the last two logic
// ticks before the world is drawn.

#include "interp.h"
#include "../common/tick.h"
#include "../common/chara_pose.h"
#include "../common/play_clock.h"
#include "../common/ui_pace.h"

#include "../host.h"

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <cmath>
#include <unordered_map>

namespace ff8_world
{
	namespace
	{
		constexpr int LOGIC_FPS = 30;

		// Engine functions (FF8 2000 v1.2 / Steam 2013)
		constexpr uint32_t FN_HEAD = 0x559240;                // void (int)            pad double-buffer + global tick
		constexpr uint32_t FN_HEAD2 = 0x559170;               // void ()
		constexpr uint32_t FN_INPUT = 0x5573C0;               // void (short*)
		constexpr uint32_t FN_INPUT_LOCKED = 0x559140;        // void (short*, char)
		constexpr uint32_t FN_VEHICLE_POSITION = 0x548270;    // void (char*, int*, void*)
		constexpr uint32_t FN_MOVEMENT = 0x557A90;            // void (int*, short*, short*, void*)
		constexpr uint32_t FN_FACING_WRAP = 0x558950;         // void (void*, short*)
		constexpr uint32_t FN_CAMERA = 0x552AD0;              // void (short*, void*, void*, int)  builds view matrix + GTE regs
		constexpr uint32_t FN_DOCKING = 0x545480;             // uint (char*)
		constexpr uint32_t FN_ENCOUNTER = 0x541C80;           // int (short*)
		constexpr uint32_t FN_JUMBO = 0x54E730;               // int (short*)
		constexpr uint32_t FN_STEPPER = 0x54A2A0;             // uint (int mode, int slot)  advances chara model frame counter
		constexpr uint32_t FN_VEHICLE_SOUND = 0x5484B0;       // uint (char*)  vehicle state/sound selection, writes player pos too
		constexpr uint32_t FN_VEHICLE_SOUND_APPLY = 0x54A230; // void (int)
		constexpr uint32_t FN_INTEGRATE = 0x54B460;           // void ()  vehicle/event position writes (15 stores)
		constexpr uint32_t FN_TERRAIN_FOG = 0x53BB90;         // void ()  terrain render + walk integration (collision-resolved pos written)
		constexpr uint32_t FN_TERRAIN_NO_FOG = 0x53C750;
		// World map skeleton builder: 0x653DA0(p1, p2) decodes one packed pose per instance of the WORLD
		// slot table (0x25030D8, 32 slots), builds the skeleton and submits the model (0x651FB0). Called
		// once per frame through the wrapper 0x651F90 from the model draw 0x5472C0 (after the stepper).
		// (The field engine's twin 0x533CD0 / table 0x1DCB340 is never called on the world map.)
		constexpr uint32_t FN_WORLD_ANIMATE = 0x651F90;  // void (int, int) wrapper: 0x653DA0(p1 + 4, p2)
		constexpr uint32_t CALL_SITE_WORLD_ANIMATE = 0x547A8C;  // its only call site (in 0x5472C0)
		uint8_t** const WORLD_INSTANCES = (uint8_t**)0x25030D8; // 32 slots (0x25030D8..0x2503158)

		// Call sites inside FFWorldDirector (0x53FAC0) and the model draw routine (0x5472C0)
		constexpr uint32_t CALL_SITE_HEAD[3] = {0x53FADE, 0x540607, 0x540622};
		constexpr uint32_t CALL_SITE_HEAD2 = 0x53FAE6;
		constexpr uint32_t CALL_SITE_INPUT = 0x53FB74;
		constexpr uint32_t CALL_SITE_INPUT_LOCKED = 0x53FB84;
		constexpr uint32_t CALL_SITE_VEHICLE_POSITION = 0x53FB9B;
		constexpr uint32_t CALL_SITE_MOVEMENT = 0x53FBB4;
		constexpr uint32_t CALL_SITE_FACING_WRAP = 0x53FBC3;
		constexpr uint32_t CALL_SITE_CAMERA = 0x53FBD8;
		constexpr uint32_t CALL_SITE_DOCKING = 0x53FC08;
		constexpr uint32_t CALL_SITE_ENCOUNTER = 0x53FFA6;
		constexpr uint32_t CALL_SITE_JUMBO = 0x53FFB6;
		constexpr uint32_t CALL_SITE_STEPPER[4] = {0x547758, 0x547771, 0x547787, 0x54779D};
		constexpr uint32_t CALL_SITE_VEHICLE_SOUND = 0x540089;
		constexpr uint32_t CALL_SITE_VEHICLE_SOUND_APPLY = 0x54008F;
		constexpr uint32_t CALL_SITE_INTEGRATE = 0x540097;
		constexpr uint32_t CALL_SITE_TERRAIN_FOG = 0x53FD94;
		constexpr uint32_t CALL_SITE_TERRAIN_NO_FOG = 0x53FD9B;

		// Texture animation (wmset sections 16 and 40), both stepped from inside the head 0x559240:
		//   0x554940  image-frame animation: phase 0x204DAAC += 0x20403A0 (mod 0x180), per descriptor
		//             (list 0x2040390) derives a frame index and re-uploads 256x1 strips only when it
		//             changes -- the moving sea/waterfall pixels.
		//   0x554BC0  CLUT animation: phase 0x204DAB0 += 0x20403A0 (mod 0x180), rewrites the palette of
		//             the animated TIMs (list built by 0x554AA0 at 0x204A480, last frame 0x204B378).
		// Both are pure phase-advance + conditional VRAM write, so skipping a call simply leaves the last
		// uploaded frame on screen; gating them to the logic tick restores the original speed.
		constexpr uint32_t FN_TEXTURE_ANIM = 0x554940;   // void ()
		constexpr uint32_t FN_CLUT_ANIM = 0x554BC0;  // void (void*)
		constexpr uint32_t CALL_SITE_TEXTURE_ANIM = 0x55947D;
		constexpr uint32_t CALL_SITE_CLUT_ANIM = 0x55949B;
		// Play clock 0x4701B0 (play time + event countdown, one 1/60 s step per call), called twice per
		// frame by the world mode loop 0x53F0F0 = 60 steps/s at the native 30 fps. See play_clock.h.
		constexpr uint32_t CALL_SITE_PLAY_CLOCK[2] = { 0x53F20C, 0x53F211 };

		// v23: per-call state of the director that ran at host rate. Every one of these is called once per director pass, i.e. per HOST frame.
		// Message windows: the director's per-frame 0x49A970(0x20403A0) at 0x5405BE, see ui_pace.h. (The
		// 0x49A970(2) calls at 0x53FCD5 / 0x53FF3D / 0x540052 / 0x54016D / 0x540210 / 0x54028A are one-shot
		// exit paths and stay as they are.)
		constexpr uint32_t CALL_SITE_WINDOW_STEP = 0x5405BE;
		// Entry darken: 0x203FD54 = 0x18 (8 after exit kind 4) frames of World_DrawScreenDarken 0x542AE0
		// (site 0x53FB3B), decremented by the director right after the call (0x53FB48); input stays locked
		// and events / encounters are off until it reaches 0.
		constexpr uint32_t FN_DARKEN = 0x542AE0;       // void (char level)
		constexpr uint32_t CALL_SITE_DARKEN = 0x53FB3B;
		int32_t* const DARKEN_FRAMES = (int32_t*)0x203FD54;
		// Exit fade: while 0x203ED2C != 0 (and the exit kind 0x2036B4C != 3, battle) the director only draws
		// the fade 0x559190(0x2040078), runs the head (site CALL_SITE_HEAD[2] = 0x540622) and adds 0x10 to 0x2040078
		// (0x54062F) until it reaches 0x100: 16 frames = 0.53 s natively.
		int32_t* const EXIT_FADE = (int32_t*)0x2040078;
		// World_UpdateCarFuelConsumption 0x54FDA0 (site 0x53FE35): fuel counter +0x70 += speed 0x20409F0 per
		// call, plus the 30-frame close timer 0x2045C78 of the "out of fuel" window.
		constexpr uint32_t FN_CAR_FUEL = 0x54FDA0;     // char* (int)
		constexpr uint32_t CALL_SITE_CAR_FUEL = 0x53FE35;
		// 0x54DCF0 (site 0x53FCF2): the called-chocobo state machine (0x2045C48 = 1..5): path search (state 2)
		// and the walk of object 0xC76644 toward the player, 0x30 units per call (state 4). Pure logic,
		// returns 0; the object is drawn by 0x5472C0.
		constexpr uint32_t FN_CHOCOBO_PATH = 0x54DCF0; // int ()
		constexpr uint32_t CALL_SITE_CHOCOBO_PATH = 0x53FCF2;
		// World_UpdateFollowerTrail 0x54DAD0 (site 0x53FE1A): pushes the player (or chocobo) position into
		// a 16-entry ring whenever it changed (ring 0x2043148 x 16 B, write index 0x2043BC0, last position
		// 0x2043C68..0x2043C77) and steps the follower index 0x2044006 toward write - 3, then places object
		// 0xC76648 at that entry. At host rate the ring filled with display positions every frame, so the
		// follower trailed 3 host frames behind instead of 3 ticks. The object placement must still run
		// every frame (0x548300 moves the same object onto the player just before), so non-tick calls run
		// on a copy of the ring state.
		constexpr uint32_t FN_FOLLOWER = 0x54DAD0;     // void ()
		constexpr uint32_t CALL_SITE_FOLLOWER = 0x53FE1A;
		struct MemoryBlock { uint32_t address; uint32_t size; };
		constexpr MemoryBlock FOLLOWER_STATE[4] = { { 0x2043148, 0x100 }, { 0x2043BC0, 4 }, { 0x2043C68, 0x10 }, { 0x2044006, 2 } };
		// Vehicle / terrain effect particles: spawner World_SpawnVehicleTerrainEffects 0x550070 (site
		// 0x53FD38, RNG + 0x5562B0 per call) and World_UpdateEffectParticles 0x555B80 (site 0x53FD3D:
		// integrate + age + draw 64 records of 0x30 bytes at 0x204A540..0x204B140 per call).
		constexpr uint32_t FN_FX_SPAWN = 0x550070;     // void ()
		constexpr uint32_t CALL_SITE_FX_SPAWN = 0x53FD38;
		constexpr uint32_t FN_FX_UPDATE = 0x555B80;    // void ()
		constexpr uint32_t CALL_SITE_FX_UPDATE = 0x53FD3D;
		uint8_t* const FX_PARTICLES = (uint8_t*)0x204A540;
		constexpr uint32_t FX_PARTICLES_SIZE = 0xC00;
		// World_UpdateFullMapScreen 0x543CB0 (site 0x540515, minimap mode 3 = full map): moves the cursor
		// 0x2040A24 / 0x2040A28 by 1..3 per call while a direction is held, then draws it.
		constexpr uint32_t FN_FULL_MAP = 0x543CB0;     // void (int, int, uint)
		constexpr uint32_t CALL_SITE_FULL_MAP = 0x540515;
		int32_t* const FULL_MAP_CURSOR_X = (int32_t*)0x2040A24;
		int32_t* const FULL_MAP_CURSOR_Y = (int32_t*)0x2040A28;
		// Pad double buffer written by the head every host frame: index 0x20409BC (flipped first), slots
		// 0x203FDE8[2] (held port 0 remapped | port 1 << 16). Gated consumers compute edges as
		// (prev ^ cur) & cur: the input 0x5573C0 (0x100 minimap cycle, 0x10 in state 0x0D) and the terrain
		// routines (0x8000 / 0x2000).
		int16_t* const PAD_INDEX = (int16_t*)0x20409BC;
		uint32_t* const PAD_SLOTS = (uint32_t*)0x203FDE8;

		// ff8_world_interp_debug bits. 1 = no camera display pass, 2 = no position blend, 4 = no pose
		// blend (literals below, pre-v21); the v21 bits are named.
		constexpr long DBG_PLAY_CLOCK_OFF = 8;      // play clock at host rate again (pre-v21: 4.8x fast at 144 Hz)
		constexpr long DBG_ALPHA_LEAD = 16;         // pre-v21 tick_alpha = (tick_accumulator + dt) / P (one host frame lead)
		constexpr long DBG_PAD_ACCUMULATE_OFF = 32; // pre-v23: gated consumers read the live pad double buffer (taps lost)
		constexpr long DBG_WINDOW_OFF = 64;         // pre-v23 message windows / rumble at host rate
		constexpr long DBG_FADE_OFF = 128;          // pre-v23 entry darken and exit fade at host rate
		constexpr long DBG_OBJECTS_OFF = 256;       // pre-v23 car fuel, called chocobo, follower trail at host rate
		constexpr long DBG_FX_OFF = 512;            // pre-v23 vehicle / terrain particles at host rate
		constexpr long DBG_FULL_MAP_OFF = 1024;     // pre-v23 full-map cursor at host rate

		// Globals
		int32_t* const PLAYER_POSITION = (int32_t*)0x203EE80;       // x, y, z
		int16_t* const PLAYER_LOCAL_POSITION = (int16_t*)0x203FE48; // 8 shorts: sub-position x/y/z (+0..+4), +8 ?, +0xA facing yaw (4096/turn), +0xC/+0xE ?
		int16_t* const CAMERA_PARAMS = (int16_t*)0x203ECF8;         // 7 shorts: y off, x off, distance, ?, tangent Y, tangent Z, root rot
		int16_t* const CAMERA_ROTATION = (int16_t*)0x2040968;       // 3 shorts (camera orientation state)
		int32_t* const CAMERA_LOOK_AT = (int32_t*)0x203FCF8;        // smoothed look-at x, y, z
		int32_t* const CAMERA_SCRIPTED = (int32_t*)0x203FD5C;       // nonzero: scripted camera branch in 0x552AD0 (per-call state)
		int32_t* const CAMERA_HEIGHT_LOCK = (int32_t*)0x2045C3C;    // nonzero: look-at height not filtered
		uint32_t* const WORLD_TICK = (uint32_t*)0x2040088;          // global tick used by wmset object animations
		uint32_t* const WORLD_TICK_PREVIOUS = (uint32_t*)0x2040084;
		int32_t* const WORLD_TICK_DELTA = (int32_t*)0x203FE28;
		uint32_t* const STEPPER_SAVED = (uint32_t*)0x20409F8;
		uint8_t** const MODEL_SLOTS = (uint8_t**)0x25030D8;         // world chara model instance pointers
		int32_t* const MOVEMENT_SPEED = (int32_t*)0x20409E8;        // current movement speed (movement -> terrain integration)
		constexpr uint32_t INSTANCE_FRAME_COUNTER = 0x52;

		typedef void (__cdecl *head_fn)(int);
		typedef void (__cdecl *head2_fn)();
		typedef void (__cdecl *input_fn)(int16_t*);
		typedef void (__cdecl *input_locked_fn)(int16_t*, char);
		typedef void (__cdecl *vehicle_position_fn)(char*, int32_t*, void*);
		typedef void (__cdecl *movement_fn)(int32_t*, int16_t*, int16_t*, void*);
		typedef void (__cdecl *facing_wrap_fn)(void*, int16_t*);
		typedef void (__cdecl *camera_fn)(int16_t*, void*, void*, int);
		typedef uint32_t (__cdecl *docking_fn)(char*);
		typedef int (__cdecl *encounter_fn)(int16_t*);
		typedef uint32_t (__cdecl *stepper_fn)(int, int);
		typedef uint32_t (__cdecl *vehicle_sound_fn)(char*);
		typedef void (__cdecl *vehicle_sound_apply_fn)(int);
		typedef void (__cdecl *integrate_fn)();
		typedef void (__cdecl *terrain_fn)();
		typedef void (__cdecl *world_animate_fn)(int, int);
		typedef void (__cdecl *texture_anim_fn)();
		typedef void (__cdecl *clut_anim_fn)(void*);

		head_fn original_head = (head_fn)FN_HEAD;
		head2_fn original_head2 = (head2_fn)FN_HEAD2;
		input_fn original_input = (input_fn)FN_INPUT;
		input_locked_fn original_input_locked = (input_locked_fn)FN_INPUT_LOCKED;
		vehicle_position_fn original_vehicle_position = (vehicle_position_fn)FN_VEHICLE_POSITION;
		movement_fn original_movement = (movement_fn)FN_MOVEMENT;
		facing_wrap_fn original_facing_wrap = (facing_wrap_fn)FN_FACING_WRAP;
		camera_fn original_camera = (camera_fn)FN_CAMERA;
		docking_fn original_docking = (docking_fn)FN_DOCKING;
		encounter_fn original_encounter = (encounter_fn)FN_ENCOUNTER;
		encounter_fn original_jumbo = (encounter_fn)FN_JUMBO;
		stepper_fn original_stepper = (stepper_fn)FN_STEPPER;
		vehicle_sound_fn original_vehicle_sound = (vehicle_sound_fn)FN_VEHICLE_SOUND;
		vehicle_sound_apply_fn original_vehicle_sound_apply = (vehicle_sound_apply_fn)FN_VEHICLE_SOUND_APPLY;
		integrate_fn original_integrate = (integrate_fn)FN_INTEGRATE;
		terrain_fn original_terrain_fog = (terrain_fn)FN_TERRAIN_FOG;
		terrain_fn original_terrain_no_fog = (terrain_fn)FN_TERRAIN_NO_FOG;
		world_animate_fn original_world_animate = (world_animate_fn)FN_WORLD_ANIMATE;
		texture_anim_fn original_texture_anim = (texture_anim_fn)FN_TEXTURE_ANIM;
		clut_anim_fn original_clut_anim = (clut_anim_fn)FN_CLUT_ANIM;

		int world_fps = 60;
		bool installed = false;
		int last_mode_id = -1;

		// Tick state (same accumulator scheme as the battle module)
		uint32_t tick_updated_frame = 0xFFFFFFFF;
		double tick_accumulator = 0.0;
		bool is_logic_tick = true;
		float tick_alpha = 1.0f;
		uint32_t logic_tick_count = 0;

		constexpr double LOGIC_TICK_SECONDS = 1.0 / double(LOGIC_FPS);

		// Wall-clock gate: ff8_world_fps only caps the render rate, the logic rate must not depend on it.
		void update_tick()
		{
			if (tick_updated_frame == ff8_interp::frame_counter()) return;
			tick_updated_frame = ff8_interp::frame_counter();
			double dt = ff8_tick::frame_delta();
			tick_accumulator += dt;
			is_logic_tick = tick_accumulator >= LOGIC_TICK_SECONDS;
			if (is_logic_tick)
			{
				tick_accumulator -= LOGIC_TICK_SECONDS;
				if (tick_accumulator > LOGIC_TICK_SECONDS) tick_accumulator = LOGIC_TICK_SECONDS; // after a stall, catch up over the next frames
			}
			// v21: the time since the last tick, tick_accumulator / P. `tick_accumulator` already holds this frame's dt, so
			// the pre-v21 (tick_accumulator + dt) / P counted it twice: camera, facing and pose ran one host frame
			// ahead of the position (display_position() always used tick_accumulator / P) and alpha pinned to 1 on the
			// last frame before every tick, an uneven step once per tick. DBG_ALPHA_LEAD restores it.
			tick_alpha = float(((ff8_interp::cfg().world_debug & DBG_ALPHA_LEAD) ? tick_accumulator + dt : tick_accumulator) / LOGIC_TICK_SECONDS);
			if (tick_alpha > 1.0f) tick_alpha = 1.0f;
			if (tick_alpha < 0.0f) tick_alpha = 0.0f;
		}

		struct CameraState
		{
			int16_t params[7];
			int16_t rotation[3];
			int32_t look_at[3];
		};
		struct PositionState
		{
			int32_t position[3];
			int16_t local[8];
		};
		CameraState camera_previous, camera_next;
		PositionState position_previous, position_next;
		bool camera_valid = false;
		bool position_valid = false;
		uint32_t position_tick = 0xFFFFFFFF; // logic tick at which position_next was refreshed
		PositionState logic_position;        // logic position after this tick's movement (restored before terrain integration)
		uint32_t last_docking_result = 0;
		uint32_t last_vehicle_sound_result = 0xFFFFFFFF;

		void capture_camera(CameraState& state)
		{
			memcpy(state.params, CAMERA_PARAMS, sizeof(state.params));
			memcpy(state.rotation, CAMERA_ROTATION, sizeof(state.rotation));
			memcpy(state.look_at, CAMERA_LOOK_AT, sizeof(state.look_at));
		}
		void write_camera(const CameraState& state)
		{
			memcpy(CAMERA_PARAMS, state.params, sizeof(state.params));
			memcpy(CAMERA_ROTATION, state.rotation, sizeof(state.rotation));
			memcpy(CAMERA_LOOK_AT, state.look_at, sizeof(state.look_at));
		}
		void capture_position(PositionState& state)
		{
			memcpy(state.position, PLAYER_POSITION, sizeof(state.position));
			memcpy(state.local, PLAYER_LOCAL_POSITION, sizeof(state.local));
		}
		void write_position(const PositionState& state)
		{
			memcpy(PLAYER_POSITION, state.position, sizeof(state.position));
			memcpy(PLAYER_LOCAL_POSITION, state.local, sizeof(state.local));
		}

		inline int32_t lerp_i32(int32_t a, int32_t b, float t)
		{
			double offset = double(b - a) * t;
			return a + (int32_t)(offset + (offset >= 0 ? 0.5 : -0.5));
		}
		inline int16_t lerp_angle(int16_t a, int16_t b, float t)
		{
			int shortest_delta = ((int(b) - int(a) + 2048) & 4095) - 2048;
			float offset = shortest_delta * t;
			return (int16_t)(int(a) + (int)(offset + (offset >= 0.0f ? 0.5f : -0.5f)));
		}
		inline int16_t lerp_i16(int16_t a, int16_t b, float t)
		{
			float offset = (int(b) - int(a)) * t;
			return (int16_t)(int(a) + (int)(offset + (offset >= 0.0f ? 0.5f : -0.5f)));
		}

		CameraState blend_camera(const CameraState& from, const CameraState& to, float t)
		{
			CameraState blended;
			for (int k = 0; k < 4; k++)
				blended.params[k] = lerp_i16(from.params[k], to.params[k], t);
			for (int k = 4; k < 7; k++)
				blended.params[k] = lerp_angle(from.params[k], to.params[k], t);
			for (int k = 0; k < 3; k++)
			{
				blended.rotation[k] = lerp_i16(from.rotation[k], to.rotation[k], t); // segment-unit offsets, not angles
				blended.look_at[k] = lerp_i32(from.look_at[k], to.look_at[k], t);
			}
			return blended;
		}
		// t may exceed 1 (extrapolation on tick frames before the position snapshot is refreshed)
		PositionState blend_position(const PositionState& from, const PositionState& to, float t)
		{
			PositionState blended;
			for (int k = 0; k < 3; k++)
				blended.position[k] = lerp_i32(from.position[k], to.position[k], t);
			for (int k = 0; k < 8; k++)
				blended.local[k] = (k == 5) ? lerp_angle(from.local[k], to.local[k], t) : lerp_i16(from.local[k], to.local[k], t);
			return blended;
		}

		// Player local position (0x203FE48) and look-at offsets (0x2040968) are relative to the
		// player's map segment (2048 units). When the player crosses a segment edge the engine
		// re-bases both by 2048 on the next tick (before the camera runs). Express `value` in `reference`'s
		// frame so the blend stays continuous; false = a real jump (teleport).
		inline bool rebase_local(int16_t& value, int16_t reference)
		{
			int delta = int(reference) - int(value);
			if (delta > 0x400) value = (int16_t)(int(value) + 2048);
			else if (delta < -0x400) value = (int16_t)(int(value) - 2048);
			delta = int(reference) - int(value);
			return delta <= 0x400 && delta >= -0x400;
		}
		// `previous` is modified (re-based). true = teleport (caller snaps).
		bool camera_teleported(CameraState& previous, const CameraState& current)
		{
			bool teleported = false;
			for (int k = 0; k < 3; k++)
			{
				int32_t look_delta = current.look_at[k] - previous.look_at[k];
				if (look_delta > 0x8000 || look_delta < -0x8000) teleported = true;
				if (!rebase_local(previous.rotation[k], current.rotation[k])) teleported = true;
			}
			return teleported;
		}
		// The map wraps around (position jumps by 0x40000 / 0x30000): treat big deltas as teleports.
		bool position_teleported(PositionState& previous, const PositionState& current)
		{
			bool teleported = false;
			for (int k = 0; k < 3; k++)
			{
				int32_t position_delta = current.position[k] - previous.position[k];
				if (position_delta > 0x8000 || position_delta < -0x8000) teleported = true;
				if (!rebase_local(previous.local[k], current.local[k])) teleported = true;
			}
			return teleported;
		}

		// Position shown this host frame: PREDICTED forward from the latest logic position by the
		// fraction of the tick elapsed (tick_fraction = 0 on the tick frame). Tick frames therefore show the
		// exact logic position, which is where the terrain renderer draws the ground after
		// integrating; interpolating (lagging) instead put the ground a step ahead of the model
		// once per tick. Before this tick's snapshot exists the previous motion is carried one
		// tick further (predicted P_k).
		// Segment frame. The camera expresses its offsets (0x2040968) relative to the segment of the
		// player's i32 position at the time it runs (0x552AD0: look-at local + (lookseg - playerseg)
		// * 2048), and the terrain / object renderers place segments relative to the segment of the
		// i32 position at THEIR call time. Vanilla never changes segment between the camera and the
		// draws. Our predicted i32 position can cross a segment edge mid-tick, so its segment index is
		// pinned to this tick's logic segment (logic_position); the fine placement comes from the camera
		// translation and the local block, never from the i32 position, so the clamp is invisible.
		inline int32_t segment_origin_x(int32_t x) { return x - ((x + 0x60000) & 0x7FF); }
		inline int32_t segment_origin_y(int32_t y)
		{
			int wrapped_y = (y + 0x48000) % 0x30000;
			if (wrapped_y < 0) wrapped_y += 0x30000;
			return y - (wrapped_y & 0x7FF);
		}
		inline void clamp_to_segment(PositionState& shown)
		{
			int32_t origin_x = segment_origin_x(logic_position.position[0]);
			int32_t origin_y = segment_origin_y(logic_position.position[1]);
			if (shown.position[0] < origin_x)
				shown.position[0] = origin_x;
			else if (shown.position[0] > origin_x + 0x7FF)
				shown.position[0] = origin_x + 0x7FF;
			if (shown.position[1] < origin_y)
				shown.position[1] = origin_y;
			else if (shown.position[1] > origin_y + 0x7FF)
				shown.position[1] = origin_y + 0x7FF;
		}

		PositionState display_position()
		{
			// Same time base as tick_alpha since v21 (camera, facing and position agree); unclamped here,
			// but update_tick() keeps tick_accumulator <= LOGIC_TICK_SECONDS.
			float tick_fraction = float(tick_accumulator / LOGIC_TICK_SECONDS);
			PositionState shown;
			if (position_tick != logic_tick_count)
			{
				// Tick frame before this tick's integration: start from this tick's logic state
				// (already re-based by the wrap routine, like the camera) plus the predicted step.
				shown = logic_position;
				float prediction_t = 1.0f + tick_fraction;
				for (int k = 0; k < 3; k++)
				{
					shown.position[k] += lerp_i32(0, position_next.position[k] - position_previous.position[k], prediction_t);
					const int16_t local_step = (int16_t)(int(position_next.local[k]) - int(position_previous.local[k]));
					shown.local[k] = (int16_t)(int(shown.local[k]) + lerp_i16(0, local_step, prediction_t));
				}
				shown.local[5] = lerp_angle(position_next.local[5], logic_position.local[5], tick_alpha);
			}
			else if (memcmp(&position_previous, &position_next, sizeof(PositionState)) == 0)
				shown = position_next;
			else
			{
				float prediction_t = 1.0f + tick_fraction;
				shown = position_next; // unknown local fields (+6, +8, +0xC, +0xE) stay at the logic value
				for (int k = 0; k < 3; k++)
				{
					shown.position[k] = lerp_i32(position_previous.position[k], position_next.position[k], prediction_t);
					shown.local[k] = lerp_i16(position_previous.local[k], position_next.local[k], prediction_t);
				}
				// Facing turns fast; predicting it overshoots for a frame, so it is interpolated (one tick lag).
				shown.local[5] = lerp_angle(position_previous.local[5], position_next.local[5], tick_alpha);
			}
			clamp_to_segment(shown);
			return shown;
		}


		// ---- hooks -------------------------------------------------------------------------

		// The head polls the pad AND talks to the graphics system layer (0x45B620/0x45BEE0/0x45D610),
		// so it must run every host frame; only the global tick it derives is remapped to logic ticks
		// (wmset object animations index their frames by it).
		// v23 pad accumulation (see PAD_SLOTS). The head flips the double buffer every HOST frame, so an
		// edge lives for one host frame and the gated consumers (logic ticks only) saw ~21% of the taps.
		// Every host frame's word is OR-ed into pad_accumulated; on a tick it is published as published_current, with the
		// previous tick's published_current as published_previous. During the gated calls of a tick frame the two slots hold
		// (published_previous, published_current), so (prev ^ cur) & cur reports every press since the previous tick exactly
		// once (a tap shorter than a tick included); the live words are put back right after.
		uint32_t pad_accumulated = 0;
		uint32_t published_previous = 0;
		uint32_t published_current = 0;
		bool published_valid = false;
		void accumulate_pad()
		{
			const int current_slot = *PAD_INDEX & 1;
			const uint32_t current_word = PAD_SLOTS[current_slot];
			if (!published_valid)
			{
				// First tick after a reset: take the engine's own pair so a held button is no new edge.
				pad_accumulated = 0;
				if (is_logic_tick)
				{
					published_previous = PAD_SLOTS[current_slot ^ 1];
					published_current = current_word;
					published_valid = true;
				}
				return;
			}
			pad_accumulated |= current_word;
			if (is_logic_tick)
			{
				published_previous = published_current;
				published_current = pad_accumulated;
				pad_accumulated = 0;
			}
		}
		struct PublishedPadSwap
		{
			uint32_t live_slots[2];
			bool swapped = false;
			PublishedPadSwap()
			{
				if (!is_logic_tick || !published_valid || (ff8_interp::cfg().world_debug & DBG_PAD_ACCUMULATE_OFF)) return;
				const int current_slot = *PAD_INDEX & 1;
				live_slots[0] = PAD_SLOTS[0];
				live_slots[1] = PAD_SLOTS[1];
				PAD_SLOTS[current_slot] = published_current;
				PAD_SLOTS[current_slot ^ 1] = published_previous;
				swapped = true;
			}
			~PublishedPadSwap()
			{
				if (!swapped) return;
				PAD_SLOTS[0] = live_slots[0];
				PAD_SLOTS[1] = live_slots[1];
			}
		};

		void __cdecl head_hook(int arg)
		{
			update_tick();
			if (is_logic_tick)
			{
				// Logic continues from exact values, not from the displayed blend.
				if (camera_valid) write_camera(camera_next);
				if (position_valid) write_position(position_next);
			}
			original_head(arg);
			if (is_logic_tick) logic_tick_count++;
			*WORLD_TICK = logic_tick_count;
			*WORLD_TICK_PREVIOUS = logic_tick_count - (is_logic_tick ? 1 : 0);
			*WORLD_TICK_DELTA = is_logic_tick ? 1 : 0;
			accumulate_pad();
		}
		// CALL_SITE_HEAD[2] (0x540622): the head call of the exit-fade branch, right before 0x2040078 += 0x10.
		// Between ticks the step is taken back here, so the 16-step fade lasts its native 0.53 s.
		// v24: NEVER hold the value 0. 0x559190(0x2040078) runs just before this call and, with 0, LINKS
		// the fade quad (0x204DAF0 via 0x45C870, DR_MODE 0x204DB18 via 0x45C7A0) into the world OT
		// (drawbuf + 0x70); every other value only recolours it. The fade frames redraw that OT without
		// clearing it, so a second 0x559190(0) links the same prim again: 0x45C870 stores the OT head
		// (the first link's node, whose prim is this same prim) into the prim tag -> a node/prim cycle ->
		// the OT walk in 0x45D610 never ends (hang, no exception). v23 held 0 on every non-tick frame
		// before the first tick, i.e. ~79% of world -> menu / field exits (D = 0x20 = world menu) hung.
		void __cdecl head_fade_hook(int arg)
		{
			head_hook(arg);
			if (!is_logic_tick && *EXIT_FADE != 0 && !(ff8_interp::cfg().world_debug & DBG_FADE_OFF)) *EXIT_FADE -= 0x10;
		}
		void __cdecl head2_hook() { original_head2(); }
		void __cdecl play_clock_hook() { ff8_play_clock::run(!(ff8_interp::cfg().world_debug & DBG_PLAY_CLOCK_OFF)); }
		// 0x559240 has a fourth caller on the world init path (0x540F54) where head_hook did not run this
		// frame, so refresh the tick here and only gate on the world map.
		void __cdecl tex_anim_hook()
		{
			update_tick();
			if (ff8_interp::game_mode() == ff8_interp::GameMode::World && !is_logic_tick) return;
			original_texture_anim();
		}
		void __cdecl clut_anim_hook(void* arg)
		{
			update_tick();
			if (ff8_interp::game_mode() == ff8_interp::GameMode::World && !is_logic_tick) return;
			original_clut_anim(arg);
		}
		void __cdecl input_hook(int16_t* arg)
		{
			if (is_logic_tick)
			{
				PublishedPadSwap swap;
				original_input(arg);
			}
		}
		void __cdecl input_locked_hook(int16_t* arg1, char arg2) { if (is_logic_tick) original_input_locked(arg1, arg2); }
		void __cdecl vehicle_pos_hook(char* arg1, int32_t* arg2, void* arg3) { if (is_logic_tick) original_vehicle_position(arg1, arg2, arg3); }
		void __cdecl movement_hook(int32_t* arg1, int16_t* arg2, int16_t* arg3, void* arg4) { if (is_logic_tick) original_movement(arg1, arg2, arg3, arg4); }
		void __cdecl facing_wrap_hook(void* arg1, int16_t* arg2) { if (is_logic_tick) original_facing_wrap(arg1, arg2); }

		void __cdecl camera_hook(int16_t* arg1, void* arg2, void* arg3, int arg4)
		{
			if (is_logic_tick)
			{
				original_camera(arg1, arg2, arg3, arg4); // logic step: filters + matrix (uses last tick's position, as in vanilla)
				CameraState current;
				capture_camera(current);
				CameraState previous = camera_next; // re-based into this tick's segment frame by camera_teleported
				if (!camera_valid || camera_teleported(previous, current))
					camera_previous = current;
				else
					camera_previous = previous;
				camera_next = current;
				camera_valid = true;
				capture_position(logic_position); // position after this tick's movement step; terrain integrates from it
			}
			if (!camera_valid) return;
			if (ff8_interp::cfg().world_debug & 1) return; // debug: no camera display pass (camera steps at 30 Hz)
			// Display pass: rebuild the view matrix (GTE registers) from interpolated state. Display
			// values stay in place for the rest of the frame (sky/parallax/terrain read them);
			// head_hook puts the logic state back at the next tick.
			if (*CAMERA_SCRIPTED != 0) return; // scripted camera branch keeps per-call state: leave it at logic rate
			if (position_valid && !(ff8_interp::cfg().world_debug & 2)) write_position(display_position());
			CameraState display = blend_camera(camera_previous, camera_next, tick_alpha);
			// The camera filters the look-at on every call (x/y: (pos + look) / 2, height:
			// (3 look + pos - 256) / 4). Feed it the pre-image so its output is exactly the blended
			// look-at (vanilla lag, interpolated) independent of the (segment-clamped) i32 position.
			display.look_at[0] = 2 * display.look_at[0] - PLAYER_POSITION[0];
			display.look_at[1] = 2 * display.look_at[1] - PLAYER_POSITION[1];
			if (*CAMERA_HEIGHT_LOCK == 0)
			{
				int n = 4 * display.look_at[2] + 0x100 - PLAYER_POSITION[2]; // L = ceil(n/3): 3L in [n, n+2]
				display.look_at[2] = (n >= 0) ? (n + 2) / 3 : -((-n) / 3);
			}
			write_camera(display);
			original_camera(arg1, arg2, arg3, arg4);
		}

		uint32_t __cdecl docking_hook(char* arg)
		{
			if (is_logic_tick) last_docking_result = original_docking(arg);
			return last_docking_result;
		}
		int __cdecl encounter_hook(int16_t* arg) { return is_logic_tick ? original_encounter(arg) : 0; }
		int __cdecl jumbo_hook(int16_t* arg) { return is_logic_tick ? original_jumbo(arg) : 0; }

		// Vehicle state/sound + position block (director 0x540089..0x540097), per logic tick.
		uint32_t __cdecl vehicle_snd_hook(char* arg)
		{
			if (is_logic_tick) last_vehicle_sound_result = original_vehicle_sound(arg);
			return last_vehicle_sound_result;
		}
		void __cdecl vehicle_snd_apply_hook(int arg) { if (is_logic_tick) original_vehicle_sound_apply(arg); }

		// Terrain render = walk integration: on tick frames it must start from the logic position
		// (the camera display pass put the blend there for the model draws).
		// On non-tick frames the terrain must only draw at the displayed position, not step it again
		// (it integrates before drawing, which put the ground one step ahead of the model).
		// Logic outputs of the terrain routine (tile under the player after the integration): slope
		// steer 0x20409EC (movement adds it to the facing), tile pointer 0x20409FC, 0x2040A0C, 0x2040A00,
		// 0x204095C, 0x203FE30, walkability results 0xC75D0C/0xC75D10, 0x2036AD0/0x2036AD4,
		// 0x203FD10/0x203FD14, and bit0 of byte 0x6D of the struct at 0x20403A4. Our non-tick calls run
		// with the predicted display position; their outputs must not reach the logic (the facing
		// wobbled at slope edges), so they are restored to the last tick's values afterwards.
		constexpr uint32_t SLOPE_STEER_ADDRESS = 0x20409EC;
		constexpr uint32_t TERRAIN_STATE_POINTER_ADDRESS = 0x20403A4;
		constexpr uint32_t TERRAIN_STATE_FLAG_OFFSET = 0x6D;
		struct TerrainOutputs
		{
			int16_t slope_steer;
			int32_t values[11];
			uint8_t state_flag;
			bool valid = false;
		};
		TerrainOutputs terrain_outputs;
		const uint32_t TERRAIN_OUTPUT_ADDRESSES[11] = { 0x20409FC, 0x2040A0C, 0x2040A00, 0x204095C, 0x203FE30, 0xC75D0C, 0xC75D10, 0x2036AD0, 0x2036AD4, 0x203FD10, 0x203FD14 };
		void capture_terrain_outputs()
		{
			terrain_outputs.slope_steer = *(int16_t*)SLOPE_STEER_ADDRESS;
			for (int i = 0; i < 11; i++)
				terrain_outputs.values[i] = *(int32_t*)TERRAIN_OUTPUT_ADDRESSES[i];
			uint8_t* terrain_state = *(uint8_t**)TERRAIN_STATE_POINTER_ADDRESS;
			terrain_outputs.state_flag = terrain_state ? terrain_state[TERRAIN_STATE_FLAG_OFFSET] : 0;
			terrain_outputs.valid = true;
		}
		void restore_terrain_outputs()
		{
			if (!terrain_outputs.valid) return;
			*(int16_t*)SLOPE_STEER_ADDRESS = terrain_outputs.slope_steer;
			for (int i = 0; i < 11; i++)
				*(int32_t*)TERRAIN_OUTPUT_ADDRESSES[i] = terrain_outputs.values[i];
			uint8_t* terrain_state = *(uint8_t**)TERRAIN_STATE_POINTER_ADDRESS;
			if (terrain_state) terrain_state[TERRAIN_STATE_FLAG_OFFSET] = terrain_outputs.state_flag;
		}
		void run_terrain(terrain_fn terrain_routine)
		{
			if (is_logic_tick)
			{
				if (camera_valid) write_position(logic_position);
				{
					PublishedPadSwap swap; // its 0x8000 / 0x2000 edges (-> 0x2036AD0) see every press since the last tick
					terrain_routine();
				}
				capture_terrain_outputs();
				return;
			}
			int32_t saved_speed = *MOVEMENT_SPEED;
			*MOVEMENT_SPEED = 0;
			terrain_routine();
			*MOVEMENT_SPEED = saved_speed;
			restore_terrain_outputs();
			if (position_valid && !(ff8_interp::cfg().world_debug & 2)) write_position(display_position()); // undo any residual step
		}
		void __cdecl terrain_fog_hook() { run_terrain(original_terrain_fog); }
		void __cdecl terrain_nofog_hook() { run_terrain(original_terrain_no_fog); }

		void __cdecl integrate_hook()
		{
			if (is_logic_tick)
			{
				original_integrate(); // position is the logic position here (terrain hook restored it before integrating)
				PositionState current;
				capture_position(current);
				PositionState previous = position_next; // re-based into this tick's segment frame by position_teleported
				if (!position_valid || position_teleported(previous, current))
					position_previous = current;
				else
					position_previous = previous;
				position_next = current;
				position_valid = true;
				position_tick = logic_tick_count;
				if (ff8_interp::cfg().trace)
					ff8_interp::log_trace("ff8world f=%u tick=%u pos=(%d,%d,%d) local=(%d,%d,%d) yaw=%d camrot=(%d,%d,%d) look=(%d,%d,%d) cam=(%d,%d,%d,%d,%d,%d,%d)\n",
						ff8_interp::frame_counter(), logic_tick_count, current.position[0], current.position[1], current.position[2], current.local[0], current.local[1], current.local[2], current.local[5],
						camera_next.rotation[0], camera_next.rotation[1], camera_next.rotation[2], camera_next.look_at[0], camera_next.look_at[1], camera_next.look_at[2],
						camera_next.params[0], camera_next.params[1], camera_next.params[2], camera_next.params[3], camera_next.params[4], camera_next.params[5], camera_next.params[6]);
			}
			if (!position_valid) return;
			if (!(ff8_interp::cfg().world_debug & 2)) write_position(display_position());
		}

		// ---- v23: host-rate counters of the director ---------------------------------------------
		typedef void (__cdecl *int_arg_fn)(int);
		typedef void (__cdecl *no_arg_fn)();
		typedef int (__cdecl *int_result_fn)();
		typedef void (__cdecl *full_map_fn)(int, int, uint32_t);
		int_arg_fn original_window_step = (int_arg_fn)ff8_ui_pace::FN_WINDOW_STEP;
		int_arg_fn original_darken = (int_arg_fn)FN_DARKEN;
		int_arg_fn original_car_fuel = (int_arg_fn)FN_CAR_FUEL;
		int_result_fn original_chocobo_path = (int_result_fn)FN_CHOCOBO_PATH;
		no_arg_fn original_follower = (no_arg_fn)FN_FOLLOWER;
		no_arg_fn original_fx_spawn = (no_arg_fn)FN_FX_SPAWN;
		no_arg_fn original_fx_update = (no_arg_fn)FN_FX_UPDATE;
		full_map_fn original_full_map = (full_map_fn)FN_FULL_MAP;
		inline bool debug_bit_set(long bit) { return (ff8_interp::cfg().world_debug & bit) != 0; }

		// native_steps = 0x20403A0 (2, or 3 in state 0x32) window steps per native 30 fps frame.
		void __cdecl window_step_hook(int native_steps) { ff8_ui_pace::window_step(native_steps, 30.0 * native_steps, !debug_bit_set(DBG_WINDOW_OFF), true); }
		// The director decrements 0x203FD54 right after this call; between ticks it is given back.
		void __cdecl darken_hook(int level)
		{
			original_darken(level);
			if (!is_logic_tick && !debug_bit_set(DBG_FADE_OFF)) (*DARKEN_FRAMES)++;
		}
		void __cdecl car_fuel_hook(int arg) { if (is_logic_tick || debug_bit_set(DBG_OBJECTS_OFF)) original_car_fuel(arg); } // result unused
		int __cdecl chocobo_path_hook() { return (is_logic_tick || debug_bit_set(DBG_OBJECTS_OFF)) ? original_chocobo_path() : 0; } // result unused
		void __cdecl follower_hook()
		{
			if (is_logic_tick || debug_bit_set(DBG_OBJECTS_OFF))
			{
				original_follower();
				return;
			}
			uint8_t saved_state[0x100 + 4 + 0x10 + 2];
			uint32_t offset = 0;
			for (const MemoryBlock& block : FOLLOWER_STATE)
			{
				memcpy(saved_state + offset, (void*)block.address, block.size);
				offset += block.size;
			}
			original_follower(); // places the follower object (on the logic ring, one step ahead at most)
			offset = 0;
			for (const MemoryBlock& block : FOLLOWER_STATE)
			{
				memcpy((void*)block.address, saved_state + offset, block.size);
				offset += block.size;
			}
		}
		void __cdecl fx_spawn_hook() { if (is_logic_tick || debug_bit_set(DBG_FX_OFF)) original_fx_spawn(); }
		// Integration and drawing are one loop: between ticks it runs on a copy of the records (drawn one
		// step ahead), so the particles age and move at the native 30 Hz and are drawn every frame.
		uint8_t fx_particles_saved[FX_PARTICLES_SIZE];
		void __cdecl fx_update_hook()
		{
			if (is_logic_tick || debug_bit_set(DBG_FX_OFF))
			{
				original_fx_update();
				return;
			}
			memcpy(fx_particles_saved, FX_PARTICLES, FX_PARTICLES_SIZE);
			original_fx_update();
			memcpy(FX_PARTICLES, fx_particles_saved, FX_PARTICLES_SIZE);
		}
		void __cdecl full_map_hook(int x, int y, uint32_t init)
		{
			if (is_logic_tick || init != 0 || debug_bit_set(DBG_FULL_MAP_OFF))
			{
				original_full_map(x, y, init);
				return;
			}
			const int32_t saved_cursor_x = *FULL_MAP_CURSOR_X;
			const int32_t saved_cursor_y = *FULL_MAP_CURSOR_Y;
			original_full_map(x, y, init);
			*FULL_MAP_CURSOR_X = saved_cursor_x;
			*FULL_MAP_CURSOR_Y = saved_cursor_y;
		}

		// ---- character model pose interpolation (shared module, see ../chara_pose.h) --------------
		ff8_chara::PoseInterp pose_interpolator;

		void __cdecl world_animate_hook(int arg1, int arg2)
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::World || !camera_valid || (ff8_interp::cfg().world_debug & 4))
			{
				original_world_animate(arg1, arg2);
				return;
			}
			update_tick();
			pose_interpolator.animate(original_world_animate, arg1, arg2, is_logic_tick, tick_alpha, logic_tick_count);
		}

		// Model animation stepper: advances the frame counter AND switches clips (walk/idle) with a
		// counter reset. It must run only on logic ticks; on other host frames return the last
		// result so the caller's state stays consistent. (Restoring the counter after a non-tick
		// call corrupted clip switches: the old walk counter landed inside the idle clip.)
		uint32_t stepper_last_result[64] = {};
		uint32_t __cdecl stepper_hook(int mode, int slot)
		{
			int slot_index = (slot >= 0 && slot < 64) ? slot : 0;
			if (is_logic_tick) stepper_last_result[slot_index] = original_stepper(mode, slot);
			return stepper_last_result[slot_index];
		}
	}

	void interp_hook_init()
	{
		if (!ff8_interp::cfg().world_enabled) return;
		if (!ff8_interp::is_us_exe())
		{
			ff8_interp::log_warning("%s: world map interpolation only verified on the US executable, skipping.\n", __func__);
			return;
		}
		if (ff8_interp::cfg().world_fps < 30 || ff8_interp::cfg().world_fps > 1000)
		{
			ff8_interp::log_warning("%s: ff8_world_fps out of range (30..1000), got %ld. Disabled.\n", __func__, ff8_interp::cfg().world_fps);
			return;
		}
		world_fps = (int)ff8_interp::cfg().world_fps;

		for (uint32_t call_site : CALL_SITE_HEAD)
			ff8_interp::hook_call(call_site, (void*)(call_site == CALL_SITE_HEAD[2] ? head_fade_hook : head_hook));
		ff8_interp::hook_call(CALL_SITE_HEAD2, (void*)head2_hook);
		ff8_interp::hook_call(CALL_SITE_INPUT, (void*)input_hook);
		ff8_interp::hook_call(CALL_SITE_INPUT_LOCKED, (void*)input_locked_hook);
		ff8_interp::hook_call(CALL_SITE_VEHICLE_POSITION, (void*)vehicle_pos_hook);
		ff8_interp::hook_call(CALL_SITE_MOVEMENT, (void*)movement_hook);
		ff8_interp::hook_call(CALL_SITE_FACING_WRAP, (void*)facing_wrap_hook);
		ff8_interp::hook_call(CALL_SITE_CAMERA, (void*)camera_hook);
		ff8_interp::hook_call(CALL_SITE_DOCKING, (void*)docking_hook);
		// FFNx itself redirects this call (1.24: to its own function in AF3DN.P). Chain to whatever the
		// site reached before us, so FFNx keeps its behaviour and it simply runs at the logic rate.
		if (uint32_t previous_target = ff8_interp::hook_call_chain(CALL_SITE_ENCOUNTER, (void*)encounter_hook)) original_encounter = (encounter_fn)previous_target;
		ff8_interp::hook_call(CALL_SITE_JUMBO, (void*)jumbo_hook);
		for (uint32_t call_site : CALL_SITE_STEPPER)
			ff8_interp::hook_call(call_site, (void*)stepper_hook);
		ff8_interp::hook_call(CALL_SITE_VEHICLE_SOUND, (void*)vehicle_snd_hook);
		ff8_interp::hook_call(CALL_SITE_VEHICLE_SOUND_APPLY, (void*)vehicle_snd_apply_hook);
		ff8_interp::hook_call(CALL_SITE_INTEGRATE, (void*)integrate_hook);
		ff8_interp::hook_call(CALL_SITE_TERRAIN_FOG, (void*)terrain_fog_hook);
		ff8_interp::hook_call(CALL_SITE_TERRAIN_NO_FOG, (void*)terrain_nofog_hook);
		pose_interpolator.instance_table = WORLD_INSTANCES;
		ff8_interp::hook_call(CALL_SITE_WORLD_ANIMATE, (void*)world_animate_hook);
		ff8_interp::hook_call(CALL_SITE_TEXTURE_ANIM, (void*)tex_anim_hook);
		ff8_interp::hook_call(CALL_SITE_CLUT_ANIM, (void*)clut_anim_hook);
		int patched_clock_sites = 0;
		for (uint32_t call_site : CALL_SITE_PLAY_CLOCK)
			patched_clock_sites += ff8_play_clock::patch_site(call_site, (void*)play_clock_hook, __func__) ? 1 : 0;
		ff8_ui_pace::install(__func__);
		ff8_interp::hook_call(CALL_SITE_WINDOW_STEP, (void*)window_step_hook);
		ff8_interp::hook_call(CALL_SITE_DARKEN, (void*)darken_hook);
		ff8_interp::hook_call(CALL_SITE_CAR_FUEL, (void*)car_fuel_hook);
		ff8_interp::hook_call(CALL_SITE_CHOCOBO_PATH, (void*)chocobo_path_hook);
		ff8_interp::hook_call(CALL_SITE_FOLLOWER, (void*)follower_hook);
		ff8_interp::hook_call(CALL_SITE_FX_SPAWN, (void*)fx_spawn_hook);
		ff8_interp::hook_call(CALL_SITE_FX_UPDATE, (void*)fx_update_hook);
		ff8_interp::hook_call(CALL_SITE_FULL_MAP, (void*)full_map_hook);
		installed = true;
		ff8_interp::log_info("%s: world map interpolation v24 enabled (world %d fps, logic %d fps, play clock %d/2 sites).\n", __func__, world_fps, LOGIC_FPS, patched_clock_sites);
	}

	bool interp_installed() { return installed; }

	void interp_reset()
	{
		camera_valid = false;
		position_valid = false;
		position_tick = 0xFFFFFFFF;
		tick_updated_frame = 0xFFFFFFFF;
		tick_accumulator = 0.0;
		ff8_tick::clock_reset();
		last_docking_result = 0;
		last_vehicle_sound_result = 0xFFFFFFFF;
		memset(stepper_last_result, 0, sizeof(stepper_last_result));
		pose_interpolator.reset();
		terrain_outputs.valid = false;
		published_valid = false;
		pad_accumulated = 0;
	}

	void interp_on_frame()
	{
		if (!installed) return;
		int mode = (int)ff8_interp::mode_id();
		if (mode != last_mode_id)
		{
			interp_reset();
			last_mode_id = mode;
		}
	}
}
