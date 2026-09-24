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
//
// Engine facts: ~/ff8/notes/engine/field.md

#include "interp.h"
#include "../common/tick.h"
#include "../common/chara_pose.h"
#include "../common/play_clock.h"

#include "../host.h"

#include <windows.h>
#include <stdint.h>
#include <string.h>

namespace ff8_field
{
	namespace
	{
		constexpr int LOGIC_FPS = 30;

		// Frame updater 0x4767B0 call sites (FF8 2000 == Steam)
		constexpr uint32_t FN_SCRIPT_VM = 0x529FF0;     // void (int drawbuf): scripts + entity animation frame stepping
		constexpr uint32_t CS_SCRIPT_VM = 0x4768FE;
		constexpr uint32_t FN_PAD_READ = 0x49ED30;      // uint32 (int pad, int): pad state
		constexpr uint32_t CS_PAD_READ[2] = { 0x476841, 0x476859 };
		constexpr uint32_t FN_PAD_READ2 = 0x49EDE0;     // uint32 (int pad, int)
		constexpr uint32_t CS_PAD_READ2[2] = { 0x47684D, 0x476865 };
		constexpr uint32_t FN_MOTION = 0x4789A0;        // void (entities, input): player/scripted movement, jumps, ladders
		constexpr uint32_t CS_MOTION = 0x476BBF;
		constexpr uint32_t FN_TALK_PUSH = 0x4777F0;     // void (player entity, npc table)
		constexpr uint32_t CS_TALK_PUSH = 0x476BE7;
		constexpr uint32_t FN_TALK = 0x4796E0;          // void ()
		constexpr uint32_t CS_TALK = 0x476BEC;
		constexpr uint32_t FN_SCROLL_TWEEN = 0x4765C0;  // void (ctrl*): script scroll tween step
		constexpr uint32_t CS_SCROLL_TWEEN[2] = { 0x476BF6, 0x476C00 };
		constexpr uint32_t FN_CTRL_RESET = 0x475DF0;    // void (): layer controller state step
		constexpr uint32_t CS_CTRL_RESET = 0x476C08;
		constexpr uint32_t FN_PAN = 0x475E60;           // void (): camera pan from the player entity
		constexpr uint32_t CS_PAN = 0x476C0D;
		constexpr uint32_t FN_POSE = 0x472B30;          // void (entities, drawbuf): model transforms, head look, skeletons
		constexpr uint32_t CS_POSE = 0x476C79;
		constexpr uint32_t FN_PARTICLES = 0x473DE0;     // void (cam, drawbuf, pmd): emitter update
		constexpr uint32_t CS_PARTICLES = 0x476D82;
		constexpr uint32_t FN_GATEWAY = 0x47BD20;       // void (player entity, gateways)
		constexpr uint32_t CS_GATEWAY = 0x4770C5;
		constexpr uint32_t FN_FADE = 0x47B980;          // void (drawbuf): fade step + draw
		constexpr uint32_t CS_FADE = 0x4770D0;
		constexpr uint32_t FN_FIELD_ANIMATE = 0x533CD0; // void (int, int): decode + skeletons, table 0x1DCB340
		constexpr uint32_t CS_FIELD_ANIMATE = 0x530A8B; // in 0x530810 (from 0x472B30)
		// Character texture animation (eye blink): a random timer plus VRAM rectangle copies, stepped once
		// per call from its single call site. At host rate the blink is several times too fast.
		constexpr uint32_t FN_CHARA_TEXANIM = 0x532930; // void (int slot)
		constexpr uint32_t CS_CHARA_TEXANIM = 0x530840;
		// Play clock 0x4701B0 (play time + event countdown, one 1/60 s step per call), called by the field
		// mode loop twice per 30 fps frame, and twice more while 0x209A798 != 0 (the 15 fps movie path,
		// which doubles 0x472990 the same way): 60 steps/s either way. See play_clock.h.
		constexpr uint32_t CS_PLAY_CLOCK[4] = { 0x46FFF9, 0x46FFFE, 0x47000C, 0x470011 };

		// ff8_field_interp_debug bits. 2 = no entity blend, 4 = no pose blend (literals below, pre-v5);
		// the v5 bits are named.
		constexpr long DBG_PLAY_CLOCK_OFF = 8; // play clock at host rate again (pre-v5: 4.8x fast at 144 Hz)
		constexpr long DBG_ALPHA_LEAD = 16;    // pre-v5 tick_alpha = (tick_acc + dt) / P (one host frame lead)

		uint8_t** const ENTITIES_PTR = (uint8_t**)0x1D9CF88; // 612-byte character entities
		uint8_t* const ENTITY_COUNT = (uint8_t*)0x1D9D019;
		constexpr uint32_t ENTITY_SIZE = 0x264;
		constexpr uint32_t ENT_POS = 0x190;          // i32 x, y, z (fixed point >> 12)
		constexpr uint32_t ENT_MODEL_FACING = 0x241; // u8, 256 per turn
		constexpr uint32_t ENT_HEADLOOK = 0x228;     // 8 x i16 head-look tween (stepped by 0x472B30 every call)
		uint8_t** const FIELD_INSTANCES = (uint8_t**)0x1DCB340;
		uint8_t* const FADE_STATE = (uint8_t*)0x1CE486A; // i16 level + i16 step, stepped by 0x47B980 every call
		int32_t* const FIELD_FLOW = (int32_t*)0x1CE4A64;  // director state, 4 = running
		uint16_t* const CURRENT_FIELD_ID = (uint16_t*)0x1CD2FC0;
		constexpr int MAX_ENTITIES = 64;

		typedef void (__cdecl *vm_t)(int);
		typedef uint32_t (__cdecl *pad_t)(int, int);
		typedef void (__cdecl *fn2_t)(int, int);
		typedef void (__cdecl *fn0_t)();
		typedef void (__cdecl *fn1p_t)(uint8_t*);
		typedef void (__cdecl *fn3_t)(int, int, int);
		typedef void (__cdecl *fn1_t)(int);

		vm_t orig_vm = (vm_t)FN_SCRIPT_VM;
		pad_t orig_pad = (pad_t)FN_PAD_READ;
		pad_t orig_pad2 = (pad_t)FN_PAD_READ2;
		fn2_t orig_motion = (fn2_t)FN_MOTION;
		fn2_t orig_talk_push = (fn2_t)FN_TALK_PUSH;
		fn0_t orig_talk = (fn0_t)FN_TALK;
		fn1p_t orig_scroll_tween = (fn1p_t)FN_SCROLL_TWEEN;
		fn0_t orig_ctrl_reset = (fn0_t)FN_CTRL_RESET;
		fn0_t orig_pan = (fn0_t)FN_PAN;
		fn2_t orig_pose = (fn2_t)FN_POSE;
		fn3_t orig_particles = (fn3_t)FN_PARTICLES;
		fn2_t orig_gateway = (fn2_t)FN_GATEWAY;
		fn1_t orig_fade = (fn1_t)FN_FADE;
		fn1_t orig_chara_texanim = (fn1_t)FN_CHARA_TEXANIM;
		fn2_t orig_field_animate = (fn2_t)FN_FIELD_ANIMATE;

		int field_fps = 60;
		bool installed = false;
		int last_mode = -1;

		uint32_t tick_frame = 0xFFFFFFFF;
		double tick_acc = 0.0;
		bool tick_real = true;
		float tick_alpha = 1.0f;
		uint32_t logic_ticks = 0;

		constexpr double LOGIC_PERIOD = 1.0 / double(LOGIC_FPS);

		// Wall-clock gate: ff8_field_fps only caps the render rate, the logic rate must not depend on it.
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
			// v5: the time since the last tick, tick_acc / P. `tick_acc` already holds this frame's dt, so
			// the pre-v5 (tick_acc + dt) / P counted it twice: one host frame of lead and alpha pinned to 1
			// on the last frame before every tick, an uneven step once per tick. DBG_ALPHA_LEAD restores it.
			tick_alpha = float(((ff8_interp::cfg().field_debug & DBG_ALPHA_LEAD) ? tick_acc + dt : tick_acc) / LOGIC_PERIOD);
			if (tick_alpha > 1.0f) tick_alpha = 1.0f;
			if (tick_alpha < 0.0f) tick_alpha = 0.0f;
		}

		inline bool active() { return ff8_interp::game_mode() == ff8_interp::GameMode::Field; }

		void __cdecl play_clock_hook() { ff8_play_clock::run(!(ff8_interp::cfg().field_debug & DBG_PLAY_CLOCK_OFF)); }

		struct EntState { int32_t pos[3]; uint8_t facing; };
		EntState ent_prev[MAX_ENTITIES], ent_next[MAX_ENTITIES];
		int ent_count = 0;
		bool ent_valid = false;
		uint16_t snap_field = 0xFFFF;   // field the snapshots belong to
		uint8_t* snap_table = nullptr;  // entity table the snapshots belong to

		inline int entity_count() { int n = *ENTITY_COUNT; return n > MAX_ENTITIES ? MAX_ENTITIES : n; }
		inline uint8_t* entity(int i) { return *ENTITIES_PTR + i * ENTITY_SIZE; }
		void capture_entity(int i, EntState& s)
		{
			uint8_t* e = entity(i);
			memcpy(s.pos, e + ENT_POS, sizeof(s.pos));
			s.facing = e[ENT_MODEL_FACING];
		}
		void write_entity(int i, const EntState& s)
		{
			uint8_t* e = entity(i);
			memcpy(e + ENT_POS, s.pos, sizeof(s.pos));
			e[ENT_MODEL_FACING] = s.facing;
		}
		inline int32_t lerp_i32(int32_t a, int32_t b, float t)
		{
			double v = double(b - a) * t;
			return a + (int32_t)(v + (v >= 0 ? 0.5 : -0.5));
		}
		inline uint8_t lerp_facing(uint8_t a, uint8_t b, float t)
		{
			int d = ((int(b) - int(a) + 128) & 255) - 128;
			float v = d * t;
			return (uint8_t)(int(a) + (int)(v + (v >= 0.0f ? 0.5f : -0.5f)));
		}
		bool teleported(const EntState& a, const EntState& b)
		{
			for (int k = 0; k < 3; k++)
			{
				int32_t d = b.pos[k] - a.pos[k];
				if (d > 0x100000 || d < -0x100000) return true; // > 256 units in one tick: scripted placement
			}
			return false;
		}
		// Snapshots are only meaningful for the scene they were taken in: a field jump keeps
		// MODE_FIELD but reloads the entity table (positions placed by 0x477C90).
		inline void check_scene()
		{
			if (ent_valid && (*CURRENT_FIELD_ID != snap_field || *ENTITIES_PTR != snap_table || entity_count() != ent_count))
				ent_valid = false;
		}
		void write_logic()
		{
			check_scene();
			if (!ent_valid) return;
			for (int i = 0; i < ent_count; i++) write_entity(i, ent_next[i]);
		}
		void write_display()
		{
			check_scene();
			if (!ent_valid) return;
			for (int i = 0; i < ent_count; i++)
			{
				EntState o;
				for (int k = 0; k < 3; k++) o.pos[k] = lerp_i32(ent_prev[i].pos[k], ent_next[i].pos[k], tick_alpha);
				o.facing = lerp_facing(ent_prev[i].facing, ent_next[i].facing, tick_alpha);
				write_entity(i, o);
			}
		}

		// ---- hooks -----------------------------------------------------------------------------
		uint32_t pad_last[2][2] = {};
		uint32_t __cdecl pad_hook(int pad, int b)
		{
			if (!active()) return orig_pad(pad, b);
			update_tick();
			int i = pad ? 1 : 0;
			if (tick_real) pad_last[0][i] = orig_pad(pad, b);
			return pad_last[0][i];
		}
		uint32_t __cdecl pad2_hook(int pad, int b)
		{
			if (!active()) return orig_pad2(pad, b);
			update_tick();
			int i = pad ? 1 : 0;
			if (tick_real) pad_last[1][i] = orig_pad2(pad, b);
			return pad_last[1][i];
		}
		void __cdecl vm_hook(int drawbuf)
		{
			if (!active()) { orig_vm(drawbuf); return; }
			update_tick();
			write_logic(); // render frames leave display values in the entities
			if (!tick_real) return;
			orig_vm(drawbuf);
			logic_ticks++;
		}
		void __cdecl motion_hook(int a, int b)
		{
			if (!active()) { orig_motion(a, b); return; }
			if (!tick_real) return;
			orig_motion(a, b);
			int n = entity_count();
			if (n != ent_count || *CURRENT_FIELD_ID != snap_field || *ENTITIES_PTR != snap_table) ent_valid = false;
			ent_count = n;
			snap_field = *CURRENT_FIELD_ID;
			snap_table = *ENTITIES_PTR;
			for (int i = 0; i < n; i++)
			{
				EntState now;
				capture_entity(i, now);
				if (!ent_valid || teleported(ent_next[i], now)) ent_prev[i] = now; else ent_prev[i] = ent_next[i];
				ent_next[i] = now;
			}
			ent_valid = n > 0;
		}
		void __cdecl talk_push_hook(int a, int b) { if (!active() || tick_real) orig_talk_push(a, b); }
		void __cdecl talk_hook() { if (!active() || tick_real) orig_talk(); }
		void __cdecl scroll_tween_hook(uint8_t* c) { if (!active() || tick_real) orig_scroll_tween(c); }
		void __cdecl ctrl_reset_hook() { if (!active() || tick_real) orig_ctrl_reset(); }
		void __cdecl pan_hook()
		{
			if (active() && !(ff8_interp::cfg().field_debug & 2)) write_display();
			orig_pan();
		}
		void __cdecl pose_hook(int a, int b)
		{
			if (!active() || tick_real) { orig_pose(a, b); return; }
			// The head-look tween inside steps per call: keep it at logic rate.
			static uint8_t saved[MAX_ENTITIES][16];
			int n = entity_count();
			for (int i = 0; i < n; i++) memcpy(saved[i], entity(i) + ENT_HEADLOOK, 16);
			orig_pose(a, b);
			for (int i = 0; i < n; i++) memcpy(entity(i) + ENT_HEADLOOK, saved[i], 16);
		}
		ff8_chara::PoseInterp pose;
		void __cdecl field_animate_hook(int p1, int p2)
		{
			if (!active() || !ent_valid || (ff8_interp::cfg().field_debug & 4)) { orig_field_animate(p1, p2); return; }
			update_tick();
			pose.animate(orig_field_animate, p1, p2, tick_real, tick_alpha, logic_ticks);
		}
		void __cdecl chara_texanim_hook(int slot) { if (!active() || tick_real) orig_chara_texanim(slot); }
		void __cdecl particles_hook(int a, int b, int c) { if (!active() || tick_real) orig_particles(a, b, c); }
		void __cdecl gateway_hook(int a, int b)
		{
			if (!active()) { orig_gateway(a, b); return; }
			if (!tick_real) return;
			write_logic();
			orig_gateway(a, b);
		}
		void __cdecl fade_hook(int drawbuf)
		{
			if (!active() || tick_real) { orig_fade(drawbuf); return; }
			uint8_t saved[4];
			memcpy(saved, FADE_STATE, 4);
			orig_fade(drawbuf); // draws every frame, level steps only on ticks
			memcpy(FADE_STATE, saved, 4);
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
		ff8_interp::hook_call(CS_SCRIPT_VM, (void*)vm_hook);
		for (uint32_t cs : CS_PAD_READ) ff8_interp::hook_call(cs, (void*)pad_hook);
		for (uint32_t cs : CS_PAD_READ2) ff8_interp::hook_call(cs, (void*)pad2_hook);
		ff8_interp::hook_call(CS_MOTION, (void*)motion_hook);
		ff8_interp::hook_call(CS_TALK_PUSH, (void*)talk_push_hook);
		ff8_interp::hook_call(CS_TALK, (void*)talk_hook);
		for (uint32_t cs : CS_SCROLL_TWEEN) ff8_interp::hook_call(cs, (void*)scroll_tween_hook);
		ff8_interp::hook_call(CS_CTRL_RESET, (void*)ctrl_reset_hook);
		ff8_interp::hook_call(CS_PAN, (void*)pan_hook);
		ff8_interp::hook_call(CS_POSE, (void*)pose_hook);
		ff8_interp::hook_call(CS_PARTICLES, (void*)particles_hook);
		ff8_interp::hook_call(CS_GATEWAY, (void*)gateway_hook);
		ff8_interp::hook_call(CS_FADE, (void*)fade_hook);
		pose.table = FIELD_INSTANCES;
		ff8_interp::hook_call(CS_FIELD_ANIMATE, (void*)field_animate_hook);
		ff8_interp::hook_call(CS_CHARA_TEXANIM, (void*)chara_texanim_hook);
		int clock_sites = 0;
		for (uint32_t cs : CS_PLAY_CLOCK) clock_sites += ff8_play_clock::patch_site(cs, (void*)play_clock_hook, __func__) ? 1 : 0;
		installed = true;
		ff8_interp::log_info("%s: field interpolation v5 enabled (field %d fps, logic %d fps, play clock %d/4 sites).\n", __func__, field_fps, LOGIC_FPS, clock_sites);
	}

	bool interp_installed() { return installed; }

	void interp_reset()
	{
		ent_valid = false;
		ent_count = 0;
		memset(pad_last, 0, sizeof(pad_last));
		tick_frame = 0xFFFFFFFF;
		tick_acc = 0.0;
		ff8_tick::clock_reset();
		pose.reset();
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
