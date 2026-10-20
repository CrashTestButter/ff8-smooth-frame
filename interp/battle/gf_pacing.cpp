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

// GF cinematic pacing. Shares the battle module's tick state through internal.h; gated by
// DBG_GF_PACE_OFF (bit 128: every row at once - there is no per-row switch).
//
// v6 (v76) rows: Quezacotl 116, Shiva 185, Diablos 325, Phoenix 140, Doomtrain 191, Cactuar 199,
// Carbuncle 278, Pandemona 291, Odin 187, Gilgamesh 327..330 (one shared root), Zantetsuken Reverse
// 326, Gilgamesh arrival 218. v6 mechanisms: SITE_MODEL_STEP_HELD (a model advance outside the hold),
// a model-block LIST per row (Gilgamesh steps up to ten blocks), HeldRegion save/restore around a
// held INNER call (inline writes no call site can reach: Carbuncle, Pandemona).
//
// The problem: a summon cinematic is a task tree hanging off the effect root 0x1D96AAC, ticked once
// per HOST frame by the battle frame driver, so its timeline runs ~10x too fast at 144 Hz - which is
// both "the summon flashes past" and the 0-damage race against the Boost gauge.
//
// Why the tick is never skipped: an earlier attempt did that and HUNG the game. The tick rebuilds
// the whole frame (fresh primitives out of a flipping packet arena) AND owns the completion
// handshake - the cinematic is "done" only when its task returns 2, the queue empties and 0x1D96AAC
// is cleared, which is what 0x50AE80 polls in state 4 of the summon task 0x50B2A0. So the runners
// run on EVERY host frame and only their state is held on the frames between logic ticks.
//
// v72: one table row per GF (GF_TABLE below). What varies between GFs:
//   * the ROOT task - the node the GF's Init puts into the effect root. It is hooked with a generic
//     trampoline; the row is found again from the node's own callback pointer (node + 8).
//   * the HOLD primitive. Each GF module is written against one of two engine conventions:
//       HOLD_PAUSE_DWORD - Quezacotl, Diablos (v5): a module-private pause dword (0x25216DC,
//         0x2505180) that every runner of
//         the module reads and that the executable never writes; its readers skip their integrators
//         and counter steps, never their draw. The inner timeline's own counter step is NOT guarded,
//         so the row also names that runner (INNER hook) and its counter is saved and restored.
//       HOLD_FREEZE_BIT - Shiva: the module reads the battle freeze bit 0x1D96A9C & 1 instead (the
//         same bit the battle effect runners honour, see impact_fx_hook). Its timeline 0x5C0F30
//         returns 0 at the very top when the bit is set (no event, no counter step), and every
//         drawing task of the module draws first and tests the bit only before advancing. The bit
//         is driven around the one root call only - frame-wide it would stop 0x504290 as well.
//   * the one-shot call sites that still fire while held (an `== counter` event in an unguarded
//     block): replaced call sites, never the functions themselves, so nothing outside the
//     cinematic changes. One generic stub serves every site; it finds its own site from the return
//     address and the site table says what to do (forward, suppress, or learn the model block).
//   * the creature's per-tick model advance site (0x6FBDB0 for Quezacotl, 0x5C4EA0 for Shiva). The
//     creature animates through the standard battle model path, whose leaf 0x508F90 animations.cpp
//     already interpolates; the advance sits behind the hold, so on a skipped frame the hook is asked
//     once explicitly (read_animation_hook decodes nothing when is_logic_tick_now() is false).
//   * whether the cinematic writes the battle camera pose itself (Quezacotl: directly, 0xB8B7F0 /
//     0xB8B7F8 and the roll 0x1D8E038; Shiva: through the SHARED GF camera VM task 0x63E9C0 its
//     timeline links into its own queue - v72 missed it by grepping only the module). Only then does
//     battle_camera_hook stand down (gf_cinematic_owns_camera) and this file carry the camera.
//   * the particle runners with continuous int16 state that can be paired tick to tick.
//
// Because the timelines step their counter at the BOTTOM, a new counter value's body runs on the
// skipped frames first and on a logic tick last, so suppressing on skipped frames makes every
// one-shot fire exactly once, on a logic tick.

#include "internal.h"

#include "../host.h"

#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

namespace ff8_battle_anim
{
	namespace
	{
		// --- shared engine addresses ------------------------------------------------------------

		constexpr uint32_t TASK_NEXT = 0x04;
		constexpr uint32_t TASK_RUNNER = 0x08;
		constexpr uint32_t TIMELINE_COUNTER = 0x0C;       // int16 frame counter of every task node

		constexpr uint32_t FN_SPAWN_TASK    = 0x508360;   // BdLinkTask
		constexpr uint32_t FN_PLAY_SE       = 0x501330;   // BdPlaySE -> 0x46B2A0
		constexpr uint32_t FN_PLAY_SE_CUT   = 0x5018C0;   // camera-cut SE -> 0x46B2A0
		constexpr uint32_t FN_STOP_SE       = 0x4A2940;   // stops an SE handle
		constexpr uint32_t FN_APPLY_RESULTS = 0x506BA0;   // Battle_ApplyActionResultToAllTargets (v58 hook lives on it)
		constexpr uint32_t FN_STREAM_REQ    = 0x5341D0;   // appends an asset stream request
		// 0x46B2A0 is never called directly from a GF module; a global hook would hold every sound.

		constexpr uint32_t FN_READ_ANIMATION = 0x508F90;  // hooked by animations.cpp - call the address, never a trampoline
		constexpr uint32_t MODEL_ANIM_CTX = 0x60;         // AnimCtx inside a model block
		constexpr uint32_t MODEL_ANIM_CMD = 0x6C;         // AnimCmd inside a model block
		constexpr uint32_t CMD_CURRENT_FRAME = 6;         // same layout constants animations.cpp uses
		constexpr uint32_t CMD_TOTAL_FRAMES = 7;

		int16_t* const CAMERA_POSE = (int16_t*)0xB8B7F0;     // 8 int16: position xyz + pad, look-at xyz + pad
		int16_t* const CAMERA_ROLL = (int16_t*)0x1D8E038;
		constexpr int CAMERA_POSE_WORDS = 8;
		// v10: the real camera roll (render 0x5033E0 builds Rz from it; 0x1D8E038 is the zoom). The shared
		// GF camera VM 0x63E9C0 writes it with the pose.
		int16_t* const CAMERA_Z_ROLL = (int16_t*)0x1D977A2;
		// v10: the shared GF camera VM's op 1 (0x63EA8D) SETS a channel (position or look-at) - a cut. Its
		// read of the new vector is `call 0x63F210` at 0x63EA92; ops 2/3 move over N frames instead. Ops
		// only run on ticks (under the freeze bit the VM recomputes the held pose at 0x63F0AB).
		constexpr uint32_t CALL_SITE_CAMERA_VM_SET = 0x63EA92;
		constexpr uint32_t FN_CAMERA_VM_READ_VECTOR = 0x63F210;
		constexpr int CAMERA_SET_MIN_UNITS = 32;           // a set that moves the pose less than this is not a visible cut
		constexpr uint32_t MODEL_TRANSLATION = 0x54;    // model block matrix translation, 3 x int32
		constexpr int MODEL_TRANSLATION_CUT = 1024;     // a model moving further per tick is repositioned for a new shot
		constexpr int CAMERA_CUT_UNITS = 2048;               // same cut threshold battle_camera_hook uses

		// --- call-site table ----------------------------------------------------------------------

		enum SiteKind : uint8_t
		{
			SITE_SUPPRESS,      // void (or ignored) result: skipped while held
			SITE_SPAWN_NULL,    // BdLinkTask whose site null-checks the node: returns nullptr while held
			SITE_SPAWN_SCRATCH, // BdLinkTask whose site does NOT null-check: returns a private, never linked node
			SITE_MODEL_STEP,    // the creature's per-tick advance: always forwarded, arg 0 is the model block
			SITE_MODEL_STEP_HELD, // v6: a model advance that is NOT behind the hold (Pandemona): arg 0 is
			                    // learned on every call, the (void) advance is forwarded only on a logic tick
		};

		struct SiteDesc
		{
			uint32_t site;      // address of the E8 call
			uint32_t target;    // what it calls (verified at install time)
			uint8_t kind;
		};

		// --- particle runners -------------------------------------------------------------------

		constexpr int PARTICLE_FIELDS = 6;
		struct RunnerDesc
		{
			uint32_t runner_fn;        // runner address as stored at node + 8
			uint8_t sequence_offset;   // int16 that must step by sequence_step for two captures to pair (0 = skip)
			int16_t sequence_step;
			uint8_t field_count;       // interpolated int16 fields
			uint16_t field_offsets[PARTICLE_FIELDS]; // 16 bit since gf pacing v5: Diablos' creature matrix sits at node + 0x164
			uint8_t angle_mask;        // bit k: field k is a 12 bit angle, blend on the shortest arc
			int16_t cut_distance;      // a linear field moving further than this in one tick is a teleport
		};

		// --- the per-GF table -------------------------------------------------------------------

		enum HoldKind : uint8_t { HOLD_PAUSE_DWORD, HOLD_FREEZE_BIT };

		// v6: memory an unguarded INNER timeline mutates with inline code (no call site to replace) on a
		// held call. Saved before the held inner call and restored after it, so only the logic tick's
		// writes survive. Only the timeline (and its callees) runs between the save and the restore, so
		// nothing another task wrote is ever rolled back; no region may hold a linked primitive or a task
		// node header (what made the v6-v14 blind memory window hang).
		enum HeldRegionFlags : uint8_t
		{
			REGION_NODE_RELATIVE = 1, // address is an offset into the inner timeline node, not an absolute address
			REGION_ZEROED = 2,        // zeroed for the held call (an INPUT of an inline RMW); always restored, even on return 2
		};
		struct HeldRegion
		{
			uint32_t address;
			uint16_t element_size; // bytes per element
			uint8_t count;         // elements (>= 1)
			uint16_t stride;       // bytes between elements
			uint8_t flags;
		};
		constexpr size_t HELD_BUFFER_BYTES = 4096;

		struct GfDesc
		{
			const char* name;
			uint16_t effect_id;               // MagicList_Logic index + 1 (documentation / trace)
			uint32_t root_fn;                 // the task the GF's Init registers in the effect root
			const uint8_t* root_prologue;
			uint8_t root_prologue_len;
			uint32_t inner_fn;                // HOLD_PAUSE_DWORD: the timeline whose counter step ignores the pause (0 = none)
			const uint8_t* inner_prologue;
			uint8_t inner_prologue_len;
			uint8_t hold_kind;
			uint32_t pause_dword;             // HOLD_PAUSE_DWORD only
			uint32_t timeline_queue;          // queue holding the timeline node (watchdog + trace)
			uint32_t timeline_fn;             // the timeline runner (its node's +0x0C is the frame counter)
			int16_t timeline_last;            // last frame, for the trace only
			bool owns_camera;                 // writes 0xB8B7F0 / 0x1D8E038 itself
			const SiteDesc* sites;
			uint8_t site_count;
			const uint32_t* particle_queues;
			uint8_t particle_queue_count;
			const RunnerDesc* particle_runners;
			uint8_t particle_runner_count;
			// owns_camera rows only: the cinematic RECOMPUTES its pans from the frame counter on every
			// call (Diablos, 0x657DF0) instead of integrating them behind the hold, so a held frame -
			// which already runs the NEXT counter's body - rewrites the pose one tick ahead. Such a
			// change is not a cut: keep blending the two tick captures and only adopt a real cut
			// (> CAMERA_CUT_UNITS). Trailing member: rows that leave it out get false (Quezacotl, Shiva).
			bool camera_hold_pans;
			// v6, trailing: inline-written memory restored around a held INNER call (rows with an inner
			// hook only; see HeldRegion). Rows that leave it out get none.
			const HeldRegion* held_regions;
			uint8_t held_region_count;
			// v10, trailing: blend the world translation of the cinematic's model blocks (MODEL_TRANSLATION)
			// on held frames. The director steps it behind the hold (Odin + Sleipnir charge +0x5A per tick).
			bool blend_model_translation;
		};

		// ---- Quezacotl, effect 116 (Init 0x6C3550 -> 0x6C3640) ----------------------------------
		// Root 0x6C3760 (GF_116Quezacotl_SequenceTask) flips the packet arena, runs the child queues
		// (0x2521738 timeline, 0x2521728/18/08/F8/E8 particles) and steps its own counter only while
		// 0x25216DC == 0. Timeline 0x6C3940: one 0x8D0-byte node at *0x025217AC.
		const uint8_t QZ_ROOT_PROLOGUE[] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };        // push esi / mov esi,[esp+8]
		const uint8_t QZ_INNER_PROLOGUE[] = { 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00 }; // sub esp,80h
		const SiteDesc QZ_SITES[] = {
			// BdLinkTask: the timeline's own stream + the four spawner helpers. 0x6C7D3C (0x6C7D10) and
			// 0x6C87BB (0x6C87B0) sit in blocks that are NOT pause-guarded and spawned one particle per
			// host frame up to v64. All five sites null-check the node.
			{ 0x6C473C, FN_SPAWN_TASK, SITE_SPAWN_NULL }, { 0x6C7733, FN_SPAWN_TASK, SITE_SPAWN_NULL },
			{ 0x6C7D3C, FN_SPAWN_TASK, SITE_SPAWN_NULL }, { 0x6C7FB1, FN_SPAWN_TASK, SITE_SPAWN_NULL },
			{ 0x6C87BB, FN_SPAWN_TASK, SITE_SPAWN_NULL },
			// Sounds (counters 0, 0x41, 0xD1, 0x115), the camera-cut sound, the SE stop at 0x15F.
			{ 0x6C4E86, FN_PLAY_SE, SITE_SUPPRESS }, { 0x6C4EE7, FN_PLAY_SE, SITE_SUPPRESS },
			{ 0x6C4F66, FN_PLAY_SE, SITE_SUPPRESS }, { 0x6C50C8, FN_PLAY_SE, SITE_SUPPRESS },
			{ 0x6C5162, FN_PLAY_SE_CUT, SITE_SUPPRESS }, { 0x6C4D8E, FN_STOP_SE, SITE_SUPPRESS },
			// The damage, counter 0x159.
			{ 0x6C5184, FN_APPLY_RESULTS, SITE_SUPPRESS },
			// The 23 asset stream requests (12 groups gated by `if (0x534270() != 0) return 0;`).
			{ 0x6C4A63, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6C4A7A, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6C4A93, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6C4AC1, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6C4AD8, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6C4AF1, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6C4B1F, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6C4B36, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6C4B5C, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6C4B89, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6C4B9A, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6C4BC7, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6C4BD8, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6C4BEF, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6C4C08, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6C4C21, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6C4C64, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6C4C7D, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6C4CA9, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6C4CC2, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6C4D00, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6C4D17, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6C4D66, FN_STREAM_REQ, SITE_SUPPRESS },
			// The creature's advance 0x6FBDB0(node + 0x28), inside `if (0x25216DC == 0)`.
			{ 0x6C3A90, 0x6FBDB0, SITE_MODEL_STEP }, { 0x6C3B69, 0x6FBDB0, SITE_MODEL_STEP },
		};
		const uint32_t QZ_PART_QUEUES[] = { 0x25216E8, 0x25216F8, 0x2521708, 0x2521718, 0x2521728 };
		const RunnerDesc QZ_PART_RUNNERS[] = {
			// 0x6C6660, queue 0x25216E8 (0x2C nodes): bouncing spark. Draws, then (pause-guarded)
			// integrates position += velocity and spins. +0x0C is its life counter, +1 per tick.
			{ 0x6C6660, 0x0C, 1, 6, { 0x10, 0x12, 0x14, 0x20, 0x22, 0x24 }, 0x38, 2048 },
			// 0x6C8850, queue 0x2521728 (0x80 nodes): the beam. +0x0E is its phase along the path
			// (+= +0x10 per tick, clamped at 0x1000), the only continuous input of its draw.
			{ 0x6C8850, 0, 0, 1, { 0x0E }, 0x00, 4096 },
			// Stepped on purpose: 0x6C7800 (lightning chain), 0x6C7EA0 (RNG trails), 0x6C8430 (a
			// static flipbook - 0x571BC0 only reads its vector), the effect player 0x701970.
		};

		// ---- Shiva, effect 185 (Init 0x5C0D50) --------------------------------------------------
		// Init: root queue 0x22BC180 <- 0x5C7F50; sub-queue 0x22BC198 (0x64 x 0x24-byte nodes) <-
		// timeline 0x5C0F30. Root 0x5C7F50 picks the packet arena from its counter's parity, runs the
		// sub-queue and steps its counter unconditionally (the counter is only the arena parity, so it
		// keeps flipping on held frames - exactly what the double buffer wants). Timeline 0x5C0F30:
		// `if (0x1D96A9C & 0x201) { if (& 1) return 0; ... }` at the top, then every event keyed on
		// its +0x0C, damage at 0x109 (0x5C133F -> 0x506BA0), done after 0x113. Every task it spawns
		// into 0x22BC198 draws first and tests the freeze bit before advancing (0x5C4AD0, the
		// creature task, is the model: static block 0x22BD018, advance 0x5C4EA0 behind the bit).
		const uint8_t SHIVA_ROOT_PROLOGUE[] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };     // push esi / mov esi,[esp+8]
		const SiteDesc SHIVA_SITES[] = {
			// BdLinkTask sites of the module's tasks (the two in Init 0x5C0D50 are not reachable from
			// a held call). NONE of them null-checks the node, so a suppressed spawn gets a private
			// scratch node. Most sit behind the freeze test anyway; 0x5C13DD (task 0x5C1390, its
			// `counter == 0` spawn of 0x5C16B0) does not and would spawn once per held frame.
			{ 0x5C137A, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH }, { 0x5C13DD, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH },
			{ 0x5C1E8A, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH }, { 0x5C2E07, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH },
			{ 0x5C2E4F, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH }, { 0x5C4837, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH },
			{ 0x5C5BBA, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH }, { 0x5C66F7, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH },
			{ 0x5C676D, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH }, { 0x5C67CD, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH },
			{ 0x5C6FAA, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH }, { 0x5C718A, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH },
			{ 0x5C757B, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH },
			// The creature's advance 0x5C4EA0(0x22BD018), inside `if ((0x1D96A9C & 0x201) == 0)`.
			{ 0x5C4DC8, 0x5C4EA0, SITE_MODEL_STEP },
			// v73: the model texture animation 0x5C4F80 -> Battle_QueueVramMoveImage. The creature task
			// calls 0x5C4F80 on EVERY call in two unguarded branches (its counters < 5 and 0x35..0x3A),
			// so with the counter held it queued ~10 identical moves per tick into the VRAM op queue
			// 0x1D98220 - whose appenders keep incrementing the count past the 0x20 slots and whose
			// flush 0x505D20 loops over the whole count with no bound, executing whatever lies past
			// 0x1D98420 as VRAM ops, and which silently drops every op queued once it is full (the
			// 0xA3 effect-texture restore upload of 0x508660 among them). Held: the move is identical
			// to the one the tick queued, so skipping it restores vanilla traffic and loses nothing.
			{ 0x5C5047, 0x505EB0, SITE_SUPPRESS },
		};
		const uint32_t SHIVA_PART_QUEUES[] = { 0x22BC198 };
		const RunnerDesc SHIVA_PART_RUNNERS[] = {
			// 0x5C4AD0, the creature task: +0x20 is the creature's vertical offset (0x22BD070 <- +0x20
			// every call, `+0x20 += +0x22` behind the freeze test) while its counter +0x0C < 5.
			{ 0x5C4AD0, 0x0C, 1, 1, { 0x20 }, 0x00, 2048 },
			// 0x5C4900, the ice shards: draws with Y angle +0x18 and scale +0x1C, then (behind the
			// freeze test) +0x18 += +0x1A, +0x1C += +0x1E, both decaying; +0x0C steps +1 per tick
			// (while its start delay +0x0E is running +0x0C does not move, so it cannot pair).
			{ 0x5C4900, 0x0C, 1, 2, { 0x18, 0x1C }, 0x01, 4096 },
			// Stepped on purpose: every other task of the module (their per-tick state is a frame
			// index or a chain; audit them with the 1/s trace before adding rows).
		};

		// ---- Diablos, effect 325 (MagicList_Logic[324] = thunk 0x6541E0 -> Init 0x654210) --------
		// Module [0x6541E0, 0x659A80). The Quezacotl shape: root queue 0x25051B0 <- root 0x654350, which
		// flips the packet arena on +0x0E every call, spawns the timeline at its counter 2 (0x994-byte
		// node *0x2505208 into queue 0x25051A0, behind the one-shot +0x0F), runs 0x25051A0 and the debris
		// queue 0x2505190, and steps its counter only while the pause dword 0x2505180 == 0. 0x2505180 is
		// zeroed by Init (0x654236) and never written anywhere else (19 references, all in the module).
		// Timeline 0x6545F0: events on +0x0C, damage at 0x1F3, last frame 0x211 (then it waits for
		// 0x534300(2)); its step at the bottom is NOT pause-guarded -> INNER hook, like 0x6C3940. Unlike
		// Quezacotl, everything after its last dword read at 0x6557CB (camera pans, the particle array at +0x4C5C,
		// the chains at +0x7C5C) is not pause-guarded either: it spawns and integrates on every call.
		const uint8_t DB_ROOT_PROLOGUE[] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };        // push esi / mov esi,[esp+8]
		const uint8_t DB_INNER_PROLOGUE[] = { 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00 }; // sub esp,94h
		const SiteDesc DB_SITES[] = {
			// BdLinkTask 0x659860 x2 per call at counters 0x1D2..0x1D9 (unguarded, null-checked: je 0x656156).
			// The other spawns (0x6543E1 in the root behind +0x0F, 0x65990A via 0x659900, only called
			// behind the pause) need nothing.
			{ 0x6560ED, FN_SPAWN_TASK, SITE_SPAWN_NULL },
			// Sounds: BdPlaySE (counters 0, 0x5B, 0xEE, 0x14D, 0x192, 0x1C8), the camera-cut SE (0x20,
			// 0x5B, 0xAD, 0xDA, 0x1B4, 0x1D5), the stop of the handle at +0x10 (0x20E).
			{ 0x6557B3, FN_PLAY_SE, SITE_SUPPRESS },
			{ 0x655468, FN_PLAY_SE_CUT, SITE_SUPPRESS }, { 0x6554C0, FN_PLAY_SE_CUT, SITE_SUPPRESS },
			{ 0x65552F, FN_PLAY_SE_CUT, SITE_SUPPRESS }, { 0x6555BA, FN_PLAY_SE_CUT, SITE_SUPPRESS },
			{ 0x65566E, FN_PLAY_SE_CUT, SITE_SUPPRESS }, { 0x6556DA, FN_PLAY_SE_CUT, SITE_SUPPRESS },
			{ 0x656AF2, FN_STOP_SE, SITE_SUPPRESS },
			// The damage, counter 0x1F3.
			{ 0x6561F7, FN_APPLY_RESULTS, SITE_SUPPRESS },
			// Asset stream requests (counters 0, 0x5B, 0xAD, 0xCB, 0x147, 0x167, 0x1B4, 500).
			{ 0x6553D9, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6553F2, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x65540B, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x655422, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x65543B, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6554D7, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x655548, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x655579, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x655601, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x655645, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x655687, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6556FE, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x655717, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x65572D, FN_STREAM_REQ, SITE_SUPPRESS },
			// 0x501860 BdTransSummonStream (counters 0x57, 0xA9/0xD6, 0x1D1): posts the summon stream
			// request 0x1D96DB0 and sets the node's +0x0F ready flag, which 0x5B/0xAD/0xDA/0x1D5 wait
			// on. Held: +0x0F stays 0 until the tick posts it, exactly once.
			{ 0x65549B, 0x501860, SITE_SUPPRESS }, { 0x65550A, 0x501860, SITE_SUPPRESS },
			{ 0x6556B8, 0x501860, SITE_SUPPRESS },
			// Battle_QueueTIMUpload (counter 0x167) - a VRAM op append, the Shiva lesson: nothing that
			// appends to the 0x20-slot queue 0x1D98220 may repeat on held frames. The only VRAM-op
			// append reachable from the module without going through a suppressed site (closure of
			// 0x654350 / 0x6545F0 / 0x659860 / 0x659960, depth 6).
			{ 0x65562C, 0x505E30, SITE_SUPPRESS },
			// The unguarded tail. Spawners (null-checked or result ignored): 0x658B20 (alloc from the
			// particle array, counters 0xB1..0xB2, `if (p == 0) break`), 0x658B90 (4 particles per call:
			// 9..0x1D, the 0x13x pre-warm at 0x1E, 0x1E..0x3B), 0x658890 (a chain, counters 0xF6..0x129,
			// 0x12A..0x13D, 0x178..0x195, 0x196..0x1A5; the result is never read).
			{ 0x6558A4, 0x658B20, SITE_SPAWN_NULL },
			{ 0x65626D, 0x658B90, SITE_SUPPRESS }, { 0x6562D3, 0x658B90, SITE_SUPPRESS },
			{ 0x6562EC, 0x658B90, SITE_SUPPRESS },
			{ 0x655B3D, 0x658890, SITE_SUPPRESS }, { 0x655CED, 0x658890, SITE_SUPPRESS },
			{ 0x655F01, 0x658890, SITE_SUPPRESS }, { 0x656084, 0x658890, SITE_SUPPRESS },
			// ... the integrators of that particle array (position += velocity, life--, gravity) ...
			{ 0x65599D, 0x658CE0, SITE_SUPPRESS }, { 0x6561A4, 0x658CE0, SITE_SUPPRESS },
			{ 0x6562DB, 0x658C80, SITE_SUPPRESS }, { 0x6562F4, 0x658C80, SITE_SUPPRESS },
			{ 0x656340, 0x658C80, SITE_SUPPRESS }, { 0x656275, 0x658D30, SITE_SUPPRESS },
			// ... and its `== counter` resets: 0x6545B0 (clear the array), 0x658D90 (shatter the creature
			// into it, 0x1D5), 0x657E70 (reset the chains, 0xEE / 0x178 / 0x196). Idempotent, but a held
			// frame runs them one tick early and would clear what the tick still has to draw.
			{ 0x656190, 0x6545B0, SITE_SUPPRESS }, { 0x65620C, 0x6545B0, SITE_SUPPRESS },
			{ 0x65628A, 0x6545B0, SITE_SUPPRESS }, { 0x6563D1, 0x6545B0, SITE_SUPPRESS },
			{ 0x65659F, 0x6545B0, SITE_SUPPRESS }, { 0x65619C, 0x658D90, SITE_SUPPRESS },
			{ 0x6559BD, 0x657E70, SITE_SUPPRESS }, { 0x655D41, 0x657E70, SITE_SUPPRESS },
			{ 0x655F48, 0x657E70, SITE_SUPPRESS },
			// The creature's advance 0x6FBDB0(node + 0x10C), inside `if (0x2505180 == 0)`. The second
			// model at node + 0x1A8 (counters 0x12A..0x16F, advance site 0x655355) is left stepped: the
			// table carries one model block per row.
			{ 0x655232, 0x6FBDB0, SITE_MODEL_STEP },
		};
		const uint32_t DB_PART_QUEUES[] = { 0x25051A0, 0x2505190 };
		const RunnerDesc DB_PART_RUNNERS[] = {
			// 0x6545F0, the timeline node itself: +0x14C is the creature's MATRIX (model block +0x40), so
			// +0x164 is its Y translation (int32; +0x20 per tick while it rises at 0x5C..0xA6, -10 at
			// 0x10C..0x129, both behind the pause). Blended as its low int16: the value lives in
			// [-0x2000, -0x16A0] (high half 0xFFFF throughout), and the cut 0x400 refuses the 0 -> -0x2000
			// jump at 0x5C, so the high half is never wrong. Paired on the frame counter +0x0C.
			{ 0x6545F0, 0x0C, 1, 1, { 0x164 }, 0x00, 0x400 },
			// 0x659960, queue 0x2505190: rising debris (effect player, spawned by 0x659900 at 0x67 and
			// 0x5C..0xA6); +0x0E is its Y, `+= 0x18` per tick behind the pause.
			{ 0x659960, 0, 0, 1, { 0x0E }, 0x00, 2048 },
			// Stepped: the particle array at timeline +0x4C5C and the chains at +0x7C5C (not task nodes;
			// integrated by the timeline itself, suppressed while held), 0x659860 (static position), the
			// effect players 0x701970 (integer frame, paused by 0x2505180).
		};

		// ---- Phoenix, effect 140 (MagicList_Logic[139] = thunk 0x6A6300 -> Init 0x6A6430) -----------
		// Module [0x6A6300, 0x6A8830). Reached by the Phoenix Pinion item (cmd 0xF4, cmd arg 1 -> summon
		// task 0x50B2A0) and by the party-wipe auto summon (cmd 0xF5 -> 0x50B830); both store Init's
		// return, root queue 0x2517AE8 (0x14 x 1), into 0x1D96AAC. The Quezacotl / Diablos shape: root
		// 0x6A6530 flips the packet arena on +0x10 every call, spawns the timeline at its counter 2
		// (0x878-byte node into queue 0x2517AD8, behind the one-shot +0x0F and the pause dword; also
		// starts the vibration 0x4A29A0 -> node+0x10, binds the creature model 0x11B537C and requests a
		// stream), ticks 0x2517AD8 (timeline), 0x2517AC8 (sparks) and 0x2517AB8 (feathers), sets the
		// screen tint 0x5713E0(0x2517B20) and steps +0x0C only while the pause dword 0x2517AAC == 0.
		// Returns 2 once both queues are empty and the sound-state task's done byte 0x2517B24 is set.
		// 0x2517AAC: 14 references, all reads, all in the module - NOT EVEN Init writes it (BSS 0, like
		// Quezacotl's 0x25216DC). Passed as `paused` to every effect player 0x701970 call.
		// Timeline 0x6A6750 (prologue 83 EC 5C 53 55): frame +0x0C, 1..0x3C steps TWICE per tick (a
		// guarded +1 plus the bottom +1), 0x3D jumps to 0x52 (guarded), last frame 0xFE (returns 2 when
		// the step reaches 0xFF). Its bottom step is NOT guarded -> inner hook with counter restore.
		const uint8_t PH_ROOT_PROLOGUE[] = { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C };  // push ebx / push ebp / mov ebp,[esp+0Ch]
		const uint8_t PH_INNER_PROLOGUE[] = { 0x83, 0xEC, 0x5C, 0x53, 0x55 };       // sub esp,5Ch / push ebx / push ebp
		const SiteDesc PH_SITES[] = {
			// Sounds: BdPlaySE (one site for counters 0, 0x65, 0xC3), the camera-cut SE (0x65, 0xA1), the
			// vibration stop of the handle at node+0x10 (0xFB).
			{ 0x6A72B0, FN_PLAY_SE, SITE_SUPPRESS }, { 0x6A726B, FN_PLAY_SE_CUT, SITE_SUPPRESS },
			{ 0x6A75F5, FN_STOP_SE, SITE_SUPPRESS },
			// The result (the revive / damage), counter 0xF5.
			{ 0x6A75E1, FN_APPLY_RESULTS, SITE_SUPPRESS },
			// 0x508630 BattleFx_StartSoundStateTask(buf, &0x2517B24) at 0xEB: links a sound-state task (the
			// v73 0xA3-restore family) whose done byte gates the root's `return 2`. Once, on the tick.
			{ 0x6A75BC, 0x508630, SITE_SUPPRESS },
			// Asset stream requests: counter 0 (x5), 0x7E (x2), 0x96 (x2, after the Stream_IsBusy wait).
			{ 0x6A713A, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6A7151, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6A716A, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6A7183, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6A719A, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6A71C1, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6A71DA, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6A7209, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6A7221, FN_STREAM_REQ, SITE_SUPPRESS },
			// The creature's advance 0x6FBDB0(node + 0x64), inside `if (0x2517AAC == 0)`, counters 0x65..0xEC.
			{ 0x6A6BFB, 0x6FBDB0, SITE_MODEL_STEP },
			// Not needed: every BdLinkTask is pause-guarded (root 0x6A65B8 behind counter==2 && dword==0;
			// timeline 0x6A6DB9 `if (0x2517AAC == 0 && !(n & 3))`; spark spawner 0x6A7CE0 tests the dword
			// itself), the vibration start 0x6A65DA is in the root's guarded spawn block, rand() is only
			// called in those guarded spawns. The `== 0x65 / 0x97` pool inits (0x508300 at 0x6A6C9F /
			// 0x6A6D92) run before any node of that pool exists - idempotent. The `== counter` prim-layout
			// decodes 0x7016B0 are idempotent. No VRAM-op append is reachable from the held path (closure
			// of 0x6A6530/0x6A6750/0x6A7D90/0x6A7BF0/0x6A7650/0x6A7CE0 + engine callees depth 4).
			// Left running on held frames: the stream pump 0x534210 (every call; it only services the
			// stream queue faster) and the Stream_IsBusy early returns at 0x65/0x96/0xC2 (level waits).
		};
		const uint32_t PH_PART_QUEUES[] = { 0x2517AC8, 0x2517AB8 };
		const RunnerDesc PH_PART_RUNNERS[] = {
			// 0x6A7D90, queue 0x2517AC8 (0x1C nodes, spawned by 0x6A7CE0): rising sparks. Draws at the
			// SVECTOR +0x0C/+0x0E/+0x10 (0x571BC0 only builds the GTE transform from it), then (behind the
			// dword) +0x0C += +0x14 >> 4, +0x0E -= 0x4B, +0x10 += +0x16 >> 4, life +0x12 +1 (0 at spawn,
			// dies at 0x10; it is also the sprite frame, left stepped).
			{ 0x6A7D90, 0x12, 1, 3, { 0x0C, 0x0E, 0x10 }, 0x00, 2048 },
			// 0x6A7BF0, queue 0x2517AB8 (0x18 nodes, spawned at 0x97..0xC2 every 4th frame): falling
			// feathers. Draws with position +0x0C/+0x0E/+0x10 and Y angle +0x14 (0x701270), then (behind the
			// dword) +0x0E -= 0x19, +0x14 += +0x16 (|+0x16| <= 0x200), +0x12 +1 (scale, dies at 0x10).
			{ 0x6A7BF0, 0x12, 1, 4, { 0x0C, 0x0E, 0x10, 0x14 }, 0x08, 2048 },
			// Stepped: the creature's Y (model +0x58 = node +0xBC) - recomputed from the counter before the
			// draw at 0x65..0x78 and reset to 0 before the draw up to 0xEC, so no pair can survive the
			// held call; the effect players 0x701970 (integer frames, paused by 0x2517AAC).
		};

		// ---- Doomtrain, effect 191 (MagicList_Logic[190] = Init 0x63E730, no thunk) --------------------------
		// Module [0x63E730, 0x6472E0) (0x6472E0 = Odin's Init). NOTE: the shared GF camera VM (0x63E960 link /
		// 0x63E9C0 task, 0x63F1E0..0x63F240 helpers) lives INSIDE this module range, right after the Init.
		// Init: root queue 0x24FBF80 (0x508300(q, 0x24FBF60, 0x10, 2)) <- root 0x6472C0; sub-queue 0x24FC330
		// (0x64 x 0x24, pool 0x24FC340) <- timeline 0x63F2D0. Root 0x6472C0: ExecuteTaskQueue(0x24FC330),
		// `inc word [node+0x0C]` unconditionally (read by nobody - no packet arena here), 2 when empty.
		// Timeline 0x63F2D0: copies the view matrix, composes 0x24FBF90 = view x 0x24FBE88 (local frame, written
		// only by Init), then `if (0x1D96A9C & 0x201) { if (& 1) return 0; if (0x534270() < 0) return 0; }` -
		// frozen, it runs NOTHING. Events on +0x0C: camera VM link + loads + vibration at 0, SE 4/0xA0/0x126/
		// 0x150/0x1A1, character loads 5/0xF/0x1E/0x55/0xA8/0x125/0x14F (with 0x534270 load waits), cut SE
		// 0x7C/0x126/0x14F/0x16E, sub-sequence tasks (0x641AF0 at 0x14, the train sequence 0x641EA0 at 0x7C,
		// 0x641E60 texture restore at 0x196), damage 0x506BA0 at 0x1A4 (site 0x63F723), done after 0x1A7.
		// Every other task (34 linked task functions) tests the bit before advancing; see the notes.
		// No standard battle model: the train is drawn by 0x642CE0/0x642830 (own part transform 0x508C30),
		// so there is no MODEL_STEP site - its motion (0x641EA0 -> 0x642CA0 on object 0x24FC290, speed
		// 0x24FC2EC) steps at 15 fps.
		const uint8_t DT_ROOT_PROLOGUE[] = { 0x68, 0x30, 0xC3, 0x4F, 0x02 };      // push 0x24FC330 (the next insn is the E8 to 0x508420)

		// ---- Cactuar, effect 199 (MagicList_Logic[198] = Init 0x5A8750, no thunk) ----------------------------
		// Module [0x5A8750, 0x5AA3E0). Init copies the embedded model container (MAG199_Cactuar_ModelContainer
		// 0xE94918) into the magic texture buffer, root queue 0x22599F0 (pool 0x22599D0, 0x10 x 2) <- root
		// 0x5AA3A0; sub-queue 0x2259A08 (0x64 x 0x24, pool 0x2259A18) <- timeline 0x5A8940; two TIM uploads.
		// Root 0x5AA3A0: ExecuteTaskQueue(0x2259A08), `inc word [node+0x0C]` unconditionally, 2 when empty.
		// Timeline 0x5A8940: composes 0x2259978 = view x 0x2259950, then the whole body sits inside
		// `if ((0x1D96A9C & 0x201) == 0)`, else return 0. Events: camera VM link + stream 0x501860 + creature
		// spawn 0x5A8AE0 + vibration at 0, 0x5A8AF0(flash) at 10, SE + cut SE at 0xF, SE + stream + 0x5A8B10 at
		// 0x35, cut SE at 0x4C, 0x5A8B90 at 0x91, vibration stop at 0x94, done after 0x97 (returns 2 at 0x98).
		// Result: 0x506690 per target from task 0x5AA2A0 (site 0x5AA314), behind the bit - NOT 0x506BA0.
		// Creature task 0x5A8C20 (static model block 0x225A838): draws every call, animation starts
		// 0x5A8FB0 (`== counter`, behind the bit), advance 0x5A8FD0 = 0x508F90(+0x60, +0x6C) (+ 0x509440
		// restart) at site 0x5A8EF2 behind `test 0x201`; hop height 0x225A890 (= model +0x58, int32
		// Y translation) integrated from node +0x22 behind the bit.
		const uint8_t CA_ROOT_PROLOGUE[] = { 0x68, 0x08, 0x9A, 0x25, 0x02 };      // push 0x2259A08 (the next insn is the E8 to 0x508420)
		const SiteDesc CA_SITES[] = {
			// The creature's advance 0x5A8FD0(0x225A838), inside `if ((0x1D96A9C & 0x201) == 0)` of 0x5A8C20.
			// Nothing else in the module needs a site: no one-shot is reachable while the bit is set.
				{ 0x5A8EF2, 0x5A8FD0, SITE_MODEL_STEP },
		};

		// ---- Carbuncle, effect 278 (MagicList_Logic[277] = thunk 0x680C50 -> Init 0x680C80) -----
		// Module [0x680C50, 0x683F10). NOT dispatched through 0x50B2A0: cmd 0xFE with cmd arg 0x46 is
		// routed by 0x50A790 to BattleActionSequence_Tick_MagicCast 0x50A9A0 (anim state 0xB), whose
		// state 5 stores the Init's return in 0x1D96AAC (and arms the gauge, pre_computeGFBoost, for
		// 0xFE) - so the root still sits at the effect root and 0x50093A ticks it. Quezacotl shape:
		// root queue 0x2508198 <- root 0x680DF0, which flips the arena on +0x10 every call, at its
		// counter 2 (+0x0F one-shot, behind the pause) builds 7 pools and links the timeline 0x681630
		// into 0x2508188, ticks 0x2508188/78/68/58/38/48, draws 0x6811E0, flash 0x5713E0, and steps
		// +0x0C only while 0x2508114 == 0; done (returns 2) when the timeline is gone and the sound
		// state byte 0x25081D8 is set, after moving the enemies back by 0x25081D0.
		// Pause dword 0x2508114: 14 references in the whole exe, all reads, all in the module; no
		// writer at all (not even Init) -> always 0 in vanilla.
		// Timeline 0x681630: frame +0x0C, last frame 0x11A (returns 2 on reaching 0x11B). Reads the
		// dword only for the model advance and as the effect player's `paused` argument; its events,
		// spawns, camera and its own step are NOT guarded -> INNER hook with counter restore.
		const uint8_t CB_ROOT_PROLOGUE[] = { 0x51, 0x53, 0x56, 0x8B, 0x74, 0x24, 0x10 }; // push ecx/ebx/esi, mov esi,[esp+10h]
		const uint8_t CB_INNER_PROLOGUE[] = { 0x83, 0xEC, 0x50, 0x53, 0x55 };           // sub esp,50h / push ebx / push ebp
		const SiteDesc CB_SITES[] = {
			// BdLinkTask, all null-checked (timeline: cmp esi,ebx with ebx = 0 from 0x681681; 0x6822F0:
			// cmp ebx,esi / test esi,esi with esi = 0 from 0x68230A).
			//   0x681B4E: 0x683E80 into 0x2508178, EVERY call at counters 0x56..0x6D and 0xDB..0xE6;
			//   0x681BB4: 0x6822F0 (per-target task) into 0x2508158 at 0x91 / 0x95 / 0x99;
			//   0x681C3B: 0x683DC0 into 0x2508168 at 0xE7..0xEA;
			//   0x682375: 0x683E80 from task 0x6822F0, every call while its counter < 0x15 (unguarded);
			//   0x682440: 0x683760 (x8) from 0x6822F0 at its counter 0x0F (unguarded).
			// (0x6826C1 in 0x6822F0 and 0x680FAC in the root are behind the dword.)
			{ 0x681B4E, FN_SPAWN_TASK, SITE_SPAWN_NULL }, { 0x681BB4, FN_SPAWN_TASK, SITE_SPAWN_NULL },
			{ 0x681C3B, FN_SPAWN_TASK, SITE_SPAWN_NULL }, { 0x682375, FN_SPAWN_TASK, SITE_SPAWN_NULL },
			{ 0x682440, FN_SPAWN_TASK, SITE_SPAWN_NULL },
			// BdPlaySE at counters 1, 0x56, 0x91, 0xD7 (one shared call).
			{ 0x681B1C, FN_PLAY_SE, SITE_SUPPRESS },
			// Stream requests at 0x96 (after `if (0x534270()) return 0`).
			{ 0x681AA9, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x681AC1, FN_STREAM_REQ, SITE_SUPPRESS },
			// 0x534210, the stream pump: a per-call service with its own delay countdown
			// (*0x1DCD6EC[1]--); once per tick = vanilla cadence.
			{ 0x681AC9, 0x534210, SITE_SUPPRESS },
			// BattleFx_StartSoundStateTask 0x508630 at 0x107 (appends a queue-A task, clears 0x25081D8).
			{ 0x681CF2, 0x508630, SITE_SUPPRESS },
			// 0x4A2940 (Vibration_Stop / SE-handle stop) of the handle at +0x10, counter 0x117.
			{ 0x681F9E, FN_STOP_SE, SITE_SUPPRESS },
			// The creature's advance 0x6FBDB0(node + 0x20), inside `if (0x2508114 == 0)`.
			{ 0x6816A9, 0x6FBDB0, SITE_MODEL_STEP },
		};
		const uint32_t CB_PART_QUEUES[] = { 0x2508168 };
		const RunnerDesc CB_PART_RUNNERS[] = {
			// 0x683DC0, queue 0x2508168 (0x1C nodes, spawned 0xE7..0xEA): rising sparkle. Draws at the
			// SVECTOR +0x0C/+0x0E/+0x10 (0x571BC0 only READS it - it builds a billboard matrix), then
			// behind the dword: position += velocity +0x14/+0x16/+0x18, frame +0x12 += 1, dies at 0x0E.
			{ 0x683DC0, 0x12, 1, 3, { 0x0C, 0x0E, 0x10 }, 0x00, 2048 },
			// Stepped: 0x683E80 (static flipbook), 0x6822F0 (per-target shell, recomputed from its
			// counter), 0x683760 / 0x683330 / 0x6831D0 (effect players / RNG shards).
		};
		// Timeline counter 2 adds 0x25081D0 (Init: the target-centroid depth offset) to the posZ of every
		// live enemy (slots 3..6, inline loop at 0x681D6D..0x681D8B) - inline code, no call to replace.
		// Zeroed for the held timeline call it becomes a no-op; the tick adds it exactly once and the
		// root's done path (0x68119E, tick only) takes it off again. Only other reader: that done path.
		const HeldRegion CB_HELD[] = {
			{ 0x25081D0, 4, 1, 0, REGION_ZEROED },
		};

		// ---- Pandemona, effect 291 (MagicList_Logic[290] = thunk 0x6ED250 -> Init 0x6ED260) -----
		// Module [0x6ED250, 0x6F2E80). Via 0x50B2A0 as usual (cmd arg 0x48). Root queue 0x25562B8 <-
		// root 0x6ED350: arena on +0x10 parity every call; at counter 2 (+0x0F, behind the pause)
		// 5 pools + timeline 0x6ED900 (one 0x1030-byte node) into 0x25562A8; ticks 0x25562A8/98/88/78/
		// 68; flash 0x5713E0 every call; steps +0x0C only while 0x2556258 == 0; 2 when the timeline
		// queue is empty. Pause dword 0x2556258: 28 references in the exe, all reads, all in the module
		// (the `DAT_02556258 = 0` at decomp line 511 is a register artifact), no writer.
		// Timeline 0x6ED900 (0x6ED900..0x6F06E5): frame +0x0C; events up to 0x1E0; after that it
		// waits (targets' +0x74->+0x2C & 0x40 until 0x1FE, then 0x534300(2) with the counter parked
		// at 0x208) and returns 2 after writing the targets' original positions back. Its step is not
		// guarded -> INNER hook. Much of its body is unguarded: events, the particle array clears,
		// one integrator call, the model advance, camera, inline RMW blocks (see PD_HELD).
		const uint8_t PD_ROOT_PROLOGUE[] = { 0x83, 0xEC, 0x28, 0x55, 0x56 };                            // sub esp,28h / push ebp / push esi
		const uint8_t PD_INNER_PROLOGUE[] = { 0x8B, 0x54, 0x24, 0x04, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00 }; // mov edx,[esp+4] / sub esp,0A0h
		const SiteDesc PD_SITES[] = {
			// BdPlaySE at counters 0, 99, 0xDF, 0x178, 0x1C0; camera-cut SE at 0x6B, 0xB7, 0x14E; the
			// handle stop at 0x1DC.
			{ 0x6EFE07, FN_PLAY_SE, SITE_SUPPRESS }, { 0x6EFFD6, FN_PLAY_SE, SITE_SUPPRESS },
			{ 0x6F026F, FN_PLAY_SE, SITE_SUPPRESS }, { 0x6F04F2, FN_PLAY_SE, SITE_SUPPRESS },
			{ 0x6F05A1, FN_PLAY_SE, SITE_SUPPRESS },
			{ 0x6EFFF4, FN_PLAY_SE_CUT, SITE_SUPPRESS }, { 0x6F018F, FN_PLAY_SE_CUT, SITE_SUPPRESS },
			{ 0x6F042D, FN_PLAY_SE_CUT, SITE_SUPPRESS },
			{ 0x6F061A, FN_STOP_SE, SITE_SUPPRESS },
			// Stream requests (counters 0: x4, 99: x2, 0xE0: x2, 0x1E0: x3).
			{ 0x6EFB79, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6EFB92, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6EFBAB, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6EFBC2, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6EFBF6, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6EFC0F, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6EFC54, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6EFC6D, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6EFCBC, FN_STREAM_REQ, SITE_SUPPRESS }, { 0x6EFCD3, FN_STREAM_REQ, SITE_SUPPRESS },
			{ 0x6EFCEC, FN_STREAM_REQ, SITE_SUPPRESS },
			// Stream pump 0x534210 (every call).
			{ 0x6EFCFA, 0x534210, SITE_SUPPRESS },
			// The texture reload state machine on 0x13BBBE8 (reset to 0 at 0x1E0, then one step per CALL:
			// sync file load 0x48D0A0(0x168/0x169/0x16A, buf) + Battle_QueueTIMUpload 0x505E30). The state
			// word is restored after every held call (PD_HELD) and both calls are dropped while held, so
			// the three reloads land on three consecutive ticks as in vanilla (no VRAM-op flood, which a
			// held 0x1E0 re-arming the machine every host frame would otherwise cause). Results unused.
			{ 0x6EFD96, 0x48D0A0, SITE_SUPPRESS }, { 0x6EFDA8, 0x505E30, SITE_SUPPRESS }, // state 0 -> 1
			{ 0x6EFD5E, 0x48D0A0, SITE_SUPPRESS }, { 0x6EFD70, 0x505E30, SITE_SUPPRESS }, // state 1 -> 2
			{ 0x6EFD26, 0x48D0A0, SITE_SUPPRESS }, { 0x6EFD36, 0x505E30, SITE_SUPPRESS }, // state 2 -> 3
			// 0x6F1520 = clear the node's particle array (+0x42C, 0x80 x 0x18) and reset its cursor
			// +0x102C: unguarded `== counter` calls at 0x28, 0x9A, 0xE0, 0xFE, 0x14F, 0x179. A held
			// frame runs the next counter's body, so it would clear what the tick still draws.
			{ 0x6EDC52, 0x6F1520, SITE_SUPPRESS }, { 0x6EE5A7, 0x6F1520, SITE_SUPPRESS },
			{ 0x6EE761, 0x6F1520, SITE_SUPPRESS }, { 0x6EE935, 0x6F1520, SITE_SUPPRESS },
			{ 0x6EEF38, 0x6F1520, SITE_SUPPRESS }, { 0x6EF360, 0x6F1520, SITE_SUPPRESS },
			// 0x6F1B20 = the particle integrator (life--, pos += vel, gravity); unguarded only in the
			// 0x1A1..0x1B7 block (0x6EE1CC / 0x6EEEFF / 0x6EF978 sit behind the dword).
			{ 0x6EF79F, 0x6F1B20, SITE_SUPPRESS },
			// The creature's advance 0x6FBDB0(node + 0x154) - NOT behind the dword (mechanism 1).
			{ 0x6EFDDD, 0x6FBDB0, SITE_MODEL_STEP_HELD },
		};
		const uint32_t PD_PART_QUEUES[] = { 0x2556298, 0x2556288 };
		const RunnerDesc PD_PART_RUNNERS[] = {
			// 0x6F0ED0, queue 0x2556298: swirl sprite at angle +0x10 (& 0xFFF into the sin table) and radius
			// +0x0E; behind the dword +0x10 += +0x12 and +0x0C += 1 (dies after 0x0B).
			{ 0x6F0ED0, 0x0C, 1, 1, { 0x10 }, 0x01, 4096 },
			// 0x6F0FB0, queue 0x2556298: debris sprite at SVECTOR +0x0C/+0x0E/+0x10; behind the dword
			// position += char velocity * 16 (+0x14..+0x16), frame +0x12 += 1 (dies after 0x0B).
			{ 0x6F0FB0, 0x12, 1, 3, { 0x0C, 0x0E, 0x10 }, 0x00, 2048 },
			// 0x6F1060, queue 0x2556288 (0x45C nodes): the tornado ring, 0x6F29D0(angle +0x16, radius
			// +0x1A, ...); behind the dword +0x16 += +0x18, +0x1A += +0x1C from counter 0x14 (0x1000 set
			// there), +0x0C += 1. Its trail 0x6D99D0 state (+0x20/+0x24) stays stepped.
			{ 0x6F1060, 0x0C, 1, 2, { 0x16, 0x1A }, 0x01, 0x500 },
			// Stepped: the particle array at node +0x42C (not task nodes), 0x6F0D90 / 0x6F0E30 (effect
			// players, `paused` = the dword), the effect player at node +0x1F0.
		};
		// Inline, unguarded state writes of the timeline (no call to replace), restored after a held call:
		//   0x13BBBE8 - the reload state machine (reset `= 0` inline at 0x1E0, stepped inline);
		//   node +0x14..+0x113 - per-target block (8 x 0x20: entity ptr, extents +0x18 written by
		//     0x6ED770 every call, +0x28/+0x2C set inline at 0x179, `+0x2C += 0x800` RMW at 0x1B8);
		//   node +0x42C..+0x102F - particle array + cursor (`>>= 4` RMW over every live particle at
		//     0x11C, the 0x1A1 block's integrator input);
		//   targets' positions: BattleEntitySlotData (0x1D972C0, 7 x 0x9C) +0x1C..+0x21 (int16 x, y, z:
		//     `+0x1C/+0x20 =`, `+0x1E -=` RMW at 0x1B8; `+0x1E = -extent` every call from 0x1B8) and
		//     +0x54..+0x5F (int32 copies written every call from 0xE0 and by 0x6ED770).
		const HeldRegion PD_HELD[] = {
			{ 0x13BBBE8, 4, 1, 0, 0 },
			{ 0x14, 0x100, 1, 0, REGION_NODE_RELATIVE },
			{ 0x42C, 0xC04, 1, 0, REGION_NODE_RELATIVE },
			{ 0x1D972DC, 6, 7, 0x9C, 0 },
			{ 0x1D97314, 12, 7, 0x9C, 0 },
		};

		// ---- Odin, effect 187 (MagicList_Logic[186] = Init 0x6472E0) --------------------------------------
		// Module [0x6472E0, 0x6541E0) (Diablos' thunk follows). Arrives as command 0xF5 through
		// BattleActionSequence_Tick_OdinGilgamesh 0x50B830 (state 2 stores Init's return 0x24FD6C0 in the
		// effect root). The Shiva shape: root queue 0x24FD6C0 (0x10 x 2) <- root 0x64DD50, which only runs
		// ExecuteTaskQueue(0x24FD990) and steps its own +0x0C unconditionally (nothing reads it); sub-queue
		// 0x24FD990 (0x64 x 0x24) <- timeline 0x6474C0. Timeline: `if (0x1D96A9C & 0x201) { if (& 1) return 0;
		// ... }` at the very top (after a pure ComposeAffineTransform), every event keyed on its +0x0C, the
		// shared GF camera VM linked at counter 0 (0x63E960(0xE41F28, frame 0x24FE880, model 0x24FD8D8,
		// &0x24FD990, 0)), damage 0x506BA0 at 0x11F (site 0x6478D1), returns 2 once the counter passes 0x120.
		// 53 freeze-bit reads in the module; every spawn helper (0x647900 / 0x647960 / 0x6483B0 / 0x648820 /
		// 0x64A690 / 0x64B040 / 0x64B610 / 0x64BB20 / 0x64C300 / 0x64C710 / 0x64D850 / 0x64DC70) is called
		// only from the timeline or from behind `(0x1D96A9C & 0x201) == 0` in the creature director
		// 0x649D00 (whose `counter == 0` block runs on its first call, i.e. the tick that spawned it). No
		// pause dword (0x24FD50C is a done flag 0x649D00 sets at 0xC6, read by 0x64C060). No direct camera
		// write: 0x64B130 / 0x64B300 and five tasks only READ 0xB8B7F0 / 0x1D8E038.
		const uint8_t ODIN_ROOT_PROLOGUE[] = { 0x68, 0x90, 0xD9, 0x4F, 0x02 };     // push 0x24FD990 (imm32, position independent)
		const SiteDesc ODIN_SITES[] = {
			// The creature (Odin + Sleipnir, model block 0x24FD8D8): director 0x649D00 steps it through
			// 0x64A260 = 0x508F90(+0x60, +0x6C) (byte-identical to Shiva's 0x5C4EA0), all four sites inside
			// `(0x1D96A9C & 0x201) == 0`.
			{ 0x649E21, 0x64A260, SITE_MODEL_STEP }, { 0x649E65, 0x64A260, SITE_MODEL_STEP },
			{ 0x649F24, 0x64A260, SITE_MODEL_STEP }, { 0x64A191, 0x64A260, SITE_MODEL_STEP },
			// Task 0x648490 (spawned by 0x6483B0 at timeline 0x28, 0x1B ticks): warps a 128x128 texture from
			// its +0x18 phase and appends Battle_QueueVramLoad 0x505DF0(0xE41EA0, buf) on EVERY call, before
			// its freeze test - the Shiva lesson (VRAM op queue 0x1D98220, 0x20 slots, unbounded flush). Held:
			// the phase is held, the upload is byte-identical to the tick's; skip it. Result unused.
			{ 0x648571, 0x505DF0, SITE_SUPPRESS },
		};

		// ---- Gilgamesh, effects 327..330 (Inits 0x58D760 / 0x58D930 / 0x58DB10 / 0x58DCF0) --------------
		// Module [0x58D760, 0x596BA0). ONE module for all four attacks: each Init stores its variant in
		// 0x21FFA70 (327 -> 0, 328 -> 1, 329 -> 2, 330 -> 3) and registers the same tree. Arrives as command
		// 0xF5 through 0x50B830. Root queue 0x21FF4A8 (0x10 x 2) <- root 0x596B70: ExecuteTaskQueue(0x21FFD58)
		// (timeline + effect tasks), ExecuteTaskQueue(0x21FF458) (the model tasks, linked by 0x592990 /
		// 0x5947F0), +0x0C++ unconditionally, 2 when both are empty. Timeline 0x592300: the same freeze test at
		// the top, camera VM at counter 0 (0x63E960(0xCD0960[variant], frame 0x21FF3A0, model 0x2200FC0,
		// &0x21FFD58, 0)), the variant's attack task 0xCD0990[variant] (0x58DED0 / 0x590510 / 0x591250 /
		// 0x5920E0) linked at 0x111, returns 2 once the counter passes 0x147. The damage is in the attack
		// tasks, all four of which return 0 at their very top while the bit is set: 0x58DED0 calls
		// 0x506690 per target at its 0x10 (sites 0x58E092; 0x58E77D in its child 0x58E570, behind the bit),
		// the other three 0x506BA0 at their 0x11 (0x590764 / 0x591443 / 0x5922D9). 73 freeze reads; every
		// BdLinkTask site sits in a helper called only from the timeline, from a top-guarded task, or from a
		// `counter == 0` first-call block. No pause dword (0x2200FA0 is zeroed by Init and never read), no
		// direct camera write (0x593250 and four tasks only READ the pose / roll).
		const uint8_t GIL_ROOT_PROLOGUE[] = { 0x56, 0x68, 0x58, 0xFD, 0x1F, 0x02 }; // push esi / push 0x21FFD58
		const SiteDesc GIL_SITES[] = {
			// Model steps 0x595E00 = 0x508F90(+0x60, +0x6C), all behind `(0x1D96A9C & 0x201) == 0`:
			// task 0x595D50 (4 blocks 0x21FF4F0 + i*0x9C), 0x5966B0 (0x21FF760), 0x596800 (0x2200F00, then
			// 0x21FF800 + i*0x9C), 0x596A60 (0x2200FC0, then 0x21FF4F0 + i*0x9C). See note 2 above: one
			// learned block per tick unless gf_pacing learns a list.
			{ 0x595DBF, 0x595E00, SITE_MODEL_STEP }, { 0x5966E8, 0x595E00, SITE_MODEL_STEP },
			{ 0x59688B, 0x595E00, SITE_MODEL_STEP }, { 0x596899, 0x595E00, SITE_MODEL_STEP },
			{ 0x596AE0, 0x595E00, SITE_MODEL_STEP }, { 0x596AEE, 0x595E00, SITE_MODEL_STEP },
			// Nothing to suppress: no VRAM-op append outside the timeline / Init, no unguarded spawn, sound,
			// stream or damage (closure of the timeline, the 4 attack tasks and every linked task).
		};

		// ---- Zantetsuken Reverse, effect 326 (Init 0x62A7E0; Seifer cuts Odin down, disc 3) -------------
		// Module [0x62A7E0, 0x6326D0), an Odin-module copy: root queue 0x24F0ED8 <- root 0x6326B0
		// (ExecuteTaskQueue(0x24F1320), +0x0C++); timeline 0x62A9C0 in 0x24F1320 with the same top freeze
		// test, camera VM at counter 0 (0x63E960(0xE197D0, frame 0x24F2248, model 0x24F1268, &0x24F1320,
		// 0), site 0x62AA1A), damage 0x506BA0 at 0x198 (0x62AF6B), spawns the sound task 0x62CEA0 and
		// returns 2 once the counter passes 0x199. 65 freeze reads; director 0x62CEE0 = Odin's 0x649D00.
		// Held-reachable (F2_held descent from every task callback): only the VRAM load below, the
		// first-call spawn block of 0x62CEE0 (a tick), and 0x62CEA0's sound-state call 0x62CEBD (Odin's
		// 0x649CC0 pattern, must NOT be suppressed). Every camera access in the module is a read.
		const uint8_t ZREV_ROOT_PROLOGUE[] = { 0x68, 0x20, 0x13, 0x4F, 0x02 };     // push 0x24F1320
		const SiteDesc ZREV_SITES[] = {
			// Model advance 0x62CBC0 = 0x508F90(+0x60, +0x6C), all behind the bit. Director 0x62CEE0 (Odin,
			// block 0x24F1268): seven sites; tasks 0x62CA80 (block 0x24F0EE8) and 0x62CC00 (block 0x24F0D50).
			{ 0x62D005, 0x62CBC0, SITE_MODEL_STEP }, { 0x62D029, 0x62CBC0, SITE_MODEL_STEP },
			{ 0x62D063, 0x62CBC0, SITE_MODEL_STEP }, { 0x62D070, 0x62CBC0, SITE_MODEL_STEP },
			{ 0x62D08C, 0x62CBC0, SITE_MODEL_STEP }, { 0x62D1AB, 0x62CBC0, SITE_MODEL_STEP },
			{ 0x62D26C, 0x62CBC0, SITE_MODEL_STEP },
			{ 0x62CB0E, 0x62CBC0, SITE_MODEL_STEP }, { 0x62CC8F, 0x62CBC0, SITE_MODEL_STEP },
			// Task 0x62BB40 = Odin's 0x648490: Battle_QueueVramLoad on every call before its freeze test.
			{ 0x62BC21, 0x505DF0, SITE_SUPPRESS },
		};

		// ---- Gilgamesh arrival, effect 218 (Init 0x6326D0; disc 3, after 326) ----------------------------
		// Module [0x6326D0, 0x637500), a Gilgamesh-module copy: root queue 0x24F2418 <- root 0x6374D0
		// (ExecuteTaskQueue(0x24F2518), ExecuteTaskQueue(0x24F23B8), +0x0C++); timeline 0x6328A0 in 0x24F2518,
		// same top freeze test, camera VM at 0 (0x63E960(0xE25970, frame 0x24F2348, model 0x24F3558,
		// &0x24F2518, 0), site 0x6328FA), result ApplyActionResultToTarget 0x506690 at 0x17F (0x632DC7),
		// returns 2 once the counter passes 0x243. 29 freeze reads. Camera accesses are reads only.
		const uint8_t GARR_ROOT_PROLOGUE[] = { 0x56, 0x68, 0x18, 0x25, 0x4F, 0x02 }; // push esi / push 0x24F2518
		const SiteDesc GARR_SITES[] = {
			// Task 0x634C00 (linked from behind the bit at 0x633E6D): `counter == 0xF` links 24 x 0x635110
			// in a loop (0x634CEA) and `counter == 0x10` links 0x636CA0 (0x634DEB) and 0x636EB0 (0x634DFE),
			// all BEFORE its freeze test and none null-checked (`mov [eax+0xC], ...` straight after) -> one
			// batch per held frame while the counter sits there. Scratch node while held. (The loop's
			// rand() calls and its writes to the static array 0x24F3378 still run on held frames; the tick's
			// own pass rewrites the array before the spawned tasks first read it.)
			{ 0x634CEA, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH }, { 0x634DEB, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH },
			{ 0x634DFE, FN_SPAWN_TASK, SITE_SPAWN_SCRATCH },
			// Model advance 0x6373F0 (= 0x5C4EA0) in task 0x637350 behind the bit: block 0x24F2428, then
			// 0x24F3558 (Gilgamesh, the camera VM's model; the one a single-block gf_pacing keeps).
			{ 0x6373BE, 0x6373F0, SITE_MODEL_STEP }, { 0x6373C8, 0x6373F0, SITE_MODEL_STEP },
		};

		const GfDesc GF_TABLE[] = {
			{ "Quezacotl", 116, 0x6C3760, QZ_ROOT_PROLOGUE, sizeof(QZ_ROOT_PROLOGUE),
				0x6C3940, QZ_INNER_PROLOGUE, sizeof(QZ_INNER_PROLOGUE),
				HOLD_PAUSE_DWORD, 0x25216DC, 0x2521738, 0x6C3940, 0x163, true,
				QZ_SITES, (uint8_t)(sizeof(QZ_SITES) / sizeof(QZ_SITES[0])),
				QZ_PART_QUEUES, (uint8_t)(sizeof(QZ_PART_QUEUES) / sizeof(QZ_PART_QUEUES[0])),
				QZ_PART_RUNNERS, (uint8_t)(sizeof(QZ_PART_RUNNERS) / sizeof(QZ_PART_RUNNERS[0])) },
			// owns_camera (v73): the module itself has no reference to the pose, but its timeline links
			// the SHARED GF camera VM 0x63E9C0 into 0x22BC198 (0x63E960 at counter 0: script 0xD32604,
			// frame 0x22BD0C0, model 0x22BD018), and that task writes g_battleCamPos 0xB8B7F0 / F8 on
			// every call - frozen or not (the frozen path recomputes it from its held state).
			{ "Shiva", 185, 0x5C7F50, SHIVA_ROOT_PROLOGUE, sizeof(SHIVA_ROOT_PROLOGUE),
				0, nullptr, 0,
				HOLD_FREEZE_BIT, 0, 0x22BC198, 0x5C0F30, 0x113, true,
				SHIVA_SITES, (uint8_t)(sizeof(SHIVA_SITES) / sizeof(SHIVA_SITES[0])),
				SHIVA_PART_QUEUES, (uint8_t)(sizeof(SHIVA_PART_QUEUES) / sizeof(SHIVA_PART_QUEUES[0])),
				SHIVA_PART_RUNNERS, (uint8_t)(sizeof(SHIVA_PART_RUNNERS) / sizeof(SHIVA_PART_RUNNERS[0])) },
			// Camera written directly (cuts at == counters 0, 0x1E, 0x5A, 0x99, 0xCA, 0xED, 0x129, 0x195,
			// 0x1C7, 0x1F2, roll 0x1D8E038 = 0x120 at 0; pans through 0x657DF0, recomputed from the
			// counter on every call) -> owns_camera + camera_hold_pans.
			{ "Diablos", 325, 0x654350, DB_ROOT_PROLOGUE, sizeof(DB_ROOT_PROLOGUE),
				0x6545F0, DB_INNER_PROLOGUE, sizeof(DB_INNER_PROLOGUE),
				HOLD_PAUSE_DWORD, 0x2505180, 0x25051A0, 0x6545F0, 0x211, true,
				DB_SITES, (uint8_t)(sizeof(DB_SITES) / sizeof(DB_SITES[0])),
				DB_PART_QUEUES, (uint8_t)(sizeof(DB_PART_QUEUES) / sizeof(DB_PART_QUEUES[0])),
				DB_PART_RUNNERS, (uint8_t)(sizeof(DB_PART_RUNNERS) / sizeof(DB_PART_RUNNERS[0])),
				true },
			// Phoenix. Camera: 70 direct accesses to 0xB8B7F0..FF / roll
			// 0x1D8E038 / 0x1D977A2 in the timeline: absolute cuts (== 0, 0x15, 0x29, 0x3D->0x52, 100, 0x96,
			// 0xC2) plus UNGUARDED read-modify-write pans recomputed each call from the pose (orbit by
			// 8/4096 per call + dolly at 0x65..0x95, pos.vy += f(counter) at 0xC2..) -> owns_camera (the
			// root hook's camera_write(camera_next) before every call is what keeps the RMW pans from
			// accumulating on held frames) + camera_hold_pans (the held call yields the NEXT tick's pan pose;
			// every RMW step is far below CAMERA_CUT_UNITS, so no adopted cut is ever re-applied). The 0x52..
			// 0x64 GTE pans and the +200/+100 dolly are pause-guarded.
			{ "Phoenix", 140, 0x6A6530, PH_ROOT_PROLOGUE, sizeof(PH_ROOT_PROLOGUE),
				0x6A6750, PH_INNER_PROLOGUE, sizeof(PH_INNER_PROLOGUE),
				HOLD_PAUSE_DWORD, 0x2517AAC, 0x2517AD8, 0x6A6750, 0xFE, true,
				PH_SITES, (uint8_t)(sizeof(PH_SITES) / sizeof(PH_SITES[0])),
				PH_PART_QUEUES, (uint8_t)(sizeof(PH_PART_QUEUES) / sizeof(PH_PART_QUEUES[0])),
				PH_PART_RUNNERS, (uint8_t)(sizeof(PH_PART_RUNNERS) / sizeof(PH_PART_RUNNERS[0])),
				true },
			// owns_camera: both link the shared camera VM 0x63E9C0 (writes 0xB8B7F0/F8 every call; under the bit it
			// recomputes the pose from its held local pose x the frame matrix - 0x24FBE88 / 0x2259950, static after
			// Init - so a held frame reproduces the tick's pose exactly; it also reads the pose first, so the
			// camera_next restore before the root is required, as for Shiva). No direct pose writes in either module
			// outside the VM. camera_hold_pans = true: under the freeze bit the VM cannot cut, so any held-frame change
			// is noise (e.g. a frame-matrix rewrite) and must not be adopted as a 15 fps step.
			{ "Doomtrain", 191, 0x6472C0, DT_ROOT_PROLOGUE, sizeof(DT_ROOT_PROLOGUE),
				0, nullptr, 0,
				HOLD_FREEZE_BIT, 0, 0x24FC330, 0x63F2D0, 0x1A7, true,
				nullptr, 0,
				nullptr, 0,
				nullptr, 0,
				true },
			{ "Cactuar", 199, 0x5AA3A0, CA_ROOT_PROLOGUE, sizeof(CA_ROOT_PROLOGUE),
				0, nullptr, 0,
				HOLD_FREEZE_BIT, 0, 0x2259A08, 0x5A8940, 0x97, true,
				CA_SITES, (uint8_t)(sizeof(CA_SITES) / sizeof(CA_SITES[0])),
				nullptr, 0,
				nullptr, 0,
				true },
			// Camera written directly by the timeline: cuts at counters 1, 0x91, 0xD7, 0xE7 (roll 0x120 and
			// 0x1D977A2 = 0 at 1), lookAt from a creature bone (0x682270 -> 0xB8B7F8) at 0x56..0x5B and
			// 0xD7..0xE6, and read-modify-write pans every call (0x683D10 rotates pos about lookAt;
			// pos.vy += 0x32 / -0x1E, lookAt.vy -= 0x32) -> owns_camera + camera_hold_pans.
			{ "Carbuncle", 278, 0x680DF0, CB_ROOT_PROLOGUE, sizeof(CB_ROOT_PROLOGUE),
				0x681630, CB_INNER_PROLOGUE, sizeof(CB_INNER_PROLOGUE),
				HOLD_PAUSE_DWORD, 0x2508114, 0x2508188, 0x681630, 0x11A, true,
				CB_SITES, (uint8_t)(sizeof(CB_SITES) / sizeof(CB_SITES[0])),
				CB_PART_QUEUES, (uint8_t)(sizeof(CB_PART_QUEUES) / sizeof(CB_PART_QUEUES[0])),
				CB_PART_RUNNERS, (uint8_t)(sizeof(CB_PART_RUNNERS) / sizeof(CB_PART_RUNNERS[0])),
				true, CB_HELD, (uint8_t)(sizeof(CB_HELD) / sizeof(CB_HELD[0])) },
			// Camera written directly: absolute cuts at 0, 0x27, 99, 0x7B, 0x99, 0x9A+10, 0xDF, 0xFD, 0x11B,
			// 0x13F, 0x178 (roll 0xF0 / 0x120), counter-driven blends GTE_WeightedBlend2 (0xB7.., 0x14E..),
			// target-following shots from 0x1B8 (GetDefaultEffectPosition -> 0xB8B7F8), RMW pans every
			// call (0x683D10; lookAt.vy += 0x82; pos.vy += -200); screen shake g_battleShakeY
			// (absolute from the counter, 0x90..0x95) -> owns_camera + camera_hold_pans.
			{ "Pandemona", 291, 0x6ED350, PD_ROOT_PROLOGUE, sizeof(PD_ROOT_PROLOGUE),
				0x6ED900, PD_INNER_PROLOGUE, sizeof(PD_INNER_PROLOGUE),
				HOLD_PAUSE_DWORD, 0x2556258, 0x25562A8, 0x6ED900, 0x1E0, true,
				PD_SITES, (uint8_t)(sizeof(PD_SITES) / sizeof(PD_SITES[0])),
				PD_PART_QUEUES, (uint8_t)(sizeof(PD_PART_QUEUES) / sizeof(PD_PART_QUEUES[0])),
				PD_PART_RUNNERS, (uint8_t)(sizeof(PD_PART_RUNNERS) / sizeof(PD_PART_RUNNERS[0])),
				true, PD_HELD, (uint8_t)(sizeof(PD_HELD) / sizeof(PD_HELD[0])) },
			// The shared GF camera VM 0x63E9C0 (linked by the timeline at counter 0, like Shiva) writes
			// 0xB8B7F0 / F8 on every call, frozen or not -> owns_camera. v10: held frames DO move the pose
			// (gf10 trace: 108-118 adopted held-frame changes per second early on, then one per tick), so
			// every held change was taken as a cut and the camera stepped at 15 Hz: camera_hold_pans. Real
			// cuts come from the VM's set op (camera_vm_set) and the distance test on the tick. No particle rows (every task's state is a
			// frame index or a counter-derived fade; audit with the 1/s trace before adding any).
			{ "Odin", 187, 0x64DD50, ODIN_ROOT_PROLOGUE, sizeof(ODIN_ROOT_PROLOGUE),
				0, nullptr, 0,
				HOLD_FREEZE_BIT, 0, 0x24FD990, 0x6474C0, 0x120, true,
				ODIN_SITES, (uint8_t)(sizeof(ODIN_SITES) / sizeof(ODIN_SITES[0])),
				nullptr, 0, nullptr, 0, true, nullptr, 0, true },
			// One row for 327..330 (the root is shared; found from node + 8).
			{ "Gilgamesh", 327, 0x596B70, GIL_ROOT_PROLOGUE, sizeof(GIL_ROOT_PROLOGUE),
				0, nullptr, 0,
				HOLD_FREEZE_BIT, 0, 0x21FFD58, 0x592300, 0x147, true,
				GIL_SITES, (uint8_t)(sizeof(GIL_SITES) / sizeof(GIL_SITES[0])),
				nullptr, 0, nullptr, 0 },
			// Disc-3 Seifer event (cmd 0xEC via 0x50B830), module copies of Odin / Gilgamesh.
			{ "Zantetsuken Reverse", 326, 0x6326B0, ZREV_ROOT_PROLOGUE, sizeof(ZREV_ROOT_PROLOGUE),
				0, nullptr, 0,
				HOLD_FREEZE_BIT, 0, 0x24F1320, 0x62A9C0, 0x199, true,
				ZREV_SITES, (uint8_t)(sizeof(ZREV_SITES) / sizeof(ZREV_SITES[0])),
				nullptr, 0, nullptr, 0 },
			{ "Gilgamesh arrival", 218, 0x6374D0, GARR_ROOT_PROLOGUE, sizeof(GARR_ROOT_PROLOGUE),
				0, nullptr, 0,
				HOLD_FREEZE_BIT, 0, 0x24F2518, 0x6328A0, 0x243, true,
				GARR_SITES, (uint8_t)(sizeof(GARR_SITES) / sizeof(GARR_SITES[0])),
				nullptr, 0, nullptr, 0 },
			// Not rows: Brothers (205, Init 0xAF4520 / root 0xAF4B90) is the Ifrit animation-VM family
			// (its root and integrator 0xAF4EB0 are byte-for-byte Ifrit's 0xB25DF0 / 0xB26110) - see
			// gf_vm.cpp. Siren (95, Init 0x739DA0 / root 0x739F40) is built on the generic effect
			// framework (Effect_AddTaskAndInitFromCtx 0x8DC540, shared with the ordinary spells): its
			// nodes step +0x24 / the phase +0x29 on every call and nothing in the module or the
			// framework reads a pause dword or the freeze bit, so there is no hold primitive to drive -
			// it is paced through animations.cpp's spell skip + replay path instead (gf_siren.cpp), as are
			// Tonberry 90, Moogle Dance 96, the Chocobo attacks 97..100 and Moomba 338 (v76). The
			// animation-VM family (Ifrit & co., and the boss cinematics) is gf_vm.cpp. The rows above must
			// never ALSO be spell-path rows: their hold needs the tree to run on every host frame.
		};
		constexpr int GF_COUNT = (int)(sizeof(GF_TABLE) / sizeof(GF_TABLE[0]));

		// --- state ------------------------------------------------------------------------------

		typedef int (__cdecl *gf_runner_t)(uint8_t*);
		gf_runner_t original_root[GF_COUNT] = {};
		gf_runner_t original_inner[GF_COUNT] = {};
		constexpr int MAX_ROW_SITES = 255;  // SiteDesc count is a uint8_t
		bool site_patched[GF_COUNT][MAX_ROW_SITES] = {};  // per row, per site: patched

		constexpr uint32_t NO_FRAME = 0xFFFFFFFF;

		int active_row = -1;                 // the row whose root ran last (one summon at a time)
		uint32_t last_root_frame = NO_FRAME; // last host frame in which a root ran
		int suppress_depth = 0;              // > 0 while a held (skipped-frame) root call is on the stack
		bool warned_watchdog = false;
		int16_t traced_counter = -1;

		// The creature model blocks, learned from the model-step sites (v6: a list - Gilgamesh steps up to
		// ten blocks per tick). `known_models` is what the held frames ask poses for: the blocks the last
		// logic tick that stepped any model stepped (a tick that steps none keeps the previous list, the
		// pre-v6 single-block behaviour). `tick_models` collects the blocks of the tick in progress.
		constexpr int MAX_MODELS = 12;
		uint8_t* known_models[MAX_MODELS] = {};
		int known_model_count = 0;
		uint8_t* tick_models[MAX_MODELS] = {};
		int tick_model_count = 0;
		bool collecting_models = false;       // inside a logic-tick root call

		void learn_model_block(uint8_t* model_block)
		{
			if (model_block == nullptr) return;
			uint8_t** list = collecting_models ? tick_models : known_models;
			int& count = collecting_models ? tick_model_count : known_model_count;
			for (int i = 0; i < count; i++)
			{
				if (list[i] == model_block) return;
			}
			if (count < MAX_MODELS) list[count++] = model_block;
		}

		uint8_t held_buffer[HELD_BUFFER_BYTES]; // HeldRegion save area (one held inner call at a time)

		// v8 host-frame stall diagnostic (stall_check)
		LARGE_INTEGER stall_prev_perf_count = {};
		uint32_t stall_prev_frame = NO_FRAME;
		int16_t stall_prev_counter = -1;
		bool stall_prev_was_tick = false;
		bool stall_warned = false;

		uint8_t spawn_scratch[0x200];         // handed to SITE_SPAWN_SCRATCH callers while held; never linked

		struct CameraPose
		{
			int16_t words[CAMERA_POSE_WORDS];
			int16_t roll;
			int16_t z_roll;
		};
		CameraPose camera_prev = { { 0, 0, 0, 0, 0, 0, 0, 0 }, 0, 0 };
		CameraPose camera_next = { { 0, 0, 0, 0, 0, 0, 0, 0 }, 0, 0 };
		bool camera_valid = false;
		bool camera_vm_set = false;              // v10: the camera VM ran a set (op 1) during this root call

		// v10: model translation blend (GfDesc::blend_model_translation).
		struct TranslationSlot
		{
			uint8_t* model_block;
			int32_t prev[3];
			int32_t next[3];
			uint32_t tick;
		};
		TranslationSlot translation_slots[8];
		int translation_slots_used = 0;

		constexpr int PARTICLE_SLOTS = 192;
		struct ParticleSlot
		{
			uint8_t* node;
			uint8_t runner_index;
			bool has_next;   // one capture taken
			bool paired;     // prev and next are two consecutive ticks of the SAME particle
			uint32_t tick;   // logic_tick_count() of the capture in `next`
			int16_t sequence;
			int16_t prev[PARTICLE_FIELDS];
			int16_t next[PARTICLE_FIELDS];
		};
		ParticleSlot particle_slots[PARTICLE_SLOTS];
		int particle_slots_used = 0;

		// Watchdog: a cinematic that stops advancing must never be held. Re-arming.
		constexpr uint32_t GF_WATCHDOG_MS = 2000;
		struct AdvanceWatch
		{
			bool valid;
			bool stuck;
			int16_t counter;
			uint32_t stamp;
		};
		AdvanceWatch root_watch = { false, false, 0, 0 };
		AdvanceWatch inner_watch = { false, false, 0, 0 };

		// v73: once-a-second summary while a cinematic runs (trace_battle_animation only).
		int32_t* const VRAM_OP_COUNT = (int32_t*)0x1D98420; // g_battleVramOpCount, flushed at flip
		struct PaceStats
		{
			uint32_t stamp;       // GetTickCount() of the last summary
			uint32_t frames;
			uint32_t held_frames;
			uint32_t model_requests, model_skips;          // pose requests issued / skipped (no model, frame 0, finished)
			uint32_t particles_blended, particles_stepped; // node blends written / tracked nodes left stepped
			uint32_t camera_blends, camera_adopts;         // camera blends published / cuts adopted on held frames
			uint32_t camera_vm_cuts;                       // v10: cuts found through the camera VM's set op
			uint32_t suppressed;                           // one-shot calls swallowed while held
			int32_t vram_ops_max;                          // highest VRAM op count seen after a root call
		};
		PaceStats stats = {};
		bool warned_vram = false;

		// --- small helpers ------------------------------------------------------------------------

		__forceinline uint8_t* queue_head(uint32_t queue)
		{
			return *(uint8_t**)queue;
		}

		__forceinline int wrap_angle12(int difference)
		{
			return ((difference + 2048) & 4095) - 2048;
		}

		bool pacing_off()
		{
			return (ff8_interp::cfg().battle_debug & DBG_GF_PACE_OFF) != 0;
		}

		bool gf_live_now()
		{
			return !pacing_off() && last_root_frame != NO_FRAME && (ff8_interp::frame_counter() - last_root_frame) <= 1;
		}

		// True only inside a held root call, i.e. exactly while the suppressed sites must be quiet.
		bool suppressing()
		{
			return suppress_depth > 0 && in_battle();
		}

		int row_by_root(uint32_t runner_fn)
		{
			for (int row_index = 0; row_index < GF_COUNT; row_index++)
			{
				if (GF_TABLE[row_index].root_fn == runner_fn) return row_index;
			}
			return -1;
		}

		int row_by_inner(uint32_t runner_fn)
		{
			for (int row_index = 0; row_index < GF_COUNT; row_index++)
			{
				if (GF_TABLE[row_index].inner_fn != 0 && GF_TABLE[row_index].inner_fn == runner_fn) return row_index;
			}
			return -1;
		}

		// Forget everything that belongs to one cinematic.
		void cinematic_forget()
		{
			stall_warned = false;
			stall_prev_frame = NO_FRAME;
			camera_valid = false;
			particle_slots_used = 0;
			translation_slots_used = 0;
			camera_vm_set = false;
			known_model_count = 0;
			tick_model_count = 0;
			collecting_models = false;
			root_watch.valid = inner_watch.valid = false;
			root_watch.stuck = inner_watch.stuck = false;
			traced_counter = -1;
		}

		// Returns true while this counter has not moved for more than GF_WATCHDOG_MS: the hold is
		// dropped, the engine runs unpaced (vanilla behaviour) and the counter can move again, which
		// re-arms the hold. Warns once per animations_reset().
		bool watchdog_stuck(AdvanceWatch& watch, int16_t counter, const char* which)
		{
			const uint32_t now = GetTickCount();
			if (!watch.valid || watch.counter != counter)
			{
				watch.valid = true;
				watch.stuck = false;
				watch.counter = counter;
				watch.stamp = now;
				return false;
			}
			if (now - watch.stamp <= GF_WATCHDOG_MS) return watch.stuck;
			if (!watch.stuck)
			{
				watch.stuck = true;
				if (!warned_watchdog)
				{
					warned_watchdog = true;
					ff8_interp::log_warning("%s: GF cinematic %s stuck at frame %d for more than %u ms, pacing released.\n",
						__func__, which, (int)counter, GF_WATCHDOG_MS);
				}
			}
			return true;
		}

		// The timeline node of a row: the node in timeline_queue whose runner is timeline_fn. Queues are
		// {head, tail}, nodes {flags, next + 4, runner + 8}, exactly as 0x508420 walks them.
		uint8_t* timeline_node(const GfDesc& gf_row)
		{
			if (gf_row.timeline_queue == 0) return nullptr;
			uint8_t* node = queue_head(gf_row.timeline_queue);
			for (int guard = 0; node != nullptr && guard < 256; guard++)
			{
				if (*(const uint32_t*)(node + TASK_RUNNER) == gf_row.timeline_fn) return node;
				node = *(uint8_t**)(node + TASK_NEXT);
			}
			return nullptr;
		}

		// --- camera -----------------------------------------------------------------------------

		void camera_read(CameraPose& pose)
		{
			memcpy(pose.words, CAMERA_POSE, sizeof(pose.words));
			pose.roll = *CAMERA_ROLL;
			pose.z_roll = *CAMERA_Z_ROLL;
		}

		void camera_write(const CameraPose& pose)
		{
			memcpy(CAMERA_POSE, pose.words, sizeof(pose.words));
			*CAMERA_ROLL = pose.roll;
			*CAMERA_Z_ROLL = pose.z_roll;
		}

		bool camera_same(const CameraPose& a, const CameraPose& b)
		{
			return memcmp(a.words, b.words, sizeof(a.words)) == 0 && a.roll == b.roll && a.z_roll == b.z_roll;
		}

		// A cut (a new shot), not a pan: never sweep the camera through one.
		bool camera_is_cut(const CameraPose& a, const CameraPose& b)
		{
			for (int i = 0; i < 3; i++)
			{
				const int position_delta = (int)a.words[i] - (int)b.words[i];
				const int look_at_delta = (int)a.words[4 + i] - (int)b.words[4 + i];
				if (position_delta > CAMERA_CUT_UNITS || position_delta < -CAMERA_CUT_UNITS) return true;
				if (look_at_delta > CAMERA_CUT_UNITS || look_at_delta < -CAMERA_CUT_UNITS) return true;
			}
			if (abs(wrap_angle12(int(a.z_roll) - int(b.z_roll))) > 1024) return true; // v10: roll flip
			return false;
		}

		// v10: largest position / look-at change between two poses.
		int camera_largest_move(const CameraPose& a, const CameraPose& b)
		{
			int largest = 0;
			for (int i = 0; i < 3; i++)
			{
				largest = (std::max)(largest, abs((int)a.words[i] - (int)b.words[i]));
				largest = (std::max)(largest, abs((int)a.words[4 + i] - (int)b.words[4 + i]));
			}
			return largest;
		}

		int16_t lerp_int16(int16_t from, int16_t to, float t)
		{
			const float value = (float)from + ((float)to - (float)from) * t;
			return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
		}

		// Pure output: the authoritative pose is written back before the next root call.
		void camera_publish(float t)
		{
			CameraPose blended;
			for (int i = 0; i < CAMERA_POSE_WORDS; i++)
			{
				blended.words[i] = lerp_int16(camera_prev.words[i], camera_next.words[i], t);
			}
			blended.roll = lerp_int16(camera_prev.roll, camera_next.roll, t);
			blended.z_roll = (ff8_interp::cfg().battle_debug2 & DBG2_CAMERA_ROLL_OFF)
				? camera_next.z_roll
				: (int16_t)(camera_prev.z_roll + (int)(wrap_angle12(int(camera_next.z_roll) - int(camera_prev.z_roll)) * t + 0.5f));
			camera_write(blended);
		}

		// After the root: capture what the cinematic produced, adopt a cut it wrote on a held frame
		// (its cut writes are absolute and fire on held frames too), publish the blend. With
		// hold_pans (GfDesc::camera_hold_pans) a held-frame change below the cut threshold is the next
		// counter's recomputed pan, not a cut: it is left to the next tick's capture.
		void camera_after_root(bool is_logic_tick, bool hold_pans)
		{
			CameraPose current;
			camera_read(current);
			if (!camera_valid)
			{
				camera_prev = current;
				camera_next = current;
				camera_valid = true;
			}
			else if (is_logic_tick)
			{
				// v10: a VM set (op 1) that moved the pose is a cut at any distance (Odin's shots are often
				// closer together than CAMERA_CUT_UNITS and were swept through).
				const bool vm_cut = camera_vm_set
					&& !(ff8_interp::cfg().battle_debug2 & DBG2_GF_CAMERA_VM_CUT_OFF)
					&& camera_largest_move(current, camera_next) >= CAMERA_SET_MIN_UNITS;
				const bool cut = vm_cut || camera_is_cut(current, camera_next);
				if (vm_cut) stats.camera_vm_cuts++;
				if (ff8_interp::cfg().trace && cut)
					ff8_interp::log_trace("ff8gfcam cut f=%u vm=%d move=%d roll %d->%d\n",
						ff8_interp::frame_counter(), int(vm_cut), camera_largest_move(current, camera_next), camera_next.z_roll, current.z_roll);
				camera_prev = cut ? current : camera_next;
				camera_next = current;
			}
			else if (!camera_same(current, camera_next) && !(hold_pans && !camera_is_cut(current, camera_next)))
			{
				camera_prev = current;
				camera_next = current;
				stats.camera_adopts++;
			}
			if (!is_logic_tick) stats.camera_blends++;
			camera_publish(tick_alpha_now());
		}

		// --- model pose ---------------------------------------------------------------------------

		// Ask animations.cpp for this host frame's interpolated pose. Only on a skipped frame, and only
		// while the command is mid-animation: the frame 0 and the finished paths of read_animation_hook
		// call the real decoder, and decoding twice without 0x509440's reset would apply the frame's
		// deltas twice. In the mid-animation path with is_logic_tick_now() false the hook decodes nothing.
		// v10 (GfDesc::blend_model_translation): the model blocks' world translation. After a tick: capture (a jump
		// over MODEL_TRANSLATION_CUT is a reposition for a new shot - snap). Held frames: write the blend before
		// the root draws, put the tick's value back after it (the director read-modify-writes it).
		void translation_capture()
		{
			const uint32_t now = logic_tick_count();
			for (int i = 0; i < known_model_count; i++)
			{
				uint8_t* const model_block = known_models[i];
				const int32_t* translation = (const int32_t*)(model_block + MODEL_TRANSLATION);
				int slot_index = -1;
				for (int k = 0; k < translation_slots_used; k++)
				{
					if (translation_slots[k].model_block == model_block)
					{
						slot_index = k;
						break;
					}
				}
				if (slot_index < 0)
				{
					if (translation_slots_used >= (int)(sizeof(translation_slots) / sizeof(translation_slots[0]))) continue;
					slot_index = translation_slots_used++;
					translation_slots[slot_index].model_block = model_block;
					translation_slots[slot_index].tick = 0;
				}
				TranslationSlot& slot = translation_slots[slot_index];
				bool paired = slot.tick + 1 == now;
				for (int k = 0; k < 3 && paired; k++)
				{
					const int delta = translation[k] - slot.next[k];
					if (delta > MODEL_TRANSLATION_CUT || delta < -MODEL_TRANSLATION_CUT) paired = false;
				}
				for (int k = 0; k < 3; k++)
				{
					slot.prev[k] = paired ? slot.next[k] : translation[k];
					slot.next[k] = translation[k];
				}
				slot.tick = now;
			}
		}

		void translation_blend(float alpha)
		{
			const uint32_t now = logic_tick_count();
			for (int i = 0; i < translation_slots_used; i++)
			{
				TranslationSlot& slot = translation_slots[i];
				if (slot.tick != now) continue;
				int32_t* translation = (int32_t*)(slot.model_block + MODEL_TRANSLATION);
				for (int k = 0; k < 3; k++)
				{
					translation[k] = slot.prev[k]
						+ (int32_t)((float)(slot.next[k] - slot.prev[k]) * alpha + (slot.next[k] >= slot.prev[k] ? 0.5f : -0.5f));
				}
			}
		}

		void translation_restore()
		{
			const uint32_t now = logic_tick_count();
			for (int i = 0; i < translation_slots_used; i++)
			{
				if (translation_slots[i].tick == now)
					memcpy(translation_slots[i].model_block + MODEL_TRANSLATION, translation_slots[i].next, sizeof(translation_slots[i].next));
			}
		}

		// v10: camera VM op 1 read site.
		void __cdecl camera_vm_set_hook(void* a, void* b)
		{
			camera_vm_set = true;
			((void(__cdecl*)(void*, void*))FN_CAMERA_VM_READ_VECTOR)(a, b);
		}

		void model_publish_blend()
		{
			if (known_model_count == 0)
			{
				stats.model_skips++;
				return;
			}
			for (int i = 0; i < known_model_count; i++)
			{
				uint8_t* const model_block = known_models[i];
				const uint8_t* anim_cmd = model_block + MODEL_ANIM_CMD;
				const uint8_t frame = anim_cmd[CMD_CURRENT_FRAME];
				if (frame == 0 || frame >= anim_cmd[CMD_TOTAL_FRAMES])
				{
					stats.model_skips++;
					continue;
				}
				stats.model_requests++;
				((int(__cdecl*)(uint8_t*, uint8_t*))FN_READ_ANIMATION)(model_block + MODEL_ANIM_CTX, model_block + MODEL_ANIM_CMD);
			}
		}

		// --- particles ----------------------------------------------------------------------------

		int particle_runner_index(const GfDesc& gf_row, uint32_t runner_fn)
		{
			for (int i = 0; i < gf_row.particle_runner_count; i++)
			{
				if (gf_row.particle_runners[i].runner_fn == runner_fn) return i;
			}
			return -1;
		}

		// Shortest arc on the 12 bit angle circle, as in animations.cpp.
		int16_t lerp_angle12(int16_t from, int16_t to, float t)
		{
			const int delta = wrap_angle12(int(to) - int(from));
			const float scaled = delta * t;
			return (int16_t)(int(from) + (int)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f)));
		}

		void particle_capture_node(const GfDesc& gf_row, uint8_t* node, int runner_index, uint32_t now)
		{
			const RunnerDesc& runner = gf_row.particle_runners[runner_index];
			int slot_index = -1;
			for (int i = 0; i < particle_slots_used; i++)
			{
				if (particle_slots[i].node == node && particle_slots[i].runner_index == (uint8_t)runner_index)
				{
					slot_index = i;
					break;
				}
			}
			if (slot_index < 0)
			{
				if (particle_slots_used >= PARTICLE_SLOTS) return;
				slot_index = particle_slots_used++;
				ParticleSlot& fresh = particle_slots[slot_index];
				fresh.node = node;
				fresh.runner_index = (uint8_t)runner_index;
				fresh.has_next = false;
				fresh.paired = false;
				fresh.tick = 0;
				fresh.sequence = 0;
			}
			ParticleSlot& slot = particle_slots[slot_index];
			const int16_t sequence = runner.sequence_step != 0 ? *(const int16_t*)(node + runner.sequence_offset) : (int16_t)0;
			// Pair only two consecutive ticks of the same particle: the pools recycle their nodes.
			bool paired = slot.has_next && slot.tick + 1 == now
				&& (runner.sequence_step == 0 || sequence == (int16_t)(slot.sequence + runner.sequence_step));
			if (paired) memcpy(slot.prev, slot.next, sizeof(slot.prev));
			for (int k = 0; k < runner.field_count; k++)
			{
				slot.next[k] = *(const int16_t*)(node + runner.field_offsets[k]);
			}
			if (paired)
			{
				for (int k = 0; k < runner.field_count && paired; k++)
				{
					if (runner.angle_mask & (1 << k)) continue; // an angle always has a shortest arc
					const int delta = (int)slot.next[k] - (int)slot.prev[k];
					if (delta > runner.cut_distance || delta < -runner.cut_distance) paired = false; // teleport: snap, do not sweep
				}
			}
			slot.paired = paired;
			slot.has_next = true;
			slot.sequence = sequence;
			slot.tick = now;
		}

		// After a logic tick: snapshot every live particle of a paired runner, forget the rest.
		void particle_capture(const GfDesc& gf_row)
		{
			if (gf_row.particle_runner_count == 0) return;
			const uint32_t now = logic_tick_count();
			for (int queue_index = 0; queue_index < gf_row.particle_queue_count; queue_index++)
			{
				uint8_t* node = queue_head(gf_row.particle_queues[queue_index]);
				for (int guard = 0; node != nullptr && guard < PARTICLE_SLOTS * 4; guard++)
				{
					const int runner_index = particle_runner_index(gf_row, *(const uint32_t*)(node + TASK_RUNNER));
					if (runner_index >= 0) particle_capture_node(gf_row, node, runner_index, now);
					node = *(uint8_t**)(node + TASK_NEXT);
				}
			}
			int kept = 0;
			for (int i = 0; i < particle_slots_used; i++)
			{
				if (particle_slots[i].tick == now) particle_slots[kept++] = particle_slots[i];
			}
			particle_slots_used = kept;
		}

		bool particle_slot_live(const GfDesc& gf_row, const ParticleSlot& slot)
		{
			return slot.paired
				&& slot.runner_index < gf_row.particle_runner_count
				&& *(const uint32_t*)(slot.node + TASK_RUNNER) == gf_row.particle_runners[slot.runner_index].runner_fn;
		}

		void particle_blend(const GfDesc& gf_row, float t)
		{
			for (int i = 0; i < particle_slots_used; i++)
			{
				const ParticleSlot& slot = particle_slots[i];
				if (!particle_slot_live(gf_row, slot))
				{
					stats.particles_stepped++;
					continue;
				}
				stats.particles_blended++;
				const RunnerDesc& runner = gf_row.particle_runners[slot.runner_index];
				for (int k = 0; k < runner.field_count; k++)
				{
					*(int16_t*)(slot.node + runner.field_offsets[k]) = (runner.angle_mask & (1 << k))
						? lerp_angle12(slot.prev[k], slot.next[k], t)
						: lerp_int16(slot.prev[k], slot.next[k], t);
				}
			}
		}

		// Put the state the last logic tick produced back, so the engine only ever integrates from
		// its own values.
		void particle_restore(const GfDesc& gf_row)
		{
			for (int i = 0; i < particle_slots_used; i++)
			{
				const ParticleSlot& slot = particle_slots[i];
				if (!particle_slot_live(gf_row, slot)) continue;
				const RunnerDesc& runner = gf_row.particle_runners[slot.runner_index];
				for (int k = 0; k < runner.field_count; k++)
				{
					*(int16_t*)(slot.node + runner.field_offsets[k]) = slot.next[k];
				}
			}
		}

		// --- the generic call-site stub -----------------------------------------------------------

		const SiteDesc* site_lookup(uint32_t site)
		{
			for (int row_index = 0; row_index < GF_COUNT; row_index++)
			{
				const GfDesc& gf_row = GF_TABLE[row_index];
				for (int i = 0; i < gf_row.site_count; i++)
				{
					if (gf_row.sites[i].site == site) return &gf_row.sites[i];
				}
			}
			return nullptr;
		}

		// Every replaced site lands here. All targets are cdecl with at most three arguments, and the
		// caller cleans the stack, so forwarding eight dwords is exact for the ones the callee reads
		// (the same trick as the display-list linker hooks in animations.cpp). The site is recovered
		// from the return address: every patched site is a 5-byte E8 call (verified at install time).
		typedef uint32_t (__cdecl *forward8_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
		uint32_t __cdecl gf_site_stub(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
		{
			const uint32_t site = (uint32_t)(uintptr_t)_ReturnAddress() - 5;
			const SiteDesc* entry = site_lookup(site);
			if (entry == nullptr) return 0; // unreachable: only table sites are patched
			if (entry->kind == SITE_MODEL_STEP || entry->kind == SITE_MODEL_STEP_HELD)
			{
				// SITE_MODEL_STEP only runs on logic ticks (behind the hold); this is where the model block
				// becomes known. SITE_MODEL_STEP_HELD runs on held frames too: the block is learned, the
				// advance itself (void) only happens on the tick - held frames get model_publish_blend().
				learn_model_block((uint8_t*)(uintptr_t)a0);
				if (entry->kind == SITE_MODEL_STEP_HELD && suppressing())
				{
					stats.suppressed++;
					return 0;
				}
			}
			else if (suppressing())
			{
				stats.suppressed++;
				if (entry->kind == SITE_SPAWN_SCRATCH)
				{
					memset(spawn_scratch, 0, sizeof(spawn_scratch));
					return (uint32_t)(uintptr_t)spawn_scratch;
				}
				return 0; // SITE_SUPPRESS / SITE_SPAWN_NULL
			}
			// 0x506BA0 is forwarded to the ADDRESS, which carries animations.cpp's boost-settle hook.
			return ((forward8_t)entry->target)(a0, a1, a2, a3, a4, a5, a6, a7);
		}

		// --- the held call ------------------------------------------------------------------------

		// One skipped host frame of a root: the cinematic draws, nothing advances.
		int held_root(const GfDesc& gf_row, gf_runner_t original, uint8_t* node)
		{
			int16_t* const counter = (int16_t*)(node + TIMELINE_COUNTER);
			const int16_t held_counter = *counter;
			int result;
			suppress_depth++;
			if (gf_row.hold_kind == HOLD_PAUSE_DWORD)
			{
				uint32_t* const pause = (uint32_t*)gf_row.pause_dword;
				const uint32_t old_pause = *pause;
				*pause = 1;
				result = original(node);
				*pause = old_pause;
			}
			else
			{
				// Only bit 0 is ours: put back exactly its previous value and leave every other bit as
				// the call left it (v72 wrote the whole dword back).
				const uint32_t old_bit = *BATTLE_FREEZE & 1u;
				*BATTLE_FREEZE |= 1u;
				result = original(node);
				*BATTLE_FREEZE = (*BATTLE_FREEZE & ~1u) | old_bit;
			}
			suppress_depth--;
			// Bit 1 tells 0x508420 to unlink the task: leave the node exactly as its cleanup left it.
			if ((result & 2) != 0) return result;
			// HOLD_PAUSE_DWORD roots only step while the dword is 0 - the restore is belt and braces.
			// HOLD_FREEZE_BIT roots (Shiva) step their counter every call on purpose: it is only the
			// packet arena parity, and the double buffer must keep flipping.
			if (gf_row.hold_kind == HOLD_PAUSE_DWORD) *counter = held_counter;
			return result;
		}

		// --- 1/s trace ------------------------------------------------------------------------------

		void stats_after_root(const GfDesc& gf_row, bool is_logic_tick)
		{
			stats.frames++;
			if (!is_logic_tick) stats.held_frames++;
			const int32_t vram_ops = *VRAM_OP_COUNT;
			if (vram_ops > stats.vram_ops_max) stats.vram_ops_max = vram_ops;
			if (vram_ops > 0x1F && !warned_vram)
			{
				warned_vram = true;
				ff8_interp::log_warning("%s: VRAM op queue at %d entries during the %s cinematic (0x20 slots; the flush runs past the queue).\n", __func__, vram_ops, gf_row.name);
			}
			if (!ff8_interp::cfg().trace) return;
			const uint32_t now = GetTickCount();
			if (stats.stamp == 0)
			{
				stats.stamp = now;
				return;
			}
			if (now - stats.stamp < 1000) return;
			ff8_interp::log_trace("ff8gf %s 1s: frames=%u held=%u model=%u skip=%u parts blended=%u stepped=%u cam=%s blend=%u adopt=%u vmcut=%u suppressed=%u vramq_max=%d\n",
				gf_row.name, stats.frames, stats.held_frames, stats.model_requests, stats.model_skips, stats.particles_blended, stats.particles_stepped,
				gf_row.owns_camera ? "owned" : "battle", stats.camera_blends, stats.camera_adopts, stats.camera_vm_cuts, stats.suppressed, stats.vram_ops_max);
			const bool was_warned = warned_vram;
			stats = PaceStats();
			stats.stamp = now;
			warned_vram = was_warned;
		}

		// --- host-frame stall diagnostic (v8) -------------------------------------------------------
		// v79: Shiva "freezes shortly before the damage": the log shows ONE host frame (f=7647, the tick
		// that ran timeline body 241) taking 1.37 s - the thread was blocked, only 2 logic ticks passed,
		// and the same 1.36 s single-frame block appears at battle start (f=918, no summon). Pacing
		// cannot block inside a frame (a held tree is still called every frame), so this is a
		// synchronous load / upload in the engine or FFNx. To pin it, every root call measures the wall
		// time since the previous root call of the same cinematic; a gap > 250 ms across exactly one
		// host frame is logged once per cinematic with the row, the timeline counter and whether the
		// previous call was a tick. Runs with DBG_GF_PACE_OFF too (vanilla comparison: set 128 and see
		// whether the same counter blocks for the same time).
		// (state: see the declarations next to held_buffer)

		void stall_check(const GfDesc& gf_row, int16_t counter, bool is_logic_tick)
		{
			LARGE_INTEGER now, frequency;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&frequency);
			const uint32_t frame = ff8_interp::frame_counter();
			if (stall_prev_frame != NO_FRAME && frame - stall_prev_frame == 1 && !stall_warned && frequency.QuadPart != 0)
			{
				const double elapsed_ms = double(now.QuadPart - stall_prev_perf_count.QuadPart) * 1000.0 / double(frequency.QuadPart);
				if (elapsed_ms > 250.0)
				{
					stall_warned = true;
					ff8_interp::log_warning("gf_pacing: host frame %u blocked %.0f ms during the %s cinematic (previous root call: timeline frame %d, %s; paced %s).\n",
						stall_prev_frame, elapsed_ms, gf_row.name, (int)stall_prev_counter, stall_prev_was_tick ? "logic tick" : "held frame", pacing_off() ? "off" : "on");
				}
			}
			stall_prev_perf_count = now;
			stall_prev_frame = frame;
			stall_prev_counter = counter;
			stall_prev_was_tick = is_logic_tick;
		}

		// --- leftover-state diagnostic (v9) ---------------------------------------------------------
		// v83: effects drawn after Shiva look colour-inverted. The module's own writes outside its data
		// (scan of [0x5C0D50, 0x5C7FC0) + the camera VM 0x63E960..0x63F2D0) are: the camera pose / roll /
		// 0x1D977A2, the packet cursor 0x1D8E054, the screen-flash block (0x5713E0: word at 0x1D98992 +
		// k*0x2C, dword at 0x1D989B8 + k*0x2C, k < 4 - reset by the timeline's own (0,0) on its last
		// tick), a fade task 0x571300 in 0x209FAA8, VRAM (uploads / moves, restored by 0x5C4A90's 0xA3
		// reload) and the GTE CONTROL registers 0x1CA927C[32] (far colour 21..23 via 0x45DD60, back
		// colour 13..15 via 0x64DDA0, light matrix 8..12 via 0x45DE50, rotation 0..4 via 0x64DDD0 /
		// 0x64DE00, translation 5..7) - set every call by the tasks 0x5C62F0 / 0x5C6BB0 / 0x5C71B0 /
		// 0x5C7CA0 and never restored (in vanilla either). None of these writes is on a path the hold
		// skips, and the hold changes neither their values nor their order, so this pass found no
		// pacing-specific leftover. To settle it with data instead of reading: the GTE control block,
		// the flash block and 0x1D977A2 are captured when a cinematic starts and compared when it ends;
		// every difference is logged (one line per cinematic, also with DBG_GF_PACE_OFF = 128 for a
		// vanilla-rate comparison).
		constexpr uint32_t GTE_CONTROL = 0x1CA927C;    // 32 control registers (CTC2 image, 0x45D7F0)
		constexpr int GTE_CONTROL_COUNT = 32;
		struct LeftoverState
		{
			uint32_t gte_control[GTE_CONTROL_COUNT];
			uint16_t flash_words[4];
			uint32_t flash_dwords[4];
			int16_t z_roll;                             // 0x1D977A2
		};
		LeftoverState leftover_at_start = {};
		int leftover_row = -1;
		uint32_t leftover_frame = NO_FRAME;

		void leftover_read(LeftoverState& state)
		{
			memcpy(state.gte_control, (const void*)GTE_CONTROL, sizeof(state.gte_control));
			for (int k = 0; k < 4; k++)
			{
				state.flash_words[k] = *(const uint16_t*)(0x1D98992 + k * 0x2C);
				state.flash_dwords[k] = *(const uint32_t*)(0x1D989B8 + k * 0x2C);
			}
			state.z_roll = *(const int16_t*)0x1D977A2;
		}

		// Every root call: a new cinematic (row change or a gap of more than one host frame) captures.
		void leftover_on_call(int row_index)
		{
			const uint32_t frame = ff8_interp::frame_counter();
			if (row_index != leftover_row || leftover_frame == NO_FRAME || frame - leftover_frame > 1)
				leftover_read(leftover_at_start);
			leftover_row = row_index;
			leftover_frame = frame;
		}

		void leftover_on_end(const GfDesc& gf_row)
		{
			LeftoverState now;
			leftover_read(now);
			char line[768];
			int length = snprintf(line, sizeof(line), "gf_pacing: %s left behind (paced %s):", gf_row.name, pacing_off() ? "off" : "on");
			bool found_any = false;
			for (int i = 0; i < GTE_CONTROL_COUNT && length < (int)sizeof(line) - 40; i++)
			{
				if (now.gte_control[i] != leftover_at_start.gte_control[i])
				{
					found_any = true;
					length += snprintf(line + length, sizeof(line) - length, " gte%d %08X->%08X", i, leftover_at_start.gte_control[i], now.gte_control[i]);
				}
			}
			for (int k = 0; k < 4 && length < (int)sizeof(line) - 40; k++)
			{
				if (now.flash_words[k] != leftover_at_start.flash_words[k] || now.flash_dwords[k] != leftover_at_start.flash_dwords[k])
				{
					found_any = true;
					length += snprintf(line + length, sizeof(line) - length, " flash%d %04X/%08X->%04X/%08X",
						k, leftover_at_start.flash_words[k], leftover_at_start.flash_dwords[k], now.flash_words[k], now.flash_dwords[k]);
				}
			}
			if (now.z_roll != leftover_at_start.z_roll && length < (int)sizeof(line) - 40)
			{
				found_any = true;
				length += snprintf(line + length, sizeof(line) - length, " 1D977A2 %d->%d", leftover_at_start.z_roll, now.z_roll);
			}
			if (!found_any) snprintf(line + length, sizeof(line) - length, " nothing");
			ff8_interp::log_info("%s\n", line);
			leftover_row = -1;
			leftover_frame = NO_FRAME;
		}

		// --- the root hook (every row) ------------------------------------------------------------

		int __cdecl gf_root_hook(uint8_t* node)
		{
			const int row_index = node != nullptr ? row_by_root(*(const uint32_t*)(node + TASK_RUNNER)) : -1;
			if (row_index < 0 || original_root[row_index] == nullptr) return 0; // unreachable: only table roots are hooked
			const GfDesc& gf_row = GF_TABLE[row_index];
			gf_runner_t original = original_root[row_index];
			if (in_battle())
			{
				tick_update();
				const uint8_t* stall_timeline = timeline_node(gf_row);
				stall_check(gf_row,
					stall_timeline != nullptr ? *(const int16_t*)(stall_timeline + TIMELINE_COUNTER) : (int16_t)-1,
					is_logic_tick_now());
				leftover_on_call(row_index);
			}
			if (!in_battle() || pacing_off())
			{
				const int unpaced_result = original(node);
				if (in_battle() && (unpaced_result & 2) != 0) leftover_on_end(gf_row);
				return unpaced_result;
			}
			tick_update();
			if (row_index != active_row)
			{
				cinematic_forget();
				active_row = row_index;
			}
			// v7: the root was not called on the previous host frame although the same cinematic is still
			// running - the battle director did not run (battle pause: battle_cardgame_main_loop skips
			// 0x47CCB0 while g_battlePaused 0x1D28DE9 is set, so 0x500900 and every effect tree stop while
			// the frame counter and the HUD go on). Wall time spent there is not a stall of the cinematic:
			// re-stamp the watchdogs so a pause longer than GF_WATCHDOG_MS does not release the pacing
			// (v78 log: Quezacotl "stuck at frame 253" after a 3.6 s pause, then unpaced to its end).
			if (last_root_frame != NO_FRAME && ff8_interp::frame_counter() - last_root_frame > 1)
			{
				const uint32_t now = GetTickCount();
				root_watch.stamp = now;
				inner_watch.stamp = now;
			}
			// From here until one frame after the cinematic ends the cinematic is live (boost gate) and,
			// for a camera-owning row, battle_camera_hook stands down.
			last_root_frame = ff8_interp::frame_counter();

			const uint8_t* timeline = timeline_node(gf_row);
			if (timeline != nullptr)
			{
				const int16_t counter = *(const int16_t*)(timeline + TIMELINE_COUNTER);
				if (ff8_interp::cfg().trace && counter != traced_counter)
				{
					traced_counter = counter;
					ff8_interp::log_trace("ff8gf %s frame %d/%d ticks=%u\n", gf_row.name, (int)counter, (int)gf_row.timeline_last, logic_tick_count());
				}
				if (watchdog_stuck(root_watch, counter, gf_row.name))
				{
					camera_valid = false; // unpaced: the cinematic writes its own camera every frame again
					return original(node);
				}
			}

			const bool is_logic_tick = is_logic_tick_now();
			// Undo the camera blend published last frame: the timeline read-modify-writes the pose.
			if (gf_row.owns_camera && camera_valid) camera_write(camera_next);
			camera_vm_set = false;
			const bool blend_translation = gf_row.blend_model_translation && !(ff8_interp::cfg().battle_debug2 & DBG2_GF_MODEL_TRANSLATION_OFF);
			// Motion between two logic ticks, in place BEFORE the root draws.
			if (!is_logic_tick)
			{
				model_publish_blend();
				particle_blend(gf_row, tick_alpha_now());
				if (blend_translation) translation_blend(tick_alpha_now());
			}
			int result;
			if (is_logic_tick)
			{
				tick_model_count = 0;
				collecting_models = true;
				result = original(node);
				collecting_models = false;
				if (tick_model_count > 0)
				{
					memcpy(known_models, tick_models, sizeof(known_models[0]) * (size_t)tick_model_count);
					known_model_count = tick_model_count;
				}
			}
			else
			{
				result = held_root(gf_row, original, node);
			}
			if (is_logic_tick)
				particle_capture(gf_row);
			else
				particle_restore(gf_row);
			if (blend_translation)
			{
				if (is_logic_tick)
					translation_capture();
				else
					translation_restore();
			}
			if (gf_row.owns_camera) camera_after_root(is_logic_tick, gf_row.camera_hold_pans);
			stats_after_root(gf_row, is_logic_tick);
			if ((result & 2) != 0) // cinematic over; battle_camera_hook re-captures
			{
				leftover_on_end(gf_row);
				cinematic_forget();
			}
			return result;
		}

		// --- held regions (v6) ------------------------------------------------------------------------

		uint8_t* held_region_element(const HeldRegion& region, uint8_t* node, int element)
		{
			const uint32_t base = (region.flags & REGION_NODE_RELATIVE) ? (uint32_t)(uintptr_t)node + region.address : region.address;
			return (uint8_t*)(uintptr_t)(base + (uint32_t)element * region.stride);
		}

		size_t held_regions_bytes(const GfDesc& gf_row)
		{
			size_t total = 0;
			for (int i = 0; i < gf_row.held_region_count; i++)
			{
				total += (size_t)gf_row.held_regions[i].element_size * gf_row.held_regions[i].count;
			}
			return total;
		}

		// Save every region, then zero the REGION_ZEROED ones for the held call.
		void held_regions_save(const GfDesc& gf_row, uint8_t* node)
		{
			size_t offset = 0;
			for (int i = 0; i < gf_row.held_region_count; i++)
			{
				const HeldRegion& region = gf_row.held_regions[i];
				for (int k = 0; k < region.count; k++)
				{
					uint8_t* element = held_region_element(region, node, k);
					memcpy(held_buffer + offset, element, region.element_size);
					if (region.flags & REGION_ZEROED) memset(element, 0, region.element_size);
					offset += region.element_size;
				}
			}
		}

		// REGION_ZEROED regions always come back (they are inputs the tick still needs); the others only when
		// the call did not end the cinematic (its end-of-cinematic cleanup must stand).
		void held_regions_restore(const GfDesc& gf_row, uint8_t* node, bool ended)
		{
			size_t offset = 0;
			for (int i = 0; i < gf_row.held_region_count; i++)
			{
				const HeldRegion& region = gf_row.held_regions[i];
				for (int k = 0; k < region.count; k++)
				{
					if (!ended || (region.flags & REGION_ZEROED))
						memcpy(held_region_element(region, node, k), held_buffer + offset, region.element_size);
					offset += region.element_size;
				}
			}
		}

		// --- the inner hook (HOLD_PAUSE_DWORD rows whose timeline step ignores the pause) ----------

		int __cdecl gf_inner_hook(uint8_t* node)
		{
			const int row_index = node != nullptr ? row_by_inner(*(const uint32_t*)(node + TASK_RUNNER)) : -1;
			if (row_index < 0 || original_inner[row_index] == nullptr) return 0;
			gf_runner_t original = original_inner[row_index];
			if (!in_battle() || pacing_off()) return original(node);
			tick_update();
			int16_t* const counter = (int16_t*)(node + TIMELINE_COUNTER);
			if (watchdog_stuck(inner_watch, *counter, "timeline")) return original(node);
			if (is_logic_tick_now()) return original(node);
			// The root has already raised the pause dword and the suppression depth for this call.
			const int16_t held_counter = *counter;
			const GfDesc& gf_row = GF_TABLE[row_index];
			const bool restore_regions = gf_row.held_region_count != 0 && suppressing();
			if (restore_regions) held_regions_save(gf_row, node);
			const int result = original(node);
			if (restore_regions) held_regions_restore(gf_row, node, (result & 2) != 0);
			if ((result & 2) != 0) return result; // last frame: its cleanup ran, leave the node alone
			*counter = held_counter;
			return result;
		}

		// --- rumble (v7) -----------------------------------------------------------------------------
		// Vibration_UpdateObjects 0x4A2A90 steps the four vibration objects 0x1D6BBB8 (0x4A2B80 per
		// motor stream: one pattern entry per call) and hands the max levels to vibration_apply
		// 0x49F1A0, which FFNx replaces (src/ff8/vibration.cpp apply_vibrate_calc: it only STORES the
		// motor speeds, it steps nothing). Its only caller is the frame-flip HUD link 0x4A9220 (site
		// 0x4A92D6, via 0x50062D), i.e. once per RENDERED frame: 15 steps a second natively, 144 at
		// 144 Hz - every battle rumble pattern (summons, limit breaks, hits) ran ~10x fast and ended
		// after a tenth of its length. The step now runs on logic ticks only; between ticks the motors
		// keep the level of the last step (FFNx keeps the speeds until the next apply). The pause branch
		// next to it (0x1D6D624 -> 0x49F1A0(0,0,0)) is untouched. Start 0x4A29A0 / stop 0x4A2940 calls of
		// every paced summon fire once: the starts sit in each root's one-shot spawn block (or behind the
		// freeze test), the unguarded stops are SITE_SUPPRESS rows (fire on the tick). Bit 128 restores
		// host-rate stepping (no free debug bit left).
		constexpr uint32_t CALL_SITE_RUMBLE_STEP = 0x4A92D6;
		constexpr uint32_t FN_RUMBLE_STEP = 0x4A2A90;
		bool rumble_hooked = false;
		bool camera_vm_hooked = false;

		void __cdecl rumble_step_stub()
		{
			if (in_battle() && !pacing_off())
			{
				tick_update();
				if (!is_logic_tick_now()) return;
			}
			((void(__cdecl*)())FN_RUMBLE_STEP)();
		}

		// --- installation -------------------------------------------------------------------------

		bool patch_site(const SiteDesc& entry)
		{
			const uint8_t* const code = (const uint8_t*)entry.site;
			const uint32_t actual_target = entry.site + 5 + (uint32_t)(*(const int32_t*)(entry.site + 1));
			if (code[0] != 0xE8 || actual_target != entry.target)
			{
				ff8_interp::log_warning("gf_pacing_hook_init: call site %08X does not call %08X (%02X -> %08X), left alone.\n",
					entry.site, entry.target, code[0], actual_target);
				return false;
			}
			ff8_interp::hook_call(entry.site, (void*)gf_site_stub);
			return true;
		}
	}

	// See the marked block in internal.h: true while a camera-owning GF cinematic runs, so
	// battle_camera_hook must not restore, capture or blend the pose. One frame of grace, because the
	// camera hook runs later in the same host frame (0x500988) than the effect tick (0x50093A).
	bool gf_cinematic_owns_camera()
	{
		return (gf_live_now() && active_row >= 0 && GF_TABLE[active_row].owns_camera) || gf_vm_owns_camera(); // v75: + gf_vm.cpp
	}

	// True while ANY paced GF cinematic runs (same grace), for apply_results_hook's boost gate.
	bool gf_cinematic_live()
	{
		return gf_live_now() || gf_vm_live() || gf_siren_live(); // v75: + gf_vm.cpp, gf_siren.cpp
	}

	void gf_pacing_hook_init()
	{
		int rows_hooked = 0;
		int sites_patched = 0;
		int sites_total = 0;
		for (int row_index = 0; row_index < GF_COUNT; row_index++)
		{
			const GfDesc& gf_row = GF_TABLE[row_index];
			original_root[row_index] = (gf_runner_t)make_trampoline(gf_row.root_fn, gf_row.root_prologue, gf_row.root_prologue_len);
			if (original_root[row_index] == nullptr)
			{
				ff8_interp::log_warning("%s: %s root task %08X prologue mismatch, that summon stays at host rate.\n", __func__, gf_row.name, gf_row.root_fn);
				continue;
			}
			if (held_regions_bytes(gf_row) > HELD_BUFFER_BYTES || (gf_row.held_region_count != 0 && gf_row.inner_fn == 0))
			{
				ff8_interp::log_warning("%s: %s held regions do not fit (%u bytes) or have no inner hook, that summon stays at host rate.\n", __func__, gf_row.name, (unsigned)held_regions_bytes(gf_row));
				continue;
			}
			if (gf_row.inner_fn != 0)
			{
				original_inner[row_index] = (gf_runner_t)make_trampoline(gf_row.inner_fn, gf_row.inner_prologue, gf_row.inner_prologue_len);
				if (original_inner[row_index] == nullptr)
				{
					// Without the counter restore the timeline would run at host rate under a held root.
					ff8_interp::log_warning("%s: %s timeline %08X prologue mismatch, that summon stays at host rate.\n", __func__, gf_row.name, gf_row.inner_fn);
					continue;
				}
			}
			ff8_interp::hook_function(gf_row.root_fn, (void*)gf_root_hook);
			if (original_inner[row_index] != nullptr) ff8_interp::hook_function(gf_row.inner_fn, (void*)gf_inner_hook);
			rows_hooked++;
			for (int i = 0; i < gf_row.site_count && i < MAX_ROW_SITES; i++)
			{
				sites_total++;
				site_patched[row_index][i] = patch_site(gf_row.sites[i]);
				if (site_patched[row_index][i]) sites_patched++;
			}
		}
		if (!camera_vm_hooked)
		{
			const uint8_t* const code = (const uint8_t*)CALL_SITE_CAMERA_VM_SET;
			if (code[0] == 0xE8 && CALL_SITE_CAMERA_VM_SET + 5 + (uint32_t)(*(const int32_t*)(CALL_SITE_CAMERA_VM_SET + 1)) == FN_CAMERA_VM_READ_VECTOR)
			{
				ff8_interp::hook_call(CALL_SITE_CAMERA_VM_SET, (void*)camera_vm_set_hook);
				camera_vm_hooked = true;
			}
			else ff8_interp::log_warning("%s: camera VM set site %08X does not call %08X, GF camera cuts by distance only.\n", __func__, CALL_SITE_CAMERA_VM_SET, FN_CAMERA_VM_READ_VECTOR);
		}
		if (!rumble_hooked)
		{
			const uint8_t* const code = (const uint8_t*)CALL_SITE_RUMBLE_STEP;
			if (code[0] == 0xE8 && CALL_SITE_RUMBLE_STEP + 5 + (uint32_t)(*(const int32_t*)(CALL_SITE_RUMBLE_STEP + 1)) == FN_RUMBLE_STEP)
			{
				ff8_interp::hook_call(CALL_SITE_RUMBLE_STEP, (void*)rumble_step_stub);
				rumble_hooked = true;
			}
			else ff8_interp::log_warning("%s: rumble step site %08X does not call %08X, battle rumble stays at host rate.\n", __func__, CALL_SITE_RUMBLE_STEP, FN_RUMBLE_STEP);
		}
		ff8_interp::log_info("%s: gf pacing v11 enabled (%d/%d GFs, %d/%d call sites replaced, rumble %s).\n", __func__, rows_hooked, GF_COUNT, sites_patched, sites_total, rumble_hooked ? "paced" : "host rate");
	}

	void gf_pacing_reset()
	{
		suppress_depth = 0;
		warned_watchdog = false;
		warned_vram = false;
		stats = PaceStats();
		active_row = -1;
		last_root_frame = NO_FRAME;
		cinematic_forget();
	}
}
