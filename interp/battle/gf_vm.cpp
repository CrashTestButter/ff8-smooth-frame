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

// GF cinematic pacing + interpolation for the "animation-VM family" (Ifrit, Bahamut, Cerberus,
// Alexander, Brothers, Eden, Leviathan). Shares the battle module's tick state through internal.h;
// gated by DBG_GF_VM_OFF (this family only) and DBG_GF_PACE_OFF (every GF).
//
// The family is ONE engine compiled once per module (same instruction stream, module-private data
// and function addresses): the "shared cinematic engine" of the wiki census, effect slots 6 and
// 201..206 for the GFs (plus four boss cinematics, 18/75/77/208 - not rows: 75/77/208 arrive as
// monster abilities (command 0x08) and are already paced by the spell path, 18 is unreachable; see
// the comment under VM_TABLE). Each module:
//   * Entry = MagicList_Logic[eff-1]: builds a one-node root queue and links its TICK into it with
//     BdLinkTask 0x508360 (`push tick / push queue / call 0x508360`). The tick is the effect root.
//   * tick (e.g. Ifrit 0xB25DF0), void task, ignores its node:
//       setup (per-GF, loads the shared globals 0x27973EC ctx / 0x27973B8 runtime slot / ... from the
//       module's own data and Field_Alloc(0x180));  ctx+0x32 ++ (arena parity only);  rand();
//       ctx+0x35 = (*0x1D96A9C & 1) ? 0xFF : 0;          <- THE engine freeze bit, read here only
//       ... copies the view matrix, picks the packet arena from the parity ...
//       if (ctx+0x35 == 0) { advance channels (-) ; integrate ; advance channels (+) }
//       draw (per-GF, e.g. 0xB2ABE0) ; 0x56CD00(roll) ; Field_Free ; return done ? 2 : 0
//     The two "advance" calls are the byte-code VM: per object, three channels, each a stream of
//     opcodes (table per module) with its own wait timer. EVERY event lives in an opcode handler:
//     sounds (0x501330 / 0x5018C0 / 0x501860 / 0x4A29A0), damage (0x506690 / 0x506BA0), the camera
//     writes (0xB8B7F0 / F8, roll 0x1D8E038), the battle camera animation start 0x5099A0, loads.
//     The integrator is per object: vel += acc, accum += vel, rot out +0x8C/+0x8E/+0x90 =
//     accum >> 16, then a per-type update (table) that derives the position out +0x94/+0x96/+0x98.
//   * So the module is written against the freeze bit exactly like Shiva: with bit 0 set for the one
//     tick call, the VM, every event and every integrator hold and the tick still draws a complete
//     frame. The held call's draw side was closed statically (every function reachable from the tick
//     with the three VM calls removed, all draw-table entries followed, depth unlimited, engine
//     callees included): its only one-shots are VRAM load appends 0x505DF0 in a texture-scroll draw
//     type (2..4 sites per module, suppressed here while held) and the standard battle-model reader
//     0x508F90, which animations.cpp already paces and interpolates per host frame. No engine callee
//     on that path reads 0x1D96A9C.
//
// What this file does:
//   1. HOLD: the Entry's BdLinkTask call site is replaced so the linked task is a per-row thunk
//      instead of the tick (no prologue is copied anywhere - every tick starts with a relative CALL,
//      which a trampoline could not carry). On a skipped host frame the thunk runs the tick with
//      *0x1D96A9C |= 1 and restores only bit 0 afterwards; on a logic tick it runs it unchanged. The
//      tick is never skipped.
//   2. INTERPOLATION: the per-module draw call site inside the tick is replaced. That point sits AFTER
//      the VM has run (tick frames) and BEFORE anything is drawn, on held and tick frames alike, so
//      it is where the objects' output transforms are captured (tick frames: prev <- next <- now) and
//      where lerp(prev, next, alpha) is written for the draw and put back afterwards - on the tick
//      frame too (rule 2.1). Pairs snap on large deltas, identity changes and non-consecutive ticks.
//   3. CAMERA: the VM writes the battle camera pose itself (camera-follow opcode). While the engine's
//      own camera sequence is idle (0x1D97718 == 0 and 0x1D9771E == 0) this file owns the camera
//      (gf_cinematic_owns_camera() is true, battle_camera_hook stands down) and blends it like
//      gf_pacing.cpp does; while a camera animation the VM started through 0x5099A0 runs, the battle
//      camera hook keeps it.
//   4. WATCHDOG: a hash of the VM's progress (stream pointers, wait timers, accumulators of the
//      update list) is taken on every non-held call; 2 s without a change releases the hold (the tick
//      runs unpaced, as vanilla) until it changes again.

#include "internal.h"

#include "../host.h"

#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <string.h>

namespace ff8_battle_anim
{
	namespace
	{
		// --- shared engine addresses (the same for every module of the family) ---------------------

		constexpr uint32_t FN_SPAWN_TASK = 0x508360;   // BdLinkTask(queue, fn) -> node
		constexpr uint32_t FN_VRAM_LOAD  = 0x505DF0;   // appends a VRAM load op to 0x1D98220 (count 0x1D98420, 0x20 slots)

		uint8_t** const VM_CONTEXT        = (uint8_t**)0x27973EC; // g_GfCinematic_SequenceCtxPtr (set by each tick's setup)
		uint8_t** const VM_CURRENT_OBJECT = (uint8_t**)0x27973E8; // the object being updated / drawn
		const uint8_t* const VM_UPDATE_LIST = (const uint8_t*)0x2797454; // object indices (bit 7 = solo flag), 0xFF ends
		const uint8_t* const VM_DRAW_LIST   = (const uint8_t*)0x2797554; // object indices, 0xFF ends
		constexpr uint32_t CONTEXT_OBJECTS = 0x90;     // ctx+0x90: object array, 0x100 bytes per object
		constexpr uint32_t OBJECT_STRIDE = 0x100;
		constexpr int MAX_OBJECTS = 255;               // list entries are bytes, 0xFF terminates
		constexpr int LIST_LENGTH = 0x100;

		// Object fields (verified in Ifrit: integrator 0xB26110, update types 0xB262E0.., the camera
		// opcode at 0xB2B8AF that copies +0x94/+0x98 into the camera pose and +0x8C into the roll).
		constexpr uint32_t OBJECT_STREAMS       = 0x00; // 3 x channel stream pointer
		constexpr uint32_t OBJECT_WAIT_TIMERS   = 0x0C; // 3 x int16 channel wait timer
		constexpr uint32_t OBJECT_UPDATE_TYPE   = 0x18; // byte, index into the module's update table
		constexpr uint32_t OBJECT_DRAW_TYPE     = 0x1C; // byte, index into the module's draw table
		constexpr uint32_t OBJECT_ACCUMULATORS  = 0x50; // 6 x int32 accumulators
		constexpr int OBJECT_FIELD_COUNT = 6;
		constexpr uint32_t OBJECT_FIELD_OFFSETS[OBJECT_FIELD_COUNT] = { 0x8C, 0x8E, 0x90, 0x94, 0x96, 0x98 }; // rot xyz, pos xyz
		constexpr int OBJECT_ANGLE_FIELDS = 3;         // the first three are 12 bit angles (4096 = 360 deg)
		constexpr int ANGLE_CUT = 1024;                // > 90 deg in one tick: snap
		constexpr int POSITION_CUT = 4096;             // > 4096 units in one tick: a teleport, snap

		int16_t* const CAMERA_POSE = (int16_t*)0xB8B7F0;  // 8 int16: position xyz + pad, look-at xyz + pad
		int16_t* const CAMERA_ROLL = (int16_t*)0x1D8E038;
		const int16_t* const CAMERA_SEQUENCE_MODE = (const int16_t*)0x1D97718;  // battle camera sequence active
		const int16_t* const CAMERA_REST_WEIGHT = (const int16_t*)0x1D9771E;    // live -> rest blend running
		constexpr int CAMERA_POSE_WORDS = 8;
		constexpr int CAMERA_CUT_UNITS = 2048;         // same cut threshold as battle_camera_hook / gf_pacing

		const int32_t* const VRAM_OP_COUNT = (const int32_t*)0x1D98420;

		// Bytes 5..13 of every tick: mov eax,[0x27973EC] / inc word [eax+0x32]. Byte 0 is E8 -> setup.
		const uint8_t TICK_SIGNATURE[] = { 0xA1, 0xEC, 0x73, 0x79, 0x02, 0x66, 0xFF, 0x40, 0x32 };
		// test byte [0x1D96A9C],1 - the tick's freeze test (the whole hold rests on it).
		const uint8_t FREEZE_TEST[] = { 0xF6, 0x05, 0x9C, 0x6A, 0xD9, 0x01, 0x01 };

		// --- the per-GF table -----------------------------------------------------------------------

		constexpr int MAX_VRAM_SITES = 4;
		struct VmRow
		{
			const char* name;
			uint16_t effect_id;          // MagicList_Logic index + 1
			uint32_t entry;              // MagicList_Logic[effect_id - 1] (documentation / trace)
			uint32_t link_site;          // E8 -> 0x508360 in the entry; `push tick` sits 10 bytes before it
			uint32_t tick;               // the root task
			uint32_t setup;              // E8 target at tick + 0
			uint32_t draw_site;          // E8 -> draw inside the tick, after the three VM calls
			uint32_t draw_function;
			uint32_t vram_sites[MAX_VRAM_SITES]; // draw-side 0x505DF0 sites (suppressed while held), 0 = none
		};

		// Every address below was read from FF8_EN.exe (US 1.2) and is re-verified byte for byte at
		// install time; a row whose bytes do not match is left alone (that summon stays at host rate).
		const VmRow VM_TABLE[] = {
			{ "Leviathan", 6,   0xB58080, 0xB580CE, 0xB586F0, 0xB58130, 0xB588C7, 0xB5F5D0, { 0xB5913F, 0xB59376, 0xB59444, 0xB5949B } },
			{ "Ifrit",     201, 0xB25780, 0xB257CE, 0xB25DF0, 0xB25830, 0xB25FC7, 0xB2ABE0, { 0xB26774, 0xB267CB, 0, 0 } },
			{ "Bahamut",   202, 0xB189A0, 0xB189EE, 0xB19010, 0xB18A50, 0xB191E7, 0xB20270, { 0xB19A5F, 0xB19B34, 0xB19B8B, 0 } },
			{ "Cerberus",  203, 0xB0C1A0, 0xB0C1F3, 0xB0C820, 0xB0C260, 0xB0C9F7, 0xB135D0, { 0, 0, 0, 0 } },
			{ "Alexander", 204, 0xAFFCA0, 0xAFFCEE, 0xB00310, 0xAFFD50, 0xB004E7, 0xB06E00, { 0xB00C94, 0xB00CEB, 0, 0 } },
			{ "Brothers",  205, 0xAF4520, 0xAF456E, 0xAF4B90, 0xAF45D0, 0xAF4D67, 0xAF9ED0, { 0, 0, 0, 0 } },
			{ "Eden",      206, 0xAE2DD0, 0xAE2E1E, 0xAE3470, 0xAE2E80, 0xAE3647, 0xAEEC40, { 0xAE3DF4, 0xAE3E4B, 0, 0 } },
			// v85: Elvoret's death (c0m091 AI death section: specialAction / opcode 0x1E, param 18, command
			// 0xFC). The spell path cannot replay it (it links its own primitives into an ordering-table
			// bucket in per-frame memory, site 0xB5194F), fell back to unpaced and played ~10x fast after the
			// first second. As a row it is held by the freeze bit and drawn live every frame instead;
			// holdable_action() leaves trees with a row to this file (gf_vm_paces_effect).
			{ "Elvoret death", 18, 0xB4A530, 0xB4A57E, 0xB4ABA0, 0xB4A5E0, 0xB4AD77, 0xB52890, { 0xB4B5EF, 0xB4B6C4, 0xB4B71B, 0 } },
			// Same engine, deliberately NOT rows (v76 review, notes "GF animation-VM family: the boss
			// cinematics"). Their held paths are as clean as the GFs' (closure with the three VM calls
			// removed: only GTE / display-list / model-bounds leaves, 0x5088A0 is a pure shadow-quad draw,
			// VRAM sites 18: 0xB4B5EF 0xB4B6C4 0xB4B71B, 77: 0xB31314 0xB3136B, 208: 0xAD89BF 0xAD8A94
			// 0xAD8AEB, 75: none), but they never reach this engine at host rate:
			//   75 Hell's Judgement (0xB3E8E0 / link 0xB3E92E / tick 0xB3EF50 / setup 0xB3E990 / draw
			//      0xB3F127 -> 0xB45190) = kernel enemy attack 0x166, 77 Ultimecia final form death
			//      (0xB302F0 / 0xB3033E / 0xB30990 / 0xB303A0 / 0xB30B67 -> 0xB392D0) = attack 0x164, both
			//      in c0m126's ability list with type 8; 208 final "Sorceress" death (0xAD7900 / 0xAD794E /
			//      0xAD7F70 / 0xAD79B0 / 0xAD8147 -> 0xADD9B0) = attack 0x148, c0m117 type 8.
			//      Command type 0x08 (monster ability) is not a GF command, so animations.cpp's
			//      holdable_action() already paces the whole tree through the spell path (skip + replay).
			//      A row here would run only on those ticks, write tick-alpha blends into the recorded
			//      display list (the replay would trail by a tick) and take the camera from
			//      battle_camera_hook while nothing runs on the held frames (15 fps camera).
			//   18 Elvoret death: a row since v85 (above). The v76 "unreachable" was wrong - no kernel attack
			//      or ability list carries it, but Elvoret's AI death section queues it with opcode 0x1E.
		};
		constexpr int VM_COUNT = (int)(sizeof(VM_TABLE) / sizeof(VM_TABLE[0]));

		// --- state ------------------------------------------------------------------------------------

		typedef int (__cdecl *task_function_t)(uint8_t*);
		typedef int (__cdecl *draw_function_t)();

		bool row_installed[VM_COUNT] = {};
		int active_row = -1;                  // the row whose tick ran last (one summon at a time)
		int current_row = -1;                 // >= 0 only while a paced tick call is on the stack
		bool current_runs_vm = false;         // that call is a logic tick
		bool current_may_blend = false;       // that call may interpolate (not released by the watchdog)
		int held_call_depth = 0;              // > 0 while a held tick call is on the stack
		uint32_t last_paced_frame = 0xFFFFFFFF; // last host frame a paced tick ran in
		bool owns_camera = false;             // the last tick decided that this file carries the camera

		// Objects. A slot per object index; `captured_draw_list` lists the draw list of the last capture.
		struct ObjectSlot
		{
			bool has_next;
			bool paired;         // prev/next are two consecutive ticks of the same object
			bool no_blend;       // the object feeds a VRAM upload (texture scroll): never blended
			bool blend_written;
			uint32_t tick;       // logic_tick_count() of `next`
			uint8_t update_type, draw_type;
			int16_t prev[OBJECT_FIELD_COUNT];
			int16_t next[OBJECT_FIELD_COUNT];
			int16_t blended[OBJECT_FIELD_COUNT];
		};
		ObjectSlot object_slots[MAX_OBJECTS];
		uint8_t captured_draw_list[LIST_LENGTH];
		int captured_count = 0;

		struct CameraPose
		{
			int16_t words[CAMERA_POSE_WORDS];
			int16_t roll;
		};
		CameraPose camera_prev = {}, camera_next = {};
		bool camera_valid = false;

		// Watchdog.
		constexpr uint32_t VM_WATCHDOG_MS = 2000;
		uint32_t progress_signature = 0;
		bool progress_valid = false;
		uint32_t progress_stamp = 0;
		bool released = false;
		bool warned_watchdog = false;

		// Once-a-second summary (trace only) and the VRAM queue guard.
		struct VmStats
		{
			uint32_t stamp;
			uint32_t frames, held;
			uint32_t blended, paired, snapped, no_blend;
			uint32_t camera_blends, camera_adopts;
			uint32_t suppressed;
			int32_t vram_queue_max;
		};
		VmStats stats = {};
		bool warned_vram = false;

		// --- small helpers --------------------------------------------------------------------------

		bool vm_disabled()
		{
			return (ff8_interp::cfg().battle_debug & (DBG_GF_VM_OFF | DBG_GF_PACE_OFF)) != 0;
		}

		bool live_now()
		{
			return !vm_disabled() && last_paced_frame != 0xFFFFFFFF && (ff8_interp::frame_counter() - last_paced_frame) <= 1;
		}

		uint32_t relative_call_target(uint32_t site)
		{
			return site + 5 + (uint32_t)(*(const int32_t*)(site + 1));
		}

		bool is_call_to(uint32_t site, uint32_t target)
		{
			return *(const uint8_t*)site == 0xE8 && relative_call_target(site) == target;
		}

		int row_by_draw_site(uint32_t site)
		{
			for (int row_index = 0; row_index < VM_COUNT; row_index++)
			{
				if (VM_TABLE[row_index].draw_site == site) return row_index;
			}
			return -1;
		}

		int row_by_link_site(uint32_t site)
		{
			for (int row_index = 0; row_index < VM_COUNT; row_index++)
			{
				if (VM_TABLE[row_index].link_site == site) return row_index;
			}
			return -1;
		}

		void objects_forget()
		{
			for (int i = 0; i < MAX_OBJECTS; i++)
			{
				ObjectSlot& slot = object_slots[i];
				slot.has_next = slot.paired = slot.blend_written = slot.no_blend = false;
			}
			captured_count = 0;
		}

		// Forget everything that belongs to one cinematic.
		void cinematic_forget()
		{
			objects_forget();
			camera_valid = false;
			owns_camera = false;
			progress_valid = false;
			progress_stamp = GetTickCount();
			released = false;
		}

		uint8_t* object_array()
		{
			uint8_t* context = *VM_CONTEXT;
			if (context == nullptr) return nullptr;
			return *(uint8_t**)(context + CONTEXT_OBJECTS);
		}

		// --- watchdog -------------------------------------------------------------------------------

		__forceinline uint32_t fnv_mix(uint32_t hash, uint32_t value)
		{
			return (hash ^ value) * 16777619u;
		}

		// Progress of the VM: stream pointers, wait timers and accumulators of every updated object.
		// Only a logic tick (or an unpaced call) can change it, so 2 s without a change across calls
		// that were allowed to run the VM means the cinematic is not advancing.
		void progress_check(const uint8_t* objects)
		{
			uint32_t hash = 2166136261u;
			for (int i = 0; i < LIST_LENGTH && VM_UPDATE_LIST[i] != 0xFF; i++)
			{
				const uint8_t* object = objects + (uint32_t)(VM_UPDATE_LIST[i] & 0x7F) * OBJECT_STRIDE;
				for (int k = 0; k < 3; k++)
					hash = fnv_mix(hash, *(const uint32_t*)(object + OBJECT_STREAMS + 4 * k));
				for (int k = 0; k < 3; k++)
					hash = fnv_mix(hash, (uint16_t)*(const int16_t*)(object + OBJECT_WAIT_TIMERS + 2 * k));
				for (int k = 0; k < 6; k++)
					hash = fnv_mix(hash, *(const uint32_t*)(object + OBJECT_ACCUMULATORS + 4 * k));
			}
			const uint32_t now = GetTickCount();
			if (!progress_valid || hash != progress_signature)
			{
				progress_valid = true;
				progress_signature = hash;
				progress_stamp = now;
				released = false;
			}
		}

		bool watchdog_released(const VmRow& row)
		{
			if (released) return true;
			if (!progress_valid) return false;
			if (GetTickCount() - progress_stamp <= VM_WATCHDOG_MS) return false;
			released = true;
			objects_forget();
			camera_valid = false;
			if (!warned_watchdog)
			{
				warned_watchdog = true;
				ff8_interp::log_warning("gf_vm: %s cinematic made no progress for %u ms, pacing released.\n", row.name, VM_WATCHDOG_MS);
			}
			return true;
		}

		// --- object interpolation -----------------------------------------------------------------

		int16_t lerp_int16(int16_t a, int16_t b, float t)
		{
			const float value = (float)a + ((float)b - (float)a) * t;
			return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
		}

		// Shortest arc on the 12 bit circle; int16 wrap-around is a multiple of 4096, so it is harmless.
		int angle_delta(int16_t a, int16_t b)
		{
			return ((int(b) - int(a) + 2048) & 4095) - 2048;
		}

		int16_t lerp_angle(int16_t a, int16_t b, float t)
		{
			const float value = angle_delta(a, b) * t;
			return (int16_t)(int(a) + (int)(value + (value >= 0.0f ? 0.5f : -0.5f)));
		}

		// Logic tick: the VM has just run. Roll every drawn object's pair.
		void objects_capture(uint8_t* objects)
		{
			const uint32_t now = logic_tick_count();
			bool already_captured[MAX_OBJECTS] = {};
			captured_count = 0;
			for (int i = 0; i < LIST_LENGTH && VM_DRAW_LIST[i] != 0xFF; i++)
			{
				const uint8_t object_index = VM_DRAW_LIST[i];
				if (already_captured[object_index]) continue;
				already_captured[object_index] = true;
				captured_draw_list[captured_count++] = object_index;
				const uint8_t* object = objects + (uint32_t)object_index * OBJECT_STRIDE;
				ObjectSlot& slot = object_slots[object_index];
				int16_t current[OBJECT_FIELD_COUNT];
				for (int k = 0; k < OBJECT_FIELD_COUNT; k++)
					current[k] = *(const int16_t*)(object + OBJECT_FIELD_OFFSETS[k]);
				const uint8_t update_type = object[OBJECT_UPDATE_TYPE];
				const uint8_t draw_type = object[OBJECT_DRAW_TYPE];
				bool pair = slot.has_next && slot.tick + 1 == now && slot.update_type == update_type && slot.draw_type == draw_type;
				if (pair)
				{
					memcpy(slot.prev, slot.next, sizeof(slot.prev));
					for (int k = 0; k < OBJECT_FIELD_COUNT && pair; k++)
					{
						const int delta = k < OBJECT_ANGLE_FIELDS ? angle_delta(slot.prev[k], current[k]) : (int)current[k] - (int)slot.prev[k];
						const int cut = k < OBJECT_ANGLE_FIELDS ? ANGLE_CUT : POSITION_CUT;
						if (delta > cut || delta < -cut) pair = false;
					}
					if (!pair) stats.snapped++;
				}
				if (!pair && slot.has_next && (slot.update_type != update_type || slot.draw_type != draw_type)) slot.no_blend = false; // a new object in this slot
				memcpy(slot.next, current, sizeof(slot.next));
				if (!pair) memcpy(slot.prev, current, sizeof(slot.prev));
				slot.paired = pair;
				slot.has_next = true;
				slot.tick = now;
				slot.update_type = update_type;
				slot.draw_type = draw_type;
			}
		}

		void objects_blend(uint8_t* objects, float t)
		{
			for (int i = 0; i < captured_count; i++)
			{
				ObjectSlot& slot = object_slots[captured_draw_list[i]];
				if (!slot.paired) continue;
				if (slot.no_blend)
				{
					stats.no_blend++;
					continue;
				}
				uint8_t* object = objects + (uint32_t)captured_draw_list[i] * OBJECT_STRIDE;
				for (int k = 0; k < OBJECT_FIELD_COUNT; k++)
				{
					const int16_t value = k < OBJECT_ANGLE_FIELDS ? lerp_angle(slot.prev[k], slot.next[k], t) : lerp_int16(slot.prev[k], slot.next[k], t);
					*(int16_t*)(object + OBJECT_FIELD_OFFSETS[k]) = value;
					slot.blended[k] = value;
				}
				slot.blend_written = true;
				stats.blended++;
			}
		}

		// Put back what the last tick produced, so the VM only ever integrates from its own values. A
		// field the draw itself changed is left as the draw left it.
		void objects_restore(uint8_t* objects)
		{
			for (int i = 0; i < captured_count; i++)
			{
				ObjectSlot& slot = object_slots[captured_draw_list[i]];
				if (!slot.blend_written) continue;
				uint8_t* object = objects + (uint32_t)captured_draw_list[i] * OBJECT_STRIDE;
				for (int k = 0; k < OBJECT_FIELD_COUNT; k++)
				{
					int16_t* field = (int16_t*)(object + OBJECT_FIELD_OFFSETS[k]);
					if (*field == slot.blended[k]) *field = slot.next[k];
				}
				slot.blend_written = false;
			}
		}

		// --- camera (same algorithm as gf_pacing.cpp / battle_camera_hook) ------------------------

		bool engine_camera_active()
		{
			return *CAMERA_SEQUENCE_MODE != 0 || *CAMERA_REST_WEIGHT != 0;
		}

		void camera_read(CameraPose& pose)
		{
			memcpy(pose.words, CAMERA_POSE, sizeof(pose.words));
			pose.roll = *CAMERA_ROLL;
		}

		void camera_write(const CameraPose& pose)
		{
			memcpy(CAMERA_POSE, pose.words, sizeof(pose.words));
			*CAMERA_ROLL = pose.roll;
		}

		bool camera_same(const CameraPose& a, const CameraPose& b)
		{
			return memcmp(a.words, b.words, sizeof(a.words)) == 0 && a.roll == b.roll;
		}

		bool camera_is_cut(const CameraPose& a, const CameraPose& b)
		{
			for (int i = 0; i < 3; i++)
			{
				const int position_delta = (int)a.words[i] - (int)b.words[i];
				const int look_at_delta = (int)a.words[4 + i] - (int)b.words[4 + i];
				if (position_delta > CAMERA_CUT_UNITS || position_delta < -CAMERA_CUT_UNITS) return true;
				if (look_at_delta > CAMERA_CUT_UNITS || look_at_delta < -CAMERA_CUT_UNITS) return true;
			}
			return false;
		}

		void camera_after_tick(bool is_logic_tick)
		{
			CameraPose current;
			camera_read(current);
			if (!camera_valid)
			{
				camera_prev = camera_next = current;
				camera_valid = true;
			}
			else if (is_logic_tick)
			{
				camera_prev = camera_is_cut(current, camera_next) ? current : camera_next;
				camera_next = current;
			}
			else if (!camera_same(current, camera_next))
			{
				// The VM is frozen on held frames, so nothing of it moves the pose there; adopt anyway.
				camera_prev = camera_next = current;
				stats.camera_adopts++;
			}
			const float t = tick_alpha_now();
			CameraPose blended;
			for (int i = 0; i < CAMERA_POSE_WORDS; i++)
				blended.words[i] = lerp_int16(camera_prev.words[i], camera_next.words[i], t);
			blended.roll = lerp_int16(camera_prev.roll, camera_next.roll, t);
			camera_write(blended);
			if (!is_logic_tick) stats.camera_blends++;
		}

		// --- 1/s trace -----------------------------------------------------------------------------

		void stats_after_tick(const VmRow& row, bool is_logic_tick)
		{
			stats.frames++;
			if (!is_logic_tick) stats.held++;
			const int32_t vram_queue = *VRAM_OP_COUNT;
			if (vram_queue > stats.vram_queue_max) stats.vram_queue_max = vram_queue;
			if (vram_queue > 0x1F && !warned_vram)
			{
				warned_vram = true;
				ff8_interp::log_warning("gf_vm: VRAM op queue at %d entries during the %s cinematic (0x20 slots).\n", vram_queue, row.name);
			}
			if (!ff8_interp::cfg().trace) return;
			const uint32_t now = GetTickCount();
			if (stats.stamp == 0)
			{
				stats.stamp = now;
				return;
			}
			if (now - stats.stamp < 1000) return;
			ff8_interp::log_trace("ff8gfvm %s 1s: frames=%u held=%u objs=%d blended=%u snapped=%u noblend=%u cam=%s blend=%u adopt=%u suppressed=%u vramq_max=%d released=%d\n",
				row.name, stats.frames, stats.held, captured_count, stats.blended, stats.snapped, stats.no_blend,
				owns_camera ? "owned" : "battle", stats.camera_blends, stats.camera_adopts, stats.suppressed, stats.vram_queue_max, (int)released);
			stats = VmStats();
			stats.stamp = now;
		}

		// --- the tick (every row, through its thunk) -----------------------------------------------

		int vm_tick(int row_index, uint8_t* node)
		{
			const VmRow& row = VM_TABLE[row_index];
			const task_function_t original_tick = (task_function_t)row.tick;
			if (!in_battle() || vm_disabled())
			{
				current_row = -1;
				return original_tick(node);
			}
			tick_update();
			if (row_index != active_row)
			{
				cinematic_forget();
				active_row = row_index;
				if (ff8_interp::cfg().trace) ff8_interp::log_trace("ff8gfvm %s (effect %u) cinematic starts, ticks=%u\n", row.name, (unsigned)row.effect_id, logic_tick_count());
			}
			// The tick was not called on the previous host frame (battle pause: 0x500900 does not run while
			// g_battlePaused 0x1D28DE9 is set): that wall time is not a stall - re-stamp the watchdog
			// (gf_pacing v7, the v78 Quezacotl release after a pause).
			if (last_paced_frame != 0xFFFFFFFF && ff8_interp::frame_counter() - last_paced_frame > 1) progress_stamp = GetTickCount();
			last_paced_frame = ff8_interp::frame_counter();

			const bool is_logic_tick = is_logic_tick_now();
			const bool unpaced = watchdog_released(row);
			const bool hold = !is_logic_tick && !unpaced;

			// The camera: undo last frame's blend so the camera opcodes read-modify-write the pose the
			// VM produced, never one of our blends.
			if (owns_camera && camera_valid && !engine_camera_active()) camera_write(camera_next);

			current_row = row_index;
			current_runs_vm = is_logic_tick || unpaced; // an unpaced call runs the VM like a tick
			current_may_blend = !unpaced;
			int result;
			if (hold)
			{
				held_call_depth++;
				const uint32_t old_freeze_bit = *BATTLE_FREEZE & 1u;
				*BATTLE_FREEZE |= 1u;
				result = original_tick(node);
				*BATTLE_FREEZE = (*BATTLE_FREEZE & ~1u) | old_freeze_bit; // only bit 0 is ours
				held_call_depth--;
			}
			else result = original_tick(node);
			current_row = -1;

			if (engine_camera_active())
			{
				// A battle camera animation the VM started (0x5099A0) or the rest blend owns the pose:
				// battle_camera_hook carries it. Re-capture fresh when the VM takes the camera back.
				owns_camera = false;
				camera_valid = false;
			}
			else
			{
				owns_camera = true;
				if (unpaced) camera_valid = false; // unpaced: the VM writes its own camera every frame
				else camera_after_tick(is_logic_tick);
			}
			stats_after_tick(row, is_logic_tick);
			if ((result & 2) != 0)
			{
				if (ff8_interp::cfg().trace) ff8_interp::log_trace("ff8gfvm %s cinematic done, ticks=%u\n", row.name, logic_tick_count());
				cinematic_forget(); // battle_camera_hook re-captures (it dropped its cache while we owned)
			}
			return result;
		}

		template<int G> int __cdecl vm_tick_thunk(uint8_t* node) { return vm_tick(G, node); }

		const task_function_t VM_THUNKS[] = {
			vm_tick_thunk<0>, vm_tick_thunk<1>, vm_tick_thunk<2>, vm_tick_thunk<3>,
			vm_tick_thunk<4>, vm_tick_thunk<5>, vm_tick_thunk<6>, vm_tick_thunk<7>,
			vm_tick_thunk<8>, vm_tick_thunk<9>, vm_tick_thunk<10>, vm_tick_thunk<11>,
		};
		static_assert(VM_COUNT <= (int)(sizeof(VM_THUNKS) / sizeof(VM_THUNKS[0])), "add thunks for the new rows");

		// --- replaced call sites ------------------------------------------------------------------

		// The entry's BdLinkTask(queue, tick): link the row's thunk instead of the tick. The entry
		// does not use the returned node.
		uint32_t __cdecl vm_link_stub(uint32_t queue, uint32_t fn)
		{
			const int row_index = row_by_link_site((uint32_t)(uintptr_t)_ReturnAddress() - 5);
			if (row_index >= 0 && row_installed[row_index] && fn == VM_TABLE[row_index].tick) fn = (uint32_t)(uintptr_t)VM_THUNKS[row_index];
			return ((uint32_t(__cdecl*)(uint32_t, uint32_t))FN_SPAWN_TASK)(queue, fn);
		}

		// The tick's draw call: after the VM (tick frames), before anything is drawn (every frame).
		int __cdecl vm_draw_stub()
		{
			const int row_index = row_by_draw_site((uint32_t)(uintptr_t)_ReturnAddress() - 5);
			if (row_index < 0) return 0; // unreachable: only table sites are patched
			const draw_function_t draw = (draw_function_t)VM_TABLE[row_index].draw_function;
			if (current_row != row_index) return draw(); // not inside a paced tick call
			uint8_t* objects = object_array();
			if (objects == nullptr) return draw();
			if (current_runs_vm)
			{
				progress_check(objects);
				if (current_may_blend) objects_capture(objects);
				else objects_forget();
			}
			if (!current_may_blend) return draw();
			objects_blend(objects, tick_alpha_now());
			const int result = draw();
			objects_restore(objects);
			return result;
		}

		// A draw-side VRAM load append (texture-scroll draw type). The object it belongs to is never
		// blended (the upload takes its offset from the fields we would blend); while held the append
		// is dropped - it would queue the upload the tick already queued once per host frame.
		uint32_t __cdecl vm_vram_stub(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
		{
			if (current_row >= 0)
			{
				uint8_t* objects = object_array();
				const uint8_t* object = *VM_CURRENT_OBJECT;
				if (objects != nullptr && object >= objects && object < objects + MAX_OBJECTS * OBJECT_STRIDE) object_slots[(object - objects) / OBJECT_STRIDE].no_blend = true;
			}
			if (held_call_depth > 0 && in_battle())
			{
				stats.suppressed++;
				return 0; // void in the engine; no caller reads EAX
			}
			return ((uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t))FN_VRAM_LOAD)(arg0, arg1, arg2, arg3);
		}

		// --- installation -------------------------------------------------------------------------

		bool verify_row(const VmRow& row)
		{
			const char* mismatch = nullptr;
			const uint8_t* push_tick = (const uint8_t*)(row.link_site - 10);
			const uint8_t* tick_code = (const uint8_t*)row.tick;
			if (!is_call_to(row.link_site, FN_SPAWN_TASK)) mismatch = "entry link site";
			else if (push_tick[0] != 0x68 || *(const uint32_t*)(push_tick + 1) != row.tick) mismatch = "entry `push tick`";
			else if (tick_code[0] != 0xE8 || relative_call_target(row.tick) != row.setup) mismatch = "tick prologue (hooked by someone else?)";
			else if (memcmp(tick_code + 5, TICK_SIGNATURE, sizeof(TICK_SIGNATURE)) != 0) mismatch = "tick body";
			else if (!is_call_to(row.draw_site, row.draw_function)) mismatch = "draw site";
			else
			{
				bool found_freeze_test = false;
				for (uint32_t address = row.tick; address < row.draw_site && !found_freeze_test; address++)
					found_freeze_test = memcmp((const void*)address, FREEZE_TEST, sizeof(FREEZE_TEST)) == 0;
				if (!found_freeze_test) mismatch = "freeze test";
				for (int i = 0; i < MAX_VRAM_SITES && mismatch == nullptr; i++)
					if (row.vram_sites[i] != 0 && !is_call_to(row.vram_sites[i], FN_VRAM_LOAD)) mismatch = "VRAM site";
			}
			if (mismatch != nullptr)
			{
				ff8_interp::log_warning("gf_vm_hook_init: %s (effect %u): %s does not match, that summon stays at host rate.\n", row.name, (unsigned)row.effect_id, mismatch);
				return false;
			}
			return true;
		}
	}

	// See the marked gf_vm block in internal.h.
	bool gf_vm_live()
	{
		return live_now();
	}

	bool gf_vm_paces_effect(uint16_t effect_id)
	{
		if (vm_disabled()) return false;
		for (int row_index = 0; row_index < VM_COUNT; row_index++)
			if (row_installed[row_index] && VM_TABLE[row_index].effect_id == effect_id) return true;
		return false;
	}

	bool gf_vm_owns_camera()
	{
		return live_now() && owns_camera;
	}

	void gf_vm_hook_init()
	{
		int rows_installed = 0;
		int sites_replaced = 0;
		for (int row_index = 0; row_index < VM_COUNT; row_index++)
		{
			const VmRow& row = VM_TABLE[row_index];
			if (!verify_row(row)) continue;
			ff8_interp::hook_call(row.draw_site, (void*)vm_draw_stub);
			sites_replaced++;
			for (int i = 0; i < MAX_VRAM_SITES; i++)
			{
				if (row.vram_sites[i] != 0)
				{
					ff8_interp::hook_call(row.vram_sites[i], (void*)vm_vram_stub);
					sites_replaced++;
				}
			}
			// Last: only now can the thunk be linked.
			row_installed[row_index] = true;
			ff8_interp::hook_call(row.link_site, (void*)vm_link_stub);
			sites_replaced++;
			rows_installed++;
		}
		ff8_interp::log_info("%s: gf animation-VM pacing v2 enabled (%d/%d rows, %d call sites replaced).\n", __func__, rows_installed, VM_COUNT, sites_replaced);
	}

	void gf_vm_reset()
	{
		held_call_depth = 0;
		current_row = -1;
		active_row = -1;
		last_paced_frame = 0xFFFFFFFF;
		warned_watchdog = false;
		warned_vram = false;
		stats = VmStats();
		cinematic_forget();
	}
}
