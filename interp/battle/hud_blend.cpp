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

// Renzokuken trigger-bar smoothing (v66). Section A of research/smooth-hud-and-particles.md.
// Shares the battle module's tick state through internal.h; gated by DBG_HUD_BLEND_OFF.
//
// WHAT MOVES. The gunblade trigger bar is battle window slot 6 (table 0x1D76628, stride 0x14, so
// slot 6 = 0x1D766A0: update 0x4BA6C0 at +0x00, draw 0x4BAB00 at +0x08). The draw's block loop is
//
//     u        = block_x[i] - *(s16*)0x1D76798 - *(s16*)0x1D7679A + 0x10;   // 0x4BACD2..0x4BACEB
//     screen_x = u*4 + 0x58;                                                // 0x4BACF1
//
// and the update (state case 0x4BA97C) does, once per call:
//
//     sub (0x1D7679A) += 1, clamped to 3;                                   // 0x4BA97C..0x4BA997
//     if (ctx[0x21]) { scroll (0x1D76798) += 4; sub = 0; }                  // 0x4BA999..0x4BA9B3
//
// `ctx+0x21` ("a fresh UI tick", ctx = *0x1D6D490) is set by the input latch inside 0x4A8E30 at
// most ONCE per rendered frame, because its arm flag 0x1D6D494 is re-armed once per frame by the
// frame flip 0x5005A0 (0x50062D -> 0x4A9220) and the latch zeroes it again (0x4A8E9D). The HUD
// phase 0x4A84E0 runs FOUR times per rendered frame (0x47D0A2/AC/B6 and 0x47D146, all four behind
// `*(u16*)0x1CDBFE0 == 3`) and each pass runs one window update (0x4A87D1, behind
// `ctx[0x1E] & 0x40`), so vanilla gets four updates per rendered frame of which exactly one carries
// the scroll:
//
//     P = scroll + sub  advances +1 per update and +4 per logic tick = 16 screen px per tick.
//
// CADENCE (v66 finding - no change was needed). animations.cpp's ui_update_hook gates 0x4B9C80 on
// tick_is_real(), and that flag is constant for a whole host frame, so on a logic-tick frame ALL
// FOUR passes still run the update - the same four the 15 fps engine ran per rendered frame. There
// is no per-frame "already ran" latch in ui_update_hook, and menu_update_hook's `menu_last_result`
// is a return-value cache for HELD frames, not a run-once latch. (That all four passes really do
// run is also what forced the v63 pad-edge one-shot: every pass was consuming the same press.) So
// P does advance +4 per logic tick and the bar is not slowed down; what reads as sluggish is the
// 15 Hz HOLD next to a 144 fps scene, and that is what the blend below removes.
//
// THE BLEND (v66a - the bracket was far too wide in v66). The first cut hooked the whole window
// DRAW PASS through `replace_call(0x4A8BAD)` -> `0x4B9DB0`, i.e. the blended globals were live across
// all NINE window draws, the slot state machine inside `0x4B9DB0` and its two `0x47D8E0` calls. In
// game that broke the limit break outright: Rough Divide fired instantly, the bar never appeared and
// the finisher did no damage. A re-read of every reference to the three fields
// (`xref 0x1D76798 / 0x1D7679A / 0x1D767A8`) says only `0x4BACD5`, `0x4BACE5` and `0x4BAC15` - all
// inside `0x4BAB00` - read them outside the update, and the call site itself checks out (aligned
// `E8` at 0x4A8BAD, `0x4B9DB0` ends in a plain `C3` and the caller cleans 10 dwords at once with
// `add esp,0x28` at 0x4A8BD9, so the __cdecl pass-through was right). The cause was therefore NOT
// provable from the globals, so the bracket is now as small as it can be without packet surgery:
// `replace_function(0x4BAB00)` on the bar's own draw callback. Being called IS the dispatch, the
// blend exists only for the few hundred instructions that emit the bar's quads, and no other window,
// no slot state machine and no engine logic can observe it. If the symptom survives this, the cause
// is not in this file - bit DBG_HUD_BLEND_OFF (1024) turns the whole thing off in one flag.
//
// The callback is `uint32_t __cdecl draw(a, prim, slot, state)` (0x4B9F61: push ebx/ecx/edi/edx,
// `call eax`, `add esp,0x10`), `prim` is the packet cursor and the return value is the advanced
// cursor - `0x4BAB16` returns `[esp+0x1C]` (= `prim`) unchanged when the draw-enable is 0.
//
// A LEAD, NOT A LAG (v67a) - the one blend in this module that extrapolates.
// v67 shipped the textbook `lerp(prev, next, alpha)`, i.e. the drawn bar TRAILED the logic value by
// up to one whole logic tick (66 ms, 16 screen px). Every other blend in the module may do that: a
// camera, a pose or a particle is only looked at, so a uniform one-tick display lag is invisible.
// This bar is not looked at, it is PLAYED AGAINST. The R1 window is judged by the UPDATE against the
// live `0x1D7679E = (scroll + sub) - block_x[cur]`, so a trailing draw makes the player press when
// the block visually reaches the trigger zone and the logic has already moved past it - "the attack
// and the bar did not sync". The hit zone itself is static: `0x4BAB51` pushes the literal (0xA0,
// 0xB6) for the TRIGGER sprite, and `xref` shows the scroll pair has exactly ONE draw-side read
// each (0x4BACD5 / 0x4BACE5, inside the block loop), so only the blocks move and only they need the
// lead.
// Hence: P_draw = next + (next - prev) * alpha. `next` is the tick that is already current, the step
// is a constant +4 per tick, so the extrapolation is exact and continuous - at alpha = 1 it equals
// the value the next capture will produce. The lead is clamped to one normal step, and on a snap the
// capture has left prev == next so the draw is exactly `next`. Do NOT "fix" this back to a lerp.
//
// RESOLUTION. The draw multiplies by 4, so the finest position the block loop can express is 4
// screen px. Blending therefore turns one 16 px jump at 15 Hz into four 4 px steps at up to 60 Hz -
// exactly the motion the engine was designed for, and no finer. Going below 4 px would need packet
// surgery on the two G4 quads 0x49C310 emits (their X words are prim[3], prim[7], prim[0xD],
// prim[0x11]); that is a follow-up, not this change.
//
// SLOT 6 IS SHARED. Three owners register into it and they all alias the same 0x1D767xx scratch:
// the Renzokuken bar (draw 0x4BAB00), the limit-break timer bar (0x4ADBF0) and the GF boost gauge
// (0x56E130, whose value 0x209CEF0 v58's apply_results_hook also reads). The CAPTURE therefore
// dispatches on the registered draw callback *(void**)0x1D766A8 and does nothing at all unless the
// Renzokuken bar owns the slot; the blend needs no dispatch of its own since it now lives inside
// 0x4BAB00, which only ever runs for this owner.

#include "internal.h"

#include "../host.h"

#include <windows.h>
#include <stdint.h>

namespace ff8_battle_anim
{
	namespace
	{
		// --- the window slot and the shared slot-6 scratch block ---------------------------------

		void** const SLOT6_DRAW = (void**)0x1D766A8;     // slot table 0x1D76628 + 6*0x14 + 0x08
		constexpr uint32_t FN_RENZ_DRAW = 0x4BAB00;      // the Renzokuken bar's draw callback

		int16_t* const RENZ_STATE   = (int16_t*)0x1D76750; // state-machine state (14 cases at 0x4BAAC4)
		int16_t* const RENZ_BLOCKS  = (int16_t*)0x1D76792; // block count, the draw's loop bound
		int16_t* const RENZ_SCROLL  = (int16_t*)0x1D76798; // +4 per logic tick
		int16_t* const RENZ_SUB     = (int16_t*)0x1D7679A; // 0..3, +1 per update, reset on a fresh UI tick
		uint8_t* const RENZ_DRAW_EN = (uint8_t*)0x1D767A4; // 0x4BAB00 returns immediately when 0 (0x4BAB03)
		int16_t* const RENZ_EASE    = (int16_t*)0x1D767A8; // finish ease 0x1000 -> 0, -0x2AA per update

		// The draw's own gate on the ease (0x4BAC1C, `cmp ax,0x1000 / jge skip`); it is also what keeps
		// `ease >> 6` inside the 64-entry table at 0x1D2B050.
		constexpr int32_t EASE_IDLE = 0x1000;

		// Four times the normal per-tick step of P: a bigger jump is a state reset or a skipped state.
		constexpr int32_t P_SNAP = 16;

		// --- the bar's own draw callback -----------------------------------------------------------
		// Prologue `83 EC 10 A0 A4 67 D7 01` (sub esp,10h / mov al,[1D767A4]) - 8 bytes, absolute, so it
		// is position independent and safe to copy into a trampoline.
		constexpr uint32_t ADDR_RENZ_DRAW = FN_RENZ_DRAW;
		const uint8_t RENZ_DRAW_PROLOGUE[8] = { 0x83, 0xEC, 0x10, 0xA0, 0xA4, 0x67, 0xD7, 0x01 };
		typedef uint32_t (__cdecl *renz_draw_t)(uint32_t, uint32_t, uint32_t, uint32_t);
		renz_draw_t orig_renz_draw = nullptr;

		// --- captured state ----------------------------------------------------------------------

		struct HudState
		{
			int32_t p;        // scroll + sub, the pair the draw subtracts
			int32_t ease;     // 0x1D767A8
			int16_t blocks;   // 0x1D76792
			int16_t state;    // 0x1D76750
			uint8_t draw_en;  // 0x1D767A4
		};

		HudState hud_prev = {}, hud_next = {};
		bool have_prev = false, have_next = false;
		uint32_t capture_tick = 0xFFFFFFFF; // logic_tick_count() of the tick hud_next belongs to

		// Trace bookkeeping only.
		uint32_t snaps = 0, blended_frames = 0;

		inline bool blend_off() { return (ff8_interp::cfg().battle_debug & DBG_HUD_BLEND_OFF) != 0; }

		// The Renzokuken bar owns slot 6 right now. Everything in this file is a no-op otherwise, so
		// the limit-break timer bar and the GF boost gauge never see a blended value.
		inline bool renzokuken_live() { return *SLOT6_DRAW == (void*)(uintptr_t)FN_RENZ_DRAW; }

		inline void capture(HudState& s)
		{
			s.p = int32_t(*RENZ_SCROLL) + int32_t(*RENZ_SUB);
			s.ease = *RENZ_EASE;
			s.blocks = *RENZ_BLOCKS;
			s.state = *RENZ_STATE;
			s.draw_en = *RENZ_DRAW_EN;
		}

		inline void invalidate()
		{
			have_prev = have_next = false;
			capture_tick = 0xFFFFFFFF;
		}

		// EXTRAPOLATION, not interpolation - see the "A LEAD, NOT A LAG" note in the file header.
		// `next` is the state of the logic tick that is already current, so the drawn value leads from
		// it by one more step scaled by alpha instead of trailing `prev`. The per-tick step is a
		// constant (+4 for P), so next + (next - prev) * alpha is exact and continuous across a tick
		// boundary: at alpha = 1 it equals the value the next capture will produce. The step is
		// clamped to `max_lead` so a tick that slipped through the snap rules can never throw the
		// drawn value a long way past the engine.
		inline int32_t extrap_round(int32_t prev, int32_t next, float t, int32_t max_lead)
		{
			int32_t d = next - prev;
			if (d > max_lead) d = max_lead;
			if (d < -max_lead) d = -max_lead;
			const float v = float(next) + float(d) * t;
			return (int32_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
		}

		// Wrapped around the bar's OWN draw callback, so the blended value exists only inside the code
		// that emits the bar's quads. Deliberately does NOT look at tick_is_real(): rule 2.1 of
		// verify-checklist.md - the tick frame must be blended too, or every logic frame jumps ahead
		// and the next frame snaps back. On the tick frame the capture below has already run (the
		// window update is earlier in the same HUD pass), so hud_next is this tick's end state and
		// alpha is small, i.e. the tick frame shows about P_next and the frames after it lead on towards
		// the next tick's value (the extrapolation, v67a).
		uint32_t __cdecl renz_draw_hook(uint32_t a, uint32_t prim, uint32_t slot, uint32_t state)
		{
			if (orig_renz_draw == nullptr) return prim;
			if (!in_battle() || blend_off()) return orig_renz_draw(a, prim, slot, state);
			tick_update();
			if (!have_prev || !have_next) return orig_renz_draw(a, prim, slot, state);
			// Stale pair: the window update did not run on the current logic tick (it sits behind
			// `ctx[0x1E] & 0x40`), so sweeping prev -> next again would rock the bar backwards once
			// per tick. Draw what the engine owns instead. Same for a bar that is not on screen: with
			// the draw-enable clear 0x4BAB00 returns at 0x4BAB14 and there is nothing to smooth.
			if (capture_tick != logic_tick_count() || *RENZ_DRAW_EN == 0)
				return orig_renz_draw(a, prim, slot, state);

			// v71: tick_alpha_now() became tick_acc / P (the module-wide lead fix). This bar keeps the
			// pre-v71 (tick_acc + dt) / P on purpose: it is played against, and that extra host frame puts
			// the drawn block where the logic will be when this frame is on screen - the R1 timing the
			// user verified in v67a/v69. It still reaches exactly the next capture's value at alpha 1.
			const float alpha = tick_alpha_lead_now();
			const int16_t saved_scroll = *RENZ_SCROLL;
			const int16_t saved_sub = *RENZ_SUB;
			const int16_t saved_ease = *RENZ_EASE;

			// The draw computes `block_x - scroll - sub`, so the whole pair may be folded into scroll.
			// On a snap the capture left prev == next, so this writes exactly `next`.
			*RENZ_SCROLL = (int16_t)extrap_round(hud_prev.p, hud_next.p, alpha, P_SNAP);
			*RENZ_SUB = 0;
			// Both ends must already be inside the draw's own `ease < 0x1000` gate, so a blend can
			// never make 0x4BAB00 take a branch the engine would not have taken this frame. The ease
			// is extrapolated on the same time base as the bar - it only ever counts down, so leading
			// it keeps it monotonic and keeps the finisher's slide in step with the blocks.
			if (hud_next.ease < EASE_IDLE && hud_prev.ease < EASE_IDLE)
			{
				int32_t e = extrap_round(hud_prev.ease, hud_next.ease, alpha, EASE_IDLE);
				if (e < 0) e = 0;
				if (e > EASE_IDLE - 1) e = EASE_IDLE - 1; // keep `ease >> 6` inside 0x1D2B050[64]
				*RENZ_EASE = (int16_t)e;
			}

			const uint32_t r = orig_renz_draw(a, prim, slot, state);

			// Unconditional and byte-exact: the update compares 0x1D76798 against 0x1D76796*4 to end
			// its state, so a leaked blend would end the sequence early or late.
			*RENZ_SCROLL = saved_scroll;
			*RENZ_SUB = saved_sub;
			*RENZ_EASE = saved_ease;

			blended_frames++;
			if (ff8_interp::cfg().trace && (blended_frames % 480) == 1)
				ff8_interp::log_trace("ff8hud f=%u renzokuken P %d -> %d a=%.2f ease %d -> %d blocks=%d snaps=%u\n",
					ff8_interp::frame_counter(), hud_prev.p, hud_next.p, alpha, hud_prev.ease, hud_next.ease, (int)hud_next.blocks, snaps);
			return r;
		}
	}

	// Capture, called once per window-update pass on a logic-tick frame (so four times per tick) at
	// the END of ui_update_hook's tick path. The first call of a tick rolls last tick's end state
	// into `prev`; every call overwrites `next`, so `next` ends up holding what the fourth pass left
	// behind - which is exactly the state the 15 fps engine would have drawn.
	void hud_blend_after_update()
	{
		if (orig_renz_draw == nullptr || !in_battle() || blend_off()) return;
		if (!renzokuken_live()) { invalidate(); return; }
		tick_update();

		const uint32_t now_tick = logic_tick_count();
		if (capture_tick != now_tick)
		{
			// A gap of more than one logic tick means the bar was not updated in between: start over
			// rather than sweep through whatever happened meanwhile.
			const bool contiguous = have_next && (now_tick - capture_tick) == 1;
			hud_prev = hud_next;
			have_prev = contiguous;
			capture_tick = now_tick;
		}
		capture(hud_next);
		have_next = true;

		if (!have_prev)
		{
			hud_prev = hud_next;
			have_prev = true;
			return;
		}

		// Snap rule (research A.2): a state reset, a skipped state or a bar that has just appeared
		// must cut, not sweep. `state`, `blocks` and the draw-enable edge cut both movers; the two
		// movers also have one cut each, so the ease sitting idle at 0x1000 cannot stop the bar from
		// being blended.
		bool cut_all = false;
		if (hud_next.blocks != hud_prev.blocks) cut_all = true;
		if (hud_next.state != hud_prev.state) cut_all = true;
		if (hud_prev.draw_en == 0 && hud_next.draw_en != 0) cut_all = true;

		const int32_t dp = hud_next.p - hud_prev.p;
		if (cut_all || dp > P_SNAP || dp < -P_SNAP)
		{
			if (hud_prev.p != hud_next.p) snaps++;
			hud_prev.p = hud_next.p;
		}
		// The ease only ever counts down; an increase is the state machine re-arming it at 0x1000,
		// and a previous value at or above 0x1000 means it had not started yet (the draw skipped it).
		if (cut_all || hud_next.ease > hud_prev.ease || hud_prev.ease >= EASE_IDLE)
			hud_prev.ease = hud_next.ease;
	}

	void hud_blend_hook_init()
	{
		invalidate();
		snaps = blended_frames = 0;
		// A real kill switch: with the bit set the draw callback is not patched at all (v67 only
		// disabled the math, which made the 1024 bisect step meaningless).
		if (ff8_interp::cfg().battle_debug & DBG_HUD_BLEND_OFF)
		{
			ff8_interp::log_info("%s: Renzokuken trigger-bar blending disabled (DBG_HUD_BLEND_OFF).\n", __func__);
			return;
		}

		orig_renz_draw = (renz_draw_t)make_trampoline(ADDR_RENZ_DRAW, RENZ_DRAW_PROLOGUE, sizeof(RENZ_DRAW_PROLOGUE));
		if (orig_renz_draw == nullptr)
		{
			ff8_interp::log_warning("%s: Renzokuken bar draw prologue mismatch at %08X, the bar stays stepped.\n", __func__, ADDR_RENZ_DRAW);
			return;
		}
		ff8_interp::hook_function(ADDR_RENZ_DRAW, (void*)renz_draw_hook);
		ff8_interp::log_info("%s: Renzokuken trigger-bar blending installed on the bar's own draw callback %08X (slot 6).\n",
			__func__, ADDR_RENZ_DRAW);
	}

	void hud_blend_reset()
	{
		invalidate();
		snaps = blended_frames = 0;
	}
}
