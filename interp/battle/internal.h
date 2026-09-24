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

#pragma once

// Internal glue for the battle interpolation module - not part of the module interface.
//
// Everything below lives in animations.cpp (namespace ff8_battle_anim, mostly inside its anonymous
// namespace) and is exported here only so the sibling translation units of the same module
// (gf_pacing.cpp, fx_queues.cpp, fx_blend.cpp) can share the one per-host-frame tick state instead
// of each growing its own. Nothing outside src/ff8/interp/battle/ should include this file; the module's
// public surface stays animations.h.
//
// Tick contract: call tick_update() once per hooked call, then read tick_is_real() (this host frame
// consumes a real 15 Hz logic frame) and tick_alpha_now() (0..1 blend between the last two decoded
// states). Both stay constant for the rest of the host frame, so several hooks in one frame agree.

#include <stdint.h>
#include <stddef.h>

namespace ff8_battle_anim
{
	// --- shared per-host-frame tick state (defined in animations.cpp) ---

	// Advance the module tick for this host frame (idempotent within one frame_counter value).
	// Named tick_update() rather than update_tick() so it does not collide with the anonymous
	// namespace's own update_tick() inside animations.cpp.
	void tick_update();
	// True when this host frame consumes a real logic frame (the engine's own update may run).
	bool tick_is_real();
	// Blend factor 0..1 between the last two logic frames, for this host frame: the time since the
	// last tick, tick_acc / P (v71; before v71 it was (tick_acc + dt) / P, see DBG_ALPHA_LEAD).
	float tick_alpha_now();
	// The pre-v71 formula (tick_acc + dt) / P, clamped to 1: one host frame ahead of tick_alpha_now().
	// Only hud_blend's Renzokuken bar extrapolation uses it, so the user-verified R1 timing (the bar
	// drawn where the logic will be when the frame is SHOWN) is kept exactly across the v71 change.
	float tick_alpha_lead_now();
	// Logic ticks counted since the last animations_reset().
	uint32_t logic_tick_count();

	// True while the driver is in battle mode.
	bool in_battle();

	// Copy prologue_len bytes of the function at target into a fresh RWX stub and append a jmp back
	// to target + prologue_len. Returns nullptr when the prologue does not match (wrong executable).
	void* make_trampoline(uint32_t target, const uint8_t* prologue, size_t prologue_len);

	// The battle flags dword (0x1D96A9C). Bit 0 is FF8's own "frozen" flag: fades and effect runners
	// still draw while it is set but hold their own counters. Drive it around a single call only.
	extern uint32_t* const BATTLE_FREEZE;

	// --- debug bits for ff8_battle_interp_debug (toml, default 0) ---
	// Only the gates that are NOT yet confirmed in game keep a switch; every verified gate is
	// unconditional. Renumbered in v59. (the value is ff8_interp::cfg().battle_debug, toml ff8_battle_interp_debug.)
	constexpr long DBG_BLINK_OFF       = 1;  // UI clock / blink phase cadence gate off (v56)
	constexpr long DBG_POSE_CUT_OFF    = 2;  // never cut: always cross-fade a freshly queued animation (pre-v54)
	constexpr long DBG_RESULTS_GATE_ON = 4;  // opt in: pace the after-battle results screens at 60 Hz
	constexpr long DBG_PAD_REPEAT_OFF  = 8;  // menu cursor auto-repeat pacing off (v57)
	constexpr long DBG_GF_BOOST_OFF    = 16; // GF boost-gauge settle before a summon result off (v58)
	constexpr long DBG_IMPACT_FX_OFF   = 32; // impact / hit effect freeze-bit gate off (v58)
	constexpr long DBG_DEATH_FADE_SKIP = 64; // death fade back to the v57 plain skip (v58)
	constexpr long DBG_GF_PACE_OFF     = 128; // GF cinematic pacing off
	constexpr long DBG_QUEUE_PACE_OFF  = 256; // battle task-queue pacing off
	constexpr long DBG_FX_BLEND_OFF    = 512; // magic primitive blending off
	constexpr long DBG_HUD_BLEND_OFF   = 1024; // Renzokuken trigger-bar blending off (v66)
	constexpr long DBG_EDGE_ACC_OFF    = 8192; // pad-ring press-edge accumulation for Renzokuken / boost off (v69)
	constexpr long DBG_RENZ_SYNC_OFF   = 16384; // v70: a new spell tree runs at the logic rate from its first frame off (pre-v70: host rate until its 2nd tick)
	constexpr long DBG_PLAY_CLOCK_OFF  = 32768; // v71: play clock 0x4701B0 (play time, event countdown) at host rate again (9.6x fast at 144 Hz)
	constexpr long DBG_BOOST_ANY_GF_OFF = 65536; // v71: boost settle gated on Quezacotl's runner only again (pre-v71: every other GF can land for 0)
	constexpr long DBG_ALPHA_LEAD      = 131072; // v71: pre-v71 tick_alpha = (tick_acc + dt) / P everywhere (one host frame lead, pinned to 1 before a tick)
	constexpr long DBG_DUEL_EDGE_OFF   = 262144; // v71: Zell's Duel d-pad / shoulder edges (raw site 0x4AF8F2) read live again; 8192 also turns it off
	constexpr long DBG_APPEAR_RAMP_OFF = 524288; // v72: appear states 1/2/0xA/0xC ramp e[6] +1 per held host frame again (v58-v71: fade-in / intro ~6x fast)
	constexpr long DBG_INTRO_PACE_OFF  = 1048576; // v72: intro manager task chain (site 0x50090B, while 0x1D27B04 < 3) and the intro-flavour fade 0x501D10 at host rate again

	// --- entry points of the sibling translation units ---
	// Each hook_init() is called at the end of animations_hook_init(), each reset() at the end of
	// animations_reset(); both must be safe to call when the module is disabled or not installed.

	void gf_pacing_hook_init();
	void gf_pacing_reset();

	void fx_queues_hook_init();
	void fx_queues_reset();

	void fx_blend_hook_init();
	void fx_blend_reset();

	void hud_blend_hook_init();
	void hud_blend_reset();

	// --- hud_blend block (Renzokuken trigger bar, hud_blend.cpp) --------------------------------
	// The trigger bar is battle window slot 6 and its two movers, the scroll pair
	// 0x1D76798 / 0x1D7679A and the finish ease 0x1D767A8, are stepped by the window UPDATE pass
	// 0x4B9C80 - which animations.cpp owns through ui_update_hook. hud_blend.cpp owns the capture of
	// those movers and the blend it writes around the window DRAW pass 0x4B9DB0 (its one call site
	// 0x4A8BAD). Call this as the LAST thing in ui_update_hook's tick path, i.e. after the original
	// update has run, on every one of the four HUD passes of a logic-tick frame: the first call of a
	// tick rolls the previous tick's end state into `prev` and each call refreshes `next`, so `next`
	// ends up holding exactly what the 15 fps engine would have drawn for that tick. It is inert
	// outside battle, with DBG_HUD_BLEND_OFF set, and whenever slot 6 belongs to one of its other two
	// owners (the limit-break timer bar or the GF boost gauge) - it dispatches on the registered draw
	// callback *(void**)0x1D766A8 itself, so the caller needs no guard of its own.
	void hud_blend_after_update();
	// --- end hud_blend block --------------------------------------------------------------------

	// --- gf_pacing block (GF cinematic camera ownership, gf_pacing.cpp) -------------------------
	// True while a GF summon cinematic is on screen and driving the battle camera itself.
	// The cinematic writes the camera pose 0xB8B7F0 / 0xB8B7F8 and the roll 0x1D8E038 DIRECTLY -
	// absolute writes at its cut frames, read-modify-write for its pans - and it runs at frame-driver
	// call site 0x50093A, i.e. BEFORE updateBattleCamera 0x504060 (0x500988) and before the battle
	// render (0x500992) in the same host frame. While this returns true, battle_camera_hook must
	// leave 0xB8B7F0 and 0x1D8E038 completely alone, i.e. as the first thing after update_tick():
	//     if (gf_cinematic_owns_camera()) { if (tick_real) orig_battle_camera(); cam_pose_valid = false; return; }
	// - the original still runs once per logic tick (it also ticks the camera task queue 0x1D97768
	// and must not run per host frame), but there is no restore of cam_pose_next before it, no
	// capture after it and no blend. Clearing cam_pose_valid is what makes the first tick after the
	// cinematic capture a fresh pose instead of sweeping out of a stale one.
	// gf_pacing.cpp captures, holds and publishes the interpolated pose for those frames instead.
	// (During the cinematic the engine's own camera block is idle, so 0x504060 writes no pose at all
	// and the hook's capture was re-reading the value it had just written: prev == next == the pose
	// from the last tick before the summon - the camera stuck facing the summoner's spot.)
	bool gf_cinematic_owns_camera();
	// v72: true while ANY GF cinematic paced by gf_pacing.cpp is running (one frame of grace, like
	// gf_cinematic_owns_camera), whether or not that GF owns the camera. apply_results_hook's boost
	// gate uses this; gf_cinematic_owns_camera() is true only for camera-owning rows (Quezacotl).
	bool gf_cinematic_live();
	// --- end gf_pacing block --------------------------------------------------------------------

	// --- fx_blend block (magic primitive blending, fx_blend.cpp) -------------------------------
	// animations.cpp owns the recording of the display-list link calls and their replay; fx_blend.cpp
	// owns the pairing of two recorded lists, the interpolation and the scratch packets the
	// interpolated copies live in. Nothing else should use these.

	constexpr int FX_LINKERS = 5;     // display-list linkers hooked in animations.cpp
	constexpr int FX_PRIM_WORDS = 24; // dwords a recorded packet copy can hold (GT4 needs 12)

	// One recorded link call. It lives here rather than in animations.cpp's anonymous namespace so
	// fx_blend.cpp can read both recorded lists without copying them. `copy` is the packet as it
	// looked when the call was recorded: these effects rebuild their packets from a flipping arena
	// every frame, so the copy - never the live packet - is what may be read on a later frame.
	// The packet's word 0 is the PSX tag (high byte = length in words, low 24 bits = the next
	// pointer the linker patches), word 1 is the GPU command byte plus the first colour.
	struct FxCall
	{
		int fn;                       // index into FX_LINKER_ADDR
		uint32_t a[8];                // arguments, forwarded as eight dwords (all of them are cdecl)
		uint32_t* prim;               // == (uint32_t*)a[1] for the linkers that take a primitive
		int words;                    // dwords in `copy`, 0 when no copy could be taken
		uint32_t copy[FX_PRIM_WORDS];
		uint32_t ret;                 // the game call site that made this link call (v66 diagnostic)
	};

	// Bisect sub-bits for ff8_battle_interp_debug, additional to DBG_FX_BLEND_OFF (512). They live in
	// this block rather than the main DBG_ list because only fx_blend.cpp reads them.
	//   512  = everything the blending adds is off: no prepare, no scratch substitution, no deferral,
	//          no call-site capture. The display-list recording and the verbatim replay stay, so this
	//          is exactly v59 - a safe mode, not just a bisect step.
	//   2048 = keep blending on held frames but never hold a logic tick's link calls back, so the tick
	//          frame is drawn by the engine in place exactly as in v59.
	//   4096 = keep everything but the v67 SPRT / TILE masks, so the rectangle primitives fall back to
	//          being passed through unblended.
	constexpr long DBG_FX_DEFER_OFF = 2048;
	constexpr long DBG_FX_RECT_OFF  = 4096;

	// False when DBG_FX_BLEND_OFF is set. animations.cpp's linker wrappers consult it too, so with the
	// bit set nothing the blending added runs - but the recording and the verbatim replay do, leaving
	// the effect path byte-for-byte v59. 2048 sits between the two: blending on held frames, no
	// deferral.
	bool fx_blend_enabled();

	// True only while blending is actually succeeding (v63). animations.cpp asks before holding a
	// logic tick's link calls back: with the pair falling back there is nothing to substitute, so the
	// tick goes straight through the engine as it did in v59.
	bool fx_blend_defer_ok();

	// Pair the previous and the current recorded list of `slot` and build this host frame's
	// interpolated packets. Returns true when the list drawn should come from the scratch copies.
	// Call it once per list per host frame and BEFORE the first link call of that list is issued:
	// that is the only moment at which the scratch may grow.
	bool fx_blend_prepare(int slot, const FxCall* prev, size_t prev_n, const FxCall* cur, size_t cur_n, float alpha);

	// The primitive to hand the linker for call `i` of `slot`, or 0 when that call must be issued
	// against the address that was recorded (non-blendable linker, unknown command byte, ...).
	uint32_t fx_blend_arg(int slot, size_t i);
	// --- end fx_blend block --------------------------------------------------------------------
}
