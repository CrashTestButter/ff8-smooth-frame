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
// DBG_GF_PACE_OFF. Full write-up: notes/engine/battle-magic.md, "GF cinematic pacing (v60)" and
// the v63 / v65 / v72 subsections below it.
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
//       HOLD_PAUSE_DWORD - Quezacotl: a module-private pause dword (0x25216DC) that every runner of
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
//     once explicitly (read_animation_hook decodes nothing when tick_is_real() is false).
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

namespace ff8_battle_anim
{
	namespace
	{
		// --- shared engine addresses ------------------------------------------------------------

		constexpr uint32_t TIMELINE_COUNTER = 0x0C;       // int16 frame counter of every task node

		constexpr uint32_t FN_SPAWN_TASK    = 0x508360;   // BdLinkTask
		constexpr uint32_t FN_PLAY_SE       = 0x501330;   // BdPlaySE -> 0x46B2A0
		constexpr uint32_t FN_PLAY_SE_CUT   = 0x5018C0;   // camera-cut SE -> 0x46B2A0
		constexpr uint32_t FN_STOP_SE       = 0x4A2940;   // stops an SE handle
		constexpr uint32_t FN_APPLY_RESULTS = 0x506BA0;   // Battle_ApplyActionResultToAllTargets (v58 hook lives on it)
		constexpr uint32_t FN_STREAM_REQ    = 0x5341D0;   // appends an asset stream request
		// 0x46B2A0 is never called directly from a GF module; a global hook would hold every sound.

		constexpr uint32_t FN_READ_ANIMATION = 0x508F90;  // hooked by animations.cpp - call the address, never a trampoline
		constexpr uint32_t MODEL_CTX = 0x60;              // AnimCtx inside a model block
		constexpr uint32_t MODEL_CMD = 0x6C;              // AnimCmd inside a model block
		constexpr uint32_t CMD_CURRENT_FRAME = 6;         // same layout constants animations.cpp uses
		constexpr uint32_t CMD_TOTAL_FRAMES = 7;

		int16_t* const CAM_POSE = (int16_t*)0xB8B7F0;     // 8 int16: position xyz + pad, look-at xyz + pad
		int16_t* const CAM_ROLL = (int16_t*)0x1D8E038;
		constexpr int CAM_WORDS = 8;
		constexpr int CAM_CUT_UNITS = 2048;               // same cut threshold battle_camera_hook uses

		// --- call-site table ----------------------------------------------------------------------

		enum SiteKind : uint8_t
		{
			SITE_SUPPRESS,      // void (or ignored) result: skipped while held
			SITE_SPAWN_NULL,    // BdLinkTask whose site null-checks the node: returns nullptr while held
			SITE_SPAWN_SCRATCH, // BdLinkTask whose site does NOT null-check: returns a private, never linked node
			SITE_MODEL_STEP,    // the creature's per-tick advance: always forwarded, arg 0 is the model block
		};

		struct SiteDesc
		{
			uint32_t site;      // address of the E8 call
			uint32_t target;    // what it calls (verified at install time)
			uint8_t kind;
		};

		// --- particle runners -------------------------------------------------------------------

		constexpr int PART_FIELDS = 6;
		struct RunnerDesc
		{
			uint32_t fn;       // runner address as stored at node + 8
			uint8_t seq_off;   // int16 that must step by seq_step for two captures to pair (0 = skip)
			int16_t seq_step;
			uint8_t n;         // interpolated int16 fields
			uint8_t off[PART_FIELDS];
			uint8_t ang;       // bit k: field k is a 12 bit angle, blend on the shortest arc
			int16_t cut;       // a linear field moving further than this in one tick is a teleport
		};

		// --- the per-GF table -------------------------------------------------------------------

		enum HoldKind : uint8_t { HOLD_PAUSE_DWORD, HOLD_FREEZE_BIT };

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
			uint8_t hold;
			uint32_t pause_dword;             // HOLD_PAUSE_DWORD only
			uint32_t timeline_queue;          // queue holding the timeline node (watchdog + trace)
			uint32_t timeline_fn;             // the timeline runner (its node's +0x0C is the frame counter)
			int16_t timeline_last;            // last frame, for the trace only
			bool owns_camera;                 // writes 0xB8B7F0 / 0x1D8E038 itself
			const SiteDesc* sites;
			uint8_t n_sites;
			const uint32_t* part_queues;
			uint8_t n_part_queues;
			const RunnerDesc* part_runners;
			uint8_t n_part_runners;
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
		};
		constexpr int GF_COUNT = (int)(sizeof(GF_TABLE) / sizeof(GF_TABLE[0]));

		// --- state ------------------------------------------------------------------------------

		typedef int (__cdecl *gf_runner_t)(uint8_t*);
		gf_runner_t orig_root[GF_COUNT] = {};
		gf_runner_t orig_inner[GF_COUNT] = {};
		bool site_ok[GF_COUNT][64] = {};  // per row, per site: patched

		int active_gf = -1;                   // the row whose root ran last (one summon at a time)
		uint32_t gf_seen_frame = 0xFFFFFFFF;  // last host frame in which a root ran
		int suppress_depth = 0;               // > 0 while a held (skipped-frame) root call is on the stack
		bool warned_watchdog = false;
		int16_t traced_counter = -1;

		uint8_t* gf_model = nullptr;          // the creature's model block, learned from the model-step site

		uint8_t spawn_scratch[0x200];         // handed to SITE_SPAWN_SCRATCH callers while held; never linked

		struct CamPose
		{
			int16_t v[CAM_WORDS];
			int16_t roll;
		};
		CamPose cam_prev = { { 0, 0, 0, 0, 0, 0, 0, 0 }, 0 };
		CamPose cam_next = { { 0, 0, 0, 0, 0, 0, 0, 0 }, 0 };
		bool cam_valid = false;

		constexpr int PART_SLOTS = 192;
		struct PartSlot
		{
			uint8_t* node;
			uint8_t desc;
			bool has_next;   // one capture taken
			bool paired;     // prev and next are two consecutive ticks of the SAME particle
			uint32_t tick;   // logic_tick_count() of the capture in `next`
			int16_t seq;
			int16_t prev[PART_FIELDS];
			int16_t next[PART_FIELDS];
		};
		PartSlot part_slots[PART_SLOTS];
		int part_used = 0;

		// Watchdog: a cinematic that stops advancing must never be held. Re-arming.
		constexpr uint32_t GF_WATCHDOG_MS = 2000;
		struct Advance
		{
			bool valid;
			bool stuck;
			int16_t counter;
			uint32_t stamp;
		};
		Advance root_adv = { false, false, 0, 0 };
		Advance inner_adv = { false, false, 0, 0 };

		// v73: once-a-second summary while a cinematic runs (trace_battle_animation only).
		int32_t* const VRAM_OP_COUNT = (int32_t*)0x1D98420; // g_battleVramOpCount, flushed at flip
		struct PaceStats
		{
			uint32_t stamp;       // GetTickCount() of the last summary
			uint32_t frames, held;
			uint32_t model_req, model_skip;     // pose requests issued / skipped (no model, frame 0, finished)
			uint32_t part_blend, part_step;     // node blends written / tracked nodes left stepped
			uint32_t cam_blend, cam_adopt;      // camera blends published / cuts adopted on held frames
			uint32_t suppressed;                // one-shot calls swallowed while held
			int32_t vram_max;                   // highest VRAM op count seen after a root call
		};
		PaceStats stats = {};
		bool warned_vram = false;

		// --- small helpers ------------------------------------------------------------------------

		bool pacing_off()
		{
			return (ff8_interp::cfg().battle_debug & DBG_GF_PACE_OFF) != 0;
		}

		bool gf_live_now()
		{
			return !pacing_off() && gf_seen_frame != 0xFFFFFFFF && (ff8_interp::frame_counter() - gf_seen_frame) <= 1;
		}

		// True only inside a held root call, i.e. exactly while the suppressed sites must be quiet.
		bool suppressing()
		{
			return suppress_depth > 0 && in_battle();
		}

		int row_by_root(uint32_t fn)
		{
			for (int g = 0; g < GF_COUNT; g++) if (GF_TABLE[g].root_fn == fn) return g;
			return -1;
		}

		int row_by_inner(uint32_t fn)
		{
			for (int g = 0; g < GF_COUNT; g++) if (GF_TABLE[g].inner_fn != 0 && GF_TABLE[g].inner_fn == fn) return g;
			return -1;
		}

		// Forget everything that belongs to one cinematic.
		void cinematic_forget()
		{
			cam_valid = false;
			part_used = 0;
			gf_model = nullptr;
			root_adv.valid = inner_adv.valid = false;
			root_adv.stuck = inner_adv.stuck = false;
			traced_counter = -1;
		}

		// Returns true while this counter has not moved for more than GF_WATCHDOG_MS: the hold is
		// dropped, the engine runs unpaced (vanilla behaviour) and the counter can move again, which
		// re-arms the hold. Warns once per animations_reset().
		bool watchdog_stuck(Advance& a, int16_t counter, const char* which)
		{
			const uint32_t now = GetTickCount();
			if (!a.valid || a.counter != counter)
			{
				a.valid = true;
				a.stuck = false;
				a.counter = counter;
				a.stamp = now;
				return false;
			}
			if (now - a.stamp <= GF_WATCHDOG_MS) return a.stuck;
			if (!a.stuck)
			{
				a.stuck = true;
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
		uint8_t* timeline_node(const GfDesc& gd)
		{
			if (gd.timeline_queue == 0) return nullptr;
			uint8_t* n = *(uint8_t**)gd.timeline_queue;
			for (int guard = 0; n != nullptr && guard < 256; guard++)
			{
				if (*(const uint32_t*)(n + 8) == gd.timeline_fn) return n;
				n = *(uint8_t**)(n + 4);
			}
			return nullptr;
		}

		// --- camera -----------------------------------------------------------------------------

		void cam_read(CamPose& p)
		{
			memcpy(p.v, CAM_POSE, sizeof(p.v));
			p.roll = *CAM_ROLL;
		}

		void cam_write(const CamPose& p)
		{
			memcpy(CAM_POSE, p.v, sizeof(p.v));
			*CAM_ROLL = p.roll;
		}

		bool cam_same(const CamPose& a, const CamPose& b)
		{
			return memcmp(a.v, b.v, sizeof(a.v)) == 0 && a.roll == b.roll;
		}

		// A cut (a new shot), not a pan: never sweep the camera through one.
		bool cam_is_cut(const CamPose& a, const CamPose& b)
		{
			for (int i = 0; i < 3; i++)
			{
				const int dp = (int)a.v[i] - (int)b.v[i];
				const int dl = (int)a.v[4 + i] - (int)b.v[4 + i];
				if (dp > CAM_CUT_UNITS || dp < -CAM_CUT_UNITS) return true;
				if (dl > CAM_CUT_UNITS || dl < -CAM_CUT_UNITS) return true;
			}
			return false;
		}

		int16_t cam_lerp(int16_t a, int16_t b, float t)
		{
			const float v = (float)a + ((float)b - (float)a) * t;
			return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
		}

		// Pure output: the authoritative pose is written back before the next root call.
		void cam_publish(float t)
		{
			CamPose out;
			for (int i = 0; i < CAM_WORDS; i++) out.v[i] = cam_lerp(cam_prev.v[i], cam_next.v[i], t);
			out.roll = cam_lerp(cam_prev.roll, cam_next.roll, t);
			cam_write(out);
		}

		// After the root: capture what the cinematic produced, adopt a cut it wrote on a held frame
		// (its cut writes are absolute and fire on held frames too), publish the blend.
		void cam_after_root(bool real)
		{
			CamPose now;
			cam_read(now);
			if (!cam_valid)
			{
				cam_prev = now;
				cam_next = now;
				cam_valid = true;
			}
			else if (real)
			{
				const bool cut = cam_is_cut(now, cam_next);
				cam_prev = cut ? now : cam_next;
				cam_next = now;
			}
			else if (!cam_same(now, cam_next))
			{
				cam_prev = now;
				cam_next = now;
				stats.cam_adopt++;
			}
			if (!real) stats.cam_blend++;
			cam_publish(tick_alpha_now());
		}

		// --- model pose ---------------------------------------------------------------------------

		// Ask animations.cpp for this host frame's interpolated pose. Only on a skipped frame, and only
		// while the command is mid-animation: the frame 0 and the finished paths of read_animation_hook
		// call the real decoder, and decoding twice without 0x509440's reset would apply the frame's
		// deltas twice. In the mid-animation path with tick_is_real() false the hook decodes nothing.
		void model_publish_blend()
		{
			if (gf_model == nullptr) { stats.model_skip++; return; }
			const uint8_t* cmd = gf_model + MODEL_CMD;
			const uint8_t frame = cmd[CMD_CURRENT_FRAME];
			if (frame == 0 || frame >= cmd[CMD_TOTAL_FRAMES]) { stats.model_skip++; return; }
			stats.model_req++;
			((int(__cdecl*)(uint8_t*, uint8_t*))FN_READ_ANIMATION)(gf_model + MODEL_CTX, gf_model + MODEL_CMD);
		}

		// --- particles ----------------------------------------------------------------------------

		int part_desc_of(const GfDesc& gd, uint32_t fn)
		{
			for (int i = 0; i < gd.n_part_runners; i++)
				if (gd.part_runners[i].fn == fn) return i;
			return -1;
		}

		// Shortest arc on the 12 bit angle circle, as in animations.cpp.
		int16_t part_lerp_angle(int16_t a, int16_t b, float t)
		{
			const int d = ((int(b) - int(a) + 2048) & 4095) - 2048;
			const float v = d * t;
			return (int16_t)(int(a) + (int)(v + (v >= 0.0f ? 0.5f : -0.5f)));
		}

		void part_capture_node(const GfDesc& gd, uint8_t* node, int d, uint32_t now)
		{
			const RunnerDesc& rd = gd.part_runners[d];
			int idx = -1;
			for (int i = 0; i < part_used; i++)
				if (part_slots[i].node == node && part_slots[i].desc == (uint8_t)d) { idx = i; break; }
			if (idx < 0)
			{
				if (part_used >= PART_SLOTS) return;
				idx = part_used++;
				PartSlot& fresh = part_slots[idx];
				fresh.node = node;
				fresh.desc = (uint8_t)d;
				fresh.has_next = false;
				fresh.paired = false;
				fresh.tick = 0;
				fresh.seq = 0;
			}
			PartSlot& s = part_slots[idx];
			const int16_t seq = rd.seq_step != 0 ? *(const int16_t*)(node + rd.seq_off) : (int16_t)0;
			// Pair only two consecutive ticks of the same particle: the pools recycle their nodes.
			bool pair = s.has_next && s.tick + 1 == now
				&& (rd.seq_step == 0 || seq == (int16_t)(s.seq + rd.seq_step));
			if (pair) memcpy(s.prev, s.next, sizeof(s.prev));
			for (int k = 0; k < rd.n; k++) s.next[k] = *(const int16_t*)(node + rd.off[k]);
			if (pair)
				for (int k = 0; k < rd.n && pair; k++)
				{
					if (rd.ang & (1 << k)) continue; // an angle always has a shortest arc
					const int delta = (int)s.next[k] - (int)s.prev[k];
					if (delta > rd.cut || delta < -rd.cut) pair = false; // teleport: snap, do not sweep
				}
			s.paired = pair;
			s.has_next = true;
			s.seq = seq;
			s.tick = now;
		}

		// After a logic tick: snapshot every live particle of a paired runner, forget the rest.
		void part_capture(const GfDesc& gd)
		{
			if (gd.n_part_runners == 0) return;
			const uint32_t now = logic_tick_count();
			for (int q = 0; q < gd.n_part_queues; q++)
			{
				uint8_t* n = *(uint8_t**)gd.part_queues[q];
				for (int guard = 0; n != nullptr && guard < PART_SLOTS * 4; guard++)
				{
					const int d = part_desc_of(gd, *(const uint32_t*)(n + 8));
					if (d >= 0) part_capture_node(gd, n, d, now);
					n = *(uint8_t**)(n + 4);
				}
			}
			int w = 0;
			for (int i = 0; i < part_used; i++)
				if (part_slots[i].tick == now) part_slots[w++] = part_slots[i];
			part_used = w;
		}

		bool part_slot_live(const GfDesc& gd, const PartSlot& s)
		{
			return s.paired && s.desc < gd.n_part_runners && *(const uint32_t*)(s.node + 8) == gd.part_runners[s.desc].fn;
		}

		void part_blend(const GfDesc& gd, float t)
		{
			for (int i = 0; i < part_used; i++)
			{
				const PartSlot& s = part_slots[i];
				if (!part_slot_live(gd, s)) { stats.part_step++; continue; }
				stats.part_blend++;
				const RunnerDesc& rd = gd.part_runners[s.desc];
				for (int k = 0; k < rd.n; k++)
					*(int16_t*)(s.node + rd.off[k]) = (rd.ang & (1 << k))
						? part_lerp_angle(s.prev[k], s.next[k], t)
						: cam_lerp(s.prev[k], s.next[k], t);
			}
		}

		// Put the state the last logic tick produced back, so the engine only ever integrates from
		// its own values.
		void part_restore(const GfDesc& gd)
		{
			for (int i = 0; i < part_used; i++)
			{
				const PartSlot& s = part_slots[i];
				if (!part_slot_live(gd, s)) continue;
				const RunnerDesc& rd = gd.part_runners[s.desc];
				for (int k = 0; k < rd.n; k++) *(int16_t*)(s.node + rd.off[k]) = s.next[k];
			}
		}

		// --- the generic call-site stub -----------------------------------------------------------

		const SiteDesc* site_lookup(uint32_t site)
		{
			for (int g = 0; g < GF_COUNT; g++)
				for (int i = 0; i < GF_TABLE[g].n_sites; i++)
					if (GF_TABLE[g].sites[i].site == site) return &GF_TABLE[g].sites[i];
			return nullptr;
		}

		// Every replaced site lands here. All targets are cdecl with at most three arguments, and the
		// caller cleans the stack, so forwarding eight dwords is exact for the ones the callee reads
		// (the same trick as the display-list linker hooks in animations.cpp). The site is recovered
		// from the return address: every patched site is a 5-byte E8 call (verified at install time).
		typedef uint32_t (__cdecl *fwd8_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
		uint32_t __cdecl gf_site_stub(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
		{
			const uint32_t site = (uint32_t)(uintptr_t)_ReturnAddress() - 5;
			const SiteDesc* s = site_lookup(site);
			if (s == nullptr) return 0; // unreachable: only table sites are patched
			if (s->kind == SITE_MODEL_STEP)
			{
				// Only runs on logic ticks (behind the hold); this is where the model block becomes known.
				if (a0 != 0) gf_model = (uint8_t*)(uintptr_t)a0;
			}
			else if (suppressing())
			{
				stats.suppressed++;
				if (s->kind == SITE_SPAWN_SCRATCH)
				{
					memset(spawn_scratch, 0, sizeof(spawn_scratch));
					return (uint32_t)(uintptr_t)spawn_scratch;
				}
				return 0; // SITE_SUPPRESS / SITE_SPAWN_NULL
			}
			// 0x506BA0 is forwarded to the ADDRESS, which carries animations.cpp's boost-settle hook.
			return ((fwd8_t)s->target)(a0, a1, a2, a3, a4, a5, a6, a7);
		}

		// --- the held call ------------------------------------------------------------------------

		// One skipped host frame of a root: the cinematic draws, nothing advances.
		int held_root(const GfDesc& gd, gf_runner_t orig, uint8_t* node)
		{
			int16_t* const counter = (int16_t*)(node + TIMELINE_COUNTER);
			const int16_t held = *counter;
			int r;
			suppress_depth++;
			if (gd.hold == HOLD_PAUSE_DWORD)
			{
				uint32_t* const pause = (uint32_t*)gd.pause_dword;
				const uint32_t old = *pause;
				*pause = 1;
				r = orig(node);
				*pause = old;
			}
			else
			{
				// Only bit 0 is ours: put back exactly its previous value and leave every other bit as
				// the call left it (v72 wrote the whole dword back).
				const uint32_t old_bit = *BATTLE_FREEZE & 1u;
				*BATTLE_FREEZE |= 1u;
				r = orig(node);
				*BATTLE_FREEZE = (*BATTLE_FREEZE & ~1u) | old_bit;
			}
			suppress_depth--;
			// Bit 1 tells 0x508420 to unlink the task: leave the node exactly as its cleanup left it.
			if ((r & 2) != 0) return r;
			// HOLD_PAUSE_DWORD roots only step while the dword is 0 - the restore is belt and braces.
			// HOLD_FREEZE_BIT roots (Shiva) step their counter every call on purpose: it is only the
			// packet arena parity, and the double buffer must keep flipping.
			if (gd.hold == HOLD_PAUSE_DWORD) *counter = held;
			return r;
		}

		// --- 1/s trace ------------------------------------------------------------------------------

		void stats_after_root(const GfDesc& gd, bool real)
		{
			stats.frames++;
			if (!real) stats.held++;
			const int32_t q = *VRAM_OP_COUNT;
			if (q > stats.vram_max) stats.vram_max = q;
			if (q > 0x1F && !warned_vram)
			{
				warned_vram = true;
				ff8_interp::log_warning("%s: VRAM op queue at %d entries during the %s cinematic (0x20 slots; the flush runs past the queue).\n", __func__, q, gd.name);
			}
			if (!ff8_interp::cfg().trace) return;
			const uint32_t now = GetTickCount();
			if (stats.stamp == 0) { stats.stamp = now; return; }
			if (now - stats.stamp < 1000) return;
			ff8_interp::log_trace("ff8gf %s 1s: frames=%u held=%u model=%u skip=%u parts blended=%u stepped=%u cam=%s blend=%u adopt=%u suppressed=%u vramq_max=%d\n",
				gd.name, stats.frames, stats.held, stats.model_req, stats.model_skip, stats.part_blend, stats.part_step,
				gd.owns_camera ? "owned" : "battle", stats.cam_blend, stats.cam_adopt, stats.suppressed, stats.vram_max);
			const bool warned = warned_vram;
			stats = PaceStats();
			stats.stamp = now;
			warned_vram = warned;
		}

		// --- the root hook (every row) ------------------------------------------------------------

		int __cdecl gf_root_hook(uint8_t* node)
		{
			const int g = node != nullptr ? row_by_root(*(const uint32_t*)(node + 8)) : -1;
			if (g < 0 || orig_root[g] == nullptr) return 0; // unreachable: only table roots are hooked
			const GfDesc& gd = GF_TABLE[g];
			gf_runner_t orig = orig_root[g];
			if (!in_battle() || pacing_off()) return orig(node);
			tick_update();
			if (g != active_gf)
			{
				cinematic_forget();
				active_gf = g;
			}
			// From here until one frame after the cinematic ends the cinematic is live (boost gate) and,
			// for a camera-owning row, battle_camera_hook stands down.
			gf_seen_frame = ff8_interp::frame_counter();

			const uint8_t* tl = timeline_node(gd);
			if (tl != nullptr)
			{
				const int16_t counter = *(const int16_t*)(tl + TIMELINE_COUNTER);
				if (ff8_interp::cfg().trace && counter != traced_counter)
				{
					traced_counter = counter;
					ff8_interp::log_trace("ff8gf %s frame %d/%d ticks=%u\n", gd.name, (int)counter, (int)gd.timeline_last, logic_tick_count());
				}
				if (watchdog_stuck(root_adv, counter, gd.name))
				{
					cam_valid = false; // unpaced: the cinematic writes its own camera every frame again
					return orig(node);
				}
			}

			const bool real = tick_is_real();
			// Undo the camera blend published last frame: the timeline read-modify-writes the pose.
			if (gd.owns_camera && cam_valid) cam_write(cam_next);
			// Motion between two logic ticks, in place BEFORE the root draws.
			if (!real)
			{
				model_publish_blend();
				part_blend(gd, tick_alpha_now());
			}
			const int r = real ? orig(node) : held_root(gd, orig, node);
			if (real) part_capture(gd); else part_restore(gd);
			if (gd.owns_camera) cam_after_root(real);
			stats_after_root(gd, real);
			if ((r & 2) != 0) cinematic_forget(); // cinematic over; battle_camera_hook re-captures
			return r;
		}

		// --- the inner hook (HOLD_PAUSE_DWORD rows whose timeline step ignores the pause) ----------

		int __cdecl gf_inner_hook(uint8_t* node)
		{
			const int g = node != nullptr ? row_by_inner(*(const uint32_t*)(node + 8)) : -1;
			if (g < 0 || orig_inner[g] == nullptr) return 0;
			gf_runner_t orig = orig_inner[g];
			if (!in_battle() || pacing_off()) return orig(node);
			tick_update();
			int16_t* const counter = (int16_t*)(node + TIMELINE_COUNTER);
			if (watchdog_stuck(inner_adv, *counter, "timeline")) return orig(node);
			if (tick_is_real()) return orig(node);
			// The root has already raised the pause dword and the suppression depth for this call.
			const int16_t held = *counter;
			const int r = orig(node);
			if ((r & 2) != 0) return r; // last frame: its cleanup ran, leave the node alone
			*counter = held;
			return r;
		}

		// --- installation -------------------------------------------------------------------------

		bool patch_site(const SiteDesc& s)
		{
			const uint8_t* const p = (const uint8_t*)s.site;
			const uint32_t target = s.site + 5 + (uint32_t)(*(const int32_t*)(s.site + 1));
			if (p[0] != 0xE8 || target != s.target)
			{
				ff8_interp::log_warning("gf_pacing_hook_init: call site %08X does not call %08X (%02X -> %08X), left alone.\n",
					s.site, s.target, p[0], target);
				return false;
			}
			ff8_interp::hook_call(s.site, (void*)gf_site_stub);
			return true;
		}
	}

	// See the marked block in internal.h: true while a camera-owning GF cinematic runs, so
	// battle_camera_hook must not restore, capture or blend the pose. One frame of grace, because the
	// camera hook runs later in the same host frame (0x500988) than the effect tick (0x50093A).
	bool gf_cinematic_owns_camera()
	{
		return gf_live_now() && active_gf >= 0 && GF_TABLE[active_gf].owns_camera;
	}

	// True while ANY paced GF cinematic runs (same grace), for apply_results_hook's boost gate.
	bool gf_cinematic_live()
	{
		return gf_live_now();
	}

	void gf_pacing_hook_init()
	{
		int rows = 0, sites = 0, all_sites = 0;
		for (int g = 0; g < GF_COUNT; g++)
		{
			const GfDesc& gd = GF_TABLE[g];
			orig_root[g] = (gf_runner_t)make_trampoline(gd.root_fn, gd.root_prologue, gd.root_prologue_len);
			if (orig_root[g] == nullptr)
			{
				ff8_interp::log_warning("%s: %s root task %08X prologue mismatch, that summon stays at host rate.\n", __func__, gd.name, gd.root_fn);
				continue;
			}
			if (gd.inner_fn != 0)
			{
				orig_inner[g] = (gf_runner_t)make_trampoline(gd.inner_fn, gd.inner_prologue, gd.inner_prologue_len);
				if (orig_inner[g] == nullptr)
				{
					// Without the counter restore the timeline would run at host rate under a held root.
					ff8_interp::log_warning("%s: %s timeline %08X prologue mismatch, that summon stays at host rate.\n", __func__, gd.name, gd.inner_fn);
					continue;
				}
			}
			ff8_interp::hook_function(gd.root_fn, (void*)gf_root_hook);
			if (orig_inner[g] != nullptr) ff8_interp::hook_function(gd.inner_fn, (void*)gf_inner_hook);
			rows++;
			for (int i = 0; i < gd.n_sites && i < 64; i++)
			{
				all_sites++;
				site_ok[g][i] = patch_site(gd.sites[i]);
				if (site_ok[g][i]) sites++;
			}
		}
		ff8_interp::log_info("%s: gf pacing v4 enabled (%d/%d GFs, %d/%d call sites replaced).\n", __func__, rows, GF_COUNT, sites, all_sites);
	}

	void gf_pacing_reset()
	{
		suppress_depth = 0;
		warned_watchdog = false;
		warned_vram = false;
		stats = PaceStats();
		active_gf = -1;
		gf_seen_frame = 0xFFFFFFFF;
		cinematic_forget();
	}
}
