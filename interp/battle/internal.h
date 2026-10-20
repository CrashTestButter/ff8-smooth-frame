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
// Tick contract: call tick_update() once per hooked call, then read is_logic_tick_now() (this host frame
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
	bool is_logic_tick_now();
	// Blend factor 0..1 between the last two logic frames, for this host frame: the time since the
	// last tick, tick_accumulator / P (v71; before v71 it was (tick_accumulator + dt) / P, see DBG_ALPHA_LEAD).
	float tick_alpha_now();
	// The pre-v71 formula (tick_accumulator + dt) / P, clamped to 1: one host frame ahead of tick_alpha_now().
	// Only hud_blend's Renzokuken bar extrapolation uses it, so the user-verified R1 timing (the bar
	// drawn where the logic will be when the frame is SHOWN) is kept exactly across the v71 change.
	float tick_alpha_lead_now();
	// Logic ticks counted since the last animations_reset().
	uint32_t logic_tick_count();

	// True while the driver is in battle mode.
	bool in_battle();

	// Copy prologue_length bytes of the function at target into a fresh RWX stub and append a jmp back
	// to target + prologue_length. Returns nullptr when the prologue does not match (wrong executable).
	void* make_trampoline(uint32_t target, const uint8_t* prologue, size_t prologue_length);

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
	constexpr long DBG_EDGE_ACCUMULATE_OFF = 8192; // pad-ring press-edge accumulation for Renzokuken / boost off (v69)
	constexpr long DBG_RENZOKUKEN_SYNC_OFF = 16384; // v70: a new spell tree runs at the logic rate from its first frame off (pre-v70: host rate until its 2nd tick)
	constexpr long DBG_PLAY_CLOCK_OFF  = 32768; // v71: play clock 0x4701B0 (play time, event countdown) at host rate again (9.6x fast at 144 Hz)
	constexpr long DBG_BOOST_ANY_GF_OFF = 65536; // v71: boost settle gated on Quezacotl's runner only again (pre-v71: every other GF can land for 0)
	constexpr long DBG_ALPHA_LEAD      = 131072; // v71: pre-v71 tick_alpha = (tick_accumulator + dt) / P everywhere (one host frame lead, pinned to 1 before a tick)
	constexpr long DBG_DUEL_EDGE_OFF   = 262144; // v71: Zell's Duel d-pad / shoulder edges (raw site 0x4AF8F2) read live again; 8192 also turns it off
	constexpr long DBG_APPEAR_RAMP_OFF = 524288; // v72: appear states 1/2/0xA/0xC ramp e[6] +1 per held host frame again (v58-v71: fade-in / intro ~6x fast)
	constexpr long DBG_INTRO_PACE_OFF  = 1048576; // v72: intro manager task chain (site 0x50090B, while 0x1D27B04 < 3) and the intro-flavour fade 0x501D10 at host rate again
	constexpr long DBG_GF_VM_OFF       = 2097152; // v75: animation-VM family summons (Ifrit, Bahamut, Cerberus, Alexander, Brothers, Eden, Leviathan; gf_vm.cpp) at host rate again; 128 also turns it off
	constexpr long DBG_GF_SIREN_OFF    = 4194304; // v75: the spell-path GF family (gf_siren.cpp: Siren 95; since v2 also Tonberry 90, Moogle Dance 96, Choco* 97..100, Moomba 338) at host rate again: their trees are no longer skipped + replayed on held frames; 128 also turns it off
	constexpr long DBG_ESCAPE_HOLD_OFF = 8388608; // v76: escape hold counter 0x1D6D638 (HUD pass 0x4A84E0) counts every host-frame pass again (escape roll timing irregular)
	constexpr long DBG_ANALOG_REPEAT_OFF = 16777216; // v76: analog-stick menu repeat ctx+0x26/+0x27 at host rate again, held-frame stick presses lost again
	constexpr long DBG_STATUS_TIMER_OFF = 33554432; // v76: status timers 0x483470 (Doom, Poison/Regen, durations) + Angelo/Gilgamesh/Odin rolls 0x482F80 run on every host frame again (0x1D28DEB not cleared off-tick)
	constexpr long DBG_FRAME_CALLBACK_OFF = 67108864; // v76: battle frame-callback list 0x482D50 (site 0x47D7FB: AI wait/message timers) on every host frame again
	constexpr long DBG_END_WATCHDOG_OFF = 134217728; // v76: end-of-battle countdown watchdog accumulates without reset again (tail of a 60-tick fade at host rate)
	constexpr long DBG_CURSOR_HOLD_OFF = 268435456; // v77: cursor sprites (hand / fingers / active-character marker, table 0x1D6D4B0, drawn by 0x4A78E0) not redrawn on held frames again (drawn ~1 frame in 10: flicker)
	constexpr long DBG_MARKER_PACE_OFF = 536870912; // v78: active-character marker (pyramid, 0x4BB090 at site 0x500960, battle_misc.cpp) spins / pulses at host rate again
	constexpr long DBG_FX_UNPACED_OFF  = 1073741824; // v78: a spell tree that cannot be replayed safely (a call from per-frame memory without a usable copy) stays paced; those calls are just missing on held frames instead of the tree running every host frame. Never re-enables in-place replay of per-frame packets (the v77 Sleep crash). Last free bit of a 32-bit long.

	// --- second kill-bit mask: ff8_battle_interp_debug2 (ff8_interp::cfg().battle_debug2) ---
	// The first mask is full (bit 31 is the sign of a 32-bit long and cannot be written as a positive
	// toml integer), so new switches go here, numbered from 1 again and prefixed DBG2_. Test them only
	// against cfg().battle_debug2 - a DBG2_ value tested against battle_debug would alias an old bit.
	constexpr long DBG2_WINDOW_OPEN_BLEND_OFF = 1; // v79 (hud_blend.cpp): battle window open/grow scale (0x4A7510 arg 3) drawn in 15 Hz steps again (v79a: also the list page slide and the command-swap slide); the hooks are not installed at all
	constexpr long DBG2_ATB_GAUGE_BLEND_OFF = 4; // v81 (hud_blend.cpp): ATB / GF-charge gauges of the party panel (row draw 0x4B0F10, rec+0x40/+0x42) in 15 Hz steps again
	constexpr long DBG2_HP_ROLL_BLEND_OFF = 8;   // v81 (hud_blend.cpp): rolling HP numbers of the party panel (rec+0x18 -> glyphs rec+0x24..0x27) in 15 Hz steps again
	// (4 and 8: the row-draw call-site hook 0x4B17D5 is not installed when BOTH are set; 2 is fx_blend's, below)
	constexpr long DBG2_ROOT_HANDOFF_OFF = 16;  // v82 (since v83 only with 32 set) (animations.cpp root_handoff): a frame-0 snap that jumps the pose root while the sequence position moves (attack run -> leap) is blended again instead of cut: the position eases from the start while the pose is already the new animation (v81 "teleport back for one frame")
	constexpr long DBG2_ROOT_BLEND_OFF = 32;    // v83 (animations.cpp snap): a cut (poses_far_apart) snaps the pose ROOT with the bones again instead of blending it from the shown root; the v82 hand-off cut (16) is then active again
	constexpr long DBG2_HOLD_LAST_OFF = 64;     // v83 (animations.cpp read_animation_hook, finished path): a finished animation held on its last frame re-blends frame n-1 -> n on every held host frame again (dying enemies twitch at 15 Hz during the death fade)
	constexpr long DBG2_FX_VOLATILE_BLEND_OFF = 2;   // v81 (fx_blend.cpp build_identities): a spell tree's own redraw of an entity model in per-frame memory (Sleep, Cure ...) is no longer blended by face identity - replayed verbatim, stepping at 15 Hz (v80)
	constexpr long DBG2_BONE_SLERP_OFF = 128;   // v85 (animations.cpp write_interpolated): bones are blended per axis again (pre-v84): a bone that changes its angle triple (Selphie's nunchaku) swings through poses neither frame has, and no near-180 turn is cut
	constexpr long DBG2_DUEL_TIMER_BLEND_OFF = 256; // v87 (hud_blend.cpp duel_countdown_hook): Zell's Duel countdown drawn in 15 Hz steps again (hundredths jump by ~7)
	constexpr long DBG2_PAUSED_BLEND_OFF = 512; // v88 (animations.cpp paused_bones_hook): entities with a paused sequence that an effect moves (Biggs and Wedge in Elvoret's entrance) step at 15 Hz again
	constexpr long DBG2_MODEL_SPACE_OFF = 1024; // v90 (animations.cpp write_model_space): weapons (<= 16 bones) blended per bone like bodies again (v86 rule, no v87 whole-part step) instead of in model space
	constexpr long DBG2_GRIP_OFF = 2048; // v91 (animations.cpp grip_apply): weapon ends held by a hand (Selphie's far nunchaku stick) no longer kept in that hand between logic frames
	constexpr long DBG2_EXTERNAL_POSITION_OFF = 4096; // v102 (animations.cpp position_note_external): positions an effect writes to an entity whose sequence runs (Elvoret's descent) are overwritten by the blend again
	constexpr long DBG2_FX_COHERENCE_OFF = 8192; // v104 (fx_blend.cpp): spell emitter runs whose geometry is rebuilt every tick (Thundaga's bolts) are blended pair by pair again instead of drawn verbatim
	constexpr long DBG2_FX_LIVE_DRAW_OFF = 16384; // v105 (animations.cpp xatm_model_draw_hook): X-ATM092's defeat explosion replays the machine's recorded triangles again (15 Hz) instead of redrawing it live
	constexpr long DBG2_FX_RIGID_OFF = 32768; // v106 (fx_blend.cpp blend_one): spell polygons blended corner by corner again instead of as shapes (tumbling debris collapses between ticks)
	constexpr long DBG2_FX_FAR_MOVE_OFF = 65536; // v107 (fx_blend.cpp blend_one): spell polygons that move > 4x their size per tick are blended again (Thundaga's last dirt slides across the screen)
	constexpr long DBG2_FX_PROXIMITY_PAIR_OFF = 131072; // v108 (fx_blend.cpp): spell particles paired by list position again instead of by proximity when the order does not fit (X-ATM092's smoke jumps between legs and party)
	constexpr long DBG2_FX_TRAJECTORY_OFF = 262144; // v109 (fx_blend.cpp): no trajectory test - spell pieces moving > 4x their size per tick are always stepped (v107)
	constexpr long DBG2_STALL_CATCHUP = 524288; // v116 (animations.cpp): after a stall >= 100 ms the battle clock catches up with an extra tick again (pre-v116)
	constexpr long DBG2_CAMERA_ROLL_OFF = 1048576; // v117 (animations.cpp) / gf v10: camera roll 0x1D977A2 in 15 Hz steps again (intro fly-throughs, GF cinematics)
	constexpr long DBG2_GF_CAMERA_VM_CUT_OFF = 2097152; // gf v10 (gf_pacing.cpp): GF camera cuts by distance only (> 2048), not by the camera VM's set op (Odin's shots swept)
	constexpr long DBG2_GF_MODEL_TRANSLATION_OFF = 4194304; // gf v10 (gf_pacing.cpp): GF creature world position in 15 Hz steps (Odin's charge)
	constexpr long DBG2_WEAPON_ATTACH_OFF = 8388608; // v118 (animations.cpp): weapon attach transform (weapon block +0x14/+0x1C) in 15 Hz steps (Rinoa's Blaster Edge in flight)

	// --- entry points of the sibling translation units ---
	// Each hook_init() is called at the end of animations_hook_init(), each reset() at the end of
	// animations_reset(); both must be safe to call when the module is disabled or not installed.

	void gf_pacing_hook_init();
	void gf_pacing_reset();

	void fx_queues_hook_init();
	void fx_queues_reset();

	void fx_blend_hook_init();
	void fx_blend_reset();

	void battle_misc_hook_init(); // battle_misc.cpp (v78): small standalone gates
	void battle_misc_reset();

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
	// v79: hud_blend.cpp also smooths the open (grow) animation of every battle window. It is
	// self-contained - hud_blend_hook_init() installs a trampoline on the shared window-frame drawer
	// 0x4A7510 and a call-site hook on the draw pass (0x4A8BAD -> 0x4B9DB0); no caller elsewhere.
	// Only the scale ARGUMENT handed to the frame drawer is blended, never a window global. v79a adds
	// trampolines on 0x4BD220 (list page slide ctx+0x32) and 0x4BCE10 (command-swap byte 0x1D7684A),
	// both written just before the original draw and restored right after it.
	// --- end hud_blend block --------------------------------------------------------------------

	// --- gf_pacing block (GF cinematic camera ownership, gf_pacing.cpp) -------------------------
	// True while a GF summon cinematic is on screen and driving the battle camera itself.
	// The cinematic writes the camera pose 0xB8B7F0 / 0xB8B7F8 and the roll 0x1D8E038 DIRECTLY -
	// absolute writes at its cut frames, read-modify-write for its pans - and it runs at frame-driver
	// call site 0x50093A, i.e. BEFORE updateBattleCamera 0x504060 (0x500988) and before the battle
	// render (0x500992) in the same host frame. While this returns true, battle_camera_hook must
	// leave 0xB8B7F0 and 0x1D8E038 completely alone, i.e. as the first thing after update_tick():
	//     if (gf_cinematic_owns_camera()) { if (is_logic_tick) original_battle_camera(); camera_pose_valid = false; return; }
	// - the original still runs once per logic tick (it also ticks the camera task queue 0x1D97768
	// and must not run per host frame), but there is no restore of camera_pose_next before it, no
	// capture after it and no blend. Clearing camera_pose_valid is what makes the first tick after the
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

	// --- gf_vm block (animation-VM family summons, gf_vm.cpp) -----------------------------------
	// Ifrit, Bahamut, Cerberus, Alexander, Brothers, Eden, Leviathan: one byte-code VM engine compiled
	// once per module. gf_vm.cpp holds it with the battle freeze bit on skipped frames, interpolates
	// the VM objects' output transforms at the tick's draw call site and carries the camera the VM
	// writes. gf_vm_hook_init() / gf_vm_reset() are called right after gf_pacing_hook_init() /
	// gf_pacing_reset() (animations.cpp). The two accessors feed gf_pacing.cpp's
	// gf_cinematic_owns_camera() / gf_cinematic_live(), so battle_camera_hook and apply_results_hook
	// see a VM-family cinematic exactly like a GF_TABLE one. Both are false with DBG_GF_VM_OFF or
	// DBG_GF_PACE_OFF set.
	void gf_vm_hook_init();
	void gf_vm_reset();
	// True while a VM-family cinematic runs (one frame of grace, like gf_cinematic_live()).
	bool gf_vm_live();
	// True while it runs AND the VM, not the engine's camera sequence (0x1D97718 / 0x1D9771E), is what
	// writes 0xB8B7F0 - battle_camera_hook must stand down then.
	bool gf_vm_owns_camera();
	// True when a gf_vm row paces this effect id (Elvoret's death, 18): the spell path must not also
	// skip and replay that tree (holdable_action).
	bool gf_vm_paces_effect(uint16_t effect_id);
	// --- end gf_vm block ------------------------------------------------------------------------

	// --- gf_siren block (spell-path GF family, gf_siren.cpp) -----------------------------------
	// Siren (95), Tonberry (90), Moogle Dance (96), ChocoFire / ChocoFlare / ChocoMeteor / ChocoBocle
	// (97..100): one effect-framework code generator, one table row each; plus Moomba (338), a plain
	// timeline tree. Every Init builds an ordinary tree (Siren: root queue 0x257FA80, root task
	// 0x739F40) that 0x50B2A0 (Moomba: 0x50A9A0) puts into the effect root, so it is paced by
	// effect_tick_hook's spell path (whole tree on logic ticks only, recorded display list replayed +
	// fx_blended in between).
	// gf_siren_hook_init() / gf_siren_reset() are called right after gf_vm_hook_init() / gf_vm_reset()
	// (animations.cpp). The three accessors are false with DBG_GF_SIREN_OFF (since v2 the whole family,
	// not only Siren) or DBG_GF_PACE_OFF set.
	void gf_siren_hook_init();
	void gf_siren_reset();
	// Asked by holdable_action() for a GF command (0x26/0xF4/0xFE/0xEC/0xF5): true when the action's
	// effect id is a row of the table, the effect root is that row's queue and the 2 s progress
	// watchdog has not released the hold. Call once per host frame (effect_tick_hook does): on held
	// frames it also re-publishes the screen shake 0x1D97712 the row's last tick wrote.
	bool gf_siren_holdable(const uint8_t* action);
	// True while a row's tree is at the effect root (one frame of grace). Feeds gf_cinematic_live().
	// No row owns the camera: their pose writes are absolute and land before battle_camera_hook,
	// whose v69 foreign-write rule captures and blends them.
	bool gf_siren_live();
	// True only inside a row's root task on a paced logic tick: read_animation_hook must leave the exact
	// decoded pose (no tick-frame blend), because the replay blends the recorded lists instead.
	bool gf_siren_exact_pose();
	// --- end gf_siren block ---------------------------------------------------------------------

	// --- fx_blend block (magic primitive blending, fx_blend.cpp) -------------------------------
	// animations.cpp owns the recording of the display-list link calls and their replay; fx_blend.cpp
	// owns the pairing of two recorded lists, the interpolation and the scratch packets the
	// interpolated copies live in. Nothing else should use these.

	constexpr int FX_LINKER_COUNT = 5;     // display-list linkers hooked in animations.cpp
	constexpr int FX_PRIMITIVE_WORDS = 24; // dwords a recorded packet copy can hold (GT4 needs 12)

	// One recorded link call. It lives here rather than in animations.cpp's anonymous namespace so
	// fx_blend.cpp can read both recorded lists without copying them. `copy` is the packet as it
	// looked when the call was recorded: these effects rebuild their packets from a flipping arena
	// every frame, so the copy - never the live packet - is what may be read on a later frame.
	// The packet's word 0 is the PSX tag (high byte = length in words, low 24 bits = the next
	// pointer the linker patches), word 1 is the GPU command byte plus the first colour.
	struct FxCall
	{
		int linker;                   // index into FX_LINKER_ADDRESSES
		uint32_t args[8];             // arguments, forwarded as eight dwords (all of them are cdecl)
		uint32_t* primitive;          // == (uint32_t*)args[1] for the linkers that take a primitive
		int words;                    // dwords in `copy`, 0 when no copy could be taken
		uint32_t copy[FX_PRIMITIVE_WORDS];
		uint32_t call_site;           // the game call site that made this link call (v66 diagnostic)
		// v78: the primitive lives in per-frame engine memory (the 0x5082B0 stack or the global packet
		// area 0x1D8E054). Such a call is only ever replayed from a stable copy of `copy`, never in place,
		// and (v79) never index-paired: model redraws there are back-face compacted. v81 pairs them by
		// face identity (call site + type + texture words, fx_blend.cpp build_identities).
		bool is_volatile;
		// v80: 0x45C7A0 (fn 0) takes no depth arguments - it copies the GTE SZ FIFO globals
		// 0x1CA8A50..0x1CA8A5C into node[1..4] at link time, and the render pass reads them back as the
		// per-vertex depth (0x461229 / 0x461469 / 0x462537 / 0x4628D7: `mov ecx,[0x1CA8A08]` = current
		// node, `mov eax,[ecx+4]`, fild). They are captured here and put back around every replayed fn 0.
		uint32_t sz_fifo[4];
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
	constexpr long DBG_FX_RECTANGLE_OFF = 4096;

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
	bool fx_blend_prepare(int slot, const FxCall* previous, size_t previous_count, const FxCall* current, size_t current_count, float alpha);
	// v109: the list of the tick before `previous` (trajectory test for fast pieces); nullptr = none.
	void fx_blend_set_two_ticks_ago(const FxCall* list, size_t count);

	// The primitive to hand the linker for call `call_index` of `slot`, or 0 when that call must be issued
	// against the address that was recorded (non-blendable linker, unknown command byte, ...).
	uint32_t fx_blend_arg(int slot, size_t call_index);
	// --- end fx_blend block --------------------------------------------------------------------
}
