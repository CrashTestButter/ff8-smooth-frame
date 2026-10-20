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

// Spell-path GF pacing: Siren (95) and the summons built by the same effect-framework code generator -
// Tonberry (90), Moogle Dance / MiniMog (96), ChocoFire (97), ChocoFlare (98), ChocoMeteor (99),
// ChocoBocle (100), plus Moomba / Friendship (338, a plain timeline tree, see its row). Shares the battle module's tick state through internal.h; gated by
// DBG_GF_SIREN_OFF (this whole family, not only Siren since v2) and DBG_GF_PACE_OFF (every GF).
//
// v2: one row per module (SPELL_GF_TABLE). Every module of the family is the same generated code with
// module-private addresses: Init = 0x508300 pool + Effect_AddTaskAndInitFromCtx(root queue, root, 100),
// root = `sub esp,2Ch / push esi / push edi` phase dispatcher that ticks its sub-queues, root phase 3
// links the timeline (4 steps: scene/loader init, result, 2 tail steps), timeline step 0 sets the
// load-script cursor and the loader block pointer and links the loader task and the director. What is
// said below for Siren holds for every row; the per-module proof (externals, engine globals, waits,
// camera) is in the notes. Differences handled per row: only the addresses. One addition for every
// row (Siren never triggers it): the screen shake 0x1D97712 that ChocoMeteor / ChocoBocle write from
// a table on each tick is pure output that 0x5033E0 consumes and clears every host frame, so a tick's
// value is re-published on the held frames after it (vanilla shows it for the whole 1/15 s).
//
// The Siren write-up (v75, unchanged):
//
// Siren is not a timeline-family module with a pause dword (gf_pacing.cpp) nor an animation VM
// (gf_vm.cpp): Init 0x739DA0 builds an ORDINARY spell tree through the effect framework
// (Effect_AddTaskAndInitFromCtx 0x8DC540 -> BdLinkTask 0x508360) and returns its root queue
// 0x257FA80, which the summon task 0x50B2A0 (state 3) stores in the effect root 0x1D96AAC - so it is
// ticked at driver site 0x50093A exactly like a spell. Nothing in the module or the framework reads
// the freeze bit or a pause dword, and every node steps +0x24 / its phase +0x29 on every call, so
// there is nothing to "hold". Instead Siren rides the spell path of animations.cpp's
// effect_tick_hook: the whole tree runs on logic ticks only, and the frames in between replay (and
// fx_blend interpolates) the display-list link calls the tick recorded.
//
// Why skipping the whole tree is safe for Siren (the reason GF commands were excluded was a v20
// Quezacotl freeze): every wait in the module is level-triggered on state that stays set -
//   * the load script (0x258BFA8, 16-byte commands) waits on the loader block (*0x258EAA0 =
//     0x258FB88) word +0x164, set to 1 before the async request (0x48D0A0 / BdTransSummonStream
//     0x501860) and cleared by that request's completion callback (0x73A5B0 / 0x73A5E0..); command 6
//     additionally waits for the stream's ready byte +0x16A, which the step itself clears;
//   * the director task 0x73A7B0 (26 steps) and the timeline 0x73A1A0 (4 steps) wait on the scene
//     block *0x1533010 (= 0x258EAA8, written only by the module): scene index +0x40 / +0x42 / +0x44,
//     frames-in-scene +0x46 (max compared value 0x2D), result flag +0x48, +0x4C;
//   * the result is applied by timeline step 0x747440 (0x506690 per target once +0x48 == 1) - one
//     call per tick, as in vanilla; completion is the root's own `return 2`, after which the queue
//     empties and the driver clears 0x1D96AAC for 0x50B2A0 state 4 to see.
// Nothing polls a one-frame pulse, so a tree that runs on ticks only sees everything vanilla sees.
// One-shots (sounds 0x501330, vibration 0x4A29A0, VRAM ops) fire once per tick, i.e. vanilla's count.
//
// Camera: Siren writes the pose itself (0x73B160: 0xB8B7F0..0xB8B7FF and the roll 0x1D8E038, all
// ABSOLUTE stores computed from its own camera block *0x15339D0 - it never reads the pose back). It
// runs inside the tick, before 0x500988, so battle_camera_hook's v69 foreign-write rule already does
// the right thing: on a tick the pose differs from what the hook published, it is not restored over,
// it is captured as `next` (a > 2048-unit jump is a cut) and lerp(prev, next, alpha) is published on
// that frame and every held frame. Siren therefore must NOT own the camera: with
// gf_cinematic_owns_camera() true the camera hook would stand down and, the tree not running on held
// frames, the pose would step at 15 fps. Only gf_cinematic_live() is extended.
//
// Creature pose: the creature animates through 0x8DD1C0 -> Battle_ReadAnimation 0x508F90
// (read_animation_hook). On a tick that hook would publish lerp(prev, next, alpha) with the tick
// frame's small alpha, i.e. the recorded display list would show the pose of the PREVIOUS tick and
// the replay blend would sit one tick behind the camera. While Siren's root runs on a paced tick,
// gf_siren_exact_pose() tells it to leave the exact decoded pose; the replay does the blending.
//
// Watchdog: a signature of the module's state machines (root / director / timeline phases, the
// scene block, the load script and the loader block; frames-in-scene only while < 0x40, i.e. while
// a timed wait can still be running) is taken after every root call. 2 s of wall time without a
// change releases the hold (the tree runs every host frame, as vanilla) until it changes again.

#include "internal.h"

#include "../host.h"

#include <windows.h>
#include <stdint.h>
#include <string.h>

namespace ff8_battle_anim
{
	namespace
	{
		constexpr uint32_t MAGIC_LIST_LOGIC = 0xC81774;       // MagicList_Logic[effect - 1] = Init

		// How the 2 s watchdog reads a row's progress.
		enum SignatureKind : uint8_t
		{
			SIGNATURE_FRAMEWORK, // the effect-framework generator (Siren and friends): phases, scene block, load script, loader
			SIGNATURE_COUNTER,   // a plain timeline tree (Moomba): root +0x0F ready byte, root +0x0C before it,
			                     // the timeline node's +0x0C frame counter (it stops on every wait of the tree)
		};

		constexpr uint32_t FN_EFFECT_ADD_TASK = 0x8DC540;   // Effect_AddTaskAndInitFromCtx(queue, fn, size, parent)
		constexpr uint32_t FN_LINK_TASK       = 0x508360;   // BdLinkTask(queue, fn)

		// Root prologues: position independent, re-verified at install time.
		const uint8_t FRAMEWORK_ROOT_PROLOGUE[5] = { 0x83, 0xEC, 0x2C, 0x56, 0x57 }; // sub esp,2Ch / push esi / push edi
		const uint8_t MOOMBA_ROOT_PROLOGUE[6] = { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C }; // push ebx / push ebp / mov ebp,[esp+0Ch]

		// One module of the family. Every address is re-verified at install time.
		struct SpellGfRow
		{
			const char* name;
			uint16_t effect;          // MagicList_Logic index + 1
			uint32_t init;            // MagicList_Logic[effect - 1] (for Moomba the thunk in front of its Init)
			uint32_t link_pushes;     // `push root / push root_queue` (68 xx xx xx xx 68 xx xx xx xx), then E8 -> link_target
			uint32_t link_target;     // 0x8DC540 (framework rows) or 0x508360 (Moomba)
			const uint8_t* prologue;  // root prologue
			uint8_t prologue_length;
			uint8_t signature_kind;   // SignatureKind
			uint32_t root;            // root task (framework node, phase +0x29)
			uint32_t root_queue;      // what Init returns -> 0x1D96AAC
			uint32_t director;        // director task (step table on +0x29) and its queue
			uint32_t director_queue;
			uint32_t timeline;        // timeline task (step 1 applies the result) and its queue
			uint32_t timeline_queue;
			uint32_t scene_block_ptr; // static pointer to the module's scene block (+0x40..+0x4C)
			uint32_t load_cursor;     // load-script cursor
			uint32_t loader_block_ptr; // pointer to the loader block (+0x164 busy, +0x166 command taken, +0x16A stream ready)
			bool watch_director;      // v2 rows: the signature also takes the director's +0x44 countdown, its +0x24 while < 0x40
			                          // and loader +0x166 (Siren keeps its v1 signature exactly)
		};

#define FRAMEWORK_ROW(effect, init, link_pushes) effect, init, link_pushes, FN_EFFECT_ADD_TASK, FRAMEWORK_ROOT_PROLOGUE, (uint8_t)sizeof(FRAMEWORK_ROOT_PROLOGUE), SIGNATURE_FRAMEWORK
		const SpellGfRow SPELL_GF_TABLE[] = {
			//  name                        effect init      link_pushes   root      root_queue director  director_queue timeline timeline_queue scene_block_ptr load_cursor loader_block_ptr watch_director
			{ "Siren",       FRAMEWORK_ROW(95,  0x739DA0, 0x739DCC), 0x739F40, 0x257FA80, 0x73A7B0, 0x257F988, 0x73A1A0, 0x258BE30, 0x1533010, 0x258BFA8, 0x258EAA0, false },
			{ "Tonberry",    FRAMEWORK_ROW(90,  0x762360, 0x76238C), 0x7624D0, 0x259F088, 0x762C60, 0x259EF90, 0x762690, 0x25A2B80, 0x1547168, 0x25A2CF8, 0x25A3DEC, true },
			{ "MoogleDance", FRAMEWORK_ROW(96,  0x731DE0, 0x731E0C), 0x731F70, 0x257B920, 0x7327B0, 0x257B828, 0x7321D0, 0x257E268, 0x152BB90, 0x257E3E0, 0x257F4D8, true },
			{ "ChocoFire",   FRAMEWORK_ROW(97,  0x729A60, 0x729A8C), 0x729BD0, 0x2575448, 0x72A410, 0x2575350, 0x729E20, 0x2579D48, 0x152B5B8, 0x2579EC0, 0x257AFB8, true },
			{ "ChocoFlare",  FRAMEWORK_ROW(98,  0x721860, 0x72188C), 0x7219D0, 0x256E538, 0x722210, 0x256E440, 0x721C20, 0x2573AC8, 0x152AF30, 0x2573C40, 0x2574D38, true },
			{ "ChocoMeteor", FRAMEWORK_ROW(99,  0x717D30, 0x717D5C), 0x717ED0, 0x2565C10, 0x718750, 0x2565B18, 0x718130, 0x256C480, 0x1529FA0, 0x256C5F8, 0x256D6F0, true },
			{ "ChocoBocle",  FRAMEWORK_ROW(100, 0x70D390, 0x70D3BC), 0x70EB00, 0x255ECA8, 0x70F370, 0x255EBB0, 0x70ED60, 0x2564168, 0x15297F0, 0x25642E0, 0x25653D8, true },
			// Moomba / Friendship (item, cmd 0xF4 arg 0x0F -> BattleActionSequence_Tick_MagicCast 0x50A9A0, whose
			// case 5 stores Init's return in the effect root). Not the framework: a plain timeline tree -
			// Init 0x6EBD60 (behind the thunk 0x6EBD50) links root 0x6EBE20 into 0x2556210 with BdLinkTask and
			// starts an ENGINE camera sequence (0x5099A0, battle_camera_hook's); the root spawns the timeline
			// 0x6EC0F0 into 0x2556200 at its counter 2 and, once it is gone, polls 0x5342C0(0). No camera
			// writes, no freeze bit, no pause dword. Waits: the timeline's sparkle-pool-full early return
			// (before its +0x0C step) and that completion poll - both level-triggered.
			{ "Moomba", 338, 0x6EBD50, 0x6EBDE6, FN_LINK_TASK, MOOMBA_ROOT_PROLOGUE, (uint8_t)sizeof(MOOMBA_ROOT_PROLOGUE), SIGNATURE_COUNTER,
				0x6EBE20, 0x2556210, 0, 0, 0x6EC0F0, 0x2556200, 0, 0, 0, false },
		};
#undef FRAMEWORK_ROW
		constexpr int ROW_COUNT = (int)(sizeof(SPELL_GF_TABLE) / sizeof(SPELL_GF_TABLE[0]));

		uint32_t* const EFFECT_ROOT = (uint32_t*)0x1D96AAC;
		int16_t* const SHAKE_Y = (int16_t*)0x1D97712;        // screen shake, consumed and cleared by 0x5033E0 each host frame

		constexpr uint32_t ACTION_EFFECT_ID = 6;
		constexpr uint32_t NODE_NEXT = 0x04;
		constexpr uint32_t NODE_FUNCTION = 0x08;
		constexpr uint32_t NODE_COUNTER = 0x24;
		constexpr uint32_t NODE_PHASE = 0x29;
		constexpr uint32_t NODE_COUNTDOWN = 0x44;
		constexpr uint32_t WATCHDOG_MS = 2000;
		constexpr uint16_t SCENE_TIMED_WAIT_MAX = 0x40;      // largest +0x46 compare in Siren is 0x2D
		constexpr uint16_t DIRECTOR_TIMED_WAIT_MAX = 0x40;   // largest director +0x24 compare in the v2 rows is 0x1C

		typedef int (__cdecl *root_function_t)(uint8_t*);
		root_function_t original_root[ROW_COUNT] = {};
		bool row_installed[ROW_COUNT] = {};
		int rows_installed = 0;

		int live_row = -1;                  // the row whose tree was last seen at the effect root
		uint32_t live_frame = 0xFFFFFFFF;   // last host frame that tree was seen at the effect root
		uint32_t paced_frame = 0xFFFFFFFF;  // last host frame gf_siren_holdable() answered true
		bool exact_pose = false;            // inside a root, on a paced logic tick
		int16_t tick_shake = 0;             // shake the last paced tick wrote (0 = none)

		// watchdog
		bool signature_valid = false;
		uint32_t signature = 0;
		uint32_t signature_stamp = 0;
		bool released = false;
		bool warned = false;

		// trace (1/s while live)
		struct Stats
		{
			uint32_t frames, held, ticks, released_frames, shakes;
		};
		Stats stats = {};
		uint32_t stats_stamp = 0;

		inline bool enabled()
		{
			return rows_installed > 0 && !(ff8_interp::cfg().battle_debug & (DBG_GF_SIREN_OFF | DBG_GF_PACE_OFF));
		}

		inline bool seen_within_one_frame(uint32_t stamp)
		{
			return stamp != 0xFFFFFFFF && (ff8_interp::frame_counter() - stamp) <= 1;
		}

		int row_by_effect(uint16_t effect)
		{
			for (int row_index = 0; row_index < ROW_COUNT; row_index++)
			{
				if (row_installed[row_index] && SPELL_GF_TABLE[row_index].effect == effect) return row_index;
			}
			return -1;
		}

		int row_by_root(uint32_t fn)
		{
			for (int row_index = 0; row_index < ROW_COUNT; row_index++)
			{
				if (SPELL_GF_TABLE[row_index].root == fn) return row_index;
			}
			return -1;
		}

		// The task in `queue` whose callback is `fn`, or nullptr. Queue: [0] head; node +4 next, +8 fn.
		const uint8_t* find_task(uint32_t queue, uint32_t fn)
		{
			if (queue == 0) return nullptr; // a row without that task (Moomba has no director)
			const uint8_t* node = *(const uint8_t* const*)queue;
			for (int i = 0; i < 32 && node != nullptr; i++)
			{
				if (*(const uint32_t*)(node + NODE_FUNCTION) == fn) return node;
				node = *(const uint8_t* const*)(node + NODE_NEXT);
			}
			return nullptr;
		}

		inline void hash_mix(uint32_t& hash, uint32_t value)
		{
			hash = (hash ^ value) * 16777619u;
		}

		// Progress of the module's own state machines. Free-running counters (+0x24 of the root and
		// timeline, +0x5C, the alive count +0x5E) are excluded so a real stall cannot hide behind them;
		// the timed waits (+0x46 frames in scene, the v2 rows' director +0x24) only count while they
		// are below the largest value any step compares them with.
		uint32_t progress_signature(const SpellGfRow& row)
		{
			uint32_t hash = 2166136261u;
			if (row.signature_kind == SIGNATURE_COUNTER)
			{
				// Root +0x0C steps on every call while the timeline lives (it would hide a stalled timeline),
				// so it only counts before the ready byte +0x0F is set. The timeline's +0x0C stops on its
				// pool-full early return; nothing moves during the completion poll.
				const uint8_t* root = find_task(row.root_queue, row.root);
				const uint8_t* timeline = find_task(row.timeline_queue, row.timeline);
				hash_mix(hash, root ? 0x100u | root[0x0F] : 0);
				if (root != nullptr && root[0x0F] == 0) hash_mix(hash, *(const uint16_t*)(root + 0x0C));
				hash_mix(hash, timeline ? 0x10000u | *(const uint16_t*)(timeline + 0x0C) : 0);
				return hash;
			}
			const uint8_t* root = find_task(row.root_queue, row.root);
			const uint8_t* director = find_task(row.director_queue, row.director);
			const uint8_t* timeline = find_task(row.timeline_queue, row.timeline);
			hash_mix(hash, root ? 0x100u | root[NODE_PHASE] : 0);
			hash_mix(hash, director ? 0x100u | director[NODE_PHASE] : 0);
			hash_mix(hash, timeline ? 0x100u | timeline[NODE_PHASE] : 0);
			if (row.watch_director && director != nullptr)
			{
				hash_mix(hash, *(const uint16_t*)(director + NODE_COUNTDOWN));
				const uint16_t director_counter = *(const uint16_t*)(director + NODE_COUNTER);
				if (director_counter < DIRECTOR_TIMED_WAIT_MAX) hash_mix(hash, director_counter);
			}
			const uint8_t* scene = *(const uint8_t* const*)row.scene_block_ptr;
			if (scene != nullptr)
			{
				hash_mix(hash, *(const uint32_t*)(scene + 0x40));  // scene index, next
				hash_mix(hash, *(const uint16_t*)(scene + 0x44)); // requested
				hash_mix(hash, *(const uint32_t*)(scene + 0x48)); // result flag, +0x4A
				hash_mix(hash, *(const uint16_t*)(scene + 0x4C));
				const uint16_t frames_in_scene = *(const uint16_t*)(scene + 0x46);
				if (frames_in_scene < SCENE_TIMED_WAIT_MAX) hash_mix(hash, frames_in_scene);
			}
			hash_mix(hash, *(const uint32_t*)row.load_cursor);
			const uint8_t* loader = *(const uint8_t* const*)row.loader_block_ptr;
			if (loader != nullptr)
			{
				hash_mix(hash, *(const uint16_t*)(loader + 0x164));
				hash_mix(hash, loader[0x16A]);
				if (row.watch_director) hash_mix(hash, *(const uint16_t*)(loader + 0x166));
			}
			return hash;
		}

		void cinematic_forget()
		{
			signature_valid = false;
			signature_stamp = GetTickCount();
			released = false;
			exact_pose = false;
			tick_shake = 0;
			stats = Stats();
			stats_stamp = GetTickCount();
		}

		void progress_check(const SpellGfRow& row)
		{
			const uint32_t current_signature = progress_signature(row);
			if (!signature_valid || current_signature != signature)
			{
				if (released && ff8_interp::cfg().trace) ff8_interp::log_trace("ff8siren %s progress again, pacing re-armed f=%u\n", row.name, ff8_interp::frame_counter());
				signature_valid = true;
				signature = current_signature;
				signature_stamp = GetTickCount();
				released = false;
			}
		}

		void trace_second(const SpellGfRow& row)
		{
			if (!ff8_interp::cfg().trace) return;
			const uint32_t now = GetTickCount();
			if (now - stats_stamp < 1000) return;
			const uint8_t* root = find_task(row.root_queue, row.root);
			const uint8_t* director = find_task(row.director_queue, row.director);
			const uint8_t* timeline = find_task(row.timeline_queue, row.timeline);
			const uint8_t* scene = row.scene_block_ptr != 0 ? *(const uint8_t* const*)row.scene_block_ptr : nullptr;
			ff8_interp::log_trace("ff8siren %s 1s: frames=%u held=%u ticks=%u released=%u shakes=%u phase root=%d dir=%d tl=%d scene=%d/%d\n",
				row.name, stats.frames, stats.held, stats.ticks, stats.released_frames, stats.shakes,
				root ? root[NODE_PHASE] : -1, director ? director[NODE_PHASE] : -1, timeline ? timeline[NODE_PHASE] : -1,
				scene ? *(const int16_t*)(scene + 0x40) : -1, scene ? *(const int16_t*)(scene + 0x46) : -1);
			stats = Stats();
			stats_stamp = now;
		}

		// One hook for every row's root; the row is found again from the node's own callback.
		int __cdecl siren_root_hook(uint8_t* node)
		{
			const int row_index = node != nullptr ? row_by_root(*(const uint32_t*)(node + NODE_FUNCTION)) : -1;
			if (row_index < 0 || original_root[row_index] == nullptr) return 0; // unreachable: only table roots are hooked
			const SpellGfRow& row = SPELL_GF_TABLE[row_index];
			if (!in_battle() || !enabled()) return original_root[row_index](node);
			tick_update();
			exact_pose = row_index == live_row && paced_frame == ff8_interp::frame_counter() && is_logic_tick_now();
			if (exact_pose) stats.ticks++;
			const int16_t shake_before = *SHAKE_Y;
			const int result = original_root[row_index](node);
			if (exact_pose)
			{
				// Only what the tree wrote during this call (0x5033E0 cleared the previous frame's value).
				const int16_t shake_after = *SHAKE_Y;
				tick_shake = shake_after != shake_before ? shake_after : 0;
			}
			exact_pose = false;
			progress_check(row);
			if (result & 2)
			{
				if (ff8_interp::cfg().trace) ff8_interp::log_trace("ff8siren %s root done f=%u\n", row.name, ff8_interp::frame_counter());
				cinematic_forget();
			}
			return result;
		}
	}

	// See the marked gf_siren block in internal.h.
	bool gf_siren_holdable(const uint8_t* action)
	{
		if (action == nullptr || !enabled()) return false;
		const int row_index = row_by_effect(*(const uint16_t*)(action + ACTION_EFFECT_ID));
		if (row_index < 0) return false;
		const SpellGfRow& row = SPELL_GF_TABLE[row_index];
		if (*EFFECT_ROOT != row.root_queue) return false;
		// A new cinematic: the tree was not at the root on the previous host frame.
		if (row_index != live_row || !seen_within_one_frame(live_frame)) cinematic_forget();
		live_row = row_index;
		live_frame = ff8_interp::frame_counter();
		tick_update();
		stats.frames++;
		if (!released && signature_valid && GetTickCount() - signature_stamp > WATCHDOG_MS)
		{
			released = true;
			tick_shake = 0;
			if (!warned)
			{
				warned = true;
				ff8_interp::log_warning("gf_siren: %s cinematic made no progress for %u ms, pacing released.\n", row.name, WATCHDOG_MS);
			}
		}
		trace_second(row);
		if (released)
		{
			stats.released_frames++;
			return false;
		}
		if (!is_logic_tick_now())
		{
			stats.held++;
			// The tree does not run on this frame: re-publish the shake its last tick wrote, unless
			// something else already wrote one this frame.
			if (tick_shake != 0 && *SHAKE_Y == 0)
			{
				*SHAKE_Y = tick_shake;
				stats.shakes++;
			}
		}
		paced_frame = ff8_interp::frame_counter();
		return true;
	}

	bool gf_siren_live()
	{
		return enabled() && seen_within_one_frame(live_frame);
	}

	bool gf_siren_exact_pose()
	{
		return exact_pose;
	}

	void gf_siren_hook_init()
	{
		rows_installed = 0;
		for (int row_index = 0; row_index < ROW_COUNT; row_index++)
		{
			const SpellGfRow& row = SPELL_GF_TABLE[row_index];
			// Init's `push root / push root_queue` right before its link call.
			uint8_t push_pattern[10] = { 0x68, 0, 0, 0, 0, 0x68, 0, 0, 0, 0 };
			memcpy(push_pattern + 1, &row.root, 4);
			memcpy(push_pattern + 6, &row.root_queue, 4);
			const char* mismatch = nullptr;
			const uint8_t* link_call = (const uint8_t*)(row.link_pushes + sizeof(push_pattern));
			if (*(const uint32_t*)(MAGIC_LIST_LOGIC + (row.effect - 1) * 4) != row.init) mismatch = "MagicList_Logic entry";
			else if (memcmp((const void*)row.link_pushes, push_pattern, sizeof(push_pattern)) != 0) mismatch = "Init root link";
			else if (link_call[0] != 0xE8 || (uint32_t)(uintptr_t)link_call + 5 + *(const int32_t*)(link_call + 1) != row.link_target) mismatch = "Init link call";
			else if ((original_root[row_index] = (root_function_t)make_trampoline(row.root, row.prologue, row.prologue_length)) == nullptr) mismatch = "root prologue (hooked by someone else?)";
			if (mismatch != nullptr)
			{
				ff8_interp::log_warning("gf_siren_hook_init: %s (effect %u): %s does not match, it stays at host rate.\n", row.name, (unsigned)row.effect, mismatch);
				continue;
			}
			ff8_interp::hook_function(row.root, (void*)siren_root_hook);
			row_installed[row_index] = true;
			rows_installed++;
		}
		ff8_interp::log_info("%s: spell-path GF pacing v2 enabled (%d/%d effects through the spell replay path).\n", __func__, rows_installed, ROW_COUNT);
	}

	void gf_siren_reset()
	{
		live_row = -1;
		live_frame = 0xFFFFFFFF;
		paced_frame = 0xFFFFFFFF;
		warned = false;
		cinematic_forget();
	}
}
