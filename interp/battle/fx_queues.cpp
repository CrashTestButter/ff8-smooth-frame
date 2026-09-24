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

// Battle task-queue pacing (v60). Shares the battle module's tick state through internal.h; gated
// by DBG_QUEUE_PACE_OFF.
//
// The battle frame driver 0x500900 walks four task queues per RENDERED frame. Each node is
// `node->fn(node)` and ExecuteTaskQueue 0x508420 removes the node only when the return has bit 1
// set (`test al,2` at 0x50843A), so returning 0 always means "still alive". Every runner in these
// queues either draws from a freshly allocated transient block (0x5082B0 / 0x5082D0) or writes a
// pure-output global, which means the DRAW has to keep happening on every host frame - only the
// stepping must be held. Three gate shapes are used here, in this order of preference:
//
//   A. call-site freeze  - queue B (0x500923). The whole runner family reads FF8's own freeze bit
//      0x1D96A9C & 1 and, with it set, draws and returns 0 without stepping. One save/set/restore
//      around the one call covers all 67 appenders and every future per-spell runner. Rule 2 of
//      verify-checklist.md: the bit is driven around ONE call only - frame-wide it would also stop
//      0x502AB0 from reaching 0x504290 and kill pose interpolation.
//   B. per-runner field hold - queue A (0x500917). None of its runners reads the freeze bit, so
//      each one is hooked, called through, its own counter field(s) restored afterwards and the
//      completion bit masked out of the return so the task is not removed a logic tick early.
//   C. call-through + counter hold - 0x50A0A0 (status icons), where skipping the call would remove
//      a drawn element from nine of every ten frames, and queue D (0x50097E), where only the
//      background spin accumulator 0x1D96DA4 is held across the call.
//
// Nothing here touches the battle UI: queue A's only appender is 0x506C10 and all 20 of its call
// sites live in the battle field module (0x501A4A .. 0x50F7D7) - not one is in the 0x4Bxxxx window
// module - and queue B is the effect pool. The command-window cursor is drawn by 0x4BD0C0 out of
// the UI context *0x1D6D490 (+0x28 index, +0x34/36/38/3A position, +0x44 sprite 0x5C, +0x46 the
// 0/0x140 flash offset from the clock byte +0x2C); no hook in this file can reach any of that.
//
// Everything here is inert unless in_battle() and gated by DBG_QUEUE_PACE_OFF.

#include "internal.h"

#include "../host.h"

#include <windows.h>
#include <stdint.h>
#include <string.h>

namespace ff8_battle_anim
{
	namespace
	{
		// --- shared plumbing ---------------------------------------------------------------------

		int hooks_installed = 0;

		inline bool pace_off() { return (ff8_interp::cfg().battle_debug & DBG_QUEUE_PACE_OFF) != 0; }

		// True when this host frame must hold the queues' state. Call tick_update() first.
		inline bool hold_frame() { return !tick_is_real(); }

		// Every queue runner is `int __cdecl fn(node*)`; 0x508420 pushes the node and pops it itself.
		typedef int (__cdecl *node_fn_t)(uint8_t*);

		void* install_fn(uint32_t addr, const uint8_t* prologue, size_t len, void* hook, const char* what)
		{
			void* orig = make_trampoline(addr, prologue, len);
			if (orig == nullptr)
			{
				ff8_interp::log_warning("fx_queues: prologue mismatch at 0x%X (%s), hook skipped.\n", addr, what);
				return nullptr;
			}
			ff8_interp::hook_function(addr, hook);
			hooks_installed++;
			return orig;
		}

		// =========================================================================================
		// 1. Queue B - the 300-slot effect pool 0x209FAA8, driver site 0x500923
		// =========================================================================================

		// 0x508420(queue) is __cdecl with one argument; the driver pushes three queues and pops them
		// in one `add esp` afterwards, so the hook must not clean the stack either.
		constexpr uint32_t CS_QUEUE_B = 0x500923;        // call 0x508420(*0x1D96AA0)
		constexpr uint32_t FN_TASK_QUEUE = 0x508420;
		typedef int (__cdecl *task_queue_t)(void*);
		task_queue_t orig_task_queue = (task_queue_t)FN_TASK_QUEUE;

		bool traced_queue_b = false;

		int __cdecl queue_b_hook(void* queue)
		{
			if (queue == nullptr || !in_battle() || pace_off()) return orig_task_queue(queue);
			tick_update();
			if (!hold_frame()) return orig_task_queue(queue);
			// Every runner of the 0x56F9A0..0x571FFF family and of the 56 per-spell modules tests this
			// bit and, when it is set, draws its fresh primitives and returns 0 without stepping.
			uint32_t freeze = *BATTLE_FREEZE;
			*BATTLE_FREEZE = freeze | 1;
			int r = orig_task_queue(queue);
			*BATTLE_FREEZE = freeze;
			if (ff8_interp::cfg().trace && !traced_queue_b)
			{
				traced_queue_b = true;
				ff8_interp::log_trace("ff8fxq f=%u queue B held under the freeze bit (site 0x%X)\n", ff8_interp::frame_counter(), CS_QUEUE_B);
			}
			return r;
		}

		// The two twins of 0x570BC0 (which already has v58's impact_fx_hook in animations.cpp). Both
		// call 0x571BC0 before they test the bit, and both restore node+0x0C..+0x17 around it.
		// v63 correction: 0x571BC0 does NOT write the node - it reads the SVECTOR at node+0x10 and
		// writes only the GTE feed globals 0x21DFED0 / 0x21DFED8 / 0x21DFEE0 / 0x21DFEE4 (see
		// 0x571BC8..0x571BFD) - so these restores, and the identical one in impact_fx_hook, are
		// no-ops. They are kept as cheap insurance against a runner that does write its node, and
		// because +0x0C is inside the range. Neither needs a return mask: under the bit both return 0
		// before their completion test.
		constexpr uint32_t ADDR_FX_TWIN = 0x570AC0;      // impact sprite, sprite set 0xC7E568
		const uint8_t FX_TWIN_PROLOGUE[6] = { 0x83, 0xEC, 0x08, 0x53, 0x56, 0x57 };       // sub esp,8 / push ebx,esi,edi
		constexpr uint32_t ADDR_FX_DEBRIS = 0x56FCD0;    // 15-frame debris / dust flipbook
		const uint8_t FX_DEBRIS_PROLOGUE[6] = { 0x55, 0x56, 0x8B, 0x74, 0x24, 0x0C };     // push ebp,esi / mov esi,[esp+0C]
		constexpr uint32_t FX_STATE = 0x0C;
		constexpr uint32_t FX_STATE_LEN = 12;            // +0x0C..+0x17 inclusive
		node_fn_t orig_fx_twin = nullptr;
		node_fn_t orig_fx_debris = nullptr;

		int __cdecl fx_freeze_hold(node_fn_t orig, uint8_t* node)
		{
			uint8_t saved[FX_STATE_LEN];
			memcpy(saved, node + FX_STATE, sizeof(saved));
			uint32_t freeze = *BATTLE_FREEZE;
			*BATTLE_FREEZE = freeze | 1;
			int r = orig(node);
			*BATTLE_FREEZE = freeze;
			memcpy(node + FX_STATE, saved, sizeof(saved)); // undo 0x571BC0's in-place integration
			return r;
		}

		int __cdecl fx_twin_hook(uint8_t* node)
		{
			if (orig_fx_twin == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_fx_twin(node);
			tick_update();
			if (!hold_frame()) return orig_fx_twin(node);
			return fx_freeze_hold(orig_fx_twin, node);
		}

		int __cdecl fx_debris_hook(uint8_t* node)
		{
			if (orig_fx_debris == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_fx_debris(node);
			tick_update();
			if (!hold_frame()) return orig_fx_debris(node);
			return fx_freeze_hold(orig_fx_debris, node);
		}

		// BIG TRAIL / RIBBON. The site gate already covers its stepping (+0x0C and +0x18 move only in
		// the `bit == 0` tail at 0x57054D), but its frame-15 one-shot sits ABOVE that test and is not
		// guarded at all:
		//     if (node[0x0C] == 15) { 0x502170(entity, 0x15, 0, 0x209D790); *(s16*)0x209D792 -= 0x50; }
		// which establishes the ribbon's anchor point for the rest of the effect. Under the site gate
		// node[0x0C] stays 15 for a whole logic tick, so it fires on every host frame of that tick.
		// The value is idempotent (0x502170 rewrites 0x209D790/92/94 before the -0x50, and 0x502170 is
		// a pure bone-position query), but it samples the entity's INTERPOLATED pose, so the anchor
		// that survives into frames 16..30 would be the one from the LAST host frame of the tick
		// instead of the tick's own. Holding those 6 bytes across a held call restores vanilla
		// semantics exactly: each held frame still samples a fresh anchor for its OWN draw (the same
		// call reads 0x209D790/92/94 a few lines further down) and only the tick frame's sample
		// persists.
		// NOT the alternative of no-opping the 0x502170 call on held frames: the FIRST frame on which
		// node[0x0C] == 15 is a held frame (the previous tick stepped 14 -> 15 in its tail), so those
		// ~9 frames would draw against a stale, never-initialised anchor.
		// Its frame-0 branch (rebuild of the 0x40-sample curve at 0x209D7C0) and its idempotent
		// `entity[1] |= 2` / flag-mask writes are left alone: repeating them costs CPU, not state.
		constexpr uint32_t ADDR_FX_RIBBON = 0x56FE10;
		const uint8_t FX_RIBBON_PROLOGUE[7] = { 0x83, 0xEC, 0x14, 0x53, 0x55, 0x56, 0x57 }; // sub esp,14h / push ebx,ebp,esi,edi
		uint8_t* const FX_RIBBON_ANCHOR = (uint8_t*)0x209D790;   // s16 x, y, z
		constexpr size_t FX_RIBBON_ANCHOR_LEN = 6;
		node_fn_t orig_fx_ribbon = nullptr;

		int __cdecl fx_ribbon_hook(uint8_t* node)
		{
			if (orig_fx_ribbon == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_fx_ribbon(node);
			tick_update();
			if (!hold_frame()) return orig_fx_ribbon(node);
			uint8_t saved[FX_RIBBON_ANCHOR_LEN];
			memcpy(saved, FX_RIBBON_ANCHOR, sizeof(saved));
			uint32_t freeze = *BATTLE_FREEZE;
			*BATTLE_FREEZE = freeze | 1;
			int r = orig_fx_ribbon(node);
			*BATTLE_FREEZE = freeze;
			memcpy(FX_RIBBON_ANCHOR, saved, sizeof(saved)); // the frame-15 anchor is a one-shot
			return r;
		}

		// The full-screen flash is the one runner of the family where the freeze bit is WRONG for us:
		// its draw lives inside the `bit == 0` branch (0x571364), so under the call-site gate it would
		// strobe at the logic rate. Clear the bit around its own call instead, hold its +0x0C frame
		// counter (the envelope is a pure function of it) and mask the completion return.
		constexpr uint32_t ADDR_FX_FLASH = 0x571300;     // 320x216 0x3000000 quad -> 0x45C8E0
		const uint8_t FX_FLASH_PROLOGUE[7] = { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55, 0x56 }; // push ebx / mov ebx,[esp+8] / push ebp,esi
		constexpr uint32_t FX_FLASH_FRAME = 0x0C;
		node_fn_t orig_fx_flash = nullptr;

		int __cdecl fx_flash_hook(uint8_t* node)
		{
			if (orig_fx_flash == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_fx_flash(node);
			tick_update();
			if (!hold_frame()) return orig_fx_flash(node);
			uint16_t frame = *(uint16_t*)(node + FX_FLASH_FRAME);
			uint32_t freeze = *BATTLE_FREEZE;
			*BATTLE_FREEZE = freeze & ~1u;
			int r = orig_fx_flash(node);
			*BATTLE_FREEZE = freeze;
			*(uint16_t*)(node + FX_FLASH_FRAME) = frame;
			return r & ~2; // the hold undid the completion this call would have reported
		}

		// =========================================================================================
		// 2. Queue A - the 16-slot pool 0x1D986C8, driver site 0x500917
		// =========================================================================================
		// None of these reads the freeze bit and all of them allocate their primitives fresh (or write
		// a pure-output global that the frame's consumer clears again), so the shape is always: call
		// through, restore the node's own counter field(s), mask bit 1 out of the return.

		// SCREEN SHAKE. 0x1D97712 = lerp(node+0x0C, node+0x0E) over node+0x10 frames, sign flipped on
		// odd frames; 0x5033E0 adds it into the view matrix and clears it again every frame, so the
		// write is pure output. Counter: u16 at +0x12.
		constexpr uint32_t ADDR_SHAKE = 0x50F6C0;
		const uint8_t SHAKE_PROLOGUE[7] = { 0x8B, 0x4C, 0x24, 0x04, 0x53, 0x55, 0x56 };  // mov ecx,[esp+4] / push ebx,ebp,esi
		constexpr uint32_t SHAKE_COUNTER = 0x12;
		node_fn_t orig_shake = nullptr;

		int __cdecl shake_hook(uint8_t* node)
		{
			if (orig_shake == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_shake(node);
			tick_update();
			if (!hold_frame()) return orig_shake(node);
			uint16_t saved = *(uint16_t*)(node + SHAKE_COUNTER);
			int r = orig_shake(node);
			*(uint16_t*)(node + SHAKE_COUNTER) = saved;
			return r & ~2;
		}

		// CAMERA LIVE->REST BLEND. 0x1D9771E = sin(node[0x0C] / node[0x0D]). Counter: u8 at +0x0C.
		constexpr uint32_t ADDR_CAM_SETTLE = 0x509930;
		const uint8_t CAM_SETTLE_PROLOGUE[5] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };          // push esi / mov esi,[esp+8]
		constexpr uint32_t CAM_SETTLE_COUNTER = 0x0C;
		node_fn_t orig_cam_settle = nullptr;

		int __cdecl cam_settle_hook(uint8_t* node)
		{
			if (orig_cam_settle == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_cam_settle(node);
			tick_update();
			if (!hold_frame()) return orig_cam_settle(node);
			uint8_t saved = node[CAM_SETTLE_COUNTER];
			int r = orig_cam_settle(node);
			node[CAM_SETTLE_COUNTER] = saved;
			return r & ~2;
		}

		// FULL-SCREEN COLOUR QUAD (limit-break / impact fade). Ramps node[0x10] -> node[0x11] over
		// node+0x0E frames and links a fresh 320x216 quad through 0x45C8E0 every call. Counter: s16 at
		// +0x0C. Its completion also clears the dword 0x1D96EBC ("a fade owns the screen"), which a
		// held frame would publish up to one logic tick early, so that word is held with it.
		constexpr uint32_t ADDR_FADE_QUAD = 0x501E80;
		const uint8_t FADE_QUAD_PROLOGUE[6] = { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C };     // push esi,edi / mov edi,[esp+0C]
		constexpr uint32_t FADE_QUAD_COUNTER = 0x0C;
		uint32_t* const FADE_QUAD_DONE = (uint32_t*)0x1D96EBC;
		node_fn_t orig_fade_quad = nullptr;

		int __cdecl fade_quad_hook(uint8_t* node)
		{
			if (orig_fade_quad == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_fade_quad(node);
			tick_update();
			if (!hold_frame()) return orig_fade_quad(node);
			uint16_t saved = *(uint16_t*)(node + FADE_QUAD_COUNTER);
			uint32_t done = *FADE_QUAD_DONE;
			int r = orig_fade_quad(node);
			*(uint16_t*)(node + FADE_QUAD_COUNTER) = saved;
			*FADE_QUAD_DONE = done;
			return r & ~2;
		}

		// STAGE BRIGHTNESS RAMP. Writes a brightness word into the four stage-part records
		// 0x1D98992 + i*0x2C, which 0x500FD0 consumes when it draws them. Counter: s16 at +0x0C.
		constexpr uint32_t ADDR_STAGE_BRIGHT = 0x501F90;
		const uint8_t STAGE_BRIGHT_PROLOGUE[5] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };        // push esi / mov esi,[esp+8]
		constexpr uint32_t STAGE_BRIGHT_COUNTER = 0x0C;
		node_fn_t orig_stage_bright = nullptr;

		int __cdecl stage_bright_hook(uint8_t* node)
		{
			if (orig_stage_bright == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_stage_bright(node);
			tick_update();
			if (!hold_frame()) return orig_stage_bright(node);
			uint16_t saved = *(uint16_t*)(node + STAGE_BRIGHT_COUNTER);
			int r = orig_stage_bright(node);
			*(uint16_t*)(node + STAGE_BRIGHT_COUNTER) = saved;
			return r & ~2;
		}

		// HIT / CRIT COLOUR BLINK. Walks a script of durations (cursor: u32 at +0x10) and toggles the
		// entity colour through 0x50C780 when the u16 counter at +0x16 reaches the current entry; the
		// phase byte at +0x14 flips with it. +0x10..+0x17 is held as one block (+0x15 is read only).
		// Its second exit - the state-driven one at the tail - also ORs bit 4 into entity[1] before
		// returning 2; that OR is idempotent and the masked return only delays the removal by at most
		// one logic tick.
		constexpr uint32_t ADDR_CRIT_FLASH = 0x5057D0;
		const uint8_t CRIT_FLASH_PROLOGUE[5] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };          // push esi / mov esi,[esp+8]
		constexpr uint32_t CRIT_FLASH_STATE = 0x10;
		constexpr uint32_t CRIT_FLASH_STATE_LEN = 8;
		node_fn_t orig_crit_flash = nullptr;

		int __cdecl crit_flash_hook(uint8_t* node)
		{
			if (orig_crit_flash == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_crit_flash(node);
			tick_update();
			if (!hold_frame()) return orig_crit_flash(node);
			uint8_t saved[CRIT_FLASH_STATE_LEN];
			memcpy(saved, node + CRIT_FLASH_STATE, sizeof(saved));
			int r = orig_crit_flash(node);
			memcpy(node + CRIT_FLASH_STATE, saved, sizeof(saved));
			return r & ~2;
		}

		// 15-FRAME STENCIL FLASH on a model (0x50F190 -> 0x5099D0, alpha node[0x13]*0x1000/15).
		// Only the u8 frame at +0x13 is held. The byte at +0x12 is a LATCH, not a counter (0 = first
		// call: clear the model's own draw bit entity+0x7C, then 1 for the rest of the effect); a held
		// frame really is that first call, so the latch is left to advance exactly as in vanilla.
		// PROLOGUE CONSTRAINT: E8 at +5, so the cut must be exactly 5 bytes.
		constexpr uint32_t ADDR_STENCIL_FLASH = 0x50F0E0;
		const uint8_t STENCIL_FLASH_PROLOGUE[5] = { 0x53, 0x56, 0x57, 0x6A, 0x4C };       // push ebx,esi,edi / push 4Ch  (E8 follows)
		constexpr uint32_t STENCIL_FRAME = 0x13;
		node_fn_t orig_stencil_flash = nullptr;

		int __cdecl stencil_flash_hook(uint8_t* node)
		{
			if (orig_stencil_flash == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_stencil_flash(node);
			tick_update();
			if (!hold_frame()) return orig_stencil_flash(node);
			uint8_t saved = node[STENCIL_FRAME];
			int r = orig_stencil_flash(node);
			node[STENCIL_FRAME] = saved;
			return r & ~2;
		}

		// The same flash on a BONE CHAIN, with a decaying wobble. This one already guards its wobble
		// (0x56CFB0 on the saved chain 0x1D99BF8 plus the accumulators 0x1D99C0C/10/14) with the freeze
		// bit at 0x50F40A, but its +0x13 frame counter is outside that guard. The wobble is applied to
		// the bone BEFORE the draw and stepped after it, so holding it under the bit keeps the last
		// tick's wobble on screen instead of snapping the chain back - no strobe.
		// The +0x12 latch must NOT be held here either, and for a stronger reason than above: its
		// zero branch re-snapshots the live bone block into 0x1D99BF8, and the pose interpolator
		// rewrites those matrices after queue A has run, so re-arming the latch would re-base the
		// wobble accumulator from the current pose on every host frame and flatten the wobble.
		// PROLOGUE CONSTRAINT: E8 at +6, so the cut must be exactly 6 bytes.
		constexpr uint32_t ADDR_BONE_FLASH = 0x50F2E0;
		const uint8_t BONE_FLASH_PROLOGUE[6] = { 0x53, 0x55, 0x56, 0x57, 0x6A, 0x4C };    // push ebx,ebp,esi,edi / push 4Ch  (E8 follows)
		node_fn_t orig_bone_flash = nullptr;

		int __cdecl bone_flash_hook(uint8_t* node)
		{
			if (orig_bone_flash == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_bone_flash(node);
			tick_update();
			if (!hold_frame()) return orig_bone_flash(node);
			uint8_t saved = node[STENCIL_FRAME];
			uint32_t freeze = *BATTLE_FREEZE;
			*BATTLE_FREEZE = freeze | 1;
			int r = orig_bone_flash(node);
			*BATTLE_FREEZE = freeze;
			node[STENCIL_FRAME] = saved;
			return r & ~2;
		}

		// DRAG ENTITY TO TARGET. Adds (source bone - target bone) into entity+0x54/58/5C every call;
		// the add is a servo that converges within one frame, so repeating it per host frame is inert.
		// Counter: s32 countdown at +0x18. Its completion also clears flag bit 0x20 at entity+2, which
		// a held frame publishes at most one logic tick early (idempotent).
		// PROLOGUE CONSTRAINT: E8 at +5, so the cut must be exactly 5 bytes.
		constexpr uint32_t ADDR_DRAG = 0x50F500;
		const uint8_t DRAG_PROLOGUE[5] = { 0x53, 0x56, 0x57, 0x6A, 0x10 };                // push ebx,esi,edi / push 10h  (E8 follows)
		constexpr uint32_t DRAG_COUNTER = 0x18;
		node_fn_t orig_drag = nullptr;

		int __cdecl drag_hook(uint8_t* node)
		{
			if (orig_drag == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_drag(node);
			tick_update();
			if (!hold_frame()) return orig_drag(node);
			int32_t saved = *(int32_t*)(node + DRAG_COUNTER);
			int r = orig_drag(node);
			*(int32_t*)(node + DRAG_COUNTER) = saved;
			return r & ~2;
		}

		// TWEEN of two shorts (camera zoom / roll) on the struct at *(node+0x0C), over node+0x14
		// frames. Counter: u16 at +0x16. Its completion copies the tweened values +0x1C/+0x20 back
		// over the tween's own SOURCE +0x14/+0x18, so those two shorts are held as well - otherwise a
		// held frame would rebase the tween and snap it to its end value a logic tick early.
		constexpr uint32_t ADDR_TWEEN = 0x50F750;
		const uint8_t TWEEN_PROLOGUE[7] = { 0x8B, 0x4C, 0x24, 0x04, 0x53, 0x55, 0x56 };   // mov ecx,[esp+4] / push ebx,ebp,esi
		constexpr uint32_t TWEEN_COUNTER = 0x16;
		constexpr uint32_t TWEEN_TARGET = 0x0C;
		node_fn_t orig_tween = nullptr;

		int __cdecl tween_hook(uint8_t* node)
		{
			if (orig_tween == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_tween(node);
			tick_update();
			if (!hold_frame()) return orig_tween(node);
			uint16_t saved = *(uint16_t*)(node + TWEEN_COUNTER);
			uint8_t* target = *(uint8_t**)(node + TWEEN_TARGET);
			uint16_t from_a = 0, from_b = 0;
			if (target != nullptr)
			{
				from_a = *(uint16_t*)(target + 0x14);
				from_b = *(uint16_t*)(target + 0x18);
			}
			int r = orig_tween(node);
			*(uint16_t*)(node + TWEEN_COUNTER) = saved;
			if (target != nullptr)
			{
				*(uint16_t*)(target + 0x14) = from_a;
				*(uint16_t*)(target + 0x18) = from_b;
			}
			return r & ~2;
		}

		// MULTI-HIT SPAWNER. The only queue A runner that draws NOTHING: every `script[0] & 0x3F`
		// frames it spawns a hit effect (0x570E60) and a sound (0x501740) and advances its script
		// cursor. Holding its counter would re-fire that one-shot on every held frame, so this one is
		// skipped outright on held frames - which is also exactly the vanilla cadence.
		constexpr uint32_t ADDR_MULTIHIT = 0x50F830;
		const uint8_t MULTIHIT_PROLOGUE[6] = { 0x53, 0x56, 0x8B, 0x74, 0x24, 0x0C };      // push ebx,esi / mov esi,[esp+0C]
		node_fn_t orig_multihit = nullptr;

		int __cdecl multihit_hook(uint8_t* node)
		{
			if (orig_multihit == nullptr) return 0;
			if (node == nullptr || !in_battle() || pace_off()) return orig_multihit(node);
			tick_update();
			if (!hold_frame()) return orig_multihit(node);
			return 0; // nothing to draw, nothing to hold - just do not spawn ten times per logic frame
		}

		// =========================================================================================
		// 3. Status glow / float / confuse / icons - 0x50A0A0, call site 0x502B5D
		// =========================================================================================
		// 0x50A410 (the icon and the damage digits above the head) and the per-status handlers behind
		// 0x5711C0 DRAW fresh transient primitives on every call, so the call must NOT be skipped -
		// that is what made the icon strobe. 0x50A0A0 never reads the freeze bit either. What has to
		// be held is the per-status byte array at *(e+0x88): indices 0x09 (float bob phase), 0x0A and
		// 0x12 (status glow phases, 0x50A1C0), 0x0B (confuse/berserk spin, 0x50A370), plus 0x11 and
		// 0x19, and the animation state bytes of the five animated statuses at indices 5, 6, 7, 8 and
		// 0x0C, which 0x5711C0 reads and writes back. e+0x22 (float Y) and e+0x40 (spin) are pure
		// functions of those phases and need no restore.
		// NOTE: the inventory names two blocks, "*(e+0x88)" and "*(e+0x44)". They are the SAME block:
		// Ghidra types the parameter as ushort*, so its `*(int *)(param_1 + 0x44)` is byte offset
		// 0x88. Verified in the listing: 0x50A18B/0x50A19E, 0x50A237/0x50A265, 0x50A3C0, 0x50A614 all
		// load [entity+0x88] and nothing in 0x50A0A0 touches entity+0x44.
		constexpr uint32_t CS_STATUS_FX = 0x502B5D;      // call 0x50A0A0(entity), cdecl, one argument
		constexpr uint32_t FN_STATUS_FX = 0x50A0A0;      // prologue 57 8B 7C 24 08 (call site hooked, no trampoline)
		typedef void (__cdecl *status_fx_t)(uint8_t*);
		status_fx_t orig_status_fx = (status_fx_t)FN_STATUS_FX;

		const uint8_t STATUS_PHASE_IDX[6] = { 0x09, 0x0A, 0x0B, 0x11, 0x12, 0x19 };
		const uint8_t STATUS_ANIM_IDX[5]  = { 0x05, 0x06, 0x07, 0x08, 0x0C };

		void __cdecl status_fx_hook(uint8_t* e)
		{
			if (e == nullptr || !in_battle() || pace_off()) { orig_status_fx(e); return; }
			tick_update();
			if (!hold_frame()) { orig_status_fx(e); return; }
			uint8_t* s = *(uint8_t**)(e + 0x88);
			if (s == nullptr) { orig_status_fx(e); return; }
			uint8_t phase[sizeof(STATUS_PHASE_IDX)];
			uint8_t anim[sizeof(STATUS_ANIM_IDX)];
			for (size_t i = 0; i < sizeof(STATUS_PHASE_IDX); i++) phase[i] = s[STATUS_PHASE_IDX[i]];
			for (size_t i = 0; i < sizeof(STATUS_ANIM_IDX); i++) anim[i] = s[STATUS_ANIM_IDX[i]];
			orig_status_fx(e);
			for (size_t i = 0; i < sizeof(STATUS_PHASE_IDX); i++) s[STATUS_PHASE_IDX[i]] = phase[i];
			for (size_t i = 0; i < sizeof(STATUS_ANIM_IDX); i++) s[STATUS_ANIM_IDX[i]] = anim[i];
		}

		// =========================================================================================
		// 4. Queue D - the stage / battlefield queue (driver site 0x50097E -> 0x506C30)
		// =========================================================================================
		// DO NOT RETRY: holding 0x500FD0's dummies makes the camera spin (v23, v61).
		// v61 hooked 0x500FD0 itself (6-byte cut `53 55 56 57 6A 54`) and made its four dummy reads at
		// the call site 0x50102E -> 0x508F90 answer 0 on held frames. The user saw the battle camera
		// spin between frames again, with the intro camera flow visible through it - exactly the v23
		// symptom. Both hooks are gone and neither should come back in this shape. The four stage
		// dummies 0x1D989A4 + i*0x2C have to be left alone: 0x508F90's own pose hook in animations.cpp
		// already decodes them once per logic tick and interpolates in between, and dummy 3 is what
		// feeds the stage view matrix, so ANY answer invented for them moves the camera. Stage geometry
		// stepping at host rate is the lesser evil.
		//
		// What is left is the narrowest piece that touches no dummy, no track and no matrix: the
		// background scroll / spin accumulator 0x1D96DA4, held across the whole queue D call. 0x500FD0
		// reads it at 0x501069 (sky angle = (short)0x1D96DA4 >> 3) and accumulates 0x1D98988 into it at
		// 0x5010CF; the ONLY other reference in the binary is 0x500EA9, which zeroes it when a stage
		// loads (xref: exactly three references, all three accounted for). The draw reads the value
		// BEFORE the accumulate, so saving the dword before 0x506C30 and restoring it after makes every
		// host frame draw the same sky angle and a tick frame advance it exactly once. It cannot move
		// the camera: the value only reaches 0x56CE30 as a Y rotation for the stage parts.
		constexpr uint32_t CS_QUEUE_D = 0x50097E;        // call 0x506C30, void __cdecl(void)
		constexpr uint32_t FN_QUEUE_D = 0x506C30;        // if (*0x1D98A40) 0x508420(*0x1D98A40)
		typedef void (__cdecl *queue_d_t)();
		queue_d_t orig_queue_d = (queue_d_t)FN_QUEUE_D;
		uint32_t* const STAGE_SPIN = (uint32_t*)0x1D96DA4;

		bool traced_stage = false;

		void __cdecl queue_d_hook()
		{
			if (!in_battle() || pace_off()) { orig_queue_d(); return; }
			tick_update();
			if (!hold_frame()) { orig_queue_d(); return; }
			uint32_t spin = *STAGE_SPIN;
			orig_queue_d();
			*STAGE_SPIN = spin;
			if (ff8_interp::cfg().trace && !traced_stage)
			{
				traced_stage = true;
				ff8_interp::log_trace("ff8fxq f=%u background spin 0x1D96DA4 held (%u) across queue D\n", ff8_interp::frame_counter(), spin);
			}
		}
	}

	void fx_queues_hook_init()
	{
		hooks_installed = 0;

		// 1. Queue B: the call-site freeze plus the four runners with work outside the bit test.
		ff8_interp::hook_call(CS_QUEUE_B, (void*)queue_b_hook);
		hooks_installed++;
		orig_fx_twin = (node_fn_t)install_fn(ADDR_FX_TWIN, FX_TWIN_PROLOGUE, sizeof(FX_TWIN_PROLOGUE), (void*)fx_twin_hook, "impact fx twin");
		orig_fx_debris = (node_fn_t)install_fn(ADDR_FX_DEBRIS, FX_DEBRIS_PROLOGUE, sizeof(FX_DEBRIS_PROLOGUE), (void*)fx_debris_hook, "debris flipbook");
		orig_fx_ribbon = (node_fn_t)install_fn(ADDR_FX_RIBBON, FX_RIBBON_PROLOGUE, sizeof(FX_RIBBON_PROLOGUE), (void*)fx_ribbon_hook, "big trail/ribbon");
		orig_fx_flash = (node_fn_t)install_fn(ADDR_FX_FLASH, FX_FLASH_PROLOGUE, sizeof(FX_FLASH_PROLOGUE), (void*)fx_flash_hook, "full-screen flash");

		// 2. Queue A: the ten runners that ignore the freeze bit.
		orig_shake = (node_fn_t)install_fn(ADDR_SHAKE, SHAKE_PROLOGUE, sizeof(SHAKE_PROLOGUE), (void*)shake_hook, "screen shake");
		orig_cam_settle = (node_fn_t)install_fn(ADDR_CAM_SETTLE, CAM_SETTLE_PROLOGUE, sizeof(CAM_SETTLE_PROLOGUE), (void*)cam_settle_hook, "camera settle");
		orig_fade_quad = (node_fn_t)install_fn(ADDR_FADE_QUAD, FADE_QUAD_PROLOGUE, sizeof(FADE_QUAD_PROLOGUE), (void*)fade_quad_hook, "fade quad");
		orig_stage_bright = (node_fn_t)install_fn(ADDR_STAGE_BRIGHT, STAGE_BRIGHT_PROLOGUE, sizeof(STAGE_BRIGHT_PROLOGUE), (void*)stage_bright_hook, "stage brightness");
		orig_crit_flash = (node_fn_t)install_fn(ADDR_CRIT_FLASH, CRIT_FLASH_PROLOGUE, sizeof(CRIT_FLASH_PROLOGUE), (void*)crit_flash_hook, "crit colour blink");
		orig_stencil_flash = (node_fn_t)install_fn(ADDR_STENCIL_FLASH, STENCIL_FLASH_PROLOGUE, sizeof(STENCIL_FLASH_PROLOGUE), (void*)stencil_flash_hook, "stencil flash");
		orig_bone_flash = (node_fn_t)install_fn(ADDR_BONE_FLASH, BONE_FLASH_PROLOGUE, sizeof(BONE_FLASH_PROLOGUE), (void*)bone_flash_hook, "bone stencil flash");
		orig_drag = (node_fn_t)install_fn(ADDR_DRAG, DRAG_PROLOGUE, sizeof(DRAG_PROLOGUE), (void*)drag_hook, "drag to target");
		orig_tween = (node_fn_t)install_fn(ADDR_TWEEN, TWEEN_PROLOGUE, sizeof(TWEEN_PROLOGUE), (void*)tween_hook, "camera tween");
		orig_multihit = (node_fn_t)install_fn(ADDR_MULTIHIT, MULTIHIT_PROLOGUE, sizeof(MULTIHIT_PROLOGUE), (void*)multihit_hook, "multi-hit spawner");

		// 3. Status glow / float / icons: the call site only, the call itself always runs.
		ff8_interp::hook_call(CS_STATUS_FX, (void*)status_fx_hook);
		hooks_installed++;

		// 4. Queue D: the background spin hold only - see the DO NOT RETRY note above.
		ff8_interp::hook_call(CS_QUEUE_D, (void*)queue_d_hook);
		hooks_installed++;

		ff8_interp::log_info("%s: battle task-queue pacing installed, %d hooks (queue B site + 4 runners, queue A 10 runners, status fx, queue D spin hold).\n", __func__, hooks_installed);
	}

	void fx_queues_reset()
	{
		traced_queue_b = false;
		traced_stage = false;
	}
}
