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

// Renzokuken trigger-bar smoothing (v66).
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
// is_logic_tick_now(), and that flag is constant for a whole host frame, so on a logic-tick frame ALL
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

// WINDOW OPEN (GROW) ANIMATION (v79, bit DBG2_WINDOW_OPEN_BLEND_OFF in ff8_battle_interp_debug2).
// User report on v78: the command menu and the sub menus (Draw's list and Stock/Cast, Magic, Item ...)
// "look awfully slow" when they appear. Measured against the binary it is NOT slower than vanilla:
//   * every battle window's open state is one s16 scale 0..0x1000 in the window's own scratch,
//     stepped by its UPDATE callback by +0x199 per CALL - command window 0x1D76840 (0x4BB9E0, state
//     case 0x4BBD43), list windows 0x1D768E8 (0x4FDD90 case 0x4FDFC7), slot window (Stock/Cast)
//     0x1D768D4 (0x4C73E0 case 1). None of these steps looks at ctx+0x21.
//   * ui_update_hook runs 0x4B9C80 on all four HUD passes of a logic-tick frame, i.e. 4 updates per
//     1/15 s - exactly vanilla's 4 per rendered 15 fps frame. A full open is 10-11 updates = 2.5-2.75
//     ticks = ~170 ms in both. ctx+0x21 only gates the close (the list draws zero a negative scale on
//     a fresh pass: 0x4C7F0F / 0x4C832F / 0x4C85AF / 0x4C88BF, slot draw 0x4C79E3) and some "wait one
//     fresh tick" states; the latch sets it once per HOST frame now, so those can only come sooner.
// So the step count per second is vanilla's; what the user sees is the 15 Hz hold: 3 sizes, each
// held ~66 ms, next to a 144 Hz scene.
// The scale reaches the screen through one function: the window-frame drawer 0x4A7510
// `uint32 __cdecl (prim, cursor, int scale, sprite)` (13 call sites; the list and slot windows via
// 0x4BD220 -> 0x4BD53E, the command window at 0x4BD012). It turns the scale into the frame rect
// (0x4A7440: w,h *= (0x40 - 0x1D2B00C[scale >> 6]) / 64; negative scale uses 0x1D2B050) and, while
// scale < 0x1000, emits the clip rect (0x4A73A0) that cuts the content to it. So blending that ONE
// ARGUMENT grows frame and content together and no window global is ever written.
// What else the argument reaches (all draw-side scratch, checked): the scaled rect lands in
// *ctx->unk3C (rewritten by every frame draw before use) and 0x4B1660 -> 0x4B1670 max-accumulates
// its right edge into the party rows' +0x44/+0x46 and 0x1D7686E/0x1D76870, which the latch resets
// every fresh pass (0x4B0ED0) and only the status-panel draws read (0x4B0F10, 0x4B23ED) - so the
// party panel is pushed aside by the size that is DRAWN, as it should be. The status update 0x4B0500
// reads none of them.
// Keeping the caller's own branches exact: 0x4BD220 / 0x4BCE10 decide "scale < 0x1000 -> emit the
// draw-area reset 0x4A7630" from the RAW scale, and 0x4A7510 emits its clip only for a passed scale
// < 0x1000. Hence the blend EXTRAPOLATES (like the Renzokuken bar) instead of trailing:
//     drawn = raw + step * alpha,  clamped to [raw, 0xFFF] while raw < 0x1000;  raw when raw is 0x1000
// A raw 0x1000 is always drawn as 0x1000 and a raw < 0x1000 always stays < 0x1000, so the pair of
// packets is never split. Continuous by construction: the growth is linear, at alpha -> 1 the drawn
// value reaches the next tick's raw. alpha = tick_alpha_lead_now() (v79a, as the Renzokuken bar),
// applied on the tick frame too (2.1).
// step (v79a) = raw(this tick) - raw(previous tick), both taken per window key: `prev` is the last
// raw drawn during the previous logic tick, rolled on the first draw of a new tick. v79 measured the
// step from "the four passes of the tick frame" instead; in game (logs/v79_menu_not_smooth.log) that
// measurement was always 0 (`step 0` on every ff8win line: a window key is drawn once per host frame
// at the latest state, never pass by pass), so only the fallback ever worked, and it has no history
// on the FIRST visible tick of an open - the window held its first size for a whole tick, i.e. one
// of the ~2.5 ticks of the animation stayed stepped. The first tick now uses the KNOWN rate where
// the owner is known: 4 updates x 0x199 = 0x664 per tick for the command window (site 0x4BD012) and
// for every list / slot window, identified by the 0x4BD220 caller (0x4C7A4C slot, 0x4C7FEA,
// 0x4C83F0, 0x4C866A, 0x4C897A lists) that the 0x4BD220 hook publishes while it runs. Any other
// site (limit windows 0x4AF635/0x4AF6BF, help boxes, captions) is blended only once it has history.
// Keying: a window is (call site of 0x4A7510 - or the 0x4BD220 caller when the site is 0x4BD53E -,
// occurrence of that site within one draw pass); the occurrence counter is reset by a call-site hook
// on the draw pass 0x4A8BAD -> 0x4B9DB0.
// Snap (draw raw): a window without history and no known rate, raw <= 0 (closing / closed:
// negative scales are the instant close), raw >= 0x1000 (open), raw < prev (another window took the
// key, or it re-opened), a step > 0x800 (one tick is 0x664).
//
// THE TWO SLIDES (v79a, same guarantees, same bit).
// * List page slide: 0x1D768F8 (s16) is set to +-0x199 by the page-change cases of 0x4FDD90
//   (0x4FEA01 / 0x4FEAB2), += +-0x199 per update (case 0x4FE043), and reset to 0 once |v| > 0xFFF.
//   The list draws copy it to ctx+0x32 right before 0x4BD220, whose ONLY reader it is
//   (`iVar8 = ctx+0x32`: == 0 -> no slide; < 0 / > 0 pick the direction; tbl[|v| >> 6] the offset).
//   Hook: trampoline on 0x4BD220 (prologue 83 EC 1C A1 90 D4 D6 01), key = its return site. The
//   blend is in magnitude with the sign kept and never 0: |drawn| = |raw| + step*alpha clamped to
//   [|raw|, 0xFFF], so every branch of 0x4BD220 is the one the raw value takes. ctx+0x32 is written
//   before the original and restored after it; the global 0x1D768F8 is never touched.
// * Command-swap slide: byte 0x1D7684A, 0..0x40, +-8 per update of 0x4BB9E0 (0x4BC33F..0x4BC375,
//   holding right on a command with an alternative, e.g. a limit break). Draw-side readers are only
//   0x4BCE87 inside the command draw 0x4BCE10 (`== 0` test and tbl[0x1D7684A] offset); the other
//   readers are the update, the init 0x4BCC0D and the target selector 0x4C7151 - none runs inside the
//   draw. Hook: trampoline on 0x4BCE10 (prologue 83 EC 08 0F BF 05 40 68 D7 01, two whole
//   instructions), write the blend, call, restore. Kept in [1, 0x40] while raw != 0 (the `== 0`
//   branch is the raw one); raw 0 is drawn as 0. Step = raw - prev (+-32 per tick); prev is always
//   known because the command window is drawn every tick while open.

// PARTY PANEL: ATB / GF-CHARGE GAUGES AND ROLLING HP NUMBERS (v81, debug2 bits 4 / 8).
// Where the values come from (update 0x4B0500, window slot 7, 4 calls per logic tick):
//   * ATB width rec+0x40 = clamp(*(int*)(0x1CFF184 + slot*0x1D0) * 0x30 / *(int*)(0x1CFF180 + ...),
//     0, 0x30) - the real ATB counter/max the ATB tick 0x4842B0 steps (paced: all 4 HUD passes of a
//     tick frame, nothing on held frames). rec = 0x1D76928 + n*0x6C, slot = rec+0x48.
//   * GF-charge width rec+0x42 = u16 0x1CFF014 * 0x30 / u16 0x1CFF016 while 0x1CFF01C bit 0 (a GF is
//     being summoned; the row then shows the GF's name) - drawn instead of +0x40 when rec+0x3E bit 0.
//   * displayed HP rec+0x18 (20.12) moves towards rec+0x1E << 12 by rec+0x14 per update and is
//     formatted into the glyphs rec+0x24..0x27 (0x4B87F0 / 0x4B8840, or 0x4B07C0 on the way down).
//     That is the HP "drain" after damage: a rolling number, there is no HP bar and no limit gauge in
//     this panel. The second entry (+0x2C, glyphs +0x38) is the summoned GF's HP; not touched.
// How the gauge is drawn: row draw 0x4B0F10 (single site 0x4B17D5) sets ctx+0x34..0x3A to
// (row_x + 0x87, row_y + 7) .. (+width, +3) and 0x4B0D20 emits one G4 quad (0x38, left/right colours)
// plus a 4 px semi-transparent tip for widths 1..0x2F; width 0x30 takes the "full" colours (tables
// 0xB8792C / 0xB8793C instead of 0xB8790C / 0xB8791C). The quad corners are s16 PSX pixels, so ONE
// PSX PIXEL (1/48 of the gauge) is the finest step the draw can express - no sub-pixel path without
// packet surgery. At normal speed the gauge gains < 1 px per tick (the 15 Hz step is a 1 px step
// at a tick boundary); hasted / high-speed it gains 2-4 px per tick, and that multi-pixel jump at
// 15 Hz is what reads as choppy.
// What is done: the fill is tracked in 8.8 px from the engine's counters (read only) once per logic
// tick, and the row draw gets floor(lerp(prev, next, tick_alpha_now())) - LAG, not lead: a full gauge
// enables the command, so the drawn fill must never run ahead of the logic; it is also clamped to
// the engine's own rec+0x40 (never wider than what the engine draws this frame) and to 0x2F unless
// the engine says 0x30 (the "full" colour branch stays the engine's). Result: 1 px steps at the right
// moment at host rate instead of N px jumps at 15 Hz. Snap (raw): full, fill dropped (acted, KO,
// reset), a jump > 12 px in one tick, gauge kind / slot / max changed, no previous tick.
// HP: the displayed value lerps between the last two ticks (same direction, same target) and is
// formatted by the engine's own 0x4B07C0 into the four glyph bytes for the row draw only; a new
// target mid-roll snaps. The low-HP colour and the KO test read rec+0x1C/+0x1E/+0x18, untouched.
// Written only inside the bracket and restored after it: rec+0x40 or +0x42 and rec+0x24..0x27. The
// update 0x4B0500 (tick frames) rewrites both itself; no logic reads the glyphs.

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
		// --- the window slot and the shared slot-6 scratch block ---------------------------------

		void** const SLOT6_DRAW_CALLBACK = (void**)0x1D766A8;  // slot table 0x1D76628 + 6*0x14 + 0x08
		constexpr uint32_t FN_RENZOKUKEN_DRAW = 0x4BAB00;      // the Renzokuken bar's draw callback

		int16_t* const RENZOKUKEN_STATE       = (int16_t*)0x1D76750; // state-machine state (14 cases at 0x4BAAC4)
		int16_t* const RENZOKUKEN_BLOCK_COUNT = (int16_t*)0x1D76792; // block count, the draw's loop bound
		int16_t* const RENZOKUKEN_SCROLL      = (int16_t*)0x1D76798; // +4 per logic tick
		int16_t* const RENZOKUKEN_SUB_STEP    = (int16_t*)0x1D7679A; // 0..3, +1 per update, reset on a fresh UI tick
		uint8_t* const RENZOKUKEN_DRAW_ENABLE = (uint8_t*)0x1D767A4; // 0x4BAB00 returns immediately when 0 (0x4BAB03)
		int16_t* const RENZOKUKEN_FINISH_EASE = (int16_t*)0x1D767A8; // finish ease 0x1000 -> 0, -0x2AA per update

		// The draw's own gate on the ease (0x4BAC1C, `cmp ax,0x1000 / jge skip`); it is also what keeps
		// `ease >> 6` inside the 64-entry table at 0x1D2B050.
		constexpr int32_t EASE_IDLE = 0x1000;

		// Four times the normal per-tick step of P: a bigger jump is a state reset or a skipped state.
		constexpr int32_t POSITION_SNAP_STEP = 16;

		// --- the bar's own draw callback -----------------------------------------------------------
		// Prologue `83 EC 10 A0 A4 67 D7 01` (sub esp,10h / mov al,[1D767A4]) - 8 bytes, absolute, so it
		// is position independent and safe to copy into a trampoline.
		constexpr uint32_t ADDR_RENZOKUKEN_DRAW = FN_RENZOKUKEN_DRAW;
		const uint8_t RENZOKUKEN_DRAW_PROLOGUE[8] = { 0x83, 0xEC, 0x10, 0xA0, 0xA4, 0x67, 0xD7, 0x01 };
		typedef uint32_t (__cdecl *renzokuken_draw_t)(uint32_t, uint32_t, uint32_t, uint32_t);
		renzokuken_draw_t original_renzokuken_draw = nullptr;

		// --- captured state ----------------------------------------------------------------------

		struct RenzokukenBarState
		{
			int32_t position;     // scroll + sub, the pair the draw subtracts
			int32_t ease;         // 0x1D767A8
			int16_t block_count;  // 0x1D76792
			int16_t state;        // 0x1D76750
			uint8_t draw_enabled; // 0x1D767A4
		};

		RenzokukenBarState hud_prev = {};
		RenzokukenBarState hud_next = {};
		bool have_prev = false;
		bool have_next = false;
		uint32_t capture_tick = 0xFFFFFFFF; // logic_tick_count() of the tick hud_next belongs to

		// Trace bookkeeping only.
		uint32_t snap_count = 0, blended_frames = 0;

		inline bool renzokuken_blend_disabled() { return (ff8_interp::cfg().battle_debug & DBG_HUD_BLEND_OFF) != 0; }

		// The Renzokuken bar owns slot 6 right now. Everything in this file is a no-op otherwise, so
		// the limit-break timer bar and the GF boost gauge never see a blended value.
		inline bool renzokuken_live() { return *SLOT6_DRAW_CALLBACK == (void*)(uintptr_t)FN_RENZOKUKEN_DRAW; }

		inline void capture_bar_state(RenzokukenBarState& out)
		{
			out.position = int32_t(*RENZOKUKEN_SCROLL) + int32_t(*RENZOKUKEN_SUB_STEP);
			out.ease = *RENZOKUKEN_FINISH_EASE;
			out.block_count = *RENZOKUKEN_BLOCK_COUNT;
			out.state = *RENZOKUKEN_STATE;
			out.draw_enabled = *RENZOKUKEN_DRAW_ENABLE;
		}

		inline void invalidate_capture()
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
		inline int32_t extrapolate_rounded(int32_t prev, int32_t next, float t, int32_t max_lead)
		{
			int32_t step = next - prev;
			if (step > max_lead) step = max_lead;
			if (step < -max_lead) step = -max_lead;
			const float value = float(next) + float(step) * t;
			return (int32_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
		}

		// Wrapped around the bar's OWN draw callback, so the blended value exists only inside the code
		// that emits the bar's quads. Deliberately does NOT look at is_logic_tick_now(): the tick
		// frame must be blended too, or every logic frame jumps ahead
		// and the next frame snaps back. On the tick frame the capture below has already run (the
		// window update is earlier in the same HUD pass), so hud_next is this tick's end state and
		// alpha is small, i.e. the tick frame shows about P_next and the frames after it lead on towards
		// the next tick's value (the extrapolation, v67a).
		uint32_t __cdecl renzokuken_draw_hook(uint32_t a, uint32_t prim, uint32_t slot, uint32_t state)
		{
			if (original_renzokuken_draw == nullptr) return prim;
			if (!in_battle() || renzokuken_blend_disabled()) return original_renzokuken_draw(a, prim, slot, state);
			tick_update();
			if (!have_prev || !have_next) return original_renzokuken_draw(a, prim, slot, state);
			// Stale pair: the window update did not run on the current logic tick (it sits behind
			// `ctx[0x1E] & 0x40`), so sweeping prev -> next again would rock the bar backwards once
			// per tick. Draw what the engine owns instead. Same for a bar that is not on screen: with
			// the draw-enable clear 0x4BAB00 returns at 0x4BAB14 and there is nothing to smooth.
			if (capture_tick != logic_tick_count() || *RENZOKUKEN_DRAW_ENABLE == 0)
				return original_renzokuken_draw(a, prim, slot, state);

			// v71: tick_alpha_now() became tick_accumulator / P (the module-wide lead fix). This bar keeps the
			// pre-v71 (tick_accumulator + dt) / P on purpose: it is played against, and that extra host frame puts
			// the drawn block where the logic will be when this frame is on screen - the R1 timing the
			// user verified in v67a/v69. It still reaches exactly the next capture's value at alpha 1.
			const float alpha = tick_alpha_lead_now();
			const int16_t saved_scroll = *RENZOKUKEN_SCROLL;
			const int16_t saved_sub = *RENZOKUKEN_SUB_STEP;
			const int16_t saved_ease = *RENZOKUKEN_FINISH_EASE;

			// The draw computes `block_x - scroll - sub`, so the whole pair may be folded into scroll.
			// On a snap the capture left prev == next, so this writes exactly `next`.
			*RENZOKUKEN_SCROLL = (int16_t)extrapolate_rounded(hud_prev.position, hud_next.position, alpha, POSITION_SNAP_STEP);
			*RENZOKUKEN_SUB_STEP = 0;
			// Both ends must already be inside the draw's own `ease < 0x1000` gate, so a blend can
			// never make 0x4BAB00 take a branch the engine would not have taken this frame. The ease
			// is extrapolated on the same time base as the bar - it only ever counts down, so leading
			// it keeps it monotonic and keeps the finisher's slide in step with the blocks.
			if (hud_next.ease < EASE_IDLE && hud_prev.ease < EASE_IDLE)
			{
				int32_t ease = extrapolate_rounded(hud_prev.ease, hud_next.ease, alpha, EASE_IDLE);
				if (ease < 0) ease = 0;
				if (ease > EASE_IDLE - 1) ease = EASE_IDLE - 1; // keep `ease >> 6` inside 0x1D2B050[64]
				*RENZOKUKEN_FINISH_EASE = (int16_t)ease;
			}

			const uint32_t next_prim = original_renzokuken_draw(a, prim, slot, state);

			// Unconditional and byte-exact: the update compares 0x1D76798 against 0x1D76796*4 to end
			// its state, so a leaked blend would end the sequence early or late.
			*RENZOKUKEN_SCROLL = saved_scroll;
			*RENZOKUKEN_SUB_STEP = saved_sub;
			*RENZOKUKEN_FINISH_EASE = saved_ease;

			blended_frames++;
			if (ff8_interp::cfg().trace && (blended_frames % 480) == 1)
				ff8_interp::log_trace("ff8hud f=%u renzokuken P %d -> %d a=%.2f ease %d -> %d blocks=%d snaps=%u\n",
					ff8_interp::frame_counter(), hud_prev.position, hud_next.position, alpha, hud_prev.ease, hud_next.ease, (int)hud_next.block_count, snap_count);
			return next_prim;
		}

		// --- Zell's Duel countdown, v87 -------------------------------------------------------------
		// The Duel window (slot 6 owner: update 0x4AF840, draw 0x4B00E0) counts its timer 0x1D76750 (s16,
		// 1/60 s units) down by one per window update while the 4-update budget 0x1D7677F lasts, i.e.
		// 4 per logic tick = 60 per second, and the draw shows it as seconds + hundredths through
		// 0x4AFD10(a, b, prim, timer) (call site 0x4B0251, timer = movsx [0x1D76750]). Updates run on
		// logic ticks only, so on screen the hundredths jumped by ~7 at 15 Hz. The countdown is shown as
		// it would be between ticks: next - step * alpha, with step = last tick's decrement (0..4), so
		// the drawn value reaches the next tick's value at alpha 1 and never passes it. Only the argument
		// changes; the timer itself and the rest of the window are untouched.
		constexpr uint32_t CALL_SITE_DUEL_COUNTDOWN = 0x4B0251;
		constexpr uint32_t FN_DUEL_COUNTDOWN = 0x4AFD10;
		int16_t* const DUEL_TIMER = (int16_t*)0x1D76750;
		constexpr int32_t DUEL_STEP_MAX = 4; // updates per logic tick
		typedef uint32_t (__cdecl *duel_countdown_t)(uint32_t, uint32_t, uint32_t, int32_t);
		duel_countdown_t original_duel_countdown = nullptr;
		int32_t duel_prev = 0, duel_next = 0;
		uint32_t duel_tick = 0xFFFFFFFF;

		uint32_t __cdecl duel_countdown_hook(uint32_t a, uint32_t b, uint32_t prim, int32_t timer)
		{
			if (!in_battle() || (ff8_interp::cfg().battle_debug2 & DBG2_DUEL_TIMER_BLEND_OFF))
				return original_duel_countdown(a, b, prim, timer);
			tick_update();
			const uint32_t tick = logic_tick_count();
			if (tick != duel_tick)
			{
				// First draw of a new logic tick (the window updates of this tick already ran).
				duel_prev = (duel_tick == tick - 1) ? duel_next : timer;
				duel_next = timer;
				duel_tick = tick;
			}
			int32_t step = duel_prev - duel_next;
			if (step < 0 || step > DUEL_STEP_MAX || timer != duel_next) step = 0; // reset / not counting
			int32_t shown_timer = timer;
			if (step > 0)
			{
				const float value = float(timer) - float(step) * tick_alpha_lead_now();
				shown_timer = (int32_t)(value + 0.5f);
				if (shown_timer < timer - step) shown_timer = timer - step;
				if (shown_timer < 0) shown_timer = 0;
			}
			return original_duel_countdown(a, b, prim, shown_timer);
		}

		// --- window open (grow) blend and the two menu slides, v79 / v79a ----------------------------
		// See the WINDOW OPEN and THE TWO SLIDES headers above. Everything here is draw-side: it only
		// changes an argument of the frame drawer, or a value that exists for the length of one draw
		// callback and is restored right after it.

		constexpr uint32_t ADDR_WINDOW_FRAME = 0x4A7510; // uint32 __cdecl (prim, cursor, int scale, sprite)
		// push ecx / mov eax,[1D6D490] - 6 bytes, absolute, position independent.
		const uint8_t WINDOW_FRAME_PROLOGUE[6] = { 0x51, 0xA1, 0x90, 0xD4, 0xD6, 0x01 };
		typedef uint32_t (__cdecl *window_frame_t)(uint32_t, uint32_t, int32_t, uint32_t);
		window_frame_t original_window_frame = nullptr;

		// The draw pass: its single call site in 0x4A8870. __cdecl, two args, the caller batches its
		// cleanup (`add esp,0x28` at 0x4A8BD9), so the hook must forward and not clean the stack.
		constexpr uint32_t CALL_SITE_DRAW_PASS = 0x4A8BAD;
		constexpr uint32_t FN_DRAW_PASS = 0x4B9DB0;
		typedef uint32_t (__cdecl *draw_pass_t)(uint32_t, uint32_t);
		draw_pass_t original_draw_pass = nullptr;

		// Generic list / slot window draw: uint32 __cdecl (prim, cursor, data, row_fn, tail_fn), caller
		// cleans 0x14. sub esp,1Ch / mov eax,[1D6D490] - 8 bytes, absolute.
		constexpr uint32_t ADDR_LIST_DRAW = 0x4BD220;
		const uint8_t LIST_DRAW_PROLOGUE[8] = { 0x83, 0xEC, 0x1C, 0xA1, 0x90, 0xD4, 0xD6, 0x01 };
		typedef uint32_t (__cdecl *list_draw_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
		list_draw_t original_list_draw = nullptr;
		constexpr uint32_t CALL_SITE_LIST_FRAME = 0x4BD53E;  // 0x4A7510 call inside 0x4BD220

		// Command window draw callback (slot 4), called as draw(a, prim, slot, state) by 0x4B9F61.
		// sub esp,8 / movsx eax,word [1D76840] - 10 bytes (two whole instructions), absolute.
		constexpr uint32_t ADDR_COMMAND_DRAW = 0x4BCE10;
		const uint8_t COMMAND_DRAW_PROLOGUE[10] = { 0x83, 0xEC, 0x08, 0x0F, 0xBF, 0x05, 0x40, 0x68, 0xD7, 0x01 };
		typedef uint32_t (__cdecl *command_draw_t)(uint32_t, uint32_t, uint32_t, uint32_t);
		command_draw_t original_command_draw = nullptr;
		constexpr uint32_t CALL_SITE_COMMAND_FRAME = 0x4BD012; // 0x4A7510 call inside 0x4BCE10
		// v79b: the target-selection name windows, drawn by 0x4AAC70 (from the target selector's draw
		// 0x4C73C0 and at the head of every list draw) with the scale g_targetCursorCtx[i]+0x2A.
		constexpr uint32_t CALL_SITE_TARGET_PARTY = 0x4AAD9E; // party-name window (sprite 0x49)
		constexpr uint32_t CALL_SITE_TARGET_ENEMY = 0x4AAE40; // enemy-name window (sprite 0x51)

		// Distinct trace tags per window family (v79b).
		const char* window_trace_tag(uint32_t site)
		{
			switch (site)
			{
			case CALL_SITE_COMMAND_FRAME: return "cmd";
			case 0x4C7A4C: return "slot(stock/cast)";
			case 0x4C7FEA: return "list7F00";
			case 0x4C83F0: return "list8320";
			case 0x4C866A: return "list85A0";
			case 0x4C897A: return "list88B0";
			case CALL_SITE_TARGET_PARTY: return "tgt-party";
			case CALL_SITE_TARGET_ENEMY: return "tgt-enemy";
			case CALL_SITE_LIST_FRAME: return "list(no caller)";
			case ADDR_COMMAND_DRAW: return "cmd-swap";
			default: return "other";
			}
		}

		uint8_t* const COMMAND_SWAP_SLIDE = (uint8_t*)0x1D7684A; // 0..0x40, +-8 per command-window update
		uint8_t** const UI_CONTEXT = (uint8_t**)0x1D6D490;
		constexpr int CONTEXT_LIST_SLIDE = 0x32;                 // s16, copied from 0x1D768F8 by the list draws

		constexpr int32_t WINDOW_FULLY_OPEN = 0x1000;           // fully open: 0x4A7440 returns the rect unscaled
		constexpr int32_t WINDOW_MAX_TICK_STEP = 0x800;         // 4 updates x 0x199 = 0x664 per tick, plus margin
		constexpr int32_t WINDOW_KNOWN_TICK_STEP = 4 * 0x199;   // per logic tick for the known owners
		constexpr int32_t SWAP_SLIDE_MAX = 0x40;
		constexpr int32_t SWAP_SLIDE_MAX_TICK_STEP = 40;        // 4 updates x 8 = 32 per tick, plus margin

		// Owners whose rate is verified: +0x199 per update for the scale and the page slide.
		inline bool has_known_open_rate(uint32_t site)
		{
			switch (site)
			{
			case CALL_SITE_COMMAND_FRAME: // command window scale 0x1D76840 (0x4BBD43)
			case 0x4C7A4C:         // slot window (Stock/Cast) 0x1D768D4 (0x4C73E0 case 1)
			case 0x4C7FEA:         // list windows 0x1D768E8 (0x4FDFC7), slide 0x1D768F8 (0x4FE043)
			case 0x4C83F0:
			case 0x4C866A:
			case 0x4C897A:
			case CALL_SITE_TARGET_PARTY:  // v79b: target windows, ctx+0x2A +0x199 per 0x4AA1D0 call (0x4AA37A), once per update
			case CALL_SITE_TARGET_ENEMY:
				return true;
			default:
				return false;
			}
		}

		// Key kinds share one table: `kind_and_occurrence` carries the kind in its high bits.
		constexpr uint32_t KIND_SCALE = 0x00000; // + occurrence within the draw pass
		constexpr uint32_t KIND_SLIDE = 0x10000; // + occurrence within the draw pass
		constexpr uint32_t KIND_SWAP  = 0x20000;

		struct WindowTrack
		{
			uint32_t site = 0;                // owning call site
			uint32_t kind_and_occurrence = 0; // kind | occurrence of that site inside one draw pass
			uint32_t tick = 0;                // logic_tick_count() of `next`
			int32_t prev = 0, next = 0;       // raw at the end of the previous tick / latest raw this tick
			bool in_use = false;
			bool have_prev = false;
			uint32_t traced_tick = 0xFFFFFFFF; // trace only: last tick a line was written for this key
		};
		constexpr int WINDOW_TRACK_COUNT = 32;
		WindowTrack window_tracks[WINDOW_TRACK_COUNT];

		struct SiteOccurrenceCounter
		{
			uint32_t site;
			uint32_t count;
		};
		constexpr int SITE_COUNTER_CAPACITY = 24;
		SiteOccurrenceCounter site_counters[SITE_COUNTER_CAPACITY];
		int site_counters_used = 0;

		uint32_t list_caller = 0; // return site of the 0x4BD220 call in progress, 0 outside it

		uint32_t window_blend_count = 0, window_snap_count = 0; // trace only
		uint32_t window_trace_count = 0;

		inline bool window_blend_disabled() { return (ff8_interp::cfg().battle_debug2 & DBG2_WINDOW_OPEN_BLEND_OFF) != 0; }

		inline float window_alpha()
		{
			float alpha = tick_alpha_lead_now();
			if (alpha < 0.0f) alpha = 0.0f;
			if (alpha > 1.0f) alpha = 1.0f;
			return alpha;
		}

		void window_blend_reset()
		{
			for (WindowTrack& track : window_tracks)
				track = WindowTrack();
			site_counters_used = 0;
			list_caller = 0;
			window_blend_count = window_snap_count = window_trace_count = 0;
		}

		uint32_t next_site_occurrence(uint32_t site, uint32_t kind)
		{
			const uint32_t counter_key = site ^ (kind << 8); // separate counters per kind
			for (int i = 0; i < site_counters_used; i++)
				if (site_counters[i].site == counter_key) return site_counters[i].count++;
			if (site_counters_used < SITE_COUNTER_CAPACITY)
			{
				site_counters[site_counters_used++] = { counter_key, 1 };
				return 0;
			}
			return 0xFFFF; // table full: untracked, drawn raw
		}

		WindowTrack* find_or_claim_window_track(uint32_t site, uint32_t kind_and_occurrence, uint32_t now)
		{
			WindowTrack* free_slot = nullptr;
			for (WindowTrack& track : window_tracks)
			{
				if (track.in_use && track.site == site && track.kind_and_occurrence == kind_and_occurrence) return &track;
				// A key not drawn for two ticks is a closed window: its slot may be reused.
				if (free_slot == nullptr && (!track.in_use || now - track.tick > 2)) free_slot = &track;
			}
			if (free_slot == nullptr) return nullptr;
			*free_slot = WindowTrack();
			free_slot->in_use = true;
			free_slot->site = site;
			free_slot->kind_and_occurrence = kind_and_occurrence;
			free_slot->tick = now - 2; // "not drawn on the previous tick"
			return free_slot;
		}

		// Once per logic tick per key: prev = the last raw drawn in the previous tick (history only if
		// the key was drawn on that tick); next = the latest raw.
		void roll_window_track(WindowTrack& track, int32_t raw)
		{
			const uint32_t now = logic_tick_count();
			if (track.tick != now)
			{
				track.have_prev = (now - track.tick) == 1;
				track.prev = track.next;
				track.tick = now;
			}
			track.next = raw;
		}

		WindowTrack* window_track_for(uint32_t site, uint32_t kind, bool per_pass)
		{
			const uint32_t occurrence = per_pass ? next_site_occurrence(site, kind) : 0;
			if (occurrence == 0xFFFF) return nullptr;
			return find_or_claim_window_track(site, kind | occurrence, logic_tick_count());
		}

		// Growth in magnitude (the open scale, the page slide): raw + step*alpha in [raw, 0xFFF].
		// `magnitude` > 0 always; the caller keeps the sign and the exact-raw cases.
		int32_t grow_magnitude(const WindowTrack& track, int32_t magnitude, int32_t prev_magnitude, bool prev_usable, uint32_t site, int32_t& step_out)
		{
			int32_t step = 0;
			if (prev_usable)
			{
				if (magnitude < prev_magnitude) // another window, or reopened
				{
					window_snap_count++;
					step_out = -1;
					return magnitude;
				}
				step = magnitude - prev_magnitude;
			}
			else if (has_known_open_rate(site))
				step = WINDOW_KNOWN_TICK_STEP; // first visible tick of a known owner
			else if (site == 0x4AF635 || site == 0x4AF6BF)
				step = 0x800; // v79c: the choice window (Draw "Stock / Cast") opens 0x400 -> 0xC00 -> 0x1000, 0x800 per tick (trace v79b)
			step_out = step;
			if (step <= 0) return magnitude;
			if (step > WINDOW_MAX_TICK_STEP)
			{
				window_snap_count++;
				return magnitude;
			}
			int32_t drawn = magnitude + (int32_t)(float(step) * window_alpha() + 0.5f);
			if (drawn > WINDOW_FULLY_OPEN - 1) drawn = WINDOW_FULLY_OPEN - 1; // stay inside every "< 0x1000" branch
			if (drawn < magnitude) drawn = magnitude;
			if (drawn != magnitude) window_blend_count++;
			(void)track;
			return drawn;
		}

		// v79b trace: one line per logic tick per key while it is animating (an open is ~3 lines), plus
		// one line the first time a site is ever seen - so every window family shows up by its tag even
		// when it never blends. (v79a sampled 1 blended call in 120 across all windows, which hid the
		// rarer windows completely.)
		uint32_t traced_sites[32];
		int traced_site_count = 0;

		void trace_window(const char* kind_label, uint32_t site, int32_t raw, int32_t drawn, const WindowTrack& track, int32_t step)
		{
			if (!ff8_interp::cfg().trace) return;
			bool seen = false;
			for (int i = 0; i < traced_site_count; i++)
			{
				if (traced_sites[i] == (site ^ (uint32_t)(uintptr_t)kind_label))
				{
					seen = true;
					break;
				}
			}
			if (!seen && traced_site_count < 32)
			{
				traced_sites[traced_site_count++] = site ^ (uint32_t)(uintptr_t)kind_label;
				ff8_interp::log_trace("ff8win f=%u new %s %s site %08x raw %d\n", ff8_interp::frame_counter(), kind_label, window_trace_tag(site), site, raw);
			}
			if (track.traced_tick == track.tick) return;
			const_cast<WindowTrack&>(track).traced_tick = track.tick;
			ff8_interp::log_trace("ff8win f=%u %s %s site %08x raw %d -> %d (prev %d%s step %d a=%.2f) blended=%u snapped=%u\n",
				ff8_interp::frame_counter(), kind_label, window_trace_tag(site), site, raw, drawn, track.prev, track.have_prev ? "" : " none", step, window_alpha(),
				window_blend_count, window_snap_count);
		}

		uint32_t __cdecl window_frame_hook(uint32_t prim, uint32_t cursor, int32_t scale, uint32_t sprite)
		{
			uint32_t site = (uint32_t)(uintptr_t)_ReturnAddress() - 5;
			if (!in_battle() || window_blend_disabled()) return original_window_frame(prim, cursor, scale, sprite);
			tick_update();
			// Every list / slot window frames itself through the one call inside 0x4BD220: key it by
			// the list draw that called 0x4BD220 instead, so each window owns its own history.
			if (site == CALL_SITE_LIST_FRAME && list_caller != 0) site = list_caller;
			WindowTrack* track = window_track_for(site, KIND_SCALE, true);
			if (track == nullptr) return original_window_frame(prim, cursor, scale, sprite);
			roll_window_track(*track, scale);
			if (scale <= 0 || scale >= WINDOW_FULLY_OPEN) return original_window_frame(prim, cursor, scale, sprite); // closed / closing / open: exact
			// A previous tick at 0x1000 (or closing) means this is a fresh open on a reused key - e.g. the
			// two lists that share the draw 0x4C85A0 - so treat it like a first tick (known rate), not a snap.
			const bool prev_usable = track->have_prev && track->prev > 0 && track->prev < WINDOW_FULLY_OPEN;
			int32_t step = 0;
			const int32_t drawn = grow_magnitude(*track, scale, track->prev, prev_usable, site, step);
			trace_window("scale", site, scale, drawn, *track, step);
			return original_window_frame(prim, cursor, drawn, sprite);
		}

		uint32_t __cdecl list_draw_hook(uint32_t prim, uint32_t cursor, uint32_t data, uint32_t row_fn, uint32_t tail_fn)
		{
			const uint32_t site = (uint32_t)(uintptr_t)_ReturnAddress() - 5;
			if (!in_battle() || window_blend_disabled()) return original_list_draw(prim, cursor, data, row_fn, tail_fn);
			tick_update();
			const uint32_t saved_caller = list_caller;
			list_caller = site;

			int16_t* slide = (int16_t*)(*UI_CONTEXT + CONTEXT_LIST_SLIDE);
			const int16_t raw = *slide;
			int16_t drawn = raw;
			WindowTrack* track = window_track_for(site, KIND_SLIDE, true);
			if (track != nullptr)
			{
				roll_window_track(*track, raw);
				if (raw != 0 && raw > -WINDOW_FULLY_OPEN && raw < WINDOW_FULLY_OPEN)
				{
					const int32_t sign = raw < 0 ? -1 : 1;
					const int32_t magnitude = raw * sign;
					// History only in the same direction: a new page turn starts from +-0x199 again.
					const bool prev_usable = track->have_prev && track->prev != 0 && ((track->prev < 0) == (raw < 0));
					int32_t step = 0;
					const int32_t drawn_magnitude = grow_magnitude(*track, magnitude, track->prev * sign, prev_usable, site, step);
					drawn = (int16_t)(drawn_magnitude * sign);
					trace_window("slide", site, raw, drawn, *track, step);
				}
			}

			*slide = drawn;
			const uint32_t result = original_list_draw(prim, cursor, data, row_fn, tail_fn);
			// 0x4BD220 only reads ctx+0x32; put the engine's value back unless something rewrote it.
			if (*slide == drawn) *slide = raw;
			list_caller = saved_caller;
			return result;
		}

		uint32_t __cdecl command_draw_hook(uint32_t a, uint32_t prim, uint32_t slot, uint32_t state)
		{
			if (!in_battle() || window_blend_disabled()) return original_command_draw(a, prim, slot, state);
			tick_update();
			const uint8_t raw = *COMMAND_SWAP_SLIDE;
			uint8_t drawn = raw;
			WindowTrack* track = window_track_for(ADDR_COMMAND_DRAW, KIND_SWAP, false);
			if (track != nullptr)
			{
				roll_window_track(*track, raw);
				// raw 0 is drawn as 0 (the draw's `== 0` branch); otherwise stay in [1, 0x40].
				if (raw != 0 && raw <= SWAP_SLIDE_MAX && track->have_prev)
				{
					const int32_t step = int32_t(raw) - track->prev;
					if (step != 0 && step <= SWAP_SLIDE_MAX_TICK_STEP && step >= -SWAP_SLIDE_MAX_TICK_STEP)
					{
						int32_t value = int32_t(raw) + (int32_t)(float(step) * window_alpha() + (step > 0 ? 0.5f : -0.5f));
						if (value < 1) value = 1;
						if (value > SWAP_SLIDE_MAX) value = SWAP_SLIDE_MAX;
						// never behind the engine's value in the direction of travel
						if (step > 0 && value < raw) value = raw;
						if (step < 0 && value > raw) value = raw;
						drawn = (uint8_t)value;
						if (drawn != raw) window_blend_count++;
						trace_window("swap", ADDR_COMMAND_DRAW, raw, drawn, *track, step);
					}
					else if (step != 0) window_snap_count++;
				}
			}
			*COMMAND_SWAP_SLIDE = drawn;
			const uint32_t result = original_command_draw(a, prim, slot, state);
			if (*COMMAND_SWAP_SLIDE == drawn) *COMMAND_SWAP_SLIDE = raw; // the draw never writes it; restore unconditionally in effect
			return result;
		}

		uint32_t __cdecl draw_pass_hook(uint32_t a, uint32_t b)
		{
			// Nothing else: no blended value exists while the pass runs (unlike the v66 bracket that
			// used this same site), the per-window changes live inside the callbacks hooked above.
			site_counters_used = 0; // occurrences count from 0 in every draw pass
			return original_draw_pass(a, b);
		}

		// --- party panel: ATB / GF-charge gauges and rolling HP numbers, v81 ------------------------
		// See the PARTY PANEL header above. Bracket around the row draw BattleHUD_DrawCharaRow 0x4B0F10
		// (its single call site 0x4B17D5 in BattleHUD_DrawPartyRows 0x4B1740): the blended width / glyphs
		// exist only while that one row is drawn and are restored right after it.

		constexpr uint32_t CALL_SITE_PARTY_ROW = 0x4B17D5;   // call 0x4B0F10(rec, a, prim, state), caller cleans
		constexpr uint32_t FN_PARTY_ROW = 0x4B0F10;
		typedef uint32_t (__cdecl *party_row_t)(uint32_t, uint32_t, uint32_t, uint32_t);
		party_row_t original_party_row = nullptr;

		constexpr uint32_t PARTY_RECORD_BASE = 0x1D76928;  // 3 rows, stride 0x6C
		constexpr uint32_t PARTY_RECORD_STRIDE = 0x6C;
		constexpr int RECORD_HP_DISPLAYED = 0x18; // int, displayed HP in 20.12, rolls towards RECORD_HP_CURRENT << 12
		constexpr int RECORD_HP_CURRENT   = 0x1E; // s16 current HP (the roll's target)
		constexpr int RECORD_HP_GLYPHS    = 0x24; // 4 glyph bytes, 0x60 + digit, 0x6B blank
		constexpr int RECORD_GAUGE_MODE   = 0x3E; // bit 0: the row shows the GF-charge gauge (+0x42) instead of ATB (+0x40)
		constexpr int RECORD_ATB_WIDTH    = 0x40; // s16 0..0x30 = cur * 0x30 / max of 0x1CFF184 / 0x1CFF180
		constexpr int RECORD_GF_WIDTH     = 0x42; // s16 0..0x30 = u16 0x1CFF014 * 0x30 / u16 0x1CFF016
		constexpr int RECORD_BATTLE_SLOT  = 0x48; // s8 battle slot of the character (index into the 0x1D0 records)
		constexpr uint32_t CHARACTER_STRIDE = 0x1D0;
		constexpr uint32_t ATB_MAXIMUM = 0x1CFF180;       // int, + slot * 0x1D0
		constexpr uint32_t ATB_CURRENT = 0x1CFF184;       // int, + slot * 0x1D0
		constexpr uint32_t GF_CHARGE_CURRENT = 0x1CFF014; // u16, + slot * 0x1D0
		constexpr uint32_t GF_CHARGE_MAXIMUM = 0x1CFF016; // u16, + slot * 0x1D0
		constexpr int32_t GAUGE_FULL = 0x30;
		constexpr int32_t GAUGE_MAX_TICK_STEP = 12 * 256; // 8.8 px; a normal tick is < 4 px even hasted
		typedef void (__cdecl *format_hp_digits_t)(int32_t, uint8_t*);
		const format_hp_digits_t format_hp_digits = (format_hp_digits_t)0x4B07C0; // (disp 20.12, out[4]): the engine's own formatter, pure

		struct GaugeTrack
		{
			uint32_t tick = 0xFFFFFFFF;
			bool have_prev = false, valid = false;
			int kind = -1, slot = -1;           // of `next`
			int kind_prev = -1, slot_prev = -1;
			int32_t max = 0, max_prev = 0;
			int32_t prev = 0, next = 0;         // fill in 8.8 px
		};
		struct HpTrack
		{
			uint32_t tick = 0xFFFFFFFF;
			bool have_prev = false, valid = false;
			int32_t prev = 0, next = 0;         // displayed HP, 20.12
			int32_t target_prev = 0, target = 0;
		};
		GaugeTrack gauge_tracks[3];
		HpTrack hp_tracks[3];
		uint32_t gauge_blended = 0, hp_blended = 0, panel_traced_tick = 0xFFFFFFFF;

		inline bool gauge_blend_disabled() { return (ff8_interp::cfg().battle_debug2 & DBG2_ATB_GAUGE_BLEND_OFF) != 0; }
		inline bool hp_blend_disabled() { return (ff8_interp::cfg().battle_debug2 & DBG2_HP_ROLL_BLEND_OFF) != 0; }

		void party_panel_reset()
		{
			for (int i = 0; i < 3; i++)
			{
				gauge_tracks[i] = GaugeTrack();
				hp_tracks[i] = HpTrack();
			}
			gauge_blended = hp_blended = 0;
			panel_traced_tick = 0xFFFFFFFF;
		}

		// Gauge fill in 8.8 px from the engine's own counters (read only), -1 when not computable.
		int32_t gauge_fill_fixed_point(int kind, int slot, int32_t& max_out)
		{
			const uint32_t base = (uint32_t)slot * CHARACTER_STRIDE;
			int64_t current, maximum;
			if (kind == 0)
			{
				current = *(const int32_t*)(ATB_CURRENT + base);
				maximum = *(const int32_t*)(ATB_MAXIMUM + base);
			}
			else
			{
				current = *(const uint16_t*)(GF_CHARGE_CURRENT + base);
				maximum = *(const uint16_t*)(GF_CHARGE_MAXIMUM + base);
			}
			max_out = (int32_t)maximum;
			if (maximum <= 0) return -1;
			int64_t fill = current * GAUGE_FULL * 256 / maximum;
			if (fill < 0) fill = 0;
			if (fill > GAUGE_FULL * 256) fill = GAUGE_FULL * 256;
			return (int32_t)fill;
		}

		// lerp (NOT lead): a full gauge enables the command, so the drawn fill must never run ahead of
		// the logic. Returns the width to draw; `raw` whenever the rules say snap.
		int16_t gauge_blend(int row, uint8_t* record, int16_t raw)
		{
			GaugeTrack& track = gauge_tracks[row];
			const int kind = (record[RECORD_GAUGE_MODE] & 1) ? 1 : 0;
			const int slot = (int8_t)record[RECORD_BATTLE_SLOT];
			if (slot < 0 || slot > 2)
			{
				track = GaugeTrack();
				return raw;
			}
			int32_t max = 0;
			const int32_t fill = gauge_fill_fixed_point(kind, slot, max);
			const uint32_t now = logic_tick_count();
			if (track.tick != now)
			{
				track.have_prev = track.valid && (now - track.tick) == 1;
				track.prev = track.next;
				track.kind_prev = track.kind;
				track.slot_prev = track.slot;
				track.max_prev = track.max;
				track.tick = now;
			}
			track.next = fill;
			track.kind = kind;
			track.slot = slot;
			track.max = max;
			track.valid = fill >= 0;
			if (!track.valid || !track.have_prev) return raw;
			if (raw < 0 || raw >= GAUGE_FULL) return raw;                                            // full: exact (colour branch 0x30)
			if (track.kind_prev != kind || track.slot_prev != slot || track.max_prev != max) return raw; // gauge switched
			const int32_t fill_step = track.next - track.prev;
			if (fill_step <= 0 || fill_step > GAUGE_MAX_TICK_STEP) return raw;                       // reset after acting, KO, Stop, jump
			float alpha = tick_alpha_now();
			if (alpha < 0.0f) alpha = 0.0f;
			if (alpha > 1.0f) alpha = 1.0f;
			int32_t width = (track.prev + (int32_t)(float(fill_step) * alpha)) >> 8;
			if (width > raw) width = raw; // never ahead of the width the engine itself draws now
			if (width > GAUGE_FULL - 1) width = GAUGE_FULL - 1;
			if (width < 0) width = 0;
			if (width != raw) gauge_blended++;
			return (int16_t)width;
		}

		// Rolling HP number: lerp between the displayed values of the last two ticks, formatted by the
		// engine's own 0x4B07C0. Returns false when the glyphs must stay as the engine wrote them.
		bool hp_blend(int row, uint8_t* record, uint8_t out[4], int32_t& shown)
		{
			HpTrack& track = hp_tracks[row];
			const int32_t displayed = *(const int32_t*)(record + RECORD_HP_DISPLAYED);
			const int32_t target = int32_t(*(const int16_t*)(record + RECORD_HP_CURRENT)) * 0x1000;
			const uint32_t now = logic_tick_count();
			if (track.tick != now)
			{
				track.have_prev = track.valid && (now - track.tick) == 1;
				track.prev = track.next;
				track.target_prev = track.target;
				track.tick = now;
			}
			track.next = displayed;
			track.target = target;
			track.valid = true;
			if (!track.have_prev || track.prev == track.next) return false; // not rolling
			if (track.target_prev != target) return false;                 // a new hit / heal mid roll: snap
			// Both ends on the same side of the target (the roll only ever moves towards it).
			if ((track.prev < target) != (track.next <= target) && track.next != target) return false;
			if (track.prev < 0 || track.next < 0) return false;
			float alpha = tick_alpha_now();
			if (alpha < 0.0f) alpha = 0.0f;
			if (alpha > 1.0f) alpha = 1.0f;
			const int32_t value = track.prev + (int32_t)(float(track.next - track.prev) * alpha);
			format_hp_digits(value, out);
			shown = value >> 12;
			return true;
		}

		uint32_t __cdecl party_row_hook(uint32_t record_address, uint32_t a, uint32_t prim, uint32_t state)
		{
			if (!in_battle()) return original_party_row(record_address, a, prim, state);
			const uint32_t record_offset = record_address - PARTY_RECORD_BASE;
			const int row = (int)(record_offset / PARTY_RECORD_STRIDE);
			if (record_offset % PARTY_RECORD_STRIDE != 0 || row < 0 || row > 2) return original_party_row(record_address, a, prim, state);
			tick_update();
			uint8_t* record = (uint8_t*)(uintptr_t)record_address;

			// The gauge the draw will read: +0x40 (ATB) unless +0x3E bit 0 selects +0x42 (GF charge).
			int16_t* gauge = (int16_t*)(record + ((record[RECORD_GAUGE_MODE] & 1) ? RECORD_GF_WIDTH : RECORD_ATB_WIDTH));
			const int16_t gauge_raw = *gauge;
			int16_t gauge_drawn = gauge_raw;
			if (!gauge_blend_disabled()) gauge_drawn = gauge_blend(row, record, gauge_raw);

			uint8_t* glyphs = record + RECORD_HP_GLYPHS;
			uint8_t glyphs_raw[4], glyphs_drawn[4];
			memcpy(glyphs_raw, glyphs, 4);
			memcpy(glyphs_drawn, glyphs, 4);
			int32_t hp_shown = 0;
			bool hp_rolling = false;
			if (!hp_blend_disabled()) hp_rolling = hp_blend(row, record, glyphs_drawn, hp_shown);
			if (hp_rolling && memcmp(glyphs_drawn, glyphs_raw, 4) != 0) hp_blended++;

			*gauge = gauge_drawn;
			if (hp_rolling) memcpy(glyphs, glyphs_drawn, 4);
			const uint32_t result = original_party_row(record_address, a, prim, state);
			// The row draw writes neither field; restore unless something else rewrote it meanwhile.
			if (*gauge == gauge_drawn) *gauge = gauge_raw;
			if (hp_rolling && memcmp(glyphs, glyphs_drawn, 4) == 0) memcpy(glyphs, glyphs_raw, 4);

			const bool trace_this_tick = ff8_interp::cfg().trace && (gauge_drawn != gauge_raw || hp_rolling) && panel_traced_tick != logic_tick_count() && (logic_tick_count() % 15) == 0;
			if (trace_this_tick)
			{
				panel_traced_tick = logic_tick_count();
				const GaugeTrack& gauge_track = gauge_tracks[row];
				ff8_interp::log_trace("ff8atb f=%u row %d %s raw %d -> %d (fp %d -> %d a=%.2f) hp %s %d (disp %d -> %d) gauges=%u hp=%u\n",
					ff8_interp::frame_counter(), row, (record[RECORD_GAUGE_MODE] & 1) ? "gf" : "atb", gauge_raw, gauge_drawn, gauge_track.prev, gauge_track.next, tick_alpha_now(),
					hp_rolling ? "shown" : "raw", hp_shown, hp_tracks[row].prev >> 12, hp_tracks[row].next >> 12, gauge_blended, hp_blended);
			}
			return result;
		}
	}

	// Capture, called once per window-update pass on a logic-tick frame (so four times per tick) at
	// the END of ui_update_hook's tick path. The first call of a tick rolls last tick's end state
	// into `prev`; every call overwrites `next`, so `next` ends up holding what the fourth pass left
	// behind - which is exactly the state the 15 fps engine would have drawn.
	void hud_blend_after_update()
	{
		if (original_renzokuken_draw == nullptr || !in_battle() || renzokuken_blend_disabled()) return;
		if (!renzokuken_live())
		{
			invalidate_capture();
			return;
		}
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
		capture_bar_state(hud_next);
		have_next = true;

		if (!have_prev)
		{
			hud_prev = hud_next;
			have_prev = true;
			return;
		}

		// Snap rule (research A.2): a state reset, a skipped state or a bar that has just appeared
		// must cut, not sweep. `state`, `block_count` and the draw-enable edge cut both movers; the two
		// movers also have one cut each, so the ease sitting idle at 0x1000 cannot stop the bar from
		// being blended.
		bool cut_all = false;
		if (hud_next.block_count != hud_prev.block_count) cut_all = true;
		if (hud_next.state != hud_prev.state) cut_all = true;
		if (hud_prev.draw_enabled == 0 && hud_next.draw_enabled != 0) cut_all = true;

		const int32_t position_step = hud_next.position - hud_prev.position;
		if (cut_all || position_step > POSITION_SNAP_STEP || position_step < -POSITION_SNAP_STEP)
		{
			if (hud_prev.position != hud_next.position) snap_count++;
			hud_prev.position = hud_next.position;
		}
		// The ease only ever counts down; an increase is the state machine re-arming it at 0x1000,
		// and a previous value at or above 0x1000 means it had not started yet (the draw skipped it).
		if (cut_all || hud_next.ease > hud_prev.ease || hud_prev.ease >= EASE_IDLE)
			hud_prev.ease = hud_next.ease;
	}

	void hud_blend_hook_init()
	{
		invalidate_capture();
		snap_count = blended_frames = 0;
		window_blend_reset();
		party_panel_reset();
		// v81 party panel (ATB / GF gauge, HP roll): one call-site hook, installed unless both bits are set.
		if ((ff8_interp::cfg().battle_debug2 & (DBG2_ATB_GAUGE_BLEND_OFF | DBG2_HP_ROLL_BLEND_OFF)) == (DBG2_ATB_GAUGE_BLEND_OFF | DBG2_HP_ROLL_BLEND_OFF))
			ff8_interp::log_info("%s: party panel gauge / HP roll blend disabled (debug2 bits 4 and 8).\n", __func__);
		else if (original_party_row == nullptr)
		{
			const uint32_t previous_target = ff8_interp::hook_call_chain(CALL_SITE_PARTY_ROW, (void*)party_row_hook);
			if (previous_target == 0)
				ff8_interp::log_warning("%s: party row call site %08X not patched, ATB gauges / HP numbers stay stepped.\n", __func__, CALL_SITE_PARTY_ROW);
			else
			{
				original_party_row = (party_row_t)(uintptr_t)previous_target;
				if (previous_target != FN_PARTY_ROW)
					ff8_interp::log_info("%s: party row call site %08X already redirected, chaining.\n", __func__, CALL_SITE_PARTY_ROW);
				ff8_interp::log_info("%s: party panel ATB gauge / HP roll blend installed (row draw site %08X -> %08X).\n", __func__, CALL_SITE_PARTY_ROW, previous_target);
			}
		}
		// v87 Duel countdown: one call site inside the Duel window's draw.
		if (ff8_interp::cfg().battle_debug2 & DBG2_DUEL_TIMER_BLEND_OFF)
			ff8_interp::log_info("%s: Duel countdown blend disabled (debug2 bit 256).\n", __func__);
		else if (original_duel_countdown == nullptr)
		{
			const uint32_t previous_target = ff8_interp::hook_call_chain(CALL_SITE_DUEL_COUNTDOWN, (void*)duel_countdown_hook);
			if (previous_target == 0)
				ff8_interp::log_warning("%s: Duel countdown call site %08X not patched, the countdown stays stepped.\n", __func__, CALL_SITE_DUEL_COUNTDOWN);
			else
			{
				original_duel_countdown = (duel_countdown_t)(uintptr_t)previous_target;
				ff8_interp::log_info("%s: Zell's Duel countdown blend installed (site %08X -> %08X).\n", __func__, CALL_SITE_DUEL_COUNTDOWN, previous_target);
			}
		}
		// v79 window open blend: independent of the Renzokuken bar's bit, so it is installed first.
		if (ff8_interp::cfg().battle_debug2 & DBG2_WINDOW_OPEN_BLEND_OFF)
			ff8_interp::log_info("%s: battle window open blend disabled (DBG2_WINDOW_OPEN_BLEND_OFF in ff8_battle_interp_debug2).\n", __func__);
		else if (original_window_frame == nullptr)
		{
			window_frame_t frame_trampoline = (window_frame_t)make_trampoline(ADDR_WINDOW_FRAME, WINDOW_FRAME_PROLOGUE, sizeof(WINDOW_FRAME_PROLOGUE));
			if (frame_trampoline == nullptr)
				ff8_interp::log_warning("%s: window frame drawer prologue mismatch at %08X, windows open in 15 Hz steps.\n", __func__, ADDR_WINDOW_FRAME);
			else
			{
				// The pass hook goes in first: without it the occurrence counters would never reset.
				uint32_t previous_target = ff8_interp::hook_call_chain(CALL_SITE_DRAW_PASS, (void*)draw_pass_hook);
				if (previous_target == 0)
					ff8_interp::log_warning("%s: draw-pass call site %08X not patched, windows open in 15 Hz steps.\n", __func__, CALL_SITE_DRAW_PASS);
				else
				{
					if (previous_target != FN_DRAW_PASS)
						ff8_interp::log_info("%s: draw-pass call site %08X already redirected to %08X, chaining.\n", __func__, CALL_SITE_DRAW_PASS, previous_target);
					original_draw_pass = (draw_pass_t)(uintptr_t)previous_target;
					original_window_frame = frame_trampoline;
					ff8_interp::hook_function(ADDR_WINDOW_FRAME, (void*)window_frame_hook);
					// v79a: list page slide (and the per-window key of the list scale) and command-swap slide.
					list_draw_t list_trampoline = (list_draw_t)make_trampoline(ADDR_LIST_DRAW, LIST_DRAW_PROLOGUE, sizeof(LIST_DRAW_PROLOGUE));
					if (list_trampoline != nullptr)
					{
						original_list_draw = list_trampoline;
						ff8_interp::hook_function(ADDR_LIST_DRAW, (void*)list_draw_hook);
					}
					else
						ff8_interp::log_warning("%s: list window draw prologue mismatch at %08X, page slide stepped.\n", __func__, ADDR_LIST_DRAW);
					command_draw_t command_trampoline = (command_draw_t)make_trampoline(ADDR_COMMAND_DRAW, COMMAND_DRAW_PROLOGUE, sizeof(COMMAND_DRAW_PROLOGUE));
					if (command_trampoline != nullptr)
					{
						original_command_draw = command_trampoline;
						ff8_interp::hook_function(ADDR_COMMAND_DRAW, (void*)command_draw_hook);
					}
					else
						ff8_interp::log_warning("%s: command window draw prologue mismatch at %08X, command swap stepped.\n", __func__, ADDR_COMMAND_DRAW);
					ff8_interp::log_info("%s: battle window open blend + menu slides installed (frame drawer %08X, draw pass site %08X, list draw 4BD220, command draw 4BCE10).\n",
						__func__, ADDR_WINDOW_FRAME, CALL_SITE_DRAW_PASS);
				}
			}
		}
		// A real kill switch: with the bit set the draw callback is not patched at all (v67 only
		// disabled the math, which made the 1024 bisect step meaningless).
		if (ff8_interp::cfg().battle_debug & DBG_HUD_BLEND_OFF)
		{
			ff8_interp::log_info("%s: Renzokuken trigger-bar blending disabled (DBG_HUD_BLEND_OFF).\n", __func__);
			return;
		}

		original_renzokuken_draw = (renzokuken_draw_t)make_trampoline(ADDR_RENZOKUKEN_DRAW, RENZOKUKEN_DRAW_PROLOGUE, sizeof(RENZOKUKEN_DRAW_PROLOGUE));
		if (original_renzokuken_draw == nullptr)
		{
			ff8_interp::log_warning("%s: Renzokuken bar draw prologue mismatch at %08X, the bar stays stepped.\n", __func__, ADDR_RENZOKUKEN_DRAW);
			return;
		}
		ff8_interp::hook_function(ADDR_RENZOKUKEN_DRAW, (void*)renzokuken_draw_hook);
		ff8_interp::log_info("%s: Renzokuken trigger-bar blending installed on the bar's own draw callback %08X (slot 6).\n",
			__func__, ADDR_RENZOKUKEN_DRAW);
	}

	void hud_blend_reset()
	{
		invalidate_capture();
		duel_tick = 0xFFFFFFFF;
		snap_count = blended_frames = 0;
		window_blend_reset();
		party_panel_reset();
	}
}
