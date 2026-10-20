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

// FF8 field (pre-rendered scenes) interpolation.
//
// The field frame updater (Field_MainLoop_UpdateFrame 0x4767B0, called once per host frame
// from the director 0x471F70) runs the script VM, input, entity motion, triggers, camera pan,
// model posing, background and particles in one function. As on the world map we keep it
// running every host frame and gate the 30 Hz logic calls at their call sites; the render
// calls see entity positions/facings blended between the last two logic ticks and character
// poses blended by the shared pose module.

#include "interp.h"
#include "../common/tick.h"
#include "../common/chara_pose.h"
#include "../common/play_clock.h"
#include "../common/pace.h"
#include "../common/ui_pace.h"

#include "../host.h"

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

namespace ff8_field
{
	namespace
	{
		constexpr int LOGIC_FPS = 30;

		// Frame updater 0x4767B0 call sites (FF8 2000 == Steam)
		constexpr uint32_t FN_SCRIPT_VM = 0x529FF0;     // void (int drawbuf): scripts + entity animation frame stepping
		constexpr uint32_t CALL_SITE_SCRIPT_VM = 0x4768FE;
		constexpr uint32_t FN_PAD_READ = 0x49ED30;      // uint32 (int pad, int): pad state
		constexpr uint32_t CALL_SITE_PAD_READ[2] = { 0x476841, 0x476859 };
		constexpr uint32_t FN_PAD_READ2 = 0x49EDE0;     // uint32 (int pad, int)
		constexpr uint32_t CALL_SITE_PAD_READ2[2] = { 0x47684D, 0x476865 };
		constexpr uint32_t FN_MOTION = 0x4789A0;        // void (entities, input): player/scripted movement, jumps, ladders
		constexpr uint32_t CALL_SITE_MOTION = 0x476BBF;
		constexpr uint32_t FN_TALK_PUSH = 0x4777F0;     // void (player entity, npc table)
		constexpr uint32_t CALL_SITE_TALK_PUSH = 0x476BE7;
		constexpr uint32_t FN_TALK = 0x4796E0;          // void ()
		constexpr uint32_t CALL_SITE_TALK = 0x476BEC;
		constexpr uint32_t FN_SCROLL_TWEEN = 0x4765C0;  // void (ctrl*): SHAKE oscillator step (opcodes SHAKE 0x520BA0 / SHAKEOFF), outputs i8 0x1CE4885 / 0x1CE4893
		constexpr uint32_t CALL_SITE_SCROLL_TWEEN[2] = { 0x476BF6, 0x476C00 };
		constexpr uint32_t FN_CTRL_RESET = 0x475DF0;    // void (): layer controller state step
		constexpr uint32_t CALL_SITE_CTRL_RESET = 0x476C08;
		constexpr uint32_t FN_PAN = 0x475E60;           // void (): steps the 8 scroll controllers (tracking + timed scrolls, per call)
		constexpr uint32_t CALL_SITE_PAN = 0x476C0D;
		constexpr uint32_t FN_POSE = 0x472B30;          // void (entities, drawbuf): model transforms, head look, skeletons
		constexpr uint32_t CALL_SITE_POSE = 0x476C79;
		constexpr uint32_t FN_PARTICLES = 0x473DE0;     // void (cam, drawbuf, pmd): emitter update
		constexpr uint32_t CALL_SITE_PARTICLES = 0x476D82;
		constexpr uint32_t FN_GATEWAY = 0x47BD20;       // void (player entity, gateways)
		constexpr uint32_t CALL_SITE_GATEWAY = 0x4770C5;
		constexpr uint32_t FN_FADE = 0x47B980;          // void (drawbuf): fade step + draw
		constexpr uint32_t CALL_SITE_FADE = 0x4770D0;
		constexpr uint32_t FN_FIELD_ANIMATE = 0x533CD0; // void (int, int): decode + skeletons, table 0x1DCB340
		constexpr uint32_t CALL_SITE_FIELD_ANIMATE = 0x530A8B; // in 0x530810 (from 0x472B30)
		// Character texture animation (eye blink): a random timer plus VRAM rectangle copies, stepped once
		// per call from its single call site. At host rate the blink is several times too fast.
		constexpr uint32_t FN_CHARA_TEXANIM = 0x532930; // void (int slot)
		constexpr uint32_t CALL_SITE_CHARA_TEXANIM = 0x530840;
		// Play clock 0x4701B0 (play time + event countdown, one 1/60 s step per call), called by the field
		// mode loop twice per 30 fps frame, and twice more while 0x209A798 != 0 (the 15 fps movie path,
		// which doubles 0x472990 the same way): 60 steps/s either way. See play_clock.h.
		constexpr uint32_t CALL_SITE_PLAY_CLOCK[4] = { 0x46FFF9, 0x46FFFE, 0x47000C, 0x470011 };
		// v6: per-call counters of the field that ran at host rate.
		// Message windows: the updater's per-frame 0x49A970(2 | 4 with a movie) at 0x476FC1, see ui_pace.h.
		constexpr uint32_t CALL_SITE_WINDOW_STEP = 0x476FC1;
		// Field_StepTransitionFade 0x472990: the exit fade (field change 90 steps, battle / menu 34 steps,
		// timer u8 0x1CDC618, mode u8 0x1CDC74C), called by the main loop twice per frame (four times with
		// a movie) while 0x1CDBFE0 == 1: 60 steps/s natively. Every call also draws the fade quad, so a
		// call without a due step still runs, with the timer pre-decremented (the call adds it back).
		constexpr uint32_t FN_TRANSITION_FADE = 0x472990;
		constexpr uint32_t CALL_SITE_TRANSITION_FADE[4] = { 0x47007E, 0x470083, 0x470091, 0x470096 };
		uint8_t* const TRANSITION_FADE_MODE = (uint8_t*)0x1CDC74C;
		uint8_t* const TRANSITION_FADE_TIMER = (uint8_t*)0x1CDC618;
		// SPUREADY (0x51F520) zeroes the frame counter 0x1D9CDCC, SPUSYNC (0x51F5B0) waits until it reaches
		// its argument; the main loop increments it once per frame (0x47015F). Scripts synced to music /
		// voice therefore resumed 4.8x early. It now advances once per logic tick (see vm_hook).
		uint32_t* const SPU_FRAMES = (uint32_t*)0x1D9CDCC;
		// Background tile builder 0x475480 (site 0x476CFE, every frame): for each background entity
		// (ptr 0x1D9CF8C, count u8 0x1D9D0E8, 0x1B4 bytes) with a running colour shade (BGSHADE: counter
		// i16 +0x19E < length i16 +0x19C) it lerps the colour and increments the counter per call.
		constexpr uint32_t FN_BG_TILES = 0x475480;
		constexpr uint32_t CALL_SITE_BG_TILES = 0x476CFE;
		uint8_t** const BG_ENTITIES_PTR = (uint8_t**)0x1D9CF8C;
		uint8_t* const BG_ENTITY_COUNT = (uint8_t*)0x1D9D0E8;
		constexpr uint32_t BG_ENTITY_SIZE = 0x1B4;
		constexpr uint32_t BG_SHADE = 0x19C; // i16 length, i16 counter
		// Curve trail effect 0x475170 (site 0x476CC0, every frame; 8 slots started by a script opcode via
		// 0x474750 / 0x474870): flag u8 0x1CE0750[i], progress i16 at 0x1CE4D0A + i * 0x88 incremented per
		// call, slot freed when progress > length + 4. This is the draw point marker (SETDRAWPOINT
		// 0x523030): eight ribbons rising along a curve, each respawned at the base by 0x474970 the call
		// after it dies.
		constexpr uint32_t FN_TRAIL = 0x475170;
		constexpr uint32_t CALL_SITE_TRAIL = 0x476CC0;
		uint8_t* const TRAIL_FLAGS = (uint8_t*)0x1CE0750;
		uint8_t* const TRAIL_PROGRESS = (uint8_t*)0x1CE4D0A;
		constexpr uint32_t TRAIL_STRIDE = 0x88;
		constexpr int TRAIL_SLOTS = 8;
		// v9: the pieces of 0x475170, so update and draw can run separately. Slot 0x1CE4C88 + i * 0x88:
		// +0x00 ring A, +0x40 ring B (8 x {x, y, z, pad} i16, the ribbon's two edges, head at
		// progress & 7), +0x80 length, +0x82 progress. Control points 0x1CDC620 + i * 0x18.
		uint8_t* const TRAIL_SLOT_BASE = (uint8_t*)0x1CE4C88;
		uint8_t* const TRAIL_CTRL_BASE = (uint8_t*)0x1CDC620;
		constexpr uint32_t TRAIL_CTRL_STRIDE = 0x18;
		constexpr uint32_t TRAIL_RING_B = 0x40, TRAIL_LENGTH = 0x80, TRAIL_PROGRESS_OFFSET = 0x82;
		int16_t* const TRAIL_SHOWN = (int16_t*)0x1CDBFEA;       // SETDRAWPOINT visibility (0x4747A0), 1 = shown
		constexpr uint32_t FN_TRAIL_UPDATE = 0x474AC0;          // (slot, ctrl): respawn dead slots, head into ring
		constexpr uint32_t FN_TRAIL_DRAW = 0x474D90;            // (slot, ot, prims + i*0xB4, prims2 + i*0x20); reads slot only
		constexpr uint32_t FN_GTE_PUSH = 0x56BB40;
		constexpr uint32_t FN_GTE_ROT = 0x56BAE0;
		constexpr uint32_t FN_GTE_TRANS = 0x56BB30;
		constexpr uint32_t FN_GTE_POP = 0x56BB70;
		// v7: background scroll.
		// Eight scroll controllers at 0x1CE4780, 0x1C bytes each: +0 u8 mode (0 track actor, 1 LSCROLLA,
		// 2 CSCROLLA, 3 direct DSCROLL*, 4 LSCROLL*, 5 CSCROLL*), +1 u8 tracked entity, +2 u8 state
		// (0 armed, 1 running, 2 done), +4 i16 length, +6 i16 step, +8/+A i16 out x/y, +C/+E start,
		// +10/+12 target, +14/+16 layer base (SCROLLMODE2), +18/+1A layer ratio /256 (SCROLLMODE2 /
		// SCROLLRATIO2). Controller 0 (1 with camera index 0x1CE4906 == 1) is the camera, the others
		// scroll background layers 1..7. 0x475E60 steps them PER CALL (tracking + timed scrolls).
		uint8_t* const SCROLL_CTRL = (uint8_t*)0x1CE4780;
		constexpr uint32_t SCROLL_STRIDE = 0x1C;
		constexpr int SCROLL_CTRLS = 8;
		constexpr uint32_t CTRL_MODE = 0x0, CTRL_STATE = 0x2, CTRL_OUT = 0x8, CTRL_RATIO = 0x18;
		int8_t* const SHAKE_OUT_X = (int8_t*)0x1CE4885; // 0x4765C0 output of controller 0x1CE4882
		int8_t* const SHAKE_OUT_Y = (int8_t*)0x1CE4893; // 0x4765C0 output of controller 0x1CE4890
		uint8_t* const CAMERA_INDEX = (uint8_t*)0x1CE4906;
		uint8_t** const CAMERA_PTR = (uint8_t**)0x1CE4C80;  // current .ca camera, +0x20/+0x22 i16
		int32_t* const SCREEN_CENTER_X = (int32_t*)0x1CF3D50;
		int32_t* const SCREEN_CENTER_Y = (int32_t*)0x1CF3D54;
		// Field_BG_UpdateScreenScroll 0x476450(drawbuf, 0x1CD2F00, cam index == 1): DRAWENV offset (+8/+A of
		// 0x1CD2F00 + buf * 0x5C, buf 1 when drawbuf != 0x1CE50D0, x + 0x200) = cam+0x20 - ctrl.out + centre
		// + shake. The DRAWENV is encoded as GP0 E5 by 0x45C0F0 and applies to every field primitive
		// (models, shadows, tiles, particles), so models and background share it.
		constexpr uint32_t FN_DRAWENV_SCROLL = 0x476450;
		constexpr uint32_t CALL_SITE_DRAWENV_SCROLL = 0x476C37;
		constexpr uint32_t DRAWBUF0 = 0x1CE50D0;
		uint8_t* const FIELD_DRAWENV = (uint8_t*)0x1CD2F00;
		constexpr uint32_t DRAWENV_SIZE = 0x5C;
		// Tile list 0x475480 (CALL_SITE_BG_TILES): records *(*(i16**)0x1CF3D98), 16 bytes, i16 x/y at +0/+2, u8 layer
		// at +0xC, end x == 0x7FFF; one 16-byte sprite per record in the prim buffer (3rd arg), x/y at
		// +8/+A written as (tile + layer offset) << 3, i.e. in 1/8 pixel units.
		uint8_t** const BG_TILE_RECORDS_PP = (uint8_t**)0x1CF3D98;
		// SSIGPU draw-env handlers (tables 0xB7CF08 / 0xB7D708 / 0x467120 group): E3 0x45BA30, E4 0x45BA60,
		// E5 0x45BAD0, all recompute the float vertex offset 0x1CA89E0/E4 from the integer draw offset
		// 0x1CA8950/52 in 0x45B6F0 ((ofs - area) * px scale 0xB7CC0C/10 + viewport - 0.5; only when
		// 0x1CA8A0C == 1). Sprites and polygons add that float, so a sub-pixel draw offset is possible there.
		constexpr uint32_t FN_GPU_E3 = 0x45BA30;
		constexpr uint32_t FN_GPU_E4 = 0x45BA60;
		constexpr uint32_t FN_GPU_E5 = 0x45BAD0;
		constexpr uint32_t CALL_SITE_GPU_E3[3] = { 0x462ED5, 0x463235, 0x467125 };
		constexpr uint32_t CALL_SITE_GPU_E4[3] = { 0x462EE5, 0x463245, 0x467135 };
		constexpr uint32_t CALL_SITE_GPU_E5[3] = { 0x462EF5, 0x463255, 0x467145 };
		int16_t* const GPU_DRAW_OFS = (int16_t*)0x1CA8950;   // x, y (11-bit sign-extended)
		int32_t* const GPU_OFS_MODE = (int32_t*)0x1CA8A0C;   // 1 = offset applied to the float offset
		float* const GPU_VERTEX_OFS = (float*)0x1CA89E0;     // x, y
		float* const GPU_PX_SCALE = (float*)0xB7CC0C;        // x, y (per pixel of draw offset)

		// ff8_field_interp_debug bits. 2 = no entity blend, 4 = no pose blend (literals below, pre-v5);
		// the v5 bits are named.
		constexpr long DBG_PLAY_CLOCK_OFF = 8; // play clock at host rate again (pre-v5: 4.8x fast at 144 Hz)
		constexpr long DBG_ALPHA_LEAD = 16;    // pre-v5 tick_alpha = (tick_accumulator + dt) / P (one host frame lead)
		constexpr long DBG_PAD_ACCUMULATE_OFF = 32;   // pre-v6 pad reads: only the tick frame's edges / held state reach the VM
		constexpr long DBG_WINDOW_OFF = 64;    // pre-v6 message windows, cursor, countdown pulse, rumble at host rate
		constexpr long DBG_TRANSITION_OFF = 128; // pre-v6 field exit fade at host rate
		constexpr long DBG_SPU_OFF = 256;      // pre-v6 SPUSYNC frame counter at host rate
		constexpr long DBG_BACKGROUND_FX_OFF = 512;    // pre-v6 background shade and curve trail counters at host rate
		constexpr long DBG_SCROLL_OFF = 1024;  // pre-v7 scroll: 0x475E60 every host frame, no scroll blend, no sub-pixel
		constexpr long DBG_SCROLL_WHOLE_PIXELS = 2048;  // v7 scroll blend in whole pixels only (no sub-pixel draw offset / tile shift)
		constexpr long DBG_TRAIL_BLEND_OFF = 4096; // pre-v9 draw point trails: stepped at 30 Hz, dead slots respawned on non-tick calls
		// 8192 (chara_pose.cpp, DBG_BONE_POP_OFF): v12 bone turns over 90 deg per frame swept again instead of switched at mid-tick
		// 16384 (chara_pose.cpp, DBG_RAW_SKIP_OFF): v13 raw feed ignores skipped bones again (limbs garbled while a script holds a bone)
		constexpr long DBG_WINDOW_PRESS_SYNC_OFF = 32768; // pre-v15 windows take a press before the script's tick (draw point menu reopens)

		uint8_t** const ENTITIES_PTR = (uint8_t**)0x1D9CF88; // 612-byte character entities
		uint8_t* const ENTITY_COUNT = (uint8_t*)0x1D9D019;
		constexpr uint32_t ENTITY_SIZE = 0x264;
		constexpr uint32_t ENTITY_POS = 0x190;         // i32 x, y, z (fixed point >> 12)
		constexpr uint32_t ENTITY_MODEL_FACING = 0x241; // u8, 256 per turn
		constexpr uint32_t ENTITY_HEAD_LOOK = 0x228;    // 8 x i16 head-look tween (stepped by 0x472B30 every call)
		uint8_t** const FIELD_INSTANCES = (uint8_t**)0x1DCB340;
		uint8_t* const FADE_STATE = (uint8_t*)0x1CE486A; // i16 level + i16 step, stepped by 0x47B980 every call
		int32_t* const FIELD_FLOW = (int32_t*)0x1CE4A64;  // director state, 4 = running
		uint16_t* const CURRENT_FIELD_ID = (uint16_t*)0x1CD2FC0;
		constexpr int MAX_ENTITIES = 64;

		typedef void (__cdecl *fn_script_vm_t)(int);
		typedef uint32_t (__cdecl *fn_pad_read_t)(int, int);
		typedef void (__cdecl *fn_int_int_t)(int, int);
		typedef void (__cdecl *fn_void_t)();
		typedef void (__cdecl *fn_ptr_t)(uint8_t*);
		typedef void (__cdecl *fn_int_int_int_t)(int, int, int);
		typedef void (__cdecl *fn_int_t)(int);

		fn_script_vm_t original_vm = (fn_script_vm_t)FN_SCRIPT_VM;
		fn_pad_read_t original_pad = (fn_pad_read_t)FN_PAD_READ;
		fn_pad_read_t original_pad2 = (fn_pad_read_t)FN_PAD_READ2;
		fn_int_int_t original_motion = (fn_int_int_t)FN_MOTION;
		fn_int_int_t original_talk_push = (fn_int_int_t)FN_TALK_PUSH;
		fn_void_t original_talk = (fn_void_t)FN_TALK;
		fn_ptr_t original_scroll_tween = (fn_ptr_t)FN_SCROLL_TWEEN;
		fn_void_t original_ctrl_reset = (fn_void_t)FN_CTRL_RESET;
		fn_void_t original_pan = (fn_void_t)FN_PAN;
		fn_int_int_t original_pose = (fn_int_int_t)FN_POSE;
		fn_int_int_int_t original_particles = (fn_int_int_int_t)FN_PARTICLES;
		fn_int_int_t original_gateway = (fn_int_int_t)FN_GATEWAY;
		fn_int_t original_fade = (fn_int_t)FN_FADE;
		fn_int_t original_chara_texanim = (fn_int_t)FN_CHARA_TEXANIM;
		fn_int_int_t original_field_animate = (fn_int_int_t)FN_FIELD_ANIMATE;

		// hook_call_chain: `original` becomes what the site reached before (the engine function, or FFNx's
		// wrapper of it). A refused site keeps the engine function and is counted as skipped by the host.
		template<typename Fn> void chain_call(uint32_t site, void* hook, Fn& original)
		{
			if (const uint32_t previous_target = ff8_interp::hook_call_chain(site, hook)) original = (Fn)previous_target;
		}

		int field_fps = 60;
		bool installed = false;
		int last_mode = -1;

		uint32_t tick_host_frame = 0xFFFFFFFF;
		double tick_accumulator = 0.0;
		bool is_logic_tick = true;
		float tick_alpha = 1.0f;
		uint32_t logic_tick_count = 0;

		constexpr double LOGIC_PERIOD = 1.0 / double(LOGIC_FPS);

		// Wall-clock gate: ff8_field_fps only caps the render rate, the logic rate must not depend on it.
		void update_tick()
		{
			if (tick_host_frame == ff8_interp::frame_counter()) return;
			tick_host_frame = ff8_interp::frame_counter();
			double dt = ff8_tick::frame_delta();
			tick_accumulator += dt;
			is_logic_tick = tick_accumulator >= LOGIC_PERIOD;
			if (is_logic_tick)
			{
				tick_accumulator -= LOGIC_PERIOD;
				if (tick_accumulator > LOGIC_PERIOD) tick_accumulator = LOGIC_PERIOD; // after a stall, catch up over the next frames
			}
			// v5: the time since the last tick, tick_accumulator / P. `tick_accumulator` already holds this frame's dt, so
			// the pre-v5 (tick_accumulator + dt) / P counted it twice: one host frame of lead and alpha pinned to 1
			// on the last frame before every tick, an uneven step once per tick. DBG_ALPHA_LEAD restores it.
			const double time_since_tick = (ff8_interp::cfg().field_debug & DBG_ALPHA_LEAD) ? tick_accumulator + dt : tick_accumulator;
			tick_alpha = float(time_since_tick / LOGIC_PERIOD);
			if (tick_alpha > 1.0f) tick_alpha = 1.0f;
			if (tick_alpha < 0.0f) tick_alpha = 0.0f;
		}

		inline bool field_active() { return ff8_interp::game_mode() == ff8_interp::GameMode::Field; }

		void __cdecl play_clock_hook() { ff8_play_clock::run(!(ff8_interp::cfg().field_debug & DBG_PLAY_CLOCK_OFF)); }

		struct EntityState { int32_t pos[3]; uint8_t facing; };
		EntityState entity_prev[MAX_ENTITIES], entity_next[MAX_ENTITIES];
		int snapshot_entity_count = 0;
		bool entity_snapshots_valid = false;
		uint16_t snapshot_field = 0xFFFF;   // field the snapshots belong to
		uint8_t* snapshot_table = nullptr;  // entity table the snapshots belong to

		inline int entity_count()
		{
			int count = *ENTITY_COUNT;
			return count > MAX_ENTITIES ? MAX_ENTITIES : count;
		}
		inline uint8_t* entity(int index) { return *ENTITIES_PTR + index * ENTITY_SIZE; }
		void capture_entity(int index, EntityState& state)
		{
			uint8_t* entity_data = entity(index);
			memcpy(state.pos, entity_data + ENTITY_POS, sizeof(state.pos));
			state.facing = entity_data[ENTITY_MODEL_FACING];
		}
		void write_entity(int index, const EntityState& state)
		{
			uint8_t* entity_data = entity(index);
			memcpy(entity_data + ENTITY_POS, state.pos, sizeof(state.pos));
			entity_data[ENTITY_MODEL_FACING] = state.facing;
		}
		inline int32_t lerp_i32(int32_t a, int32_t b, float t)
		{
			double scaled = double(b - a) * t;
			return a + (int32_t)(scaled + (scaled >= 0 ? 0.5 : -0.5));
		}
		inline uint8_t lerp_facing(uint8_t a, uint8_t b, float t)
		{
			int delta = ((int(b) - int(a) + 128) & 255) - 128;
			float scaled = delta * t;
			return (uint8_t)(int(a) + (int)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f)));
		}
		bool teleported(const EntityState& from, const EntityState& to)
		{
			for (int axis = 0; axis < 3; axis++)
			{
				int32_t delta = to.pos[axis] - from.pos[axis];
				if (delta > 0x100000 || delta < -0x100000) return true; // > 256 units in one tick: scripted placement
			}
			return false;
		}
		// Snapshots are only meaningful for the scene they were taken in: a field jump keeps
		// MODE_FIELD but reloads the entity table (positions placed by 0x477C90).
		inline void drop_stale_snapshots()
		{
			if (entity_snapshots_valid && (*CURRENT_FIELD_ID != snapshot_field || *ENTITIES_PTR != snapshot_table || entity_count() != snapshot_entity_count))
				entity_snapshots_valid = false;
		}
		void write_logic_positions()
		{
			drop_stale_snapshots();
			if (!entity_snapshots_valid) return;
			for (int i = 0; i < snapshot_entity_count; i++)
				write_entity(i, entity_next[i]);
		}
		void write_display_positions()
		{
			drop_stale_snapshots();
			if (!entity_snapshots_valid) return;
			for (int i = 0; i < snapshot_entity_count; i++)
			{
				EntityState blended;
				for (int axis = 0; axis < 3; axis++)
					blended.pos[axis] = lerp_i32(entity_prev[i].pos[axis], entity_next[i].pos[axis], tick_alpha);
				blended.facing = lerp_facing(entity_prev[i].facing, entity_next[i].facing, tick_alpha);
				write_entity(i, blended);
			}
		}

		// ---- v7: scroll interpolation ------------------------------------------------------------
		// 0x475E60 steps every scroll controller per call (tracking projects the followed entity and clamps
		// it, LSCROLL / CSCROLL* advance their step counter +6 by one), so it now runs on logic ticks only;
		// the tick values of the controller outputs (+8/+A) and of the two shake outputs are captured
		// (prev/next) and the two draw consumers - the DRAWENV offset 0x476450 and the tile list 0x475480 -
		// see lerp(prev, next, tick_alpha) rounded to whole pixels, with the exact logic values put back
		// right after each call. The remainder below one pixel goes to the GPU: the DRAWENV part through the
		// float vertex offset set by the E3/E4/E5 handlers (exact, all field primitives), the per-layer
		// parallax part as a shift of the tile sprites in their native 1/8 pixel units.
		struct ScrollState
		{
			int16_t out[SCROLL_CTRLS][2];
			uint8_t mode[SCROLL_CTRLS];
			int8_t shake[2];
		};
		ScrollState scroll_prev, scroll_next;
		bool scroll_valid = false;
		uint16_t scroll_field = 0xFFFF;
		uint8_t scroll_camera_index = 0xFF;
		uint8_t* scroll_camera_ptr = nullptr;

		inline uint8_t* scroll_controller(int index) { return SCROLL_CTRL + index * SCROLL_STRIDE; }
		inline bool scroll_blend_enabled() { return !(ff8_interp::cfg().field_debug & DBG_SCROLL_OFF); }
		inline bool subpixel_enabled() { return !(ff8_interp::cfg().field_debug & (DBG_SCROLL_OFF | DBG_SCROLL_WHOLE_PIXELS)); }

		void read_scroll_state(ScrollState& state)
		{
			for (int k = 0; k < SCROLL_CTRLS; k++)
			{
				memcpy(state.out[k], scroll_controller(k) + CTRL_OUT, 4);
				state.mode[k] = scroll_controller(k)[CTRL_MODE];
			}
			state.shake[0] = *SHAKE_OUT_X;
			state.shake[1] = *SHAKE_OUT_Y;
		}
		// v8: once-per-second scroll diagnostics (trace flag = trace_battle_animation, cfg().trace).
		struct ScrollStats
		{
			uint32_t frames = 0, ticks = 0, blended = 0, not_blended = 0;
			uint32_t snap_scene = 0, snap_big = 0, snap_direct = 0;
			double camera_step_sum[2] = {}, layer_step_sum[2] = {};  // sum |next - prev| per tick, camera controller / others
			uint32_t env_armed = 0, env_formula_miss = 0;
			uint32_t gpu_calls = 0, gpu_mode_off = 0, gpu_match = 0, gpu_nomatch_armed = 0;
			int16_t gpu_last_offset[2] = {}, env_last_offset[2] = {};
			float last_remainder[2] = {};
			uint32_t tile_calls = 0, tile_shifted = 0, tile_nonzero_layers = 0;
			uint32_t dt_over_10ms = 0, dt_over_20ms = 0;
			double dt_max = 0.0;
			double elapsed = 0.0;
		};
		ScrollStats scroll_stats;
		void scroll_diag_frame()
		{
			if (!ff8_interp::cfg().trace) return;
			const double dt = ff8_tick::frame_delta();
			scroll_stats.frames++;
			if (dt > 0.010) scroll_stats.dt_over_10ms++;
			if (dt > 0.020) scroll_stats.dt_over_20ms++;
			if (dt > scroll_stats.dt_max) scroll_stats.dt_max = dt;
			scroll_stats.elapsed += dt;
			if (scroll_stats.elapsed < 1.0) return;
			const ScrollStats& stats = scroll_stats;
			const uint8_t* camera_ctrl = SCROLL_CTRL + (*CAMERA_INDEX & 7) * SCROLL_STRIDE;
			ff8_interp::log_trace("ff8fieldscroll f=%u field=%u frames=%u dtmax=%.1fms >10ms=%u >20ms=%u ticks=%u blended=%u unblended=%u "
				"snap(scene=%u big=%u direct=%u) dcam=(%.0f,%.0f) dlayers=(%.0f,%.0f) cam=%u camptr=%p ctrl(mode=%u state=%u out=%d,%d) "
				"env(armed=%u formula_miss=%u ofs=%d,%d r=%.3f,%.3f) gpu(calls=%u mode_off=%u match=%u armed_nomatch=%u ofs=%d,%d) "
				"tiles(calls=%u shifted=%u layers_nonzero=%u) dbg=%ld\n",
				ff8_interp::frame_counter(), (unsigned)*CURRENT_FIELD_ID, stats.frames, stats.dt_max * 1000.0, stats.dt_over_10ms, stats.dt_over_20ms,
				stats.ticks, stats.blended, stats.not_blended, stats.snap_scene, stats.snap_big, stats.snap_direct,
				stats.camera_step_sum[0], stats.camera_step_sum[1], stats.layer_step_sum[0], stats.layer_step_sum[1],
				(unsigned)*CAMERA_INDEX, (void*)*CAMERA_PTR, camera_ctrl[CTRL_MODE], camera_ctrl[CTRL_STATE],
				*(const int16_t*)(camera_ctrl + CTRL_OUT), *(const int16_t*)(camera_ctrl + CTRL_OUT + 2),
				stats.env_armed, stats.env_formula_miss, stats.env_last_offset[0], stats.env_last_offset[1], stats.last_remainder[0], stats.last_remainder[1],
				stats.gpu_calls, stats.gpu_mode_off, stats.gpu_match, stats.gpu_nomatch_armed, stats.gpu_last_offset[0], stats.gpu_last_offset[1],
				stats.tile_calls, stats.tile_shifted, stats.tile_nonzero_layers, ff8_interp::cfg().field_debug);
			scroll_stats = ScrollStats();
		}

		// A cut between two ticks is shown as a cut: more than half a screen (320x240 space) in one tick,
		// or a direct placement (DSCROLL* = mode 3) of more than 8 px. Small direct steps are blended so a
		// script that re-issues DSCROLL every tick still moves smoothly.
		bool scroll_cut(const ScrollState& previous, const ScrollState& current, int k)
		{
			const int dx = current.out[k][0] - previous.out[k][0];
			const int dy = current.out[k][1] - previous.out[k][1];
			if (dx > 160 || dx < -160 || dy > 120 || dy < -120) return true;
			if (current.mode[k] == 3 && (dx > 8 || dx < -8 || dy > 8 || dy < -8)) return true;
			return false;
		}

		struct ScrollBlend
		{
			double blended[SCROLL_CTRLS][2]; // blended output, pixels
			int16_t rounded[SCROLL_CTRLS][2]; // rounded: what the engine is given
			double shake_blended[2];
			int8_t shake_rounded[2];
		};
		ScrollBlend scroll_blend_cache;
		uint32_t scroll_blend_frame = 0xFFFFFFFF;

		// Logic tick, right after 0x475E60.
		void scroll_capture()
		{
			ScrollState current;
			read_scroll_state(current);
			const bool scene_changed = !scroll_valid || *CURRENT_FIELD_ID != scroll_field || *CAMERA_INDEX != scroll_camera_index || *CAMERA_PTR != scroll_camera_ptr;
			scroll_stats.ticks++;
			if (scene_changed) scroll_stats.snap_scene++;
			for (int k = 0; k < SCROLL_CTRLS; k++)
			{
				const bool cut = !scene_changed && scroll_cut(scroll_next, current, k);
				if (cut)
				{
					const int dx = current.out[k][0] - scroll_next.out[k][0];
					const int dy = current.out[k][1] - scroll_next.out[k][1];
					if (dx > 160 || dx < -160 || dy > 120 || dy < -120)
						scroll_stats.snap_big++;
					else
						scroll_stats.snap_direct++;
				}
				if (!scene_changed)
					for (int axis = 0; axis < 2; axis++)
					{
						const double step = fabs(double(current.out[k][axis]) - scroll_next.out[k][axis]);
						if (k == (*CAMERA_INDEX & 7))
							scroll_stats.camera_step_sum[axis] += step;
						else
							scroll_stats.layer_step_sum[axis] += step;
					}
				memcpy(scroll_prev.out[k], (scene_changed || cut) ? current.out[k] : scroll_next.out[k], 4);
			}
			memcpy(scroll_prev.shake, scene_changed ? current.shake : scroll_next.shake, 2);
			scroll_next = current;
			scroll_valid = true;
			scroll_field = *CURRENT_FIELD_ID;
			scroll_camera_index = *CAMERA_INDEX;
			scroll_camera_ptr = *CAMERA_PTR;
			scroll_blend_frame = 0xFFFFFFFF;
		}

		inline int16_t round_to_pixel(double value) { return (int16_t)floor(value + 0.5); }
		const ScrollBlend* current_scroll_blend()
		{
			if (!scroll_valid || *CURRENT_FIELD_ID != scroll_field) return nullptr;
			if (scroll_blend_frame == ff8_interp::frame_counter()) return &scroll_blend_cache;
			const double t = tick_alpha; // rule 2.1: blended on the tick frame as well
			for (int k = 0; k < SCROLL_CTRLS; k++)
				for (int axis = 0; axis < 2; axis++)
				{
					scroll_blend_cache.blended[k][axis] = scroll_prev.out[k][axis] + (double(scroll_next.out[k][axis]) - scroll_prev.out[k][axis]) * t;
					scroll_blend_cache.rounded[k][axis] = round_to_pixel(scroll_blend_cache.blended[k][axis]);
				}
			for (int axis = 0; axis < 2; axis++)
			{
				scroll_blend_cache.shake_blended[axis] = scroll_prev.shake[axis] + (double(scroll_next.shake[axis]) - scroll_prev.shake[axis]) * t;
				scroll_blend_cache.shake_rounded[axis] = (int8_t)round_to_pixel(scroll_blend_cache.shake_blended[axis]);
			}
			scroll_blend_frame = ff8_interp::frame_counter();
			return &scroll_blend_cache;
		}

		struct ScrollSave
		{
			uint8_t out[SCROLL_CTRLS][4];
			int8_t shake[2];
		};
		void scroll_write(const ScrollBlend& blend, ScrollSave& save)
		{
			for (int k = 0; k < SCROLL_CTRLS; k++)
			{
				memcpy(save.out[k], scroll_controller(k) + CTRL_OUT, 4);
				memcpy(scroll_controller(k) + CTRL_OUT, blend.rounded[k], 4);
			}
			save.shake[0] = *SHAKE_OUT_X;
			save.shake[1] = *SHAKE_OUT_Y;
			*SHAKE_OUT_X = blend.shake_rounded[0];
			*SHAKE_OUT_Y = blend.shake_rounded[1];
		}
		void scroll_restore(const ScrollSave& save)
		{
			for (int k = 0; k < SCROLL_CTRLS; k++)
				memcpy(scroll_controller(k) + CTRL_OUT, save.out[k], 4);
			*SHAKE_OUT_X = save.shake[0];
			*SHAKE_OUT_Y = save.shake[1];
		}

		// Sub-pixel remainder of the DRAWENV offset of each field buffer, matched by value in the GPU
		// draw-env handlers (the display list may be walked after the next buffer was built).
		struct DrawEnvRemainder
		{
			bool armed;
			int16_t x, y;
			float remainder_x, remainder_y;
		};
		DrawEnvRemainder drawenv_remainder[2] = {};
		inline int16_t sign_extend_11(int value)
		{
			value &= 0x7FF;
			return (int16_t)(value >= 0x400 ? value - 0x800 : value);
		}
		int16_t* const FIELD_DRAW_BUFFER_INDEX = (int16_t*)0x1CDC6F0; // g_fieldDrawBufIndex
		inline int current_field_buffer() { return (*FIELD_DRAW_BUFFER_INDEX) & 1; }

		// Layer k (> camera controller c) of the tile list: offset = base - trunc(ratio * C / 256) - K + C
		// (C camera controller, K the layer's own controller). With C, K rounded for the engine and the
		// DRAWENV carrying the camera remainder, the tile still misses
		//   (Cb - Cw) - (Kb - Kw) - ratio * Cb / 256 + trunc(ratio * Cw / 256)   pixels,
		// added to its sprite in 1/8 pixel units.
		void shift_tiles_subpixel(const ScrollBlend& blend, uint8_t* prims)
		{
			if (*CAMERA_INDEX > 1 || *BG_TILE_RECORDS_PP == nullptr || prims == nullptr) return;
			const int camera = *CAMERA_INDEX; // 0x475480: layers 1..7 against controller 0, or 2..7 against 1
			int16_t shift_eighths[SCROLL_CTRLS][2] = {};
			bool any_shift = false;
			for (int k = camera + 1; k < SCROLL_CTRLS; k++)
				for (int axis = 0; axis < 2; axis++)
				{
					const int ratio = *(int16_t*)(scroll_controller(k) + CTRL_RATIO + 2 * axis);
					const double camera_blended = blend.blended[camera][axis];
					const int camera_rounded = blend.rounded[camera][axis];
					const double missing = (camera_blended - camera_rounded) - (blend.blended[k][axis] - blend.rounded[k][axis])
						- ratio * camera_blended / 256.0 + double((ratio * camera_rounded) / 256);
					shift_eighths[k][axis] = (int16_t)floor(missing * 8.0 + 0.5);
					any_shift |= shift_eighths[k][axis] != 0;
				}
			scroll_stats.tile_calls++;
			for (int k = 0; k < SCROLL_CTRLS; k++)
				if (shift_eighths[k][0] || shift_eighths[k][1]) scroll_stats.tile_nonzero_layers++;
			if (!any_shift) return;
			uint8_t* record = *(uint8_t**)*BG_TILE_RECORDS_PP;
			if (record == nullptr) return;
			for (int record_index = 0; record_index < 0x10000 && *(int16_t*)record != 0x7FFF; record_index++, record += 16, prims += 16)
			{
				const uint8_t layer = record[0xC];
				if (layer >= SCROLL_CTRLS) continue;
				// Tiles outside the screen keep their unshifted pixel coordinates but are not linked.
				*(int16_t*)(prims + 8) += shift_eighths[layer][0];
				*(int16_t*)(prims + 10) += shift_eighths[layer][1];
				if (shift_eighths[layer][0] || shift_eighths[layer][1]) scroll_stats.tile_shifted++;
			}
		}

		// ---- hooks -----------------------------------------------------------------------------
		// Pad reads at the head of the updater (every host frame): 0x49ED30 = held (-> 0x1CE48A0 / 0x1CE48B0),
		// 0x49EDE0 = press edges of the pad ring (-> 0x1CE48A8 / 0x1CE48B8, read by the VM: MESSYNC
		// 0x52994E closes a window on 0xC0, 0x52A1C8, 0x51DAA7). The engine sees one value per logic
		// tick (vanilla: one poll per 30 fps frame). v6: the words of every host frame since the previous
		// tick are OR-ed and published on the tick, so a press edge (which the ring holds for ONE host
		// frame) and a tap shorter than a tick are no longer lost (before: ~79% of edges at 144 Hz).
		// The updater's own prev/cur edge test (0x1CE48A4) then still yields one edge per press.
		uint32_t pad_published[2][2] = {};
		uint32_t pad_accumulated[2][2] = {};
		inline uint32_t pad_publish(int reader, int pad_index, uint32_t value)
		{
			if (ff8_interp::cfg().field_debug & DBG_PAD_ACCUMULATE_OFF)
			{
				if (is_logic_tick) pad_published[reader][pad_index] = value;
				return pad_published[reader][pad_index];
			}
			pad_accumulated[reader][pad_index] |= value;
			if (is_logic_tick)
			{
				pad_published[reader][pad_index] = pad_accumulated[reader][pad_index];
				pad_accumulated[reader][pad_index] = 0;
			}
			return pad_published[reader][pad_index];
		}
		uint32_t __cdecl pad_hook(int pad, int arg2)
		{
			if (!field_active()) return original_pad(pad, arg2);
			update_tick();
			return pad_publish(0, pad ? 1 : 0, original_pad(pad, arg2));
		}
		uint32_t __cdecl pad2_hook(int pad, int arg2)
		{
			if (!field_active()) return original_pad2(pad, arg2);
			update_tick();
			return pad_publish(1, pad ? 1 : 0, original_pad2(pad, arg2));
		}
		// SPUSYNC frame counter (see SPU_FRAMES): overwritten before the VM with the value it had after the
		// previous VM call plus one per logic tick; the main loop's per-host-frame increments in between
		// are dropped. spu_valid is cleared by interp_reset() (scene loads, where the engine zeroes it).
		uint32_t spu_frames_after_vm = 0;
		bool spu_valid = false;
		void __cdecl vm_hook(int drawbuf)
		{
			if (!field_active())
			{
				original_vm(drawbuf);
				return;
			}
			update_tick();
			write_logic_positions(); // render frames leave display values in the entities
			const bool spu_paced = !(ff8_interp::cfg().field_debug & DBG_SPU_OFF);
			if (spu_paced && spu_valid) *SPU_FRAMES = spu_frames_after_vm + (is_logic_tick ? 1 : 0);
			if (is_logic_tick)
			{
				original_vm(drawbuf);
				logic_tick_count++;
			}
			spu_frames_after_vm = *SPU_FRAMES; // SPUREADY in this tick may have zeroed it
			spu_valid = spu_paced;
		}
		void __cdecl motion_hook(int entities, int input)
		{
			if (!field_active())
			{
				original_motion(entities, input);
				return;
			}
			if (!is_logic_tick) return;
			original_motion(entities, input);
			int count = entity_count();
			if (count != snapshot_entity_count || *CURRENT_FIELD_ID != snapshot_field || *ENTITIES_PTR != snapshot_table)
				entity_snapshots_valid = false;
			snapshot_entity_count = count;
			snapshot_field = *CURRENT_FIELD_ID;
			snapshot_table = *ENTITIES_PTR;
			for (int i = 0; i < count; i++)
			{
				EntityState current;
				capture_entity(i, current);
				const bool teleport_cut = entity_snapshots_valid && teleported(entity_next[i], current);
				// v10 trace: per tick, every entity that moved - position step (fixed >> 12 units), whether
				// the > 256-unit teleport cut fired, and the model offset +0x1E0/+0x1E6/+0x1EC (not blended)
				if (ff8_interp::cfg().trace && entity_snapshots_valid)
				{
					const uint8_t* entity_data = *ENTITIES_PTR + i * ENTITY_SIZE;
					const int dx = (current.pos[0] - entity_next[i].pos[0]) >> 12;
					const int dy = (current.pos[1] - entity_next[i].pos[1]) >> 12;
					const int dz = (current.pos[2] - entity_next[i].pos[2]) >> 12;
					static int16_t last_model_offset[MAX_ENTITIES][3];
					const int16_t model_offset[3] = { *(const int16_t*)(entity_data + 0x1E0), *(const int16_t*)(entity_data + 0x1E6), *(const int16_t*)(entity_data + 0x1EC) };
					const bool model_offset_changed = i < MAX_ENTITIES && memcmp(model_offset, last_model_offset[i], sizeof(model_offset)) != 0;
					if (dx || dy || dz || teleport_cut || model_offset_changed || current.facing != entity_next[i].facing)
						ff8_interp::log_trace("ff8fent t=%u field=%u ent=%d pos=(%d,%d,%d) d=(%d,%d,%d) facing=%u%s off=(%d,%d,%d)\n", logic_tick_count, (unsigned)*CURRENT_FIELD_ID, i,
							current.pos[0] >> 12, current.pos[1] >> 12, current.pos[2] >> 12, dx, dy, dz, current.facing, teleport_cut ? " TELEPORT-CUT" : "",
							model_offset[0], model_offset[1], model_offset[2]);
					if (i < MAX_ENTITIES) memcpy(last_model_offset[i], model_offset, sizeof(model_offset));
				}
				if (!entity_snapshots_valid || teleport_cut)
					entity_prev[i] = current;
				else
					entity_prev[i] = entity_next[i];
				entity_next[i] = current;
			}
			entity_snapshots_valid = count > 0;
		}
		void __cdecl talk_push_hook(int player_entity, int npc_table) { if (!field_active() || is_logic_tick) original_talk_push(player_entity, npc_table); }
		void __cdecl talk_hook() { if (!field_active() || is_logic_tick) original_talk(); }
		void __cdecl scroll_tween_hook(uint8_t* controller) { if (!field_active() || is_logic_tick) original_scroll_tween(controller); }
		void __cdecl ctrl_reset_hook() { if (!field_active() || is_logic_tick) original_ctrl_reset(); }
		void __cdecl pan_hook()
		{
			if (!field_active())
			{
				original_pan();
				return;
			}
			const bool entities_blend = !(ff8_interp::cfg().field_debug & 2);
			if (!scroll_blend_enabled())
			{
				// pre-v7: every host frame, tracking fed the display entities (timed scrolls ran at host rate)
				if (entities_blend) write_display_positions();
				original_pan();
				return;
			}
			// v7: logic ticks only, on the logic entity values (vm_hook put them back this frame). Until the
			// first capture of a scene it keeps running every frame, so the first frames have a camera.
			if (is_logic_tick || !scroll_valid)
			{
				original_pan();
				if (is_logic_tick) scroll_capture();
			}
			if (entities_blend) write_display_positions();
		}

		// DRAWENV offset (0x476450): blended controllers + shake in, exact logic values back out, then the
		// sub-pixel remainder of what it wrote is armed for the GPU draw-env handlers.
		fn_int_int_int_t original_drawenv_scroll = (fn_int_int_int_t)FN_DRAWENV_SCROLL;
		void __cdecl drawenv_scroll_hook(int drawbuf, int envbase, int flag)
		{
			const int buffer = (uint32_t)drawbuf == DRAWBUF0 ? 0 : 1;
			drawenv_remainder[buffer].armed = false;
			const ScrollBlend* blend = (field_active() && scroll_blend_enabled()) ? current_scroll_blend() : nullptr;
			if (field_active()) scroll_diag_frame();
			if (blend == nullptr)
			{
				if (field_active()) scroll_stats.not_blended++;
				original_drawenv_scroll(drawbuf, envbase, flag);
				return;
			}
			scroll_stats.blended++;
			ScrollSave save;
			scroll_write(*blend, save);
			original_drawenv_scroll(drawbuf, envbase, flag);
			scroll_restore(save);
			const int camera = (int16_t)flag;
			if (!subpixel_enabled() || camera < 0 || camera >= SCROLL_CTRLS || *CAMERA_PTR == nullptr) return;
			const uint8_t* env = (const uint8_t*)envbase + buffer * DRAWENV_SIZE;
			const int16_t written_x = *(const int16_t*)(env + 8);
			const int16_t written_y = *(const int16_t*)(env + 10);
			// The movie branch ignores the controllers: arm only when the engine wrote exactly the scroll formula.
			const uint8_t* camera_data = *CAMERA_PTR;
			const int16_t expected_x = (int16_t)(*(const int16_t*)(camera_data + 0x20) - blend->rounded[camera][0] + *SCREEN_CENTER_X + blend->shake_rounded[0] + (buffer ? 0x200 : 0));
			const int16_t expected_y = (int16_t)(*(const int16_t*)(camera_data + 0x22) - blend->rounded[camera][1] + *SCREEN_CENTER_Y + blend->shake_rounded[1]);
			if (written_x != expected_x || written_y != expected_y)
			{
				scroll_stats.env_formula_miss++;
				return;
			}
			DrawEnvRemainder& remainder = drawenv_remainder[buffer];
			remainder.x = sign_extend_11(written_x);
			remainder.y = sign_extend_11(written_y);
			remainder.remainder_x = float(-(blend->blended[camera][0] - blend->rounded[camera][0]) + (blend->shake_blended[0] - blend->shake_rounded[0]));
			remainder.remainder_y = float(-(blend->blended[camera][1] - blend->rounded[camera][1]) + (blend->shake_blended[1] - blend->shake_rounded[1]));
			remainder.armed = true; // also gates the tile shift, which can be non-zero with a whole-pixel camera
			scroll_stats.env_armed++;
			scroll_stats.env_last_offset[0] = remainder.x;
			scroll_stats.env_last_offset[1] = remainder.y;
			scroll_stats.last_remainder[0] = remainder.remainder_x;
			scroll_stats.last_remainder[1] = remainder.remainder_y;
		}

		// GPU draw-env handlers E3/E4/E5 (display-list walk): after the engine recomputed the float vertex
		// offset from the integer draw offset, add the remainder when that offset is a field buffer's.
		fn_int_t original_gpu_e3 = (fn_int_t)FN_GPU_E3;
		fn_int_t original_gpu_e4 = (fn_int_t)FN_GPU_E4;
		fn_int_t original_gpu_e5 = (fn_int_t)FN_GPU_E5;
		void apply_drawenv_remainder()
		{
			if (!field_active() || !subpixel_enabled()) return;
			scroll_stats.gpu_calls++;
			scroll_stats.gpu_last_offset[0] = GPU_DRAW_OFS[0];
			scroll_stats.gpu_last_offset[1] = GPU_DRAW_OFS[1];
			if (*GPU_OFS_MODE != 1)
			{
				scroll_stats.gpu_mode_off++;
				return;
			}
			bool any_armed = false;
			for (const DrawEnvRemainder& remainder : drawenv_remainder)
			{
				any_armed |= remainder.armed;
				if (remainder.armed && remainder.x == GPU_DRAW_OFS[0] && remainder.y == GPU_DRAW_OFS[1])
				{
					GPU_VERTEX_OFS[0] += remainder.remainder_x * GPU_PX_SCALE[0];
					GPU_VERTEX_OFS[1] += remainder.remainder_y * GPU_PX_SCALE[1];
					scroll_stats.gpu_match++;
					return;
				}
			}
			if (any_armed) scroll_stats.gpu_nomatch_armed++;
		}
		void __cdecl gpu_e3_hook(int arg)
		{
			original_gpu_e3(arg);
			apply_drawenv_remainder();
		}
		void __cdecl gpu_e4_hook(int arg)
		{
			original_gpu_e4(arg);
			apply_drawenv_remainder();
		}
		void __cdecl gpu_e5_hook(int arg)
		{
			original_gpu_e5(arg);
			apply_drawenv_remainder();
		}
		void __cdecl pose_hook(int entities, int drawbuf)
		{
			if (!field_active() || is_logic_tick)
			{
				original_pose(entities, drawbuf);
				return;
			}
			// The head-look tween inside steps per call: keep it at logic rate.
			static uint8_t saved_head_look[MAX_ENTITIES][16];
			int count = entity_count();
			for (int i = 0; i < count; i++)
				memcpy(saved_head_look[i], entity(i) + ENTITY_HEAD_LOOK, 16);
			original_pose(entities, drawbuf);
			for (int i = 0; i < count; i++)
				memcpy(entity(i) + ENTITY_HEAD_LOOK, saved_head_look[i], 16);
		}
		ff8_chara::PoseInterp pose_interp;
		void __cdecl field_animate_hook(int arg1, int arg2)
		{
			if (!field_active() || !entity_snapshots_valid || (ff8_interp::cfg().field_debug & 4))
			{
				original_field_animate(arg1, arg2);
				return;
			}
			update_tick();
			pose_interp.animate(original_field_animate, arg1, arg2, is_logic_tick, tick_alpha, logic_tick_count);
		}
		void __cdecl chara_texanim_hook(int slot) { if (!field_active() || is_logic_tick) original_chara_texanim(slot); }
		void __cdecl particles_hook(int camera, int drawbuf, int pmd) { if (!field_active() || is_logic_tick) original_particles(camera, drawbuf, pmd); }
		void __cdecl gateway_hook(int player_entity, int gateways)
		{
			if (!field_active())
			{
				original_gateway(player_entity, gateways);
				return;
			}
			if (!is_logic_tick) return;
			write_logic_positions();
			original_gateway(player_entity, gateways);
		}
		void __cdecl fade_hook(int drawbuf)
		{
			if (!field_active() || is_logic_tick)
			{
				original_fade(drawbuf);
				return;
			}
			uint8_t saved_fade_state[4];
			memcpy(saved_fade_state, FADE_STATE, 4);
			original_fade(drawbuf); // draws every frame, level steps only on ticks
			memcpy(FADE_STATE, saved_fade_state, 4);
		}

		// ---- v6: host-rate counters -----------------------------------------------------------------
		typedef void (__cdecl *fn_int_int_int_int_t)(int, int, int, int);
		fn_int_t original_window_step = (fn_int_t)ff8_ui_pace::FN_WINDOW_STEP;
		fn_void_t original_transition_fade = (fn_void_t)FN_TRANSITION_FADE;
		fn_int_int_int_int_t original_bg_tiles = (fn_int_int_int_int_t)FN_BG_TILES;
		fn_int_int_int_int_t original_trail = (fn_int_int_int_int_t)FN_TRAIL;

		void __cdecl window_step_hook(int n)
		{
			if (!field_active())
			{
				original_window_step(n);
				return;
			}
			// v15: a press reaches the windows from the tick that published it to the script, see ui_pace.h.
			update_tick();
			const bool presses_released = is_logic_tick || (ff8_interp::cfg().field_debug & DBG_WINDOW_PRESS_SYNC_OFF);
			ff8_ui_pace::window_step(n, 60.0, !(ff8_interp::cfg().field_debug & DBG_WINDOW_OFF), presses_released);
		}

		// Runs in the main loop 0x46FEE0 (outside the updater, also in director flows 5..7 while the scene
		// is left), so it has its own 60 Hz budget rather than the logic tick.
		ff8_pace::Budget transition_budget;
		void __cdecl transition_fade_hook()
		{
			if (!field_active() || (ff8_interp::cfg().field_debug & DBG_TRANSITION_OFF) || *TRANSITION_FADE_MODE == 0)
			{
				original_transition_fade();
				return;
			}
			transition_budget.feed(60.0, 4.0);
			if (!transition_budget.take()) (*TRANSITION_FADE_TIMER)--; // u8; the call increments it back and draws
			original_transition_fade();
		}

		// Hold the background shade counters on host frames between ticks: the call still builds the
		// tile list (and the shaded colour of the current counter), the counter only advances on ticks.
		uint8_t bg_shade_saved[256][4];
		void bg_tiles_shaded(int arg1, int arg2, int prims, int arg4)
		{
			if (!field_active() || is_logic_tick || (ff8_interp::cfg().field_debug & DBG_BACKGROUND_FX_OFF) || *BG_ENTITIES_PTR == nullptr)
			{
				original_bg_tiles(arg1, arg2, prims, arg4);
				return;
			}
			const int count = *BG_ENTITY_COUNT;
			uint8_t* const bg_entities = *BG_ENTITIES_PTR;
			for (int i = 0; i < count; i++)
				memcpy(bg_shade_saved[i], bg_entities + i * BG_ENTITY_SIZE + BG_SHADE, 4);
			original_bg_tiles(arg1, arg2, prims, arg4);
			for (int i = 0; i < count; i++)
				memcpy(bg_entities + i * BG_ENTITY_SIZE + BG_SHADE, bg_shade_saved[i], 4);
		}
		// v7: the tile list reads the scroll controllers too (per-layer offsets, parallax); same blended
		// values as the DRAWENV this frame, then the per-layer sub-pixel shift.
		void __cdecl bg_tiles_hook(int arg1, int arg2, int prims, int arg4)
		{
			const ScrollBlend* blend = (field_active() && scroll_blend_enabled()) ? current_scroll_blend() : nullptr;
			if (blend == nullptr)
			{
				bg_tiles_shaded(arg1, arg2, prims, arg4);
				return;
			}
			ScrollSave save;
			scroll_write(*blend, save);
			bg_tiles_shaded(arg1, arg2, prims, arg4);
			scroll_restore(save);
			if (subpixel_enabled() && drawenv_remainder[current_field_buffer()].armed) shift_tiles_subpixel(*blend, (uint8_t*)prims);
		}

		// v9 draw point trails. The update (0x474AC0: respawn pass + new head, then progress / lifetime)
		// runs on logic ticks only; the draw runs every frame from a scratch copy of the slot whose ring
		// points are blended between the last two ticks (one tick behind, like the entities). The ring is
		// refilled with the start point on respawn, so the jump back to the base stays a cut. Pre-v9 the
		// whole call ran every frame with progress and flags put back, and a slot that died on a tick was
		// respawned (randomly aimed) on each in-between call too.
		typedef void (__cdecl *trail_update_t)(uint8_t* slot, uint8_t* ctrl);
		typedef void (__cdecl *trail_draw_t)(uint8_t* slot, int ot, int prims, int prims2);
		uint8_t trail_scratch[TRAIL_STRIDE];
		uint8_t trail_dying[TRAIL_SLOTS]; // flag of a slot that died on the last tick: vanilla drew it that call

		void trail_blended(int transform, int ot, int prims, int prims2)
		{
			update_tick(); // once per frame; the update below must not run twice for one tick
			if (is_logic_tick)
			{
				memset(trail_dying, 0, sizeof(trail_dying));
				for (int i = 0; i < TRAIL_SLOTS; i++)
				{
					if (TRAIL_FLAGS[i] == 0) continue;
					uint8_t* const slot = TRAIL_SLOT_BASE + i * TRAIL_STRIDE;
					((trail_update_t)FN_TRAIL_UPDATE)(slot, TRAIL_CTRL_BASE + i * TRAIL_CTRL_STRIDE);
					int16_t& progress = *(int16_t*)(slot + TRAIL_PROGRESS_OFFSET);
					progress++;
					if (progress > *(int16_t*)(slot + TRAIL_LENGTH) + 4)
					{
						trail_dying[i] = TRAIL_FLAGS[i];
						TRAIL_FLAGS[i] = 0;
					}
				}
			}
			const float t = tick_alpha;
			((fn_void_t)FN_GTE_PUSH)();
			((fn_int_t)FN_GTE_ROT)(transform);
			((fn_int_t)FN_GTE_TRANS)(transform);
			for (int i = 0; i < TRAIL_SLOTS; i++)
			{
				const uint8_t flag = TRAIL_FLAGS[i] ? TRAIL_FLAGS[i] : trail_dying[i];
				if (!(flag == 2 || (flag == 1 && *TRAIL_SHOWN == 1))) continue;
				const uint8_t* const slot = TRAIL_SLOT_BASE + i * TRAIL_STRIDE;
				memcpy(trail_scratch, slot, TRAIL_STRIDE);
				// The draw reads the head at progress & 7 and the five entries behind it.
				const int head = *(const int16_t*)(slot + TRAIL_PROGRESS_OFFSET) - 1;
				*(int16_t*)(trail_scratch + TRAIL_PROGRESS_OFFSET) = (int16_t)head;
				for (int j = 0; j <= 5; j++)
				{
					const int entry = (head - j) & 7;
					const int previous_entry = (head - j - 1) & 7;
					for (uint32_t ring : { 0u, TRAIL_RING_B })
						for (int k = 0; k < 3; k++)
						{
							const int16_t previous_coord = *(const int16_t*)(slot + ring + previous_entry * 8 + k * 2);
							const int16_t current_coord = *(const int16_t*)(slot + ring + entry * 8 + k * 2);
							const float blended = float(previous_coord) + float(current_coord - previous_coord) * t;
							*(int16_t*)(trail_scratch + ring + entry * 8 + k * 2) = (int16_t)(blended >= 0.0f ? blended + 0.5f : blended - 0.5f);
						}
				}
				((trail_draw_t)FN_TRAIL_DRAW)(trail_scratch, ot, prims + i * 0xB4, prims2 + i * 0x20);
			}
			((fn_void_t)FN_GTE_POP)();
		}

		void __cdecl trail_hook(int transform, int ot, int prims, int prims2)
		{
			if (!field_active() || (ff8_interp::cfg().field_debug & DBG_BACKGROUND_FX_OFF))
			{
				original_trail(transform, ot, prims, prims2);
				return;
			}
			if (!(ff8_interp::cfg().field_debug & DBG_TRAIL_BLEND_OFF))
			{
				trail_blended(transform, ot, prims, prims2);
				return;
			}
			if (is_logic_tick)
			{
				original_trail(transform, ot, prims, prims2);
				return;
			}
			uint8_t saved_flags[TRAIL_SLOTS];
			int16_t saved_progress[TRAIL_SLOTS];
			memcpy(saved_flags, TRAIL_FLAGS, TRAIL_SLOTS);
			for (int i = 0; i < TRAIL_SLOTS; i++)
				saved_progress[i] = *(int16_t*)(TRAIL_PROGRESS + i * TRAIL_STRIDE);
			original_trail(transform, ot, prims, prims2);
			memcpy(TRAIL_FLAGS, saved_flags, TRAIL_SLOTS);
			for (int i = 0; i < TRAIL_SLOTS; i++)
				*(int16_t*)(TRAIL_PROGRESS + i * TRAIL_STRIDE) = saved_progress[i];
		}
	}

	void interp_hook_init()
	{
		if (!ff8_interp::cfg().field_enabled) return;
		if (!ff8_interp::is_us_exe())
		{
			ff8_interp::log_warning("%s: field interpolation only verified on the US executable, skipping.\n", __func__);
			return;
		}
		field_fps = ff8_interp::cfg().field_fps > 0 ? (int)ff8_interp::cfg().field_fps : 60;
		if (field_fps < LOGIC_FPS) field_fps = LOGIC_FPS;
		if (field_fps > 1000) field_fps = 1000;
		// FFNx 1.24.3 canary builds from mid-October 2026 on (field movie sync, FFNx #1014) redirect the
		// script VM, motion, shake, scroll, pose and fade calls to wrappers that only differ from the engine
		// function while a movie faster than 15 fps plays. Those sites chain to whatever they reach.
		chain_call(CALL_SITE_SCRIPT_VM, (void*)vm_hook, original_vm);
		for (uint32_t call_site : CALL_SITE_PAD_READ)
			ff8_interp::hook_call(call_site, (void*)pad_hook);
		for (uint32_t call_site : CALL_SITE_PAD_READ2)
			ff8_interp::hook_call(call_site, (void*)pad2_hook);
		chain_call(CALL_SITE_MOTION, (void*)motion_hook, original_motion);
		ff8_interp::hook_call(CALL_SITE_TALK_PUSH, (void*)talk_push_hook);
		ff8_interp::hook_call(CALL_SITE_TALK, (void*)talk_hook);
		for (uint32_t call_site : CALL_SITE_SCROLL_TWEEN)
			chain_call(call_site, (void*)scroll_tween_hook, original_scroll_tween);
		ff8_interp::hook_call(CALL_SITE_CTRL_RESET, (void*)ctrl_reset_hook);
		chain_call(CALL_SITE_PAN, (void*)pan_hook, original_pan);
		chain_call(CALL_SITE_POSE, (void*)pose_hook, original_pose);
		ff8_interp::hook_call(CALL_SITE_PARTICLES, (void*)particles_hook);
		ff8_interp::hook_call(CALL_SITE_GATEWAY, (void*)gateway_hook);
		chain_call(CALL_SITE_FADE, (void*)fade_hook, original_fade);
		pose_interp.instance_table = FIELD_INSTANCES;
		ff8_interp::hook_call(CALL_SITE_FIELD_ANIMATE, (void*)field_animate_hook);
		ff8_interp::hook_call(CALL_SITE_CHARA_TEXANIM, (void*)chara_texanim_hook);
		int clock_sites = 0;
		for (uint32_t call_site : CALL_SITE_PLAY_CLOCK)
			clock_sites += ff8_play_clock::patch_site(call_site, (void*)play_clock_hook, __func__) ? 1 : 0;
		ff8_ui_pace::install(__func__);
		ff8_interp::hook_call(CALL_SITE_WINDOW_STEP, (void*)window_step_hook);
		for (uint32_t call_site : CALL_SITE_TRANSITION_FADE)
			ff8_interp::hook_call(call_site, (void*)transition_fade_hook);
		ff8_interp::hook_call(CALL_SITE_BG_TILES, (void*)bg_tiles_hook);
		ff8_interp::hook_call(CALL_SITE_TRAIL, (void*)trail_hook);
		// v7 scroll interpolation (the pan 0x476C0D and tile list 0x476CFE sites are hooked above)
		ff8_interp::hook_call(CALL_SITE_DRAWENV_SCROLL, (void*)drawenv_scroll_hook);
		for (uint32_t call_site : CALL_SITE_GPU_E3)
			ff8_interp::hook_call(call_site, (void*)gpu_e3_hook);
		for (uint32_t call_site : CALL_SITE_GPU_E4)
			ff8_interp::hook_call(call_site, (void*)gpu_e4_hook);
		for (uint32_t call_site : CALL_SITE_GPU_E5)
			ff8_interp::hook_call(call_site, (void*)gpu_e5_hook);
		installed = true;
		ff8_interp::log_info("%s: field interpolation v15 enabled (field %d fps, logic %d fps, play clock %d/4 sites).\n", __func__, field_fps, LOGIC_FPS, clock_sites);
	}

	bool interp_installed() { return installed; }

	void interp_reset()
	{
		entity_snapshots_valid = false;
		snapshot_entity_count = 0;
		memset(pad_published, 0, sizeof(pad_published));
		memset(pad_accumulated, 0, sizeof(pad_accumulated));
		spu_valid = false;
		memset(trail_dying, 0, sizeof(trail_dying));
		scroll_valid = false;
		scroll_blend_frame = 0xFFFFFFFF;
		memset(drawenv_remainder, 0, sizeof(drawenv_remainder));
		tick_host_frame = 0xFFFFFFFF;
		tick_accumulator = 0.0;
		ff8_tick::clock_reset();
		pose_interp.reset();
	}

	void interp_on_frame()
	{
		if (!installed) return;
		int mode = (int)ff8_interp::mode_id();
		if (mode != last_mode)
		{
			last_mode = mode;
			interp_reset();
		}
		else if (ff8_interp::game_mode() == ff8_interp::GameMode::Field && *FIELD_FLOW != 4)
		{
			interp_reset(); // loading / leaving a scene
		}
	}
}
