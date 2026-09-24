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
//
// Engine facts: ~/ff8/notes/engine/worldmap.md

#include "interp.h"
#include "../common/tick.h"
#include "../common/chara_pose.h"
#include "../common/play_clock.h"

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
		constexpr uint32_t FN_HEAD = 0x559240;        // void (int)            pad double-buffer + global tick
		constexpr uint32_t FN_HEAD2 = 0x559170;       // void ()
		constexpr uint32_t FN_INPUT = 0x5573C0;       // void (short*)
		constexpr uint32_t FN_INPUT_LOCKED = 0x559140;// void (short*, char)
		constexpr uint32_t FN_VEHICLE_POS = 0x548270; // void (char*, int*, void*)
		constexpr uint32_t FN_MOVEMENT = 0x557A90;    // void (int*, short*, short*, void*)
		constexpr uint32_t FN_FACING_WRAP = 0x558950; // void (void*, short*)
		constexpr uint32_t FN_CAMERA = 0x552AD0;      // void (short*, void*, void*, int)  builds view matrix + GTE regs
		constexpr uint32_t FN_DOCKING = 0x545480;     // uint (char*)
		constexpr uint32_t FN_ENCOUNTER = 0x541C80;   // int (short*)
		constexpr uint32_t FN_JUMBO = 0x54E730;       // int (short*)
		constexpr uint32_t FN_STEPPER = 0x54A2A0;     // uint (int mode, int slot)  advances chara model frame counter
		constexpr uint32_t FN_VEHICLE_SND = 0x5484B0; // uint (char*)  vehicle state/sound selection, writes player pos too
		constexpr uint32_t FN_VEHICLE_SND_APPLY = 0x54A230; // void (int)
		constexpr uint32_t FN_INTEGRATE = 0x54B460;   // void ()  vehicle/event position writes (15 stores)
		constexpr uint32_t FN_TERRAIN_FOG = 0x53BB90; // void ()  terrain render + walk integration (collision-resolved pos written)
		constexpr uint32_t FN_TERRAIN_NOFOG = 0x53C750;
		// World map skeleton builder: 0x653DA0(p1, p2) decodes one packed pose per instance of the WORLD
		// slot table (0x25030D8, 32 slots), builds the skeleton and submits the model (0x651FB0). Called
		// once per frame through the wrapper 0x651F90 from the model draw 0x5472C0 (after the stepper).
		// (The field engine's twin 0x533CD0 / table 0x1DCB340 is never called on the world map.)
		constexpr uint32_t FN_WORLD_ANIMATE = 0x651F90;  // void (int, int) wrapper: 0x653DA0(p1 + 4, p2)
		constexpr uint32_t CS_WORLD_ANIMATE = 0x547A8C;  // its only call site (in 0x5472C0)
		uint8_t** const WORLD_INSTANCES = (uint8_t**)0x25030D8; // 32 slots (0x25030D8..0x2503158)

		// Call sites inside FFWorldDirector (0x53FAC0) and the model draw routine (0x5472C0)
		constexpr uint32_t CS_HEAD[3] = {0x53FADE, 0x540607, 0x540622};
		constexpr uint32_t CS_HEAD2 = 0x53FAE6;
		constexpr uint32_t CS_INPUT = 0x53FB74;
		constexpr uint32_t CS_INPUT_LOCKED = 0x53FB84;
		constexpr uint32_t CS_VEHICLE_POS = 0x53FB9B;
		constexpr uint32_t CS_MOVEMENT = 0x53FBB4;
		constexpr uint32_t CS_FACING_WRAP = 0x53FBC3;
		constexpr uint32_t CS_CAMERA = 0x53FBD8;
		constexpr uint32_t CS_DOCKING = 0x53FC08;
		constexpr uint32_t CS_ENCOUNTER = 0x53FFA6;
		constexpr uint32_t CS_JUMBO = 0x53FFB6;
		constexpr uint32_t CS_STEPPER[4] = {0x547758, 0x547771, 0x547787, 0x54779D};
		constexpr uint32_t CS_VEHICLE_SND = 0x540089;
		constexpr uint32_t CS_VEHICLE_SND_APPLY = 0x54008F;
		constexpr uint32_t CS_INTEGRATE = 0x540097;
		constexpr uint32_t CS_TERRAIN_FOG = 0x53FD94;
		constexpr uint32_t CS_TERRAIN_NOFOG = 0x53FD9B;

		// Texture animation (wmset sections 16 and 40), both stepped from inside the head 0x559240:
		//   0x554940  image-frame animation: phase 0x204DAAC += 0x20403A0 (mod 0x180), per descriptor
		//             (list 0x2040390) derives a frame index and re-uploads 256x1 strips only when it
		//             changes -- the moving sea/waterfall pixels.
		//   0x554BC0  CLUT animation: phase 0x204DAB0 += 0x20403A0 (mod 0x180), rewrites the palette of
		//             the animated TIMs (list built by 0x554AA0 at 0x204A480, last frame 0x204B378).
		// Both are pure phase-advance + conditional VRAM write, so skipping a call simply leaves the last
		// uploaded frame on screen; gating them to the logic tick restores the original speed.
		constexpr uint32_t FN_TEX_ANIM = 0x554940;   // void ()
		constexpr uint32_t FN_CLUT_ANIM = 0x554BC0;  // void (void*)
		constexpr uint32_t CS_TEX_ANIM = 0x55947D;
		constexpr uint32_t CS_CLUT_ANIM = 0x55949B;
		// Play clock 0x4701B0 (play time + event countdown, one 1/60 s step per call), called twice per
		// frame by the world mode loop 0x53F0F0 = 60 steps/s at the native 30 fps. See play_clock.h.
		constexpr uint32_t CS_PLAY_CLOCK[2] = { 0x53F20C, 0x53F211 };

		// ff8_world_interp_debug bits. 1 = no camera display pass, 2 = no position blend, 4 = no pose
		// blend (literals below, pre-v21); the v21 bits are named.
		constexpr long DBG_PLAY_CLOCK_OFF = 8; // play clock at host rate again (pre-v21: 4.8x fast at 144 Hz)
		constexpr long DBG_ALPHA_LEAD = 16;    // pre-v21 tick_alpha = (tick_acc + dt) / P (one host frame lead)

		// Globals
		int32_t* const PLAYER_POS = (int32_t*)0x203EE80;   // x, y, z
		int16_t* const PLAYER_LOCAL = (int16_t*)0x203FE48; // 8 shorts: sub-position x/y/z (+0..+4), +8 ?, +0xA facing yaw (4096/turn), +0xC/+0xE ?
		int16_t* const CAM_PARAMS = (int16_t*)0x203ECF8;   // 7 shorts: y off, x off, distance, ?, tangent Y, tangent Z, root rot
		int16_t* const CAM_ROT = (int16_t*)0x2040968;      // 3 shorts (camera orientation state)
		int32_t* const CAM_LOOK = (int32_t*)0x203FCF8;     // smoothed look-at x, y, z
		int32_t* const CAM_SCRIPT = (int32_t*)0x203FD5C;      // nonzero: scripted camera branch in 0x552AD0 (per-call state)
		int32_t* const CAM_HEIGHT_LOCK = (int32_t*)0x2045C3C; // nonzero: look-at height not filtered
		uint32_t* const WM_TICK = (uint32_t*)0x2040088;    // global tick used by wmset object animations
		uint32_t* const WM_TICK_PREV = (uint32_t*)0x2040084;
		int32_t* const WM_TICK_DELTA = (int32_t*)0x203FE28;
		uint32_t* const STEPPER_SAVED = (uint32_t*)0x20409F8;
		uint8_t** const MODEL_SLOTS = (uint8_t**)0x25030D8; // world chara model instance pointers
		int32_t* const MOVE_SPEED = (int32_t*)0x20409E8;   // current movement speed (movement -> terrain integration)
		constexpr uint32_t INST_FRAME_COUNTER = 0x52;

		typedef void (__cdecl *head_t)(int);
		typedef void (__cdecl *head2_t)();
		typedef void (__cdecl *input_t)(int16_t*);
		typedef void (__cdecl *input_locked_t)(int16_t*, char);
		typedef void (__cdecl *vehicle_pos_t)(char*, int32_t*, void*);
		typedef void (__cdecl *movement_t)(int32_t*, int16_t*, int16_t*, void*);
		typedef void (__cdecl *facing_wrap_t)(void*, int16_t*);
		typedef void (__cdecl *camera_t)(int16_t*, void*, void*, int);
		typedef uint32_t (__cdecl *docking_t)(char*);
		typedef int (__cdecl *encounter_t)(int16_t*);
		typedef uint32_t (__cdecl *stepper_t)(int, int);
		typedef uint32_t (__cdecl *vehicle_snd_t)(char*);
		typedef void (__cdecl *vehicle_snd_apply_t)(int);
		typedef void (__cdecl *integrate_t)();
		typedef void (__cdecl *terrain_t)();
		typedef void (__cdecl *world_animate_t)(int, int);
		typedef void (__cdecl *tex_anim_t)();
		typedef void (__cdecl *clut_anim_t)(void*);

		head_t orig_head = (head_t)FN_HEAD;
		head2_t orig_head2 = (head2_t)FN_HEAD2;
		input_t orig_input = (input_t)FN_INPUT;
		input_locked_t orig_input_locked = (input_locked_t)FN_INPUT_LOCKED;
		vehicle_pos_t orig_vehicle_pos = (vehicle_pos_t)FN_VEHICLE_POS;
		movement_t orig_movement = (movement_t)FN_MOVEMENT;
		facing_wrap_t orig_facing_wrap = (facing_wrap_t)FN_FACING_WRAP;
		camera_t orig_camera = (camera_t)FN_CAMERA;
		docking_t orig_docking = (docking_t)FN_DOCKING;
		encounter_t orig_encounter = (encounter_t)FN_ENCOUNTER;
		encounter_t orig_jumbo = (encounter_t)FN_JUMBO;
		stepper_t orig_stepper = (stepper_t)FN_STEPPER;
		vehicle_snd_t orig_vehicle_snd = (vehicle_snd_t)FN_VEHICLE_SND;
		vehicle_snd_apply_t orig_vehicle_snd_apply = (vehicle_snd_apply_t)FN_VEHICLE_SND_APPLY;
		integrate_t orig_integrate = (integrate_t)FN_INTEGRATE;
		terrain_t orig_terrain_fog = (terrain_t)FN_TERRAIN_FOG;
		terrain_t orig_terrain_nofog = (terrain_t)FN_TERRAIN_NOFOG;
		world_animate_t orig_world_animate = (world_animate_t)FN_WORLD_ANIMATE;
		tex_anim_t orig_tex_anim = (tex_anim_t)FN_TEX_ANIM;
		clut_anim_t orig_clut_anim = (clut_anim_t)FN_CLUT_ANIM;

		int world_fps = 60;
		bool installed = false;
		int last_mode = -1;

		// Tick state (same accumulator scheme as the battle module)
		uint32_t tick_frame = 0xFFFFFFFF;
		double tick_acc = 0.0;
		bool tick_real = true;
		float tick_alpha = 1.0f;
		uint32_t logic_ticks = 0;

		constexpr double LOGIC_PERIOD = 1.0 / double(LOGIC_FPS);

		// Wall-clock gate: ff8_world_fps only caps the render rate, the logic rate must not depend on it.
		void update_tick()
		{
			if (tick_frame == ff8_interp::frame_counter()) return;
			tick_frame = ff8_interp::frame_counter();
			double dt = ff8_tick::frame_delta();
			tick_acc += dt;
			tick_real = tick_acc >= LOGIC_PERIOD;
			if (tick_real)
			{
				tick_acc -= LOGIC_PERIOD;
				if (tick_acc > LOGIC_PERIOD) tick_acc = LOGIC_PERIOD; // after a stall, catch up over the next frames
			}
			// v21: the time since the last tick, tick_acc / P. `tick_acc` already holds this frame's dt, so
			// the pre-v21 (tick_acc + dt) / P counted it twice: camera, facing and pose ran one host frame
			// ahead of the position (display_pos() always used tick_acc / P) and alpha pinned to 1 on the
			// last frame before every tick, an uneven step once per tick. DBG_ALPHA_LEAD restores it.
			tick_alpha = float(((ff8_interp::cfg().world_debug & DBG_ALPHA_LEAD) ? tick_acc + dt : tick_acc) / LOGIC_PERIOD);
			if (tick_alpha > 1.0f) tick_alpha = 1.0f;
			if (tick_alpha < 0.0f) tick_alpha = 0.0f;
		}

		struct CamState { int16_t cam[7]; int16_t camrot[3]; int32_t look[3]; };
		struct PosState { int32_t pos[3]; int16_t local[8]; };
		CamState cam_prev, cam_next;
		PosState pos_prev, pos_next;
		bool cam_valid = false, pos_valid = false;
		uint32_t pos_tick = 0xFFFFFFFF; // logic tick at which pos_next was refreshed
		PosState pos_logic;             // logic position after this tick's movement (restored before terrain integration)
		uint32_t last_docking = 0;
		uint32_t last_vehicle_snd = 0xFFFFFFFF;

		void capture_cam(CamState& s)
		{
			memcpy(s.cam, CAM_PARAMS, sizeof(s.cam));
			memcpy(s.camrot, CAM_ROT, sizeof(s.camrot));
			memcpy(s.look, CAM_LOOK, sizeof(s.look));
		}
		void write_cam(const CamState& s)
		{
			memcpy(CAM_PARAMS, s.cam, sizeof(s.cam));
			memcpy(CAM_ROT, s.camrot, sizeof(s.camrot));
			memcpy(CAM_LOOK, s.look, sizeof(s.look));
		}
		void capture_pos(PosState& s)
		{
			memcpy(s.pos, PLAYER_POS, sizeof(s.pos));
			memcpy(s.local, PLAYER_LOCAL, sizeof(s.local));
		}
		void write_pos(const PosState& s)
		{
			memcpy(PLAYER_POS, s.pos, sizeof(s.pos));
			memcpy(PLAYER_LOCAL, s.local, sizeof(s.local));
		}

		inline int32_t lerp_i32(int32_t a, int32_t b, float t)
		{
			double v = double(b - a) * t;
			return a + (int32_t)(v + (v >= 0 ? 0.5 : -0.5));
		}
		inline int16_t lerp_angle(int16_t a, int16_t b, float t)
		{
			int d = ((int(b) - int(a) + 2048) & 4095) - 2048;
			float v = d * t;
			return (int16_t)(int(a) + (int)(v + (v >= 0.0f ? 0.5f : -0.5f)));
		}
		inline int16_t lerp_i16(int16_t a, int16_t b, float t)
		{
			float v = (int(b) - int(a)) * t;
			return (int16_t)(int(a) + (int)(v + (v >= 0.0f ? 0.5f : -0.5f)));
		}

		CamState blend_cam(const CamState& a, const CamState& b, float t)
		{
			CamState o;
			for (int k = 0; k < 4; k++) o.cam[k] = lerp_i16(a.cam[k], b.cam[k], t);
			for (int k = 4; k < 7; k++) o.cam[k] = lerp_angle(a.cam[k], b.cam[k], t);
			for (int k = 0; k < 3; k++)
			{
				o.camrot[k] = lerp_i16(a.camrot[k], b.camrot[k], t); // segment-unit offsets, not angles
				o.look[k] = lerp_i32(a.look[k], b.look[k], t);
			}
			return o;
		}
		// t may exceed 1 (extrapolation on tick frames before the position snapshot is refreshed)
		PosState blend_pos(const PosState& a, const PosState& b, float t)
		{
			PosState o;
			for (int k = 0; k < 3; k++) o.pos[k] = lerp_i32(a.pos[k], b.pos[k], t);
			for (int k = 0; k < 8; k++)
				o.local[k] = (k == 5) ? lerp_angle(a.local[k], b.local[k], t) : lerp_i16(a.local[k], b.local[k], t);
			return o;
		}

		// Player local position (0x203FE48) and look-at offsets (0x2040968) are relative to the
		// player's map segment (2048 units). When the player crosses a segment edge the engine
		// re-bases both by 2048 on the next tick (before the camera runs). Express `a` in `b`'s
		// frame so the blend stays continuous; false = a real jump (teleport).
		inline bool rebase_local(int16_t& a, int16_t b)
		{
			int d = int(b) - int(a);
			if (d > 0x400) a = (int16_t)(int(a) + 2048);
			else if (d < -0x400) a = (int16_t)(int(a) - 2048);
			d = int(b) - int(a);
			return d <= 0x400 && d >= -0x400;
		}
		// `a` is modified (re-based). true = teleport (caller snaps).
		bool cam_teleport(CamState& a, const CamState& b)
		{
			bool tp = false;
			for (int k = 0; k < 3; k++)
			{
				int32_t d = b.look[k] - a.look[k];
				if (d > 0x8000 || d < -0x8000) tp = true;
				if (!rebase_local(a.camrot[k], b.camrot[k])) tp = true;
			}
			return tp;
		}
		// The map wraps around (position jumps by 0x40000 / 0x30000): treat big deltas as teleports.
		bool pos_teleport(PosState& a, const PosState& b)
		{
			bool tp = false;
			for (int k = 0; k < 3; k++)
			{
				int32_t d = b.pos[k] - a.pos[k];
				if (d > 0x8000 || d < -0x8000) tp = true;
				if (!rebase_local(a.local[k], b.local[k])) tp = true;
			}
			return tp;
		}

		// Position shown this host frame: PREDICTED forward from the latest logic position by the
		// fraction of the tick elapsed (t_pos = 0 on the tick frame). Tick frames therefore show the
		// exact logic position, which is where the terrain renderer draws the ground after
		// integrating; interpolating (lagging) instead put the ground a step ahead of the model
		// once per tick. Before this tick's snapshot exists the previous motion is carried one
		// tick further (predicted P_k).
		// Segment frame. The camera expresses its offsets (0x2040968) relative to the segment of the
		// player's i32 position at the time it runs (0x552AD0: look-at local + (lookseg - playerseg)
		// * 2048), and the terrain / object renderers place segments relative to the segment of the
		// i32 position at THEIR call time. Vanilla never changes segment between the camera and the
		// draws. Our predicted i32 position can cross a segment edge mid-tick, so its segment index is
		// pinned to this tick's logic segment (pos_logic); the fine placement comes from the camera
		// translation and the local block, never from the i32 position, so the clamp is invisible.
		inline int32_t seg_origin_x(int32_t px) { return px - ((px + 0x60000) & 0x7FF); }
		inline int32_t seg_origin_y(int32_t py)
		{
			int r = (py + 0x48000) % 0x30000;
			if (r < 0) r += 0x30000;
			return py - (r & 0x7FF);
		}
		inline void clamp_to_frame(PosState& o)
		{
			int32_t ox = seg_origin_x(pos_logic.pos[0]), oy = seg_origin_y(pos_logic.pos[1]);
			if (o.pos[0] < ox) o.pos[0] = ox; else if (o.pos[0] > ox + 0x7FF) o.pos[0] = ox + 0x7FF;
			if (o.pos[1] < oy) o.pos[1] = oy; else if (o.pos[1] > oy + 0x7FF) o.pos[1] = oy + 0x7FF;
		}

		PosState display_pos()
		{
			// Same time base as tick_alpha since v21 (camera, facing and position agree); unclamped here,
			// but update_tick() keeps tick_acc <= LOGIC_PERIOD.
			float t_pos = float(tick_acc / LOGIC_PERIOD);
			PosState o;
			if (pos_tick != logic_ticks)
			{
				// Tick frame before this tick's integration: start from this tick's logic state
				// (already re-based by the wrap routine, like the camera) plus the predicted step.
				o = pos_logic;
				float t = 1.0f + t_pos;
				for (int k = 0; k < 3; k++)
				{
					o.pos[k] += lerp_i32(0, pos_next.pos[k] - pos_prev.pos[k], t);
					o.local[k] = (int16_t)(int(o.local[k]) + lerp_i16(0, (int16_t)(int(pos_next.local[k]) - int(pos_prev.local[k])), t));
				}
				o.local[5] = lerp_angle(pos_next.local[5], pos_logic.local[5], tick_alpha);
			}
			else if (memcmp(&pos_prev, &pos_next, sizeof(PosState)) == 0) o = pos_next;
			else
			{
				float t = 1.0f + t_pos;
				o = pos_next; // unknown local fields (+6, +8, +0xC, +0xE) stay at the logic value
				for (int k = 0; k < 3; k++)
				{
					o.pos[k] = lerp_i32(pos_prev.pos[k], pos_next.pos[k], t);
					o.local[k] = lerp_i16(pos_prev.local[k], pos_next.local[k], t);
				}
				// Facing turns fast; predicting it overshoots for a frame, so it is interpolated (one tick lag).
				o.local[5] = lerp_angle(pos_prev.local[5], pos_next.local[5], tick_alpha);
			}
			clamp_to_frame(o);
			return o;
		}


		// ---- hooks -------------------------------------------------------------------------

		// The head polls the pad AND talks to the graphics system layer (0x45B620/0x45BEE0/0x45D610),
		// so it must run every host frame; only the global tick it derives is remapped to logic ticks
		// (wmset object animations index their frames by it).
		void __cdecl head_hook(int a)
		{
			update_tick();
			if (tick_real)
			{
				// Logic continues from exact values, not from the displayed blend.
				if (cam_valid) write_cam(cam_next);
				if (pos_valid) write_pos(pos_next);
			}
			orig_head(a);
			if (tick_real) logic_ticks++;
			*WM_TICK = logic_ticks;
			*WM_TICK_PREV = logic_ticks - (tick_real ? 1 : 0);
			*WM_TICK_DELTA = tick_real ? 1 : 0;
		}
		void __cdecl head2_hook() { orig_head2(); }
		void __cdecl play_clock_hook() { ff8_play_clock::run(!(ff8_interp::cfg().world_debug & DBG_PLAY_CLOCK_OFF)); }
		// 0x559240 has a fourth caller on the world init path (0x540F54) where head_hook did not run this
		// frame, so refresh the tick here and only gate on the world map.
		void __cdecl tex_anim_hook() { update_tick(); if (ff8_interp::game_mode() == ff8_interp::GameMode::World && !tick_real) return; orig_tex_anim(); }
		void __cdecl clut_anim_hook(void* p) { update_tick(); if (ff8_interp::game_mode() == ff8_interp::GameMode::World && !tick_real) return; orig_clut_anim(p); }
		void __cdecl input_hook(int16_t* p) { if (tick_real) orig_input(p); }
		void __cdecl input_locked_hook(int16_t* p, char b) { if (tick_real) orig_input_locked(p, b); }
		void __cdecl vehicle_pos_hook(char* a, int32_t* b, void* c) { if (tick_real) orig_vehicle_pos(a, b, c); }
		void __cdecl movement_hook(int32_t* a, int16_t* b, int16_t* c, void* d) { if (tick_real) orig_movement(a, b, c, d); }
		void __cdecl facing_wrap_hook(void* a, int16_t* b) { if (tick_real) orig_facing_wrap(a, b); }

		void __cdecl camera_hook(int16_t* a, void* b, void* c, int d)
		{
			if (tick_real)
			{
				orig_camera(a, b, c, d); // logic step: filters + matrix (uses last tick's position, as in vanilla)
				CamState now;
				capture_cam(now);
				CamState prev = cam_next; // re-based into this tick's segment frame by cam_teleport
				if (!cam_valid || cam_teleport(prev, now)) cam_prev = now; else cam_prev = prev;
				cam_next = now;
				cam_valid = true;
				capture_pos(pos_logic); // position after this tick's movement step; terrain integrates from it
			}
			if (!cam_valid) return;
			if (ff8_interp::cfg().world_debug & 1) return; // debug: no camera display pass (camera steps at 30 Hz)
			// Display pass: rebuild the view matrix (GTE registers) from interpolated state. Display
			// values stay in place for the rest of the frame (sky/parallax/terrain read them);
			// head_hook puts the logic state back at the next tick.
			if (*CAM_SCRIPT != 0) return; // scripted camera branch keeps per-call state: leave it at logic rate
			if (pos_valid && !(ff8_interp::cfg().world_debug & 2)) write_pos(display_pos());
			CamState disp = blend_cam(cam_prev, cam_next, tick_alpha);
			// The camera filters the look-at on every call (x/y: (pos + look) / 2, height:
			// (3 look + pos - 256) / 4). Feed it the pre-image so its output is exactly the blended
			// look-at (vanilla lag, interpolated) independent of the (segment-clamped) i32 position.
			disp.look[0] = 2 * disp.look[0] - PLAYER_POS[0];
			disp.look[1] = 2 * disp.look[1] - PLAYER_POS[1];
			if (*CAM_HEIGHT_LOCK == 0)
			{
				int n = 4 * disp.look[2] + 0x100 - PLAYER_POS[2]; // L = ceil(n/3): 3L in [n, n+2]
				disp.look[2] = (n >= 0) ? (n + 2) / 3 : -((-n) / 3);
			}
			write_cam(disp);
			orig_camera(a, b, c, d);
		}

		uint32_t __cdecl docking_hook(char* p)
		{
			if (tick_real) last_docking = orig_docking(p);
			return last_docking;
		}
		int __cdecl encounter_hook(int16_t* p) { return tick_real ? orig_encounter(p) : 0; }
		int __cdecl jumbo_hook(int16_t* p) { return tick_real ? orig_jumbo(p) : 0; }

		// Vehicle state/sound + position block (director 0x540089..0x540097), per logic tick.
		uint32_t __cdecl vehicle_snd_hook(char* p)
		{
			if (tick_real) last_vehicle_snd = orig_vehicle_snd(p);
			return last_vehicle_snd;
		}
		void __cdecl vehicle_snd_apply_hook(int v) { if (tick_real) orig_vehicle_snd_apply(v); }

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
		struct TerrainOut { int16_t steer; int32_t v[11]; uint8_t flag; bool valid = false; } terrain_out;
		const uint32_t TERRAIN_OUT_ADDRS[11] = { 0x20409FC, 0x2040A0C, 0x2040A00, 0x204095C, 0x203FE30, 0xC75D0C, 0xC75D10, 0x2036AD0, 0x2036AD4, 0x203FD10, 0x203FD14 };
		void capture_terrain_out()
		{
			terrain_out.steer = *(int16_t*)0x20409EC;
			for (int i = 0; i < 11; i++) terrain_out.v[i] = *(int32_t*)TERRAIN_OUT_ADDRS[i];
			uint8_t* st = *(uint8_t**)0x20403A4;
			terrain_out.flag = st ? st[0x6D] : 0;
			terrain_out.valid = true;
		}
		void restore_terrain_out()
		{
			if (!terrain_out.valid) return;
			*(int16_t*)0x20409EC = terrain_out.steer;
			for (int i = 0; i < 11; i++) *(int32_t*)TERRAIN_OUT_ADDRS[i] = terrain_out.v[i];
			uint8_t* st = *(uint8_t**)0x20403A4;
			if (st) st[0x6D] = terrain_out.flag;
		}
		void run_terrain(terrain_t fn)
		{
			if (tick_real)
			{
				if (cam_valid) write_pos(pos_logic);
				fn();
				capture_terrain_out();
				return;
			}
			int32_t speed = *MOVE_SPEED;
			*MOVE_SPEED = 0;
			fn();
			*MOVE_SPEED = speed;
			restore_terrain_out();
			if (pos_valid && !(ff8_interp::cfg().world_debug & 2)) write_pos(display_pos()); // undo any residual step
		}
		void __cdecl terrain_fog_hook() { run_terrain(orig_terrain_fog); }
		void __cdecl terrain_nofog_hook() { run_terrain(orig_terrain_nofog); }

		void __cdecl integrate_hook()
		{
			if (tick_real)
			{
				orig_integrate(); // position is the logic position here (terrain hook restored it before integrating)
				PosState now;
				capture_pos(now);
				PosState prev = pos_next; // re-based into this tick's segment frame by pos_teleport
				if (!pos_valid || pos_teleport(prev, now)) pos_prev = now; else pos_prev = prev;
				pos_next = now;
				pos_valid = true;
				pos_tick = logic_ticks;
				if (ff8_interp::cfg().trace)
					ff8_interp::log_trace("ff8world f=%u tick=%u pos=(%d,%d,%d) local=(%d,%d,%d) yaw=%d camrot=(%d,%d,%d) look=(%d,%d,%d) cam=(%d,%d,%d,%d,%d,%d,%d)\n",
						ff8_interp::frame_counter(), logic_ticks, now.pos[0], now.pos[1], now.pos[2], now.local[0], now.local[1], now.local[2], now.local[5],
						cam_next.camrot[0], cam_next.camrot[1], cam_next.camrot[2], cam_next.look[0], cam_next.look[1], cam_next.look[2],
						cam_next.cam[0], cam_next.cam[1], cam_next.cam[2], cam_next.cam[3], cam_next.cam[4], cam_next.cam[5], cam_next.cam[6]);
			}
			if (!pos_valid) return;
			if (!(ff8_interp::cfg().world_debug & 2)) write_pos(display_pos());
		}

		// ---- character model pose interpolation (shared module, see ../chara_pose.h) --------------
		ff8_chara::PoseInterp pose;

		void __cdecl world_animate_hook(int p1, int p2)
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::World || !cam_valid || (ff8_interp::cfg().world_debug & 4))
			{
				orig_world_animate(p1, p2);
				return;
			}
			update_tick();
			pose.animate(orig_world_animate, p1, p2, tick_real, tick_alpha, logic_ticks);
		}

		// Model animation stepper: advances the frame counter AND switches clips (walk/idle) with a
		// counter reset. It must run only on logic ticks; on other host frames return the last
		// result so the caller's state stays consistent. (Restoring the counter after a non-tick
		// call corrupted clip switches: the old walk counter landed inside the idle clip.)
		uint32_t stepper_last[64] = {};
		uint32_t __cdecl stepper_hook(int mode, int slot)
		{
			int idx = (slot >= 0 && slot < 64) ? slot : 0;
			if (tick_real) stepper_last[idx] = orig_stepper(mode, slot);
			return stepper_last[idx];
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

		for (uint32_t cs : CS_HEAD) ff8_interp::hook_call(cs, (void*)head_hook);
		ff8_interp::hook_call(CS_HEAD2, (void*)head2_hook);
		ff8_interp::hook_call(CS_INPUT, (void*)input_hook);
		ff8_interp::hook_call(CS_INPUT_LOCKED, (void*)input_locked_hook);
		ff8_interp::hook_call(CS_VEHICLE_POS, (void*)vehicle_pos_hook);
		ff8_interp::hook_call(CS_MOVEMENT, (void*)movement_hook);
		ff8_interp::hook_call(CS_FACING_WRAP, (void*)facing_wrap_hook);
		ff8_interp::hook_call(CS_CAMERA, (void*)camera_hook);
		ff8_interp::hook_call(CS_DOCKING, (void*)docking_hook);
		// FFNx itself redirects this call (1.24: to its own function in AF3DN.P). Chain to whatever the
		// site reached before us, so FFNx keeps its behaviour and it simply runs at the logic rate.
		if (uint32_t t = ff8_interp::hook_call_chain(CS_ENCOUNTER, (void*)encounter_hook)) orig_encounter = (encounter_t)t;
		ff8_interp::hook_call(CS_JUMBO, (void*)jumbo_hook);
		for (uint32_t cs : CS_STEPPER) ff8_interp::hook_call(cs, (void*)stepper_hook);
		ff8_interp::hook_call(CS_VEHICLE_SND, (void*)vehicle_snd_hook);
		ff8_interp::hook_call(CS_VEHICLE_SND_APPLY, (void*)vehicle_snd_apply_hook);
		ff8_interp::hook_call(CS_INTEGRATE, (void*)integrate_hook);
		ff8_interp::hook_call(CS_TERRAIN_FOG, (void*)terrain_fog_hook);
		ff8_interp::hook_call(CS_TERRAIN_NOFOG, (void*)terrain_nofog_hook);
		pose.table = WORLD_INSTANCES;
		ff8_interp::hook_call(CS_WORLD_ANIMATE, (void*)world_animate_hook);
		ff8_interp::hook_call(CS_TEX_ANIM, (void*)tex_anim_hook);
		ff8_interp::hook_call(CS_CLUT_ANIM, (void*)clut_anim_hook);
		int clock_sites = 0;
		for (uint32_t cs : CS_PLAY_CLOCK) clock_sites += ff8_play_clock::patch_site(cs, (void*)play_clock_hook, __func__) ? 1 : 0;
		installed = true;
		ff8_interp::log_info("%s: world map interpolation v22 enabled (world %d fps, logic %d fps, play clock %d/2 sites).\n", __func__, world_fps, LOGIC_FPS, clock_sites);
	}

	bool interp_installed() { return installed; }

	void interp_reset()
	{
		cam_valid = false;
		pos_valid = false;
		pos_tick = 0xFFFFFFFF;
		tick_frame = 0xFFFFFFFF;
		tick_acc = 0.0;
		ff8_tick::clock_reset();
		last_docking = 0;
		last_vehicle_snd = 0xFFFFFFFF;
		memset(stepper_last, 0, sizeof(stepper_last));
		pose.reset();
		terrain_out.valid = false;
	}

	void interp_on_frame()
	{
		if (!installed) return;
		int mode = (int)ff8_interp::mode_id();
		if (mode != last_mode)
		{
			interp_reset();
			last_mode = mode;
		}
	}
}
