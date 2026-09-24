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

// FF8 battle model animation interpolation.
//
// The battle engine stores one skeletal pose per 15 fps logic tick (sec 3 of
// the .dat files, delta encoded). With ff8_fps_limiter at 30 or 60 fps the
// engine decodes a frame every host frame, so animations run 2x/4x too fast
// and still step between the same stored poses. This module hooks the single
// frame decoder, Battle_ReadAnimation, which every caller (party members,
// monsters, GFs, effect models) goes through:
//   * a real frame is consumed only every Nth host frame (N = host frames per
//     15 fps tick), at most once per host frame per animation command,
//   * on the host frames in between the engine's pose buffer receives a pose
//     interpolated between the last two decoded frames and the bone matrices
//     are rebuilt with the engine's own function,
//   * the return value (1 = animation finished) is reported exactly like the
//     original would on the frames where a real read happens.
//
// Engine layout (FF8 2000 v1.2 / Steam 2013, see ~/ff8/notes/interp-plan.md):
//   AnimCtx: +0 unknown, +4 ModelInfo*; ModelInfo[0] = PoseBuf*, ModelInfo[2] = sec-3 data
//   AnimCmd: [0] anim id, [1] flags (bit0 SLOW, bit1 FAST, bits2-3 sub-frame),
//            [2..3] i16 stream byte offset, [4..5] i16 bit offset,
//            [6] current frame, [7] total frames
//   PoseBuf: [0] bone count, [1] flags (bit0 scale channel), [2..3] i16 scale,
//            +8/+A/+C i16 root xyz, bone slots from +0x10, 0x30 bytes each:
//            +0 parent, +2 length, +4 rot xyz (i16, 4096 = full turn),
//            +0xA scale xyz (i16), +0x10 MATRIX (built by BuildBoneMatrices)

#include "animations.h"
#include "internal.h"
#include "../common/tick.h"
#include "../common/play_clock.h"

#include "../host.h"

#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <string.h>
#include <unordered_map>
#include <vector>
#include <cmath>

namespace ff8_battle_anim
{
	namespace
	{
		// The DBG_* debug bits for ff8_battle_interp_debug now live in internal.h, so the sibling
		// translation units of this module see the same numbering.

		// Verified byte-identical in the Steam 2013 FF8_EN.exe (image base 0x400000)
		constexpr uint32_t ADDR_READ_ANIMATION = 0x508F90;      // int  Battle_ReadAnimation(AnimCtx*, AnimCmd*) -> 1 when finished
		constexpr uint32_t ADDR_BUILD_BONE_MATRICES = 0x508C90; // void BuildBoneMatrices(AnimCtx*)
		constexpr uint32_t ADDR_SEQ_DRIVER = 0x504290;          // void BattleEntity_RunSequence(BattleEntity*): per entity per logic frame, runs the sec-5 VM + Advance
		constexpr uint32_t ADDR_CURRENT_ENTITY = 0x01D98204;    // globals the driver sets first thing (its caller may read them)
		constexpr uint32_t ADDR_CURRENT_CMD = 0x01D981F8;
		constexpr uint32_t ADDR_CURRENT_SEQ = 0x01D98200;

		// First four instructions of the sequence driver (sub esp,8 / push ebx / push edi / mov edi,[esp+14h]).
		const uint8_t SEQ_DRIVER_PROLOGUE[9] = {0x83, 0xEC, 0x08, 0x53, 0x57, 0x8B, 0x7C, 0x24, 0x14};

		// Entity world placement sources (int16), consumed every host frame by the entity task 0x502AB0
		// which rebuilds the entity matrix (+0x40) from them after running the sequence driver.
		constexpr uint32_t ENTITY_POS_X = 0x1C;
		constexpr uint32_t ENTITY_POS_YOFF = 0x1E;
		constexpr uint32_t ENTITY_POS_Z = 0x20;
		constexpr uint32_t ENTITY_POS_Y = 0x22;
		// Sequence VM working position (E5 0x0D/0x0E/0x0F "set position Z/X/Y"), int16 in the sequence
		// state block pointed to by entity+0x74; the entity task adds it into the world matrix too.
		constexpr uint32_t SEQ_POS_Z = 0x24;
		constexpr uint32_t SEQ_POS_X = 0x26;
		constexpr uint32_t SEQ_POS_Y = 0x28;
		constexpr int POS_FIELDS = 7;
		constexpr int ENTITY_POS_SNAP = 6000; // per tick delta above this is a teleport, not motion

		constexpr uint32_t ENTITY_CTX = 0x60;
		constexpr uint32_t ENTITY_CMD = 0x6C;
		constexpr uint32_t ENTITY_SEQ = 0x74;
		constexpr uint32_t ENTITY_WEAPON = 0x78;
		constexpr uint32_t WEAPON_CTX = 0x00;
		constexpr uint32_t WEAPON_CMD = 0x0C;

		// First three instructions of Battle_ReadAnimation (sub esp,8 / push ebx / mov ebx,[esp+14h]):
		// position independent, so they can be relocated into a trampoline.
		const uint8_t READ_ANIMATION_PROLOGUE[8] = {0x83, 0xEC, 0x08, 0x53, 0x8B, 0x5C, 0x24, 0x14};

		constexpr uint32_t POSE_FLAGS = 0x01;
		constexpr uint32_t POSE_ROOT = 0x08;
		constexpr uint32_t POSE_SLOTS = 0x10;
		constexpr uint32_t POSE_SLOT_SIZE = 0x30;
		constexpr uint32_t SLOT_ROT = 0x04;
		constexpr uint32_t SLOT_SCALE = 0x0A;

		constexpr uint32_t CMD_ANIM_ID = 0;
		constexpr uint32_t CMD_CURRENT_FRAME = 6;
		constexpr uint32_t CMD_TOTAL_FRAMES = 7;

		typedef int (__cdecl *read_animation_t)(uint8_t* ctx, uint8_t* cmd);
		typedef void (__cdecl *build_bone_matrices_t)(uint8_t* ctx);
		typedef void (__cdecl *seq_driver_t)(uint8_t* entity);

		read_animation_t original_read_animation = nullptr; // trampoline into the unpatched function
		seq_driver_t original_seq_driver = nullptr;         // trampoline into the unpatched function
		build_bone_matrices_t build_bone_matrices = (build_bone_matrices_t)ADDR_BUILD_BONE_MATRICES;

		// Snapshot of everything ReadAnimation accumulates into a PoseBuf.
		struct Pose
		{
			uint8_t flags = 0;
			int16_t root[3] = {0, 0, 0};
			std::vector<int16_t> bones; // 6 per bone: rx ry rz sx sy sz
		};

		struct State
		{
			bool valid = false;
			uint8_t anim_id = 0;
			uint32_t last_host_frame = 0xFFFFFFFF; // host frame this command was last serviced on
			int last_result = 0;
			Pose prev; // pose before the last real frame read (A)
			Pose next; // pose after the last real frame read (B)
			Pose shown; // pose last written to the buffer (for cross-fading into a newly queued animation)
			bool shown_valid = false;
			uint32_t snap_frame = 0xFFFFFFFF; // host frame of the last snap (frame 0 read)
		};

		std::unordered_map<uint8_t*, State> states; // keyed by AnimCmd*
		int battle_fps = 30;              // host frames per second in battle
		constexpr int LOGIC_FPS = 15;     // native battle tick rate

		// Per host frame tick state, derived lazily from frame_counter: a logic tick fires whenever
		// 1/15 s worth of host frames has elapsed (works for any host rate, 144 included).
		uint32_t tick_frame = 0xFFFFFFFF;
		double tick_acc = 0.0;         // seconds of real time carried into the current logic period
		bool tick_real = true;    // this host frame consumes a real animation frame
		float tick_alpha = 1.0f;  // blend between the last two decoded poses to show this host frame
		float tick_alpha_lead = 1.0f; // the pre-v71 formula, one host frame ahead (hud_blend only)
		uint32_t logic_ticks = 0; // number of logic ticks since the module was reset

		struct EntityState
		{
			bool valid = false;
			int16_t prev[POS_FIELDS] = {0};
			int16_t next[POS_FIELDS] = {0};
		};
		std::unordered_map<uint8_t*, EntityState> entity_states; // keyed by BattleEntity*
		int last_mode = -1;

		inline void capture_pos(const uint8_t* e, int16_t out[POS_FIELDS])
		{
			out[0] = *(const int16_t*)(e + ENTITY_POS_X);
			out[1] = *(const int16_t*)(e + ENTITY_POS_YOFF);
			out[2] = *(const int16_t*)(e + ENTITY_POS_Z);
			out[3] = *(const int16_t*)(e + ENTITY_POS_Y);
			const uint8_t* seq = *(const uint8_t* const*)(e + ENTITY_SEQ);
			out[4] = seq ? *(const int16_t*)(seq + SEQ_POS_Z) : 0;
			out[5] = seq ? *(const int16_t*)(seq + SEQ_POS_X) : 0;
			out[6] = seq ? *(const int16_t*)(seq + SEQ_POS_Y) : 0;
		}

		inline void restore_pos(uint8_t* e, const int16_t in[POS_FIELDS])
		{
			*(int16_t*)(e + ENTITY_POS_X) = in[0];
			*(int16_t*)(e + ENTITY_POS_YOFF) = in[1];
			*(int16_t*)(e + ENTITY_POS_Z) = in[2];
			*(int16_t*)(e + ENTITY_POS_Y) = in[3];
			uint8_t* seq = *(uint8_t**)(e + ENTITY_SEQ);
			if (seq == nullptr) return;
			*(int16_t*)(seq + SEQ_POS_Z) = in[4];
			*(int16_t*)(seq + SEQ_POS_X) = in[5];
			*(int16_t*)(seq + SEQ_POS_Y) = in[6];
		}

		constexpr double LOGIC_PERIOD = 1.0 / double(LOGIC_FPS);

		// Wall-clock gate: ff8_battle_fps only caps the render rate, the logic rate must not depend on it.
		void update_tick()
		{
			if (tick_frame == ff8_interp::frame_counter()) return;
			tick_frame = ff8_interp::frame_counter();
			double dt = ff8_tick::frame_delta();
			tick_acc += dt;
			tick_real = tick_acc >= LOGIC_PERIOD;
			if (tick_real)
			{
				logic_ticks++;
				tick_acc -= LOGIC_PERIOD;
				if (tick_acc > LOGIC_PERIOD) tick_acc = LOGIC_PERIOD; // after a stall, catch up over the next frames
			}
			// v71: the time since the last tick, tick_acc / P. `tick_acc` already holds this frame's dt, so
			// the pre-v71 (tick_acc + dt) / P counted it twice: everything drew one host frame ahead, alpha
			// was pinned to 1.0 on the last frame before every tick and the tick frame then jumped to
			// (r + dt) / P - an uneven step pair once per tick (audit F2). The old value is still kept
			// for the Renzokuken bar (tick_alpha_lead_now), and DBG_ALPHA_LEAD puts it back everywhere.
			tick_alpha_lead = float((tick_acc + dt) / LOGIC_PERIOD);
			if (tick_alpha_lead > 1.0f) tick_alpha_lead = 1.0f;
			if (tick_alpha_lead < 0.0f) tick_alpha_lead = 0.0f;
			tick_alpha = (ff8_interp::cfg().battle_debug & DBG_ALPHA_LEAD) ? tick_alpha_lead : float(tick_acc / LOGIC_PERIOD);
			if (tick_alpha > 1.0f) tick_alpha = 1.0f;
			if (tick_alpha < 0.0f) tick_alpha = 0.0f;
		}
		uint32_t hook_calls = 0;
		bool hook_installed = false;

		inline uint8_t* pose_of(uint8_t* ctx)
		{
			uint8_t** model = *(uint8_t***)(ctx + 4);
			return model != nullptr ? model[0] : nullptr;
		}

		void capture(const uint8_t* pose, Pose& out)
		{
			int bones = pose[0];
			out.flags = pose[POSE_FLAGS];
			memcpy(out.root, pose + POSE_ROOT, sizeof(out.root));
			out.bones.resize(bones * 6);
			for (int i = 0; i < bones; i++)
			{
				const uint8_t* slot = pose + POSE_SLOTS + i * POSE_SLOT_SIZE;
				memcpy(&out.bones[i * 6], slot + SLOT_ROT, 3 * sizeof(int16_t));
				memcpy(&out.bones[i * 6 + 3], slot + SLOT_SCALE, 3 * sizeof(int16_t));
			}
		}

		void restore(uint8_t* pose, const Pose& in)
		{
			int bones = pose[0];
			if ((int)in.bones.size() != bones * 6) return;
			pose[POSE_FLAGS] = in.flags;
			memcpy(pose + POSE_ROOT, in.root, sizeof(in.root));
			for (int i = 0; i < bones; i++)
			{
				uint8_t* slot = pose + POSE_SLOTS + i * POSE_SLOT_SIZE;
				memcpy(slot + SLOT_ROT, &in.bones[i * 6], 3 * sizeof(int16_t));
				memcpy(slot + SLOT_SCALE, &in.bones[i * 6 + 3], 3 * sizeof(int16_t));
			}
		}

		// Shortest arc on the 12 bit angle circle (4096 units per turn).
		inline int16_t lerp_angle(int16_t a, int16_t b, float t)
		{
			int d = ((int(b) - int(a) + 2048) & 4095) - 2048;
			float v = d * t;
			return (int16_t)(int(a) + (int)(v + (v >= 0.0f ? 0.5f : -0.5f)));
		}

		inline int16_t lerp_linear(int16_t a, int16_t b, float t)
		{
			float v = (int(b) - int(a)) * t;
			return (int16_t)(int(a) + (int)(v + (v >= 0.0f ? 0.5f : -0.5f)));
		}

		void write_interpolated(uint8_t* pose, const Pose& a, const Pose& b, float t)
		{
			int bones = pose[0];
			if ((int)a.bones.size() != bones * 6 || (int)b.bones.size() != bones * 6) return;
			pose[POSE_FLAGS] = b.flags;
			for (int k = 0; k < 3; k++)
				((int16_t*)(pose + POSE_ROOT))[k] = lerp_linear(a.root[k], b.root[k], t);
			for (int i = 0; i < bones; i++)
			{
				uint8_t* slot = pose + POSE_SLOTS + i * POSE_SLOT_SIZE;
				int16_t* rot = (int16_t*)(slot + SLOT_ROT);
				int16_t* scale = (int16_t*)(slot + SLOT_SCALE);
				for (int k = 0; k < 3; k++)
				{
					rot[k] = lerp_angle(a.bones[i * 6 + k], b.bones[i * 6 + k], t);
					scale[k] = lerp_linear(a.bones[i * 6 + 3 + k], b.bones[i * 6 + 3 + k], t);
				}
			}
		}

		// How far apart two poses are, in 12 bit angle units. A freshly queued animation whose first
		// frame is far from what is on screen must NOT be cross-faded: easing a whole body through tens
		// of degrees inside one logic tick reads as limbs swinging the wrong way, which is what a fleeing
		// enemy turning from standing into a run cycle looks like. Small changes (hit reactions, sequence
		// changes) keep the cross-fade, which is what it was added for.
		constexpr int POSE_CUT_MAX = 512;   // 45 degrees on any single rotation component
		constexpr int POSE_CUT_MEAN = 128;  // ~11 degrees averaged over every rotation component
		bool pose_far(const Pose& a, const Pose& b)
		{
			if (a.bones.size() != b.bones.size() || a.bones.empty()) return true;
			long sum = 0; int n = 0;
			for (size_t i = 0; i < a.bones.size(); i += 6)
				for (int k = 0; k < 3; k++)
				{
					int d = ((int(b.bones[i + k]) - int(a.bones[i + k]) + 2048) & 4095) - 2048;
					if (d < 0) d = -d;
					if (d > POSE_CUT_MAX) return true;
					sum += d; n++;
				}
			return n != 0 && sum / n > POSE_CUT_MEAN;
		}

		// A freshly queued animation (frame 0 just decoded): start the blend from whatever pose was on
		// screen last so the change eases in over one logic tick, unless the two poses are far apart -
		// then keep the engine's hard cut. DBG_POSE_CUT_OFF always cross-fades (the pre-v54 behaviour).
		void snap(State& s, const uint8_t* pose, const uint8_t* cmd)
		{
			capture(pose, s.next);
			if (s.shown_valid && s.shown.bones.size() == s.next.bones.size()
				&& ((ff8_interp::cfg().battle_debug & DBG_POSE_CUT_OFF) || !pose_far(s.shown, s.next)))
				s.prev = s.shown;
			else
				s.prev = s.next;
			s.valid = true;
			s.anim_id = cmd[CMD_ANIM_ID];
			s.last_host_frame = 0xFFFFFFFF;
			s.last_result = 0;
			s.snap_frame = ff8_interp::frame_counter();
		}

		// Replacement for Battle_ReadAnimation (0x508F90). The four camera dummies at 0x1D989A4 + i*0x2C
		// come through here as well; they carry no bone pose, so pose_of() below hands them to the original
		// unchanged and the camera is paced by battle_camera_hook instead.
		int __cdecl read_animation_hook(uint8_t* ctx, uint8_t* cmd)
		{
			hook_calls++;
			uint8_t* pose = pose_of(ctx);
			if (pose == nullptr || pose[0] == 0)
				return original_read_animation(ctx, cmd);

			// Already finished: the original just reports 1 and touches nothing.
			if (cmd[CMD_CURRENT_FRAME] >= cmd[CMD_TOTAL_FRAMES])
			{
				auto it = states.find(cmd);
				if (it != states.end() && it->second.valid)
				{
					// Make sure the final decoded pose is what stays on screen.
					restore(pose, it->second.next);
					build_bone_matrices(ctx);
				}
				int r = original_read_animation(ctx, cmd);
				if (ff8_interp::cfg().trace) ff8_interp::log_trace("ff8anim f=%u cmd=%p id=%d %d/%d finished -> %d\n", ff8_interp::frame_counter(), cmd, cmd[CMD_ANIM_ID], cmd[CMD_CURRENT_FRAME], cmd[CMD_TOTAL_FRAMES], r);
				return r;
			}

			State& s = states[cmd];

			// Frame 0 read right after (pre_)Battle_ReadAnimation reset the command: never gate it,
			// it is the absolute base pose of a freshly queued animation.
			if (cmd[CMD_CURRENT_FRAME] == 0)
			{
				int r = original_read_animation(ctx, cmd);
				snap(s, pose, cmd);
				update_tick();
				if (tick_alpha < 1.0f && s.prev.bones.size() == s.next.bones.size())
				{
					write_interpolated(pose, s.prev, s.next, tick_alpha);
					build_bone_matrices(ctx);
				}
				capture(pose, s.shown); s.shown_valid = true;
				if (ff8_interp::cfg().trace) ff8_interp::log_trace("ff8anim f=%u cmd=%p id=%d frame0 total=%d bones=%d flags=%02x -> %d\n", ff8_interp::frame_counter(), cmd, cmd[CMD_ANIM_ID], cmd[CMD_TOTAL_FRAMES], pose[0], cmd[1], r);
				return r;
			}

			if (!s.valid || s.anim_id != cmd[CMD_ANIM_ID])
				snap(s, pose, cmd);

			// Serviced already on this host frame (several callers per frame): keep the answer.
			if (s.last_host_frame == ff8_interp::frame_counter())
			{
				if (ff8_interp::cfg().trace) ff8_interp::log_trace("ff8anim f=%u cmd=%p id=%d %d/%d repeat -> %d\n", ff8_interp::frame_counter(), cmd, cmd[CMD_ANIM_ID], cmd[CMD_CURRENT_FRAME], cmd[CMD_TOTAL_FRAMES], s.last_result);
				return s.last_result;
			}
			s.last_host_frame = ff8_interp::frame_counter();

			update_tick();
			if (tick_real)
			{
				// Deltas accumulate in place: put the exact last decoded pose back before decoding.
				restore(pose, s.next);
				// Frame 0 and frame 1 usually decode in the same host frame: keep the cross-fade start.
				if (s.snap_frame != ff8_interp::frame_counter()) s.prev = s.next;
				s.last_result = original_read_animation(ctx, cmd);
				capture(pose, s.next);
			}
			else
			{
				s.last_result = 0;
			}

			// Show A -> B spread over the host frames of this logic tick.
			float t = tick_alpha;
			if (t < 1.0f)
				write_interpolated(pose, s.prev, s.next, t);
			else
				restore(pose, s.next);
			build_bone_matrices(ctx);
			capture(pose, s.shown); s.shown_valid = true;

			if (ff8_interp::cfg().trace) ff8_interp::log_trace("ff8anim f=%u cmd=%p id=%d %d/%d %s t=%.2f flags=%02x -> %d\n", ff8_interp::frame_counter(), cmd, cmd[CMD_ANIM_ID], cmd[CMD_CURRENT_FRAME], cmd[CMD_TOTAL_FRAMES], tick_real ? "READ" : "interp", t, cmd[1], s.last_result);
			return s.last_result;
		}

		// Skipped logic frame for one entity part: show the pose between the last two decoded frames.
		void interpolate_part(uint8_t* ctx, uint8_t* cmd)
		{
			uint8_t* pose = pose_of(ctx);
			if (pose == nullptr || pose[0] == 0) return;
			auto it = states.find(cmd);
			if (it == states.end() || !it->second.valid || it->second.anim_id != cmd[CMD_ANIM_ID]) return;
			State& s = it->second;
			float t = tick_alpha;
			if (t < 1.0f)
				write_interpolated(pose, s.prev, s.next, t);
			else
				restore(pose, s.next);
			build_bone_matrices(ctx);
			capture(pose, s.shown); s.shown_valid = true;
		}

		// Replacement for BattleEntity_RunSequence (0x504290): the sec-5 sequence VM, wait counters,
		// hit/damage triggers and the animation advance all live here, so it must run at 15 fps.
		inline int16_t lerp_i16(int16_t a, int16_t b, float t) { return lerp_linear(a, b, t); }

		void write_interpolated_pos(uint8_t* entity, const EntityState& es, float t)
		{
			int16_t v[POS_FIELDS];
			for (int k = 0; k < POS_FIELDS; k++) v[k] = (t >= 1.0f) ? es.next[k] : lerp_i16(es.prev[k], es.next[k], t);
			restore_pos(entity, v);
		}

		void __cdecl seq_driver_hook(uint8_t* entity)
		{
			update_tick();
			EntityState& es = entity_states[entity];
			if (tick_real)
			{
				// The logic must see the exact last tick's position, not the displayed blend.
				if (es.valid) restore_pos(entity, es.next);
				original_seq_driver(entity);
				int16_t now[POS_FIELDS];
				capture_pos(entity, now);
				if (!es.valid)
				{
					memcpy(es.prev, now, sizeof(now));
					es.valid = true;
				}
				else
				{
					memcpy(es.prev, es.next, sizeof(es.prev));
				}
				memcpy(es.next, now, sizeof(now));
				bool snapped = false;
				for (int k = 0; k < POS_FIELDS; k++)
				{
					int d = int(es.next[k]) - int(es.prev[k]);
					if (d > ENTITY_POS_SNAP || d < -ENTITY_POS_SNAP) { memcpy(es.prev, es.next, sizeof(es.prev)); snapped = true; break; }
				}
				if (ff8_interp::cfg().trace && (memcmp(es.next, es.prev, sizeof(es.next)) != 0 || snapped))
					ff8_interp::log_trace("ff8pos f=%u ent=%p ent(x,yo,z,y)=(%d,%d,%d,%d) seq(z,x,y)=(%d,%d,%d) d=(%d,%d,%d) rot=(%d,%d,%d)%s\n", ff8_interp::frame_counter(), entity,
						es.next[0], es.next[1], es.next[2], es.next[3], es.next[4], es.next[5], es.next[6],
						es.next[4] - es.prev[4], es.next[5] - es.prev[5], es.next[6] - es.prev[6],
						*(int16_t*)(entity + 0x0C), *(int16_t*)(entity + 0x0E), *(int16_t*)(entity + 0x10), snapped ? " SNAP" : "");
				write_interpolated_pos(entity, es, tick_alpha);
				return;
			}
			// Keep the driver's "current entity" globals coherent for whoever reads them after the call.
			*(uint8_t**)ADDR_CURRENT_ENTITY = entity;
			*(uint8_t**)ADDR_CURRENT_CMD = entity + ENTITY_CMD;
			*(uint8_t**)ADDR_CURRENT_SEQ = *(uint8_t**)(entity + ENTITY_SEQ);

			uint8_t* weapon = *(uint8_t**)(entity + ENTITY_WEAPON);
			if (weapon != nullptr) interpolate_part(weapon + WEAPON_CTX, weapon + WEAPON_CMD);
			interpolate_part(entity + ENTITY_CTX, entity + ENTITY_CMD);
			if (es.valid) write_interpolated_pos(entity, es, tick_alpha);
		}

		// ---- magic / GF effect tick and ATB -------------------------------------------------------
		// Every active spell or GF effect is one task tree whose root pointer lives in 0x1D96AAC; the
		// battle frame driver 0x500900 ticks it once per frame through ExecuteTaskQueue (call site
		// 0x50093A, cdecl, returns 0 when the effect is done). The tick also DRAWS: the effect rebuilds
		// its geometry into the battle render list every frame, there is no separate render pass, so
		// skipping it drops the effect for that frame. Instead the tick runs only on logic frames and the
		// primitives it linked are re-issued on the frames in between, so the effect advances at 15 fps
		// while it is still drawn at the host rate. Command type = byte +1 of the action data.
		constexpr uint32_t CS_EFFECT_TICK = 0x50093A;
		constexpr uint32_t FN_EXEC_TASK_QUEUE = 0x508420;
		uint8_t** const BATTLE_ACTION_DATA = (uint8_t**)0x1D99A50;
		// Action data (*0x1D99A50): +1 command type, +2 anim state, +4 command arg, +6 effect id.
		// Command types: 0x00 physical, 0x02 magic cast, 0x06 draw (casting from the Draw menu also
		// runs the magic path), 0x26/0xF4/0xFE GF cinematic, 0xEC/0xF5 Gilgamesh.
		constexpr uint32_t ACT_CMD = 1, ACT_ANIM_STATE = 2, ACT_EFFECT_ID = 6;
		inline bool cmd_is_gf(uint8_t c) { return c == 0x26 || c == 0xF4 || c == 0xFE || c == 0xEC || c == 0xF5; }
		// A spell effect is playing when the action carries an effect id and is not a physical hit or a
		// summon cinematic. The command type alone is not enough: a spell cast straight from the Draw
		// menu keeps command type 0x06 and still plays its effect through this tick, and stocking with Draw
		// rides the same path - skipping and replaying rolls nothing back, so neither needs an exclusion.
		inline bool holdable_action(uint8_t* action)
		{
			if (action == nullptr) return false;
			uint8_t cmd = action[ACT_CMD];
			uint16_t effect = *(uint16_t*)(action + ACT_EFFECT_ID);
			// Physical attacks were excluded here on the assumption that they carry no effect. They do:
			// impact flashes and the gunblade trigger explosion ride a physical action, and stayed at
			// host rate because of this rule. Only the absence of an effect id disqualifies an action.
			if (effect == 0) return false;
			// Summon cinematics hang when their tick is skipped (Quezacotl freezes as it spawns), so they
			// stay at host rate until the cinematic path gets its own treatment via its paused flag.
			if (cmd_is_gf(cmd)) return false;
			return true;
		}
		// ExecuteTaskQueue(queue) walks a linked list: queue[0] = head node, queue[1] = tail; each node
		// has its next pointer at +4 and its tick function at +8, and its context follows inline.
		// ATB gauges + GF countdown: pure logic, one call site in the HUD phase 0x4A84E0.
		constexpr uint32_t CS_ATB_TICK = 0x4A87D6;
		constexpr uint32_t FN_ATB_TICK = 0x4842B0;

		// The spell's effect tree is the value the magic handlers store into the effect root when they
		// call the spell's logic callback (0x50B190 case 3: root = (*0x21DFEC4)(&0x1D99A78)). Capturing
		// it there is the only reliable way to tell a spell effect from the other trees the same driver
		// ticks (action sequences, draw, summons), since they all pass through the one call site.
		constexpr uint32_t ADDR_MAGIC_EFFECT_TICK = 0x50B190;
		const uint8_t MAGIC_EFFECT_PROLOGUE[6] = { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x57 };
		constexpr uint32_t ADDR_MAGIC_CAST_TICK = 0x50A9A0;
		const uint8_t MAGIC_CAST_PROLOGUE[7] = { 0x53, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x57 };
		uint32_t* const EFFECT_ROOT = (uint32_t*)0x1D96AAC;

		typedef int (__cdecl *magic_tick_t)(uint8_t*);
		magic_tick_t orig_magic_effect_tick = nullptr;
		magic_tick_t orig_magic_cast_tick = nullptr;
		void* registered_root = nullptr;
		uint32_t logic_frame_no = 0;

		typedef int (__cdecl *exec_queue_t)(void*);
		typedef void (__cdecl *atb_tick_t)();

		exec_queue_t orig_exec_queue = (exec_queue_t)FN_EXEC_TASK_QUEUE;
		atb_tick_t orig_atb_tick = (atb_tick_t)FN_ATB_TICK;
		void* holdable = nullptr;   // the tree a real tick has already run, under a magic cast
		void* last_ctx = nullptr;
		void* held_ctx = nullptr;   // the tree currently being skipped, and for how many frames
		uint32_t held_frames = 0;

		bool range_writable(void* p, size_t n)
		{
			MEMORY_BASIC_INFORMATION mbi;
			uint8_t* a = (uint8_t*)p;
			uint8_t* end = a + n;
			while (a < end)
			{
				if (VirtualQuery(a, &mbi, sizeof(mbi)) == 0) return false;
				if (mbi.State != MEM_COMMIT) return false;
				if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
				if ((mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) return false;
				a = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
			}
			return true;
		}

		// The snapshot hold that used to live here - a memory window around the queue header, restored
		// after the tick - is gone. Widening it to cover every node the list reaches (tried in v10) spans a
		// large part of the battle heap and restoring that much state froze the battle outright, and even
		// the tight window hung the game; skipping the tick and replaying its primitives replaced it.

		// Display-list replay. The PSX-style draw list is built by a family of linker functions that all
		// take a 24-byte link node from the per-frame arena at 0x1CA8828, point it at a primitive and
		// chain it into a depth bucket. The primitive itself is the caller's memory (inside the effect's
		// own render data), so the calls an effect makes during a logic frame can be re-issued verbatim
		// on the frames in between: same geometry, no state advanced, nothing restored.
		// Different effects use different linkers (Fire goes through 0x45C870, others do not), so every
		// plain-cdecl linker in the family is recorded. Arguments are forwarded as eight dwords, which
		// is safe for cdecl whatever the real arity is.
		// FX_LINKERS (= 5) now lives in internal.h, together with FX_PRIM_WORDS and FxCall, because
		// fx_blend.cpp has to see the same shape. Every linker in the array is plain cdecl and proven
		// in game.
		// 0x45CEE0 and 0x45D080 are deliberately NOT in it: both open with a register push, so their calling
		// convention is not certain, and hooking 0x45D080 crashed inside it on battle entry (opt-in removed
		// in v59). That only matters if some effect draws exclusively through them.
		const uint32_t FX_LINKER_ADDR[FX_LINKERS] = { 0x45C7A0, 0x45C860, 0x45C870, 0x45C8E0, 0x45D310 };
		const uint8_t FX_LINKER_PROLOGUE[FX_LINKERS][7] = {
			{ 0xA1, 0x28, 0x88, 0xCA, 0x01, 0x00, 0x00 },
			{ 0x8B, 0x0D, 0x28, 0x88, 0xCA, 0x01, 0x00 },
			{ 0xA1, 0x28, 0x88, 0xCA, 0x01, 0x00, 0x00 },
			{ 0xA1, 0x28, 0x88, 0xCA, 0x01, 0x00, 0x00 },
			{ 0x83, 0xEC, 0x08, 0x53, 0x55, 0x56, 0x00 },
		};
		const size_t FX_LINKER_PROLOGUE_LEN[FX_LINKERS] = { 5, 6, 5, 5, 6 };

		// Ghidra types these void, but 0x45C7A0 / 0x45C870 / 0x45C8E0 all leave the bump cursor in EAX
		// (`mov eax,[0x1CA8828] … add eax,0x18 … mov [0x1CA8828],eax; ret`, and the bounds-check exit
		// returns the unchanged cursor). A review of all 2852 call sites found no caller that reads it,
		// but the deferral is the one place where our code can return WITHOUT the engine's linker having
		// run, so the value is carried through rather than left to chance: the wrappers return uint32_t,
		// pass the trampoline's EAX straight out, and the deferred path returns the cursor - exactly what
		// the engine leaves when it links nothing.
		uint32_t* const OT_ARENA_CURSOR = (uint32_t*)0x1CA8828;
		typedef uint32_t (__cdecl *fx_linker_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
		fx_linker_t orig_fx_linker[FX_LINKERS] = {};

		// A recorded link call, plus a copy of the primitive packet as it looked on that logic frame.
		// The packet's first word is the PSX tag: high byte = length in words, low 24 bits = the next
		// pointer the linker patches. Keeping the copy lets a skipped frame draw a blend of the last
		// two logic frames instead of repeating the newest one, which is what makes particle motion
		// smooth rather than stepped. `FxCall` and `FX_PRIM_WORDS` are declared in internal.h so that
		// fx_blend.cpp can read both recorded lists in place instead of being handed a copy of each.
		std::vector<FxCall> fx_prims, fx_prims_prev;
		bool fx_replaying = false;
		constexpr size_t FX_MAX_PRIMS = 16384;

		// Transient packets. 0x5082B0 is a LIFO stack allocator (cursor 0x1D999C4, rewound by 0x5082D0
		// within the same frame), and anything it hands out is dead by the next frame. Replaying a link
		// call whose primitive came from there points the display-list walker at recycled memory, which
		// is the 0x45D0FB crash. The stack's base is not a constant, so it is calibrated at runtime: the
		// lowest cursor value ever seen is the base, and a primitive between that and the live cursor is
		// by definition a live transient allocation. Such calls are never recorded, so never replayed.
		uint32_t* const PRIM_STACK_CURSOR = (uint32_t*)0x1D999C4;
		uint32_t prim_stack_low = 0xFFFFFFFF;

		inline bool prim_is_transient(uint32_t prim)
		{
			uint32_t cur = *PRIM_STACK_CURSOR;
			if (cur == 0) return false;
			if (cur < prim_stack_low) prim_stack_low = cur;
			return prim >= prim_stack_low && prim < cur;
		}

		std::vector<FxCall>* fx_record_target = nullptr;

		// Set while a logic frame's effect tick is being recorded AND its link calls are being held back,
		// so that the list which reaches the ordering table is the interpolated one issued after the tick
		// (design rule 2.1: blend on the logic frame too, or the effect jumps a tick ahead and snaps back).
		// It is cleared the moment a call arrives that cannot be recorded: such a call goes straight to
		// the engine, so everything held back has to be issued first or it would end up behind it in the
		// depth bucket.
		bool fx_defer_links = false;
		uint32_t fx_flushes = 0; // times a non-recordable call forced the held-back list out mid-tick
		void fx_replay_list(const std::vector<FxCall>& list, const std::vector<FxCall>* prev, int slot, bool allow_blend);

		// `ret` is the game's own call site (captured in the wrappers below, before anything can clobber
		// it). It is recorded per call purely as a diagnostic: it names which emitter produced each
		// packet, which is the only way to tell from a log whether a given piece of an effect is drawn
		// through the effect tree at all.
		inline uint32_t fx_linker_common(int fn, uint32_t ret, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
		{
			// DBG_FX_BLEND_OFF (512) switches off everything the blending work ADDS to v59 - the pairing
			// and prepare, the scratch substitution, the deferral (fx_blend_defer_ok() already returns
			// false) and the diagnostic call-site capture below - but deliberately NOT the recording or
			// the verbatim replay: those are v59's own effect pacing, and without them the skipped frames
			// would draw nothing and every spell would strobe at the logic rate. The bit has to stay
			// usable as a safe mode, not just as a bisect tool.
			const bool blend_on = fx_blend_enabled();
			// fn 4 (0x45D310) is the ordering-table renderer, not a plain linker: replaying it re-walks a
			// stale bump-arena root and calls primitive handlers on recycled memory. Never record it.
			bool recorded = false;
			if (fn != 4 && fx_record_target != nullptr && !fx_replaying && !prim_is_transient(a1)
				&& fx_record_target->size() < FX_MAX_PRIMS)
			{
				FxCall c = { fn, { a0, a1, a2, a3, a4, a5, a6, a7 }, (uint32_t*)a1, 0, {} };
				// The primitive is the second argument for every linker in this family.
				if (c.prim != nullptr && range_writable(c.prim, 4))
				{
					int words = int(c.prim[0] >> 24);
					if (words > 0 && words < FX_PRIM_WORDS && range_writable(c.prim, (words + 1) * 4))
					{
						c.words = words + 1;
						memcpy(c.copy, c.prim, c.words * 4);
					}
				}
				// Diagnostic only; not captured when the blending is off, so a 512 run records exactly the
				// FxCall v59 recorded. (The intrinsic in the wrapper is a plain read of the return-address
				// slot and has no side effect of its own.)
				c.ret = blend_on ? ret : 0;
				fx_record_target->push_back(c);
				recorded = true;
			}
			if (fx_defer_links)
			{
				// Issued after the tick, from the blended replay in effect_tick_hook. EAX must still look
				// like a linker return, and "linked nothing" is what the engine's own bounds-check exit
				// leaves: the unchanged cursor.
				if (recorded) return *OT_ARENA_CURSOR;
				// Not recordable (the ordering-table renderer, a transient packet, an overlong list): it has
				// to keep its place, so flush what is held back - verbatim, the list is still incomplete -
				// and let the rest of this tick through unchanged.
				fx_defer_links = false;
				fx_flushes++;
				if (fx_record_target != nullptr) fx_replay_list(*fx_record_target, nullptr, 0, false);
			}
			return orig_fx_linker[fn](a0, a1, a2, a3, a4, a5, a6, a7);
		}
		uint32_t __cdecl fx_linker0(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) { return fx_linker_common(0, (uint32_t)(uintptr_t)_ReturnAddress(), a0, a1, a2, a3, a4, a5, a6, a7); }
		uint32_t __cdecl fx_linker1(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) { return fx_linker_common(1, (uint32_t)(uintptr_t)_ReturnAddress(), a0, a1, a2, a3, a4, a5, a6, a7); }
		uint32_t __cdecl fx_linker2(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) { return fx_linker_common(2, (uint32_t)(uintptr_t)_ReturnAddress(), a0, a1, a2, a3, a4, a5, a6, a7); }
		uint32_t __cdecl fx_linker3(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) { return fx_linker_common(3, (uint32_t)(uintptr_t)_ReturnAddress(), a0, a1, a2, a3, a4, a5, a6, a7); }
		uint32_t __cdecl fx_linker4(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) { return fx_linker_common(4, (uint32_t)(uintptr_t)_ReturnAddress(), a0, a1, a2, a3, a4, a5, a6, a7); }
		void* const FX_LINKER_HOOK[FX_LINKERS] = { (void*)fx_linker0, (void*)fx_linker1, (void*)fx_linker2, (void*)fx_linker3, (void*)fx_linker4 };

		// The link nodes come from a bump arena (cursor 0x1CA8828, 0x60000 dwords from 0x1C48828).
		// 0x45C870 bounds-checks it, but 0x45D080 does not and faults once it is full - and replaying
		// several lists per frame consumes it far faster than the engine ever does. Never replay past
		// a safety margin.
		// 0x45C870 checks `cursor - 0x1C48828 < 0x60000` in BYTES (sub ecx,0x1C48828 / cmp ecx,0x60000),
		// so the arena is 0x1C48828..0x1CA8828. Multiplying the size by four here (v45) made the guard
		// useless and the arena still overran into 0x45D080, which does not bounds-check and faults.
		constexpr uint32_t OT_ARENA_END = 0x1C48828 + 0x60000;
		constexpr uint32_t OT_NODE_BYTES = 24;
		constexpr uint32_t OT_ARENA_MARGIN = 0x4000; // leave room for whatever still has to draw

		inline bool ot_arena_room(size_t nodes)
		{
			uint32_t cursor = *OT_ARENA_CURSOR;
			if (cursor >= OT_ARENA_END) return false;
			uint32_t left = OT_ARENA_END - cursor;
			return left > OT_ARENA_MARGIN + nodes * OT_NODE_BYTES;
		}

		// Issue a recorded list into the ordering table. With `allow_blend` (and a previous list to pair
		// it against) fx_blend.cpp interpolates the vertex dwords of every call it can pair and this links
		// its scratch copies instead of the effect's own packets - v60, the rebuilt version of the path
		// that was removed in v25. fx_blend_prepare() runs before the first link call goes out, which is
		// the only moment its scratch may grow, and nothing the ordering table points at is ever resized.
		// When the two lists do not pair (different length, a different linker or primitive type at some
		// index, a packet that did not move with the rest) it returns false and this stays the verbatim
		// replay it has always been.
		void fx_replay_list(const std::vector<FxCall>& list, const std::vector<FxCall>* prev, int slot, bool allow_blend)
		{
			// No bulk pre-check (v68). The per-call ot_arena_room(1) below already stops short with the
			// full margin, and refusing the whole list up front threw away an entire deferred logic
			// frame's geometry - something the engine itself never does, since its linkers bounds-check
			// per call and drop only the overflow.
			if (list.empty()) return;
			const bool blend = allow_blend && prev != nullptr
				&& fx_blend_prepare(slot, prev->data(), prev->size(), list.data(), list.size(), tick_alpha);
			fx_replaying = true;
			for (size_t i = 0; i < list.size(); i++)
			{
				const FxCall& c = list[i];
				if (orig_fx_linker[c.fn] == nullptr) continue;
				if (!ot_arena_room(1)) break; // the arena filled up mid-replay
				if (prim_is_transient(c.a[1])) continue; // became a live transient allocation since recording
				uint32_t arg_prim = c.a[1];
				if (blend)
				{
					// 0 = no interpolated copy for this call (0x45C860 takes no primitive, the command byte is
					// not one of the eight polygon types, or the packet is shorter than its own field mask).
					const uint32_t scratch = fx_blend_arg(slot, i);
					if (scratch != 0) arg_prim = scratch;
				}
				orig_fx_linker[c.fn](c.a[0], arg_prim, c.a[2], c.a[3], c.a[4], c.a[5], c.a[6], c.a[7]);
			}
			fx_replaying = false;
		}
		void fx_replay() { fx_replay_list(fx_prims, &fx_prims_prev, 0, true); }

		// Damage and heal popups animate over ten frames and draw inside their own tick, like the
		// effects. The task node is small and self-contained, so a bounded snapshot of it is safe here
		// (unlike the effect trees): the popup draws every frame and steps at the logic rate.
		constexpr uint32_t ADDR_DAMAGE_TICK = 0x5069B0;
		const uint8_t DAMAGE_TICK_PROLOGUE[6] = { 0x53, 0x55, 0x56, 0x57, 0x6A, 0x18 }; // push ebx/ebp/esi/edi, push 18h
		constexpr size_t DAMAGE_NODE_SIZE = 0x20; // highest field the tick touches is +0x1C
		typedef int (__cdecl *damage_tick_t)(uint8_t*);
		damage_tick_t orig_damage_tick = nullptr;

		int __cdecl damage_tick_hook(uint8_t* node)
		{
			if (node == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
				return orig_damage_tick(node);
			update_tick();
			if (tick_real) return orig_damage_tick(node);
			uint8_t saved[DAMAGE_NODE_SIZE];
			memcpy(saved, node, DAMAGE_NODE_SIZE);
			int r = orig_damage_tick(node); // draws this frame
			memcpy(node, saved, DAMAGE_NODE_SIZE);
			return r & ~2; // the restore undid the completion the held tick reported
		}

		// The battle main loop calls the HUD update 0x4A84E0 FOUR times per rendered frame (three hidden
		// passes with rendering off, then a visible one), which is how UI timers, the typewriter text and
		// the gauges reach their intended 60 ticks per second on a 15 fps engine.
		// Gating those four calls (sites 0x47D0A2/AC/B6 and 0x47D146) was implemented and REMOVED in v59:
		// skipping a pass removes the HUD drawing and replaying the recorded primitives does not reproduce
		// it - the 2D window layer does not go through the primitive linkers - so the gauges, names and the
		// menu flickered. What the UI really needed is gated one level down instead: the UI clock 0x4A8E30,
		// the window update 0x4B9C80, the menu callback 0x4BB9E0 and the pad reads below.

		// UI clock. `0x4A8E30` runs immediately before each of the four HUD passes and increments the UI
		// frame counter at 0x1D6D4A4, so it advances 4 x per rendered frame - 60 per second on the 15 fps
		// engine. Everything that pulses reads it: the ready-blink on a full ATB bar, the spinning arrow
		// over an active character, the cursor flash and the key-repeat timing. At host rate it runs ten
		// times too fast. The helper also sets up pointers the HUD needs, so it always runs and only the
		// counter is held back.
		// Hooked at its call sites, not as a function: its first instructions include a relative call,
		// and copying that into a trampoline sends it to the wrong address (that crashed the game).
		constexpr uint32_t FN_UI_CLOCK = 0x4A8E30;
		const uint32_t CS_UI_CLOCK[4] = { 0x47D09D, 0x47D0A7, 0x47D0B1, 0x47D141 };
		typedef void (__cdecl *ui_clock_t)();
		ui_clock_t orig_ui_clock = (ui_clock_t)FN_UI_CLOCK;

		// The blink phase is a separate byte at ctx+0x2C (ctx = *0x1D6D490), stepped by the input latch
		// inside 0x4A8E30 at most once per call. Freezing the frame counter 0x1D6D4A4 (tried in v40)
		// starved the key-repeat window and hid the menu; this instead lets the engine keep real time and
		// republishes the blink byte at most once per logic frame. Nothing else reads our value: ctx+0x2C
		// has exactly one writer in the game and seven readers, all cosmetic masks.
		// v56: the engine increments ctx+0x2C INSIDE the latch branch (0x4A8E98/0x4A8EA7), i.e. once every
		// `window` (0x1D6D620, 4 normally) calls, and calls are one per rendered frame: vanilla is one step
		// per 4 logic frames in normal battle and one per logic frame in the 4-pass menu state.
		uint8_t blink_phase = 0;
		bool blink_valid = false, blink_pending = false;
		uint32_t blink_calls = 0, blink_last_step = 0; // ui_clock calls on tick frames, and the count at the last step

		// Key repeat / input latch window - TRIED AND REVERTED in v55, do not retry this way.
		// `0x4A8E30` latches input only when `0x1D6D4A4 - 0x1D6D628 >= 0x1D6D620`. The clock at
		// 0x1D6D4A4 advances once per call (trace: 145/s at 144 fps, i.e. one call per rendered frame
		// in normal battle - the three grouped call sites only run while `0x1CDBFE0 == 3`), and the
		// window 0x1D6D620 is 4 (constant at 0xB8A3E4, written by 0x4A9220 at its only call site
		// 0x50062D). So the latch fires ~36/s at 144 fps against ~3.75/s on the native engine, which is
		// why the menu cursor overshoots.
		// v54 rescaled the window by the measured clock units per logic tick. That is the WRONG lever:
		// the latch also advances the UI ring buffer (`0x4A97F0`: index 0x1D750B8, four 0x40-byte slots
		// at 0x1D74EB0, current slot published to 0x1D75034) and republishes the pad into ctx+4/+6 and
		// ctx+0x21, all of which the HUD draw needs every rendered frame. Starving it reproduced the v40
		// symptoms exactly: menu/name/ATB flicker invisible and the gunblade trigger stopped registering
		// (Squall back to 0 damage). A correct fix has to gate the CONSUMER of the direction input, not
		// the latch.
		void __cdecl ui_clock_hook()
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle || (ff8_interp::cfg().battle_debug & DBG_BLINK_OFF))
			{
				orig_ui_clock();
				return;
			}
			update_tick();
			uint8_t* before = *(uint8_t**)0x1D6D490;
			uint8_t prev = ((uintptr_t)before > 0x10000) ? before[0x2C] : 0;
			orig_ui_clock(); // always: it refreshes the context pointer and runs the input latch
			uint8_t* ctx = *(uint8_t**)0x1D6D490;
			if ((uintptr_t)ctx <= 0x10000) return;
			if (!blink_valid) { blink_phase = ctx[0x2C]; blink_valid = true; blink_calls = blink_last_step = 0; return; }
			if (ctx == before && ctx[0x2C] != prev) blink_pending = true; // the engine stepped it this pass
			if (tick_real) blink_calls++;
			uint32_t window = *(uint32_t*)0x1D6D620;
			if (window < 1) window = 1;
			if (blink_calls - blink_last_step >= window) { blink_phase++; blink_pending = false; blink_last_step = blink_calls; }
			ctx[0x2C] = blink_phase;
		}

		// Results screens (EXP / AP / items). They live in their own module whose logic driver 0x4A3EE0 is
		// called once per rendered frame from 0x4A3E5E; nothing in the battle module reaches it. The module
		// is written for 60 logic ticks per second, so this drives it from a wall-clock accumulator instead
		// of the host frame rate. OFF by default (DBG_RESULTS_GATE_ON opts in): the module is not actually running
		// at host rate - its main loop 0x4A2690 busy-waits on its own 60 fps timer (0x1D2BB80), which the
		// FFNx limiter never replaces - so this gate only matters once that busy-wait is neutralised.
		constexpr uint32_t CS_VICTORY_LOGIC = 0x4A3E5E;
		constexpr uint32_t FN_VICTORY_LOGIC = 0x4A3EE0;
		constexpr double VICTORY_PERIOD = 1.0 / 60.0;
		typedef void (__cdecl *victory_logic_t)();
		victory_logic_t orig_victory_logic = (victory_logic_t)FN_VICTORY_LOGIC;
		double victory_acc = 0.0;

		void __cdecl victory_logic_hook()
		{
			if (!(ff8_interp::cfg().battle_debug & DBG_RESULTS_GATE_ON))
			{
				orig_victory_logic();
				return;
			}
			victory_acc += ff8_tick::frame_delta();
			if (victory_acc < VICTORY_PERIOD) return;
			victory_acc -= VICTORY_PERIOD;
			if (victory_acc > VICTORY_PERIOD) victory_acc = VICTORY_PERIOD; // never burst after a stall
			orig_victory_logic();
		}

		// End of battle. Three things run per host frame between the victory pose and the results module:
		// a countdown byte at 0x1D27B0C stepped inside the battle state tick 0x47D490 (which also calls the
		// frame driver, so it must keep running - only the byte is rolled back), the fade trigger 0x47E030
		// (pure logic), and the fade task 0x501D10, whose node holds its progress at +0x0C. The fade still
		// draws every frame; only its six progress bytes are held. A watchdog stops holding the countdown
		// after a couple of seconds so a battle can never fail to exit.
		constexpr uint32_t ADDR_BATTLE_STATE = 0x47D490;
		const uint8_t BATTLE_STATE_PROLOGUE[5] = { 0xA1, 0x48, 0xF8, 0xCF, 0x01 };
		constexpr uint32_t ADDR_FADE_TRIGGER = 0x47E030;
		const uint8_t FADE_TRIGGER_PROLOGUE[5] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };
		constexpr uint32_t ADDR_FADE_TICK = 0x501D10;
		const uint8_t FADE_TICK_PROLOGUE[7] = { 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10 };
		int8_t* const BATTLE_END_COUNTDOWN = (int8_t*)0x1D27B0C;
		constexpr double END_HOLD_MAX_SECONDS = 2.5;

		typedef void (__cdecl *battle_state_t)();
		typedef void (__cdecl *fade_trigger_t)(int);
		typedef int (__cdecl *fade_tick_t)(uint8_t*);
		battle_state_t orig_battle_state = nullptr;
		fade_trigger_t orig_fade_trigger = nullptr;
		fade_tick_t orig_fade_tick = nullptr;
		double end_hold_seconds = 0.0;

		void __cdecl battle_state_hook()
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
			{
				orig_battle_state();
				return;
			}
			update_tick();
			int8_t before = *BATTLE_END_COUNTDOWN;
			orig_battle_state();
			int8_t after = *BATTLE_END_COUNTDOWN;
			if (before <= 0) { end_hold_seconds = 0.0; return; } // idle: nothing to pace
			if (tick_real || after != (int8_t)(before - 1)) return;
			end_hold_seconds += ff8_tick::frame_delta();
			if (end_hold_seconds > END_HOLD_MAX_SECONDS) return; // never block the exit
			*BATTLE_END_COUNTDOWN = before;
		}

		void __cdecl fade_trigger_hook(int slot)
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
			{
				orig_fade_trigger(slot);
				return;
			}
			update_tick();
			if (tick_real) orig_fade_trigger(slot); // pure logic, nothing drawn
		}

		int __cdecl fade_tick_hook(uint8_t* node)
		{
			if (node == nullptr || orig_fade_tick == nullptr) return 0;
			// node[0x11] == 0 selects the end-of-battle fade; the other flavour (0x501CE0: from 0 to 0x80
			// over +0x0E steps) is the intro fade started by task 0x3EB (0x506DE0) and by task 3.
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
				return orig_fade_tick(node);
			const bool intro = node[0x11] != 0;
			if (intro && (ff8_interp::cfg().battle_debug & DBG_INTRO_PACE_OFF))
				return orig_fade_tick(node);
			update_tick();
			if (tick_real) return orig_fade_tick(node);
			uint8_t saved[6];
			memcpy(saved, node + 0x0C, sizeof(saved)); // payload only: +0 flags, +4 next, +8 callback
			// v72: the intro flavour calls 0x47D910 (0x4A6CB0(0x1000) / 0x4A6CC0(1) behind 0x1CFF6E2 & 4)
			// when its step counter +0x0C equals 5, before incrementing it. Held on 5, every held frame
			// would fire it again, so a held frame shows step 4 instead (one step, ~66 ms, behind).
			if (intro && *(int16_t*)(node + 0x0C) == 5) *(int16_t*)(node + 0x0C) = 4;
			int r = orig_fade_tick(node);              // still draws this frame
			memcpy(node + 0x0C, saved, sizeof(saved));
			return r & ~2;
		}

		// Battle stage texture animation (the beach waves, waterfalls, conveyors, blinking lights).
		// `BS_UpdateTextureAnimation 0x50CB20` blits rectangles inside video memory every frame from the
		// stage's .X animation block. It has many call sites, so it is hooked as a function: prologue
		// `53 55 8B 6C 24 10` (6 bytes, clean boundary, no relative call), cdecl with two arguments
		// (callers do `add esp,8` and it ends in a plain ret). Skipping it between logic frames simply
		// leaves the last blit on screen, so nothing flickers.
		constexpr uint32_t ADDR_STAGE_TEXANIM = 0x50CB20;
		const uint8_t STAGE_TEXANIM_PROLOGUE[6] = { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x10 };
		typedef void (__cdecl *stage_texanim_t)(int, int);
		stage_texanim_t orig_stage_texanim = nullptr;

		void __cdecl stage_texanim_hook(int a, int b)
		{
			if (orig_stage_texanim == nullptr) return;
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
			{
				orig_stage_texanim(a, b);
				return;
			}
			update_tick();
			if (tick_real) orig_stage_texanim(a, b);
		}

		// Battle intro. Each entity's appear/entry state machine is stepped by 0x50C110 from the entity
		// task, once per HOST frame, which is why the party's entry animations and their sounds run ahead
		// of the rest of the intro. The state machine draws nothing of its own, but most of its states do
		// write the model colour, so a skipped frame cannot simply return (see appear_tick_hook below);
		// the first step of a freshly set state still runs immediately so nothing is delayed by a frame.
		constexpr uint32_t CS_APPEAR_TICK = 0x502B74;
		constexpr uint32_t FN_APPEAR_TICK = 0x50C110;
		// The battle flags dword. Bit 0 is FF8's own "frozen" flag: every fade and every effect runner
		// still writes its visuals while it is set but holds its own counter (verified at 0x570C88 and in
		// 0x50C410/0x50C4D0/0x50C560; 0x50C230/0x50C330 only half, see v72 in the hook). Nothing in the battle module ever writes bit 0,
		// so it is ours to drive - but strictly around a single call, never for a whole frame: 0x502AB0
		// tests it too and would stop calling 0x504290, which is where our own pose interpolation lives.
		// (BATTLE_FREEZE itself is defined below, outside this anonymous namespace, and declared in
		// internal.h so the sibling translation units can drive the same bit.)
		typedef int (__cdecl *appear_tick_t)(uint8_t*);
		appear_tick_t orig_appear_tick = (appear_tick_t)FN_APPEAR_TICK;

		int __cdecl appear_tick_hook(uint8_t* e)
		{
			if (e == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
				return orig_appear_tick(e);
			update_tick();
			if (tick_real || e[6] == 0) return orig_appear_tick(e);
			if (ff8_interp::cfg().battle_debug & DBG_DEATH_FADE_SKIP) return 0; // pre-v58 behaviour: skip the tick entirely
			// States 5, 8 and 9 (removal 0x50C5F0, 0x50C670, 0x50C6D0) do not test the freeze bit, so
			// they keep the plain skip - otherwise a corpse would be removed at host rate.
			if (e[5] == 5 || e[5] == 8 || e[5] == 9) return 0;
			// v72: the fade-in handlers 0x50C230 (states 1/2) and 0x50C330 (states 0xA/0xC) honour the
			// freeze bit only HALF way: they open with an UNCONDITIONAL `inc byte [e+6]` (0x50C245 /
			// 0x50C346) and only the second +1 at the end sits behind `test [0x1D96A9C],1`
			// (0x50C30D / 0x50C3E9). Vanilla ramps +2 per tick (15 steps = 8 ticks, 0.53 s); v58-v71
			// added +1 on every held host frame, so at 144 Hz the entry fade finished in ~1.4 ticks and
			// monsters popped in (and their intro sequence moved on) far ahead of the camera pan. Hold
			// e[6] across the frozen call and feed the handler the value the last tick drew (h - 2 ->
			// pre-inc h - 1 < 15), so a held frame redraws that tick's colour and can never complete the
			// state early. The other states only increment behind the bit; DBG_APPEAR_RAMP_OFF = v71.
			const uint8_t st = e[5];
			const bool pre_inc = (st == 1 || st == 2 || st == 0x0A || st == 0x0C)
				&& !(ff8_interp::cfg().battle_debug & DBG_APPEAR_RAMP_OFF);
			const uint8_t ramp = e[6];
			if (pre_inc) e[6] = ramp >= 2 ? uint8_t(ramp - 2) : 0;
			// One instruction before our call site, 0x502B5C -> 0x50A0A0 rewrites the model colour
			// e[0x28..0x2B] back to DAT_00B8B7D8 every HOST frame. The gated fade 0x50C560 is a pure
			// function of the ramp counter e[6] and only ran on logic ticks, so one frame in ten showed
			// the faded colour and the other nine the full-bright default - the reported flicker-glow.
			// Under the freeze bit the fade rewrites the colour on every host frame while e[6] is held,
			// so the fade looks continuous and still ramps at the logic rate.
			uint32_t freeze = *BATTLE_FREEZE;
			*BATTLE_FREEZE = freeze | 1;
			int r = orig_appear_tick(e);
			*BATTLE_FREEZE = freeze;
			if (pre_inc)
			{
				e[6] = ramp;
				if (ff8_interp::cfg().trace && ramp <= 2)
					ff8_interp::log_trace("ff8appear f=%u ent=%p state=%d ramp held at %d (pre-inc handler)\n", ff8_interp::frame_counter(), e, st, ramp);
			}
			return r;
		}

		// v72: battle intro task chain. The manager queue 0x1D96D78 (*0x1D96AA4, one call per host frame
		// at 0x50090B) runs the manager 0x500CC0 and its sub-tasks strictly one after another. During the
		// intro (0x1D27B04 < 3: set 0 by battle init, 1 by the task-10 callback 0x47DD80, 2 once the
		// entities exist, 3 by 0x47DD70 = ATB start) it only carries the loading chain: 0x3EA stage load
		// 0x506CF0 (starts the intro camera), task 1, 0x3EB 0x506DE0, task 9 0x500AC0, the 0x66 model
		// loads 0x502670, task 10 (entity creation), 0x67 0x5027D0, 0x70 0x5085F0 (waits for the camera
		// mode 0x1D97718 to clear) and task 10 again. All of them are logic or polls, none draws. At host
		// rate the chain created the entities ~0.3 s after the camera started instead of ~1 s, so
		// monsters appeared and the party began its ready animation early in the pan. During the intro
		// the queue now steps on logic ticks only, the cadence of the camera VM (battle_camera_hook);
		// from state 3 on it is untouched (action sequences etc. keep their own gates).
		constexpr uint32_t CS_MANAGER_QUEUE = 0x50090B;       // call 0x508420(*0x1D96AA4)
		uint32_t* const BATTLE_INTRO_STATE = (uint32_t*)0x1D27B04;
		typedef int (__cdecl *manager_queue_t)(void*);
		manager_queue_t orig_manager_queue = (manager_queue_t)FN_EXEC_TASK_QUEUE;
		uint32_t intro_held_frames = 0;

		int __cdecl manager_queue_hook(void* queue)
		{
			if (queue == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle
				|| (ff8_interp::cfg().battle_debug & DBG_INTRO_PACE_OFF) || *BATTLE_INTRO_STATE >= 3)
				return orig_manager_queue(queue);
			update_tick();
			if (tick_real)
			{
				if (ff8_interp::cfg().trace && intro_held_frames != 0 && (logic_ticks % 15) == 0)
					ff8_interp::log_trace("ff8intro f=%u manager queue on tick, intro state %u, held %u frames so far\n", ff8_interp::frame_counter(), *BATTLE_INTRO_STATE, intro_held_frames);
				return orig_manager_queue(queue);
			}
			intro_held_frames++;
			return 0; // the driver ignores the return at 0x50090B (next instruction reloads ecx)
		}

		// Battle captions (the text across the top). Task 8 only starts one; the display time lives in the
		// caption task callback 0x506F70, which decrements the byte at node+0x0E once per call and reports
		// "done" when it hits zero, while also raising the flags that make the frame driver show the
		// caption window at all. So the call has to happen every frame - only the countdown is held.
		constexpr uint32_t ADDR_CAPTION_TICK = 0x506F70;
		const uint8_t CAPTION_PROLOGUE[5] = { 0x8B, 0x44, 0x24, 0x04, 0x56 }; // mov eax,[esp+4] / push esi
		constexpr uint32_t CAPTION_TIMER = 0x0E;
		typedef int (__cdecl *caption_tick_t)(uint8_t*);
		caption_tick_t orig_caption_tick = nullptr;

		int __cdecl caption_tick_hook(uint8_t* node)
		{
			if (orig_caption_tick == nullptr) return 0;
			if (node == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
				return orig_caption_tick(node);
			update_tick();
			if (tick_real) return orig_caption_tick(node);
			uint8_t saved = node[CAPTION_TIMER];
			int r = orig_caption_tick(node);
			node[CAPTION_TIMER] = saved;
			return r & ~2; // the restore undid the completion a held frame would have reported
		}

		// Battle interface. `BattleUI_UpdateAllWindows 0x4B9C80` is the per-frame update pass over the
		// nine window slots (typewriter captions, menus, gauges); `BattleUI_DrawAllWindows 0x4B9DB0` is a
		// separate draw pass. Gating the update alone keeps every window drawn on every frame, and since
		// the HUD update calls it four times per rendered frame, running it only on logic frames yields
		// the 4 x 15 = 60 interface ticks per second the engine was written for.
		constexpr uint32_t ADDR_UI_UPDATE = 0x4B9C80;
		const uint8_t UI_UPDATE_PROLOGUE[8] = { 0x83, 0xEC, 0x10, 0xA1, 0x90, 0xD4, 0xD6, 0x01 }; // sub esp,10h / mov eax,[1D6D490]
		typedef void (__cdecl *ui_update_t)();
		ui_update_t orig_ui_update = nullptr;

		void __cdecl ui_update_hook()
		{
			if (orig_ui_update == nullptr) return;
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
			{
				orig_ui_update();
				return;
			}
			update_tick();
			if (!tick_real) return;
			orig_ui_update();
			// v66: capture the Renzokuken trigger bar's movers for hud_blend.cpp. tick_real is constant
			// for a whole host frame, so this runs on all FOUR HUD passes of a logic-tick frame, exactly
			// as the 15 fps engine ran them per rendered frame; the capture wants the state the LAST of
			// them leaves behind, so it is called after every one of them. See the marked block in
			// internal.h - it is inert unless the bar owns window slot 6.
			hud_blend_after_update();
		}

		// The real battle camera. `updateBattleCamera 0x504060` (call site 0x500988 in the frame driver)
		// steps the camera byte-code sequence, ticks its own task queue and writes the live camera pose:
		// position at 0xB8B7F0 and look-at at 0xB8B7F8, each three int16 plus padding, with the rest
		// pose at 0xB8B800 and a roll/zoom short at 0x1D8E038. The battle render (0x502D40, 0x5033E0)
		// reads that pose. Running it per host frame is what swept the camera about ten times too fast.
		// (0x500FD0, chased for several rounds before this, only animates camera dummies and does not
		// own the view.)
		constexpr uint32_t CS_BATTLE_CAMERA = 0x500988;
		constexpr uint32_t FN_BATTLE_CAMERA = 0x504060;
		int16_t* const CAM_POSE = (int16_t*)0xB8B7F0;   // 8 int16: position xyz + pad, look-at xyz + pad
		int16_t* const CAM_ROLL = (int16_t*)0x1D8E038;
		constexpr int CAM_POSE_WORDS = 8;
		typedef void (__cdecl *battle_camera_t)();
		battle_camera_t orig_battle_camera = (battle_camera_t)FN_BATTLE_CAMERA;

		struct CamPose { int16_t v[CAM_POSE_WORDS]; int16_t roll; };
		CamPose cam_pose_prev, cam_pose_next;
		bool cam_pose_valid = false;
		CamPose cam_published;             // what this hook last wrote to the pose (to spot foreign writes)
		bool cam_published_valid = false;

		void __cdecl battle_camera_hook()
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
			{
				orig_battle_camera();
				return;
			}
			update_tick();
			// A summon cinematic writes the pose itself (18 direct stores in 0x6C3940) and gf_pacing
			// blends it; the engine's own camera block is idle meanwhile, so blending here only
			// republished the pre-summon pose over the cinematic's every frame (v61-v63: camera stuck
			// facing off to the side). The original still ticks the camera task queue once per tick.
			if (gf_cinematic_owns_camera()) { if (tick_real) orig_battle_camera(); cam_pose_valid = false; return; }
			if (tick_real)
			{
				// The update may exit without writing the pose: never let it capture our blend. But only put
				// `next` back when the pose is still exactly what we published: a spell effect ticked earlier
				// in this frame (0x50093A runs before 0x500988) may have written the camera itself, and
				// restoring over that erased every effect-driven camera move (v57-v68: Blizzard's camera
				// lagging behind the spell, the same failure the GF cinematic had).
				const bool ours = cam_published_valid && memcmp(CAM_POSE, cam_published.v, sizeof(cam_published.v)) == 0 && *CAM_ROLL == cam_published.roll;
				if (cam_pose_valid && ours) { memcpy(CAM_POSE, cam_pose_next.v, sizeof(cam_pose_next.v)); *CAM_ROLL = cam_pose_next.roll; }
				orig_battle_camera();
				CamPose now;
				memcpy(now.v, CAM_POSE, sizeof(now.v));
				now.roll = *CAM_ROLL;
				// A cut (a new shot) moves the camera far in one tick: snap instead of sweeping through it.
				bool cut = !cam_pose_valid;
				for (int i = 0; i < 3 && !cut; i++)
					if (abs(int(now.v[i]) - int(cam_pose_next.v[i])) > 2048 || abs(int(now.v[4 + i]) - int(cam_pose_next.v[4 + i])) > 2048) cut = true;
				cam_pose_prev = cut ? now : cam_pose_next;
				cam_pose_next = now;
				cam_pose_valid = true;
				// Fall through and blend on this frame too. Leaving the freshly captured pose on screen
				// here and blending only on the frames in between makes every logic frame jump ahead and
				// the next frame snap back - the same jitter the world map had before its display pass
				// ran on every frame.
			}
			if (!cam_pose_valid) return;
			// Pure output: the update rewrites it from the sequence state on the next logic frame, so a
			// blended value here reaches only the renderer and cannot feed back.
			int16_t blended[CAM_POSE_WORDS];
			for (int i = 0; i < CAM_POSE_WORDS; i++) blended[i] = lerp_i16(cam_pose_prev.v[i], cam_pose_next.v[i], tick_alpha);
			memcpy(CAM_POSE, blended, sizeof(blended));
			*CAM_ROLL = lerp_i16(cam_pose_prev.roll, cam_pose_next.roll, tick_alpha);
			memcpy(cam_published.v, blended, sizeof(blended));
			cam_published.roll = *CAM_ROLL;
			cam_published_valid = true;
		}

		// Summon action sequence 0x50B2A0: a state machine on its task byte +0xD. States 0..3 are the
		// caster stepping into position and vanishing (no geometry of its own, it only sets animations
		// and waits); state 3 registers the cinematic tree and from state 4 on the cinematic runs and
		// draws. Pacing the early states fixes the rushed entrance while leaving the cinematic, which
		// hangs if its tick is skipped, running exactly as before.
		constexpr uint32_t ADDR_GF_SEQ_TICK = 0x50B2A0;
		const uint8_t GF_SEQ_PROLOGUE[6] = { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C }; // push ebx/ebp, mov ebp,[esp+0Ch]
		constexpr uint8_t GF_STATE_CINEMATIC = 4;
		constexpr uint32_t GF_TASK_STATE = 0x0D;
		typedef int (__cdecl *gf_seq_tick_t)(uint8_t*);
		gf_seq_tick_t orig_gf_seq_tick = nullptr;

		// v71: "a summon cinematic is live", for apply_results_hook. Every summon - Quezacotl and all the
		// GFs gf_pacing does not pace - goes through this one task, and from state 4 on (state 3 has just
		// registered the cinematic tree and falls through into 4) until the task ends (state 9 returns 2)
		// the cinematic runs. The task is called every host frame then (the summon is not holdable, the
		// effect tick runs it at host rate), so a stamp of the last frame it was seen in state >= 4 is
		// live within one frame's grace, like gf_cinematic_owns_camera(). The end of the task, a state
		// below 4 (a new summon's entrance) and animations_reset() clear it explicitly.
		uint32_t summon_live_frame = 0xFFFFFFFF;
		inline void summon_mark(const uint8_t* node, int r)
		{
			if (r == 2 || node[GF_TASK_STATE] < GF_STATE_CINEMATIC) summon_live_frame = 0xFFFFFFFF;
			else summon_live_frame = ff8_interp::frame_counter();
		}
		inline bool summon_cinematic_live()
		{
			return summon_live_frame != 0xFFFFFFFF && (ff8_interp::frame_counter() - summon_live_frame) <= 1;
		}

		int __cdecl gf_seq_tick_hook(uint8_t* node)
		{
			if (node == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
				return orig_gf_seq_tick(node);
			int r;
			if (node[GF_TASK_STATE] >= GF_STATE_CINEMATIC) r = orig_gf_seq_tick(node); // cinematic draws, leave it
			else
			{
				update_tick();
				if (!tick_real) { summon_mark(node, 0); return 0; } // still running, just not advanced on this frame
				r = orig_gf_seq_tick(node);
			}
			summon_mark(node, r);
			return r;
		}

		// GF summon damage. A summon cinematic applies its result on exactly one timeline frame (frame
		// 0x159 of the inner timeline 0x6C3940 calls Battle_ApplyActionResultToAllTargets 0x506BA0), while
		// the Boost gauge only publishes its multiplier from the window-6 update callback 0x56DD70, which
		// runs at the logic rate. With the cinematic tree still ticking at host rate the damage frame
		// arrives ~10x early, before the gauge reaches state 5, so 0x4850A0 has never run and TARGET_DATA
		// 0x1D28344 still holds the pre-boost damage - which is literally 0 for a boostable summon
		// (0x48D9FE zeroes the multiplier byte 0x1D2A23A at resolve time). Settling a pending gauge here
		// restores vanilla ordering. Interim fix: the cinematic's own pacing is still open, see
		// notes/research/battle-open-3.md A.6 (A-2).
		constexpr uint32_t ADDR_APPLY_RESULTS = 0x506BA0;
		const uint8_t APPLY_RESULTS_PROLOGUE[5] = { 0x57, 0x8B, 0x7C, 0x24, 0x0C }; // push edi / mov edi,[esp+0Ch]
		constexpr uint32_t FN_PUBLISH_BOOST = 0x4850A0; // void __cdecl (int): publishes the boost and recomputes every target
		uint16_t* const BOOST_VALUE = (uint16_t*)0x209CEF0;  // what the minigame accumulated, capped 0xFA
		uint8_t* const BOOST_ENABLED = (uint8_t*)0x209CEF8;  // non-zero while a boostable summon is armed
		uint8_t* const BOOST_STATE = (uint8_t*)0x209CEFB;    // 0x56DD70's state: <5 pending, 5 publish, 6 published
		uint8_t** const CUR_ACTION = (uint8_t**)0x1D99A50;   // -> the action being played: cmd id at +1, TARGET_DATA at +8, target count at +0x10
		constexpr uint8_t CMD_BOOSTABLE_SUMMON = 0xFE;       // the only command that arms the gauge (both 0x56DCE0 call sites, 0x50AC32 / 0x50B4C5, guard on it)
		typedef void (__cdecl *apply_results_t)(int, int);
		apply_results_t orig_apply_results = nullptr;

		void __cdecl apply_results_hook(int target_data, int target_count)
		{
			if (orig_apply_results == nullptr) return;
			if (ff8_interp::cfg().battle_debug & DBG_GF_BOOST_OFF)
			{
				orig_apply_results(target_data, target_count);
				return;
			}
			// Only a boostable summon may publish. Renzokuken's R1 trigger leaves the same gauge block armed,
			// and publishing there recomputed its targets to 0 damage - the v58 -> v62 Renzokuken regression.
			// 0x1D99A50 can be STALE (it still named the summon when Renzokuken armed the same gauge
			// block afterwards: v64 published there, disarmed the block, and Renzokuken skipped its
			// attacks and bar straight to a 0-damage finisher). Require the cinematic to be live too.
			// v64-v70 asked gf_cinematic_owns_camera(), which only Quezacotl's runner 0x6C3760 sets, so
			// every other boostable GF was back to the 0-damage race. v71 also accepts the summon task
			// 0x50B2A0 being in its cinematic states (summon_cinematic_live, any GF); it is cleared when
			// the task ends, so Renzokuken after a summon still cannot publish. Quezacotl satisfies both.
			// DBG_BOOST_ANY_GF_OFF restores the Quezacotl-only gate. v72: gf_cinematic_live() is any GF
			// that gf_pacing.cpp paces (Quezacotl, Shiva), set by its root runner on every host frame.
			//
			// v72: NO `*BOOST_ENABLED != 0` test any more. 0x50B2A0 state 3 arms the gauge for EVERY 0xFE
			// summon - 0x56DCE0(*0x1D28DF7, action+4) - but the enable byte 0x209CEF8 is that first
			// argument, i.e. whether the Square minigame is offered, and it is 0 for a GF that has not
			// learned Boost. The gauge still runs its timers and still publishes in case 5 (v = 0 -> 100)
			// with the minigame off, and until then the multiplier byte 0x1D2A23A is the 0 that
			// 0x48D9FE wrote at resolve time. v58-v71 therefore never settled a Boost-less GF - the
			// early Shiva (eff 185) 0-damage report of v71. 0x56DCE0 always sets the state byte
			// 0x209CEFB to 0 and publishing sets it to 6, so `state < 5` alone is "armed, not yet
			// published"; the 0xFE command and the live cinematic keep Renzokuken out as before.
			const uint8_t* action = *CUR_ACTION;
			const bool live = gf_cinematic_owns_camera() || gf_cinematic_live()
				|| (!(ff8_interp::cfg().battle_debug & DBG_BOOST_ANY_GF_OFF) && summon_cinematic_live());
			if (live && action != nullptr && action[1] == CMD_BOOSTABLE_SUMMON && *BOOST_STATE < 5)
			{
				if (ff8_interp::cfg().trace)
					ff8_interp::log_trace("ff8gf boost settle f=%u value=%u enabled=%u state=%u\n", ff8_interp::frame_counter(), (unsigned)*BOOST_VALUE, (unsigned)*BOOST_ENABLED, (unsigned)*BOOST_STATE);
				// Exactly what 0x56DD70 case 5 does, and idempotent: the state byte guards it.
				uint16_t v = *BOOST_VALUE;
				if (v == 0) v = 100; // no Square presses -> the neutral multiplier, never 0
				((void(__cdecl*)(int))FN_PUBLISH_BOOST)((int)v);
				*BOOST_STATE = 6;
				// Disarm the gauge block like the engine's own case 5 does. Leaving it armed let the NEXT
				// action re-enter here (0x1D99A50 still pointed at the summon): Blizzard cast after
				// Quezacotl was republished against the summon's targets and did 0 damage (v63).
				*BOOST_ENABLED = 0;
				*(uint8_t*)0x209CEF9 = 0;
			}
			else if (ff8_interp::cfg().trace && action != nullptr && action[1] == CMD_BOOSTABLE_SUMMON)
				ff8_interp::log_trace("ff8gf boost no-settle f=%u live=%d state=%u enabled=%u\n", ff8_interp::frame_counter(), (int)live, (unsigned)*BOOST_STATE, (unsigned)*BOOST_ENABLED);
			orig_apply_results(target_data, target_count);
		}

		int __cdecl magic_effect_tick_hook(uint8_t* a)
		{
			uint32_t before = *EFFECT_ROOT;
			int r = orig_magic_effect_tick(a);
			if (*EFFECT_ROOT != before && *EFFECT_ROOT != 0)
			{
				registered_root = (void*)*EFFECT_ROOT;
				if (ff8_interp::cfg().trace) ff8_interp::log_trace("ff8fx registered effect root %p (effect tick)\n", registered_root);
			}
			return r;
		}
		int __cdecl magic_cast_tick_hook(uint8_t* a)
		{
			uint32_t before = *EFFECT_ROOT;
			int r = orig_magic_cast_tick(a);
			if (*EFFECT_ROOT != before && *EFFECT_ROOT != 0)
			{
				registered_root = (void*)*EFFECT_ROOT;
				if (ff8_interp::cfg().trace) ff8_interp::log_trace("ff8fx registered effect root %p (cast tick)\n", registered_root);
			}
			return r;
		}

		// Besides the spell-effect root the battle frame driver ticks three visual queues - 0x506BF0 (16
		// slots, call site 0x500917), 0x56F9A0 (300 slots, the effect particle pool, 0x500923) and 0x506BD0
		// (20 slots, 0x500951) - which is what hit sprites and running dust ride on. Giving them the same
		// skip-and-replay treatment was implemented and REMOVED in v59: replaying a link call is only safe
		// when the primitive lives in the effect's own memory, which is true for the spell effect tree but
		// NOT for these queues - their primitives are allocated per frame from a rotating pool, so a replayed
		// frame hands the display-list walker 0x45D080 a pointer into recycled memory and it faults reading
		// the packet's length byte (0xC0000005 at 0x45D0FB). They stay at host rate; the one effect that
		// mattered, the impact/hit explosion, is paced by impact_fx_hook below through the engine's own
		// freeze bit instead.

		int __cdecl effect_tick_hook(void* ctx)
		{
			if (ctx == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle) return orig_exec_queue(ctx);
			update_tick();
			uint8_t* action = *BATTLE_ACTION_DATA;
			if (ff8_interp::cfg().trace)
			{
				static void* last_trace_ctx = nullptr;
				static uint8_t last_cmd = 0xFF;
				static uint16_t last_effect = 0xFFFF;
				uint8_t cmd = action ? action[1] : 0xFF;
				uint16_t effect = action ? *(uint16_t*)(action + ACT_EFFECT_ID) : 0xFFFF;
				if (ctx != last_trace_ctx || cmd != last_cmd || effect != last_effect)
				{
					ff8_interp::log_trace("ff8fx f=%u ctx=%p cmd=%02x eff=%u anim=%02x arg=%d hold=%p act=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
						ff8_interp::frame_counter(), ctx, cmd,
						action ? *(uint16_t*)(action + ACT_EFFECT_ID) : 0, action ? action[ACT_ANIM_STATE] : 0,
						action ? *(int16_t*)(action + 4) : 0, holdable,
						action ? action[0] : 0, action ? action[1] : 0, action ? action[2] : 0, action ? action[3] : 0,
						action ? action[4] : 0, action ? action[5] : 0, action ? action[6] : 0, action ? action[7] : 0,
						action ? action[8] : 0, action ? action[9] : 0, action ? action[10] : 0, action ? action[11] : 0,
						action ? action[16] : 0, action ? action[17] : 0, action ? action[18] : 0, action ? action[19] : 0);
					last_trace_ctx = ctx; last_cmd = cmd; last_effect = effect;
				}
			}
			bool magic = holdable_action(action);
			// v70 (DBG_RENZ_SYNC_OFF restores the old rule): a spell tree is paced from its very first
			// frame. Before, a tree only became `holdable` on the SECOND tick that ran it (ctx ==
			// last_ctx), so from registration (the cast tick runs at host rate, usually off-tick) until
			// then it ran on every host frame - ~10-19 effect frames in one or two ticks. The Renzokuken
			// modules (e.g. eff 160 = 0x5FF080, main task 0x5FF180) set Squall's sequence on their frame
			// 1 and read the trigger buckets the sequence's opcode (0x504E50 -> 0x4BA4C0 / 0x4BA560)
			// writes to 0x1D98214 on frame 2; the sequence runs only on ticks, so frame 2 read the
			// buckets of Squall's PREVIOUS gunblade attack, and the finisher variant (action+4 == -2)
			// links its finisher when the frame counter equals that stale sum: an instant Rough Divide.
			// research/renzokuken-instant-finisher.md.
			const bool sync = !(ff8_interp::cfg().battle_debug & DBG_RENZ_SYNC_OFF);
			if (tick_real)
			{
				// A tree is only held once a real tick has run it at least once: its first tick builds
				// the task list. Record the primitives this tick links so the skipped frames can
				// re-issue them. v70: a fresh tree's first tick is recorded too (without deferral - there
				// is no pair yet), so its skipped frames replay it instead of drawing nothing.
				const bool fresh = sync && magic && ctx != holdable;
				if (fresh) { fx_prims.clear(); fx_prims_prev.clear(); }
				bool record = magic && (ctx == holdable || fresh);
				if (record)
				{
					fx_prims_prev.swap(fx_prims);
					fx_prims.clear();
					fx_record_target = &fx_prims;
					// Hold this tick's link calls back so the ordering table gets the same interpolated list
					// the frames in between get (rule 2.1); without it the logic frame would jump a whole tick
					// ahead of the models and the camera and the next frame would snap back.
					// v63: only while blending is actually succeeding. If the pair is falling back there is
					// nothing to substitute, so holding the calls back would add risk for no gain - the tick
					// then runs exactly as it did in v59, engine-drawn and in place.
					fx_defer_links = !fresh && fx_blend_defer_ok();
				}
				int r = orig_exec_queue(ctx);
				if (record)
				{
					fx_record_target = nullptr;
					const bool deferred = fx_defer_links; // cleared early if a call could not be held back
					fx_defer_links = false;
					if (deferred) fx_replay_list(fx_prims, &fx_prims_prev, 0, true);
					if (ff8_interp::cfg().trace && (logic_frame_no++ % 15) == 0)
					{
						int per[FX_LINKERS] = {};
						for (const FxCall& c : fx_prims) per[c.fn]++;
						ff8_interp::log_trace("ff8fx rec n=%u by linker %d/%d/%d/%d/%d deferred=%d flushes=%u\n",
							(unsigned)fx_prims.size(), per[0], per[1], per[2], per[3], per[4], (int)deferred, fx_flushes);
					}
				}
				void* prev = holdable;
				holdable = (magic && (ctx == last_ctx || sync)) ? ctx : nullptr;
				// A different tree: neither recorded list describes it any more, and pairing lists that
				// belong to two different effects would interpolate unrelated geometry. (A fresh tree
				// already cleared them before recording its first tick.)
				if (holdable != prev && !fresh) { fx_prims.clear(); fx_prims_prev.clear(); }
				last_ctx = ctx;
				// v70: the tree ended (the driver clears the root on 0 and stops calling us). Forget it,
				// or the next cast of the same module - same static queue address - would be taken
				// for a held tree and replay the previous cast's primitives on its first frames.
				if (sync && r == 0) { holdable = nullptr; last_ctx = nullptr; fx_prims.clear(); fx_prims_prev.clear(); }
				return r;
			}
			// v70: a spell tree registered off-tick waits for the next tick instead of running at host
			// rate until its second tick. Nothing has run or been recorded yet, so there is nothing to
			// replay; returning non-zero keeps the root alive (0x500942).
			if (sync && magic && ctx != holdable) return 1;
			if (!magic || ctx != holdable) return orig_exec_queue(ctx);
			// Skip the effect entirely between logic frames. Nothing advances and nothing is restored, so no
			// state can be left inconsistent (the snapshot hold that used to sit here hung the game). The cost
			// is that the effect contributes no geometry on these frames: it is drawn at the logic rate until
			// the emitted prims are replayed instead.
			if (ctx != held_ctx) { held_ctx = ctx; held_frames = 0; }
			held_frames++;
			if (ff8_interp::cfg().trace && (held_frames % 120) == 0)
				ff8_interp::log_trace("ff8fx skip ctx=%p eff=%u frames=%u prims=%u\n", ctx, *(uint16_t*)(action + ACT_EFFECT_ID), held_frames, (unsigned)fx_prims.size());
			fx_replay();
			return 1;
		}

		// Pad polling inside the HUD phase 0x4A84E0. It runs every host frame while the menu state
		// machine steps at the logic rate, so a held button produces an edge on many menu steps:
		// confirm repeats into the next menu and the one after. Reads are cached between logic ticks,
		// which is what the engine sees in vanilla (one poll per logic frame): the engine's own
		// current/previous copy then reports one edge per press.
		constexpr uint32_t CS_PAD_REFRESH = 0x4A84F8, FN_PAD_REFRESH = 0x49E9C0;
		constexpr uint32_t CS_PAD_A = 0x4A84FF, FN_PAD_A = 0x49ED30;
		constexpr uint32_t CS_PAD_B = 0x4A851B, FN_PAD_B = 0x49ED80;
		constexpr uint32_t CS_PAD_C = 0x4A8537, FN_PAD_C = 0x49EDE0;

		typedef void (__cdecl *pad_refresh_t)();
		typedef uint32_t (__cdecl *pad_read_t)(int, int, int);
		pad_refresh_t orig_pad_refresh = (pad_refresh_t)FN_PAD_REFRESH;
		pad_read_t orig_pad_a = (pad_read_t)FN_PAD_A;
		pad_read_t orig_pad_b = (pad_read_t)FN_PAD_B;
		pad_read_t orig_pad_c = (pad_read_t)FN_PAD_C;
		uint32_t pad_cache[3] = {};
		uint32_t pad_acc[3] = {};
		uint32_t pad_pub_frame[3] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF }; // host frame of the last publish

		inline bool pad_bypass()
		{
			return ff8_interp::game_mode() != ff8_interp::GameMode::Battle;
		}
		// The pad is still read every host frame and the presses are accumulated, so a tap that begins
		// and ends between two logic frames is not lost - that matters for the gunblade R1 trigger and
		// for quick menu presses. What the engine sees is one stable value per logic frame, so its own
		// current/previous comparison still yields exactly one edge per press.
		inline uint32_t pad_step(int i, uint32_t now)
		{
			pad_acc[i] |= now;
			// Publish once per host frame. The battle loop runs FOUR HUD passes per rendered frame in
			// state 0x1CDBFE0 == 3 and the menu consumes the pad on every pass, so the EDGE channels
			// (1 = repeat word, 2 = pressed) may be delivered exactly once per tick frame: returning the
			// cached word to passes 2-4 (v56-v62) consumed every press four times - the cursor wrapped a
			// four-entry list back to Attack, cancel cycled back to the same character and the Renzokuken
			// R1 fired four times. The LEVEL channel (0 = held) stays stable across the passes.
			if (tick_real && pad_pub_frame[i] != ff8_interp::frame_counter())
			{
				pad_pub_frame[i] = ff8_interp::frame_counter();
				pad_cache[i] = pad_acc[i];
				pad_acc[i] = 0;
				return pad_cache[i];
			}
			return i == 0 ? pad_cache[i] : 0;
		}
		// Auto-repeat of a held button. 0x49E9C0 calls 0x49EC90 once per button group (four call sites)
		// every time the ring advances; that function counts a signed byte at port+0xC+group down from
		// 0x1D2B2F0 (0x10, initial delay) and reloads it from 0x1D2B2F1 (5, interval) after each pulse.
		// Natively the ring advances 60 times a second (4 HUD passes per 15 fps frame): first repeat after
		// 0.28 s, then 10 pulses a second. Under FFNx the ring advances on every host frame (the four HUD
		// passes are not gated), so at 144 fps the countdown runs ~10x too fast and a tap held a few frames
		// overshoots. The input latch 0x4A8E30 is unrelated (v40/v54 proved it). Fix: feed a 60 Hz
		// wall-clock budget once per host frame and let the countdown step only when a period is due.
		constexpr uint32_t CS_PAD_REPEAT[4] = { 0x49EC05, 0x49EC1C, 0x49EC33, 0x49EC4A };
		constexpr uint32_t FN_PAD_REPEAT = 0x49EC90;
		constexpr double RING_PERIOD = 1.0 / 60.0;
		typedef uint16_t (__cdecl *pad_repeat_t)(int, int, uint16_t, uint16_t, int);
		pad_repeat_t orig_pad_repeat = (pad_repeat_t)FN_PAD_REPEAT;
		double ring_acc = 0.0;            // wall-clock budget (seconds) not yet spent on a countdown step
		uint32_t ring_frame = 0xFFFFFFFF; // host frame that last fed the budget
		bool ring_step = false;           // this ring advance may count the repeat byte down
		// Press edges read straight from the pad ring. 0x4A8420(port) = remap(0x49EDE0(port, 0)) is how three
		// logic consumers poll "pressed this frame": the Renzokuken trigger bar (0x4BA6C0, call site
		// 0x4BA6F2, R1 = bit 8), the GF boost gauge (0x56DD70, site 0x56DD93, Square) and 0x4AD8D0 (site
		// 0x4AD8D5). None of them go through pad_step's cache. The ring advances on every host frame but
		// these consumers only run on logic ticks, so an edge survived only when the press landed on a
		// tick frame - about one press in ten at 144 fps (v68: "could not hit a single explosion in two
		// Renzokukens"; also why boosting felt weak). Each site now gets every edge collected since its
		// previous poll, once per tick frame (the bar polls on all four HUD passes: delivering on each would
		// score one press four times). A site that was not polled on the previous tick gets its backlog
		// dropped, so a press made before the bar opened cannot fire on its first update.
		//
		// v71: a FOURTH consumer, site 3 below, the only one that calls 0x49EDE0 RAW (not via 0x4A8420):
		// Zell's Duel. Its window (slot 6, update 0x4AF840 / draw 0x4B00E0, registered at 0x4B03F9 by
		// 0x4B03B0 <- 0x48E5A0, the init of internal command 0xF1; 0x4B0280 reads Zell's learned limits
		// 0x1CFE76E = savemap +0xAC6 and the per-move tables at 0x1CF8841) takes the timer 0x1D76750 and
		// records every input into the 8-entry history 0x1D76754[(0x1D76768 + 1) & 7] that the moves are
		// matched against. 0x4AF8EE: push 0 / push 0 / call 0x49EDE0 / add esp,8 (cdecl, two args) ->
		// low byte of the raw ring edge (d-pad 0xF0, shoulders 0x0F) | high byte of ctx+0x14 (the
		// face buttons, remapped, which pad_step's channel 2 already accumulates) & 0xF0FF. So the raw
		// mask is returned WITHOUT the 0x4A2D60 remap. And unlike the three sites above, it is a queue,
		// not an OR: in vanilla each of the four HUD passes of a 15 fps frame advanced the ring and fed
		// this history once, so two different directions inside one tick were two entries. OR-merging
		// them would record one two-bit entry that matches no move, so every ring advance with a low-byte
		// edge is queued and each poll (up to four per tick frame) takes the next one. Stale rule as above.
		constexpr int EDGE_SITES = 4;
		constexpr int EDGE_RAW_SITE = 3; // the Duel site: raw 0x49EDE0 (FN_PAD_C), queued
		constexpr uint32_t CS_EDGE_READ[EDGE_SITES] = { 0x4BA6F2, 0x56DD93, 0x4AD8D5, 0x4AF8F2 };
		constexpr uint32_t FN_EDGE_READ = 0x4A8420, FN_PAD_REMAP = 0x4A2D60;
		typedef uint32_t (__cdecl *edge_read_t)(int);
		typedef uint32_t (__cdecl *pad_remap_t)(uint32_t);
		edge_read_t orig_edge_read = (edge_read_t)FN_EDGE_READ;
		uint32_t edge_pending[EDGE_SITES] = {};
		uint32_t edge_frame[EDGE_SITES] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
		uint32_t edge_tick[EDGE_SITES] = {};
		constexpr int EDGE_QUEUE = 8; // same depth as the Duel history
		uint32_t edge_queue[EDGE_QUEUE] = {};
		int edge_queue_n = 0;
		inline bool duel_edge_off() { return (ff8_interp::cfg().battle_debug & (DBG_EDGE_ACC_OFF | DBG_DUEL_EDGE_OFF)) != 0; }
		inline uint32_t edge_read(int site, int port)
		{
			if (pad_bypass() || port != 0 || (ff8_interp::cfg().battle_debug & DBG_EDGE_ACC_OFF)) return orig_edge_read(port);
			update_tick();
			if (!tick_real || edge_frame[site] == ff8_interp::frame_counter()) return 0; // delivered once per tick frame
			const bool stale = edge_tick[site] + 1 < logic_ticks;
			edge_frame[site] = ff8_interp::frame_counter();
			edge_tick[site] = logic_ticks;
			const uint32_t raw = edge_pending[site];
			edge_pending[site] = 0;
			if (stale || raw == 0) return 0;
			return ((pad_remap_t)FN_PAD_REMAP)(raw) & 0xFFFF;
		}
		uint32_t __cdecl edge_read0(int port) { return edge_read(0, port); }
		uint32_t __cdecl edge_read1(int port) { return edge_read(1, port); }
		uint32_t __cdecl edge_read2(int port) { return edge_read(2, port); }
		// Site 3 (Duel), 0x49EDE0(port, back) signature: anything but the live port-0 read passes through.
		uint32_t __cdecl edge_read3(int port, int back)
		{
			if (pad_bypass() || port != 0 || back != 0 || duel_edge_off()) return orig_pad_c(port, back, 0);
			update_tick();
			if (!tick_real) return 0;
			if (edge_frame[EDGE_RAW_SITE] != ff8_interp::frame_counter()) // first poll of this tick frame
			{
				const bool stale = edge_tick[EDGE_RAW_SITE] + 1 < logic_ticks;
				edge_frame[EDGE_RAW_SITE] = ff8_interp::frame_counter();
				edge_tick[EDGE_RAW_SITE] = logic_ticks;
				if (stale) edge_queue_n = 0;
			}
			if (edge_queue_n == 0) return 0;
			const uint32_t raw = edge_queue[0];
			for (int i = 1; i < edge_queue_n; i++) edge_queue[i - 1] = edge_queue[i];
			edge_queue_n--;
			return raw;
		}
		void* const EDGE_READ_HOOK[EDGE_SITES] = { (void*)edge_read0, (void*)edge_read1, (void*)edge_read2, (void*)edge_read3 };

		void __cdecl pad_refresh_hook()
		{
			if (pad_bypass()) { orig_pad_refresh(); return; }
			update_tick();
			// One countdown step per 60 Hz period, decided per ring advance. The cap of four periods means a
			// stall, or the opt-in HUD gate (one advance per logic frame), grants at most the native four steps.
			if (ring_frame != ff8_interp::frame_counter())
			{
				ring_frame = ff8_interp::frame_counter();
				ring_acc += ff8_tick::frame_delta();
				if (ring_acc > 4 * RING_PERIOD) ring_acc = 4 * RING_PERIOD;
			}
			ring_step = ring_acc >= RING_PERIOD;
			if (ring_step) ring_acc -= RING_PERIOD;
			orig_pad_refresh(); // cheap, and the reads below need fresh hardware state; the ring must advance every pass (edges)
			// Collect the press edges of this ring advance for the consumers that read the ring directly.
			if (!(ff8_interp::cfg().battle_debug & DBG_EDGE_ACC_OFF))
			{
				const uint32_t e = orig_pad_c(0, 0, 0);
				for (int i = 0; i < EDGE_RAW_SITE; i++) edge_pending[i] |= e;
				// The Duel reads only the low byte of the raw mask; a queue full of backlog merges into its last entry.
				if ((e & 0xFF) != 0 && !(ff8_interp::cfg().battle_debug & DBG_DUEL_EDGE_OFF))
				{
					if (edge_queue_n < EDGE_QUEUE) edge_queue[edge_queue_n++] = e;
					else edge_queue[EDGE_QUEUE - 1] |= e;
				}
			}
		}
		uint32_t __cdecl pad_a_hook(int a, int b, int c) { if (pad_bypass()) return orig_pad_a(a, b, c); update_tick(); return pad_step(0, orig_pad_a(a, b, c)); }
		uint32_t __cdecl pad_b_hook(int a, int b, int c) { if (pad_bypass()) return orig_pad_b(a, b, c); update_tick(); return pad_step(1, orig_pad_b(a, b, c)); }
		uint32_t __cdecl pad_c_hook(int a, int b, int c) { if (pad_bypass()) return orig_pad_c(a, b, c); update_tick(); return pad_step(2, orig_pad_c(a, b, c)); }
		// Replaces the four 0x49EC90 calls (cdecl; the caller cleans all four at once with add esp,50h, so
		// the hook must not touch the stack). On a non-step pass a press edge or a changed button set still
		// reaches the engine so it sets or restarts the counter; a held, unchanged set returns 0 with the
		// byte untouched - exactly what the original returns on a non-expired step. DBG_PAD_REPEAT_OFF disables it.
		uint16_t __cdecl pad_repeat_hook(int big, int port, uint16_t prev, uint16_t cur, int group)
		{
			if (pad_bypass() || (ff8_interp::cfg().battle_debug & DBG_PAD_REPEAT_OFF) || ring_step) return orig_pad_repeat(big, port, prev, cur, group);
			uint16_t mask = *(uint16_t*)(port + 0x10 + group * 2); // group button mask, as read by 0x49EC90
			if (((prev & cur) & mask) == 0 || ((prev ^ cur) & mask) != 0) return orig_pad_repeat(big, port, prev, cur, group);
			return 0;
		}

		// Battle command menu state machine, registered as the menu window's UPDATE callback (the draw
		// callback is separate), so gating it leaves the menu on screen and only slows its stepping.
		constexpr uint32_t ADDR_MENU_UPDATE = 0x4BB9E0;
		const uint8_t MENU_UPDATE_PROLOGUE[5] = { 0x83, 0xEC, 0x14, 0x53, 0x55 }; // sub esp,14h / push ebx / push ebp
		typedef int (__cdecl *menu_update_t)();
		menu_update_t orig_menu_update = nullptr;
		int menu_last_result = 0;

		int __cdecl menu_update_hook()
		{
			if (orig_menu_update == nullptr) return 0;
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle) return orig_menu_update();
			update_tick();
			if (tick_real) menu_last_result = orig_menu_update();
			return menu_last_result;
		}

		void __cdecl atb_tick_hook()
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle) { orig_atb_tick(); return; }
			update_tick();
			if (tick_real) orig_atb_tick();
		}

		// Play clock 0x4701B0 (save play time 0x1CFE928 + event countdown 0x1CFE92C, one 1/60 s step per
		// call). The battle main loop 0x47CF60 calls it four times per frame, unconditionally, before
		// the HUD passes: 4 x 15 = 60 steps/s natively, 576/s at 144 Hz (9.6x). v71 hands every call to
		// the shared 60 Hz wall-clock budget (play_clock.h); DBG_PLAY_CLOCK_OFF restores host rate.
		constexpr uint32_t CS_PLAY_CLOCK[4] = { 0x47D076, 0x47D07B, 0x47D080, 0x47D085 };
		void __cdecl play_clock_hook() { ff8_play_clock::run(!(ff8_interp::cfg().battle_debug & DBG_PLAY_CLOCK_OFF)); }

		// Impact effects. AnimSeq B0 (fx on the attacker) and B4 (fx on the target) reach 0x505900, which
		// registers the type-0 launcher 0x570CC0 in the 300-slot particle pool 0x209FAA8; the launcher
		// tail-calls 0x508360 with the per-frame runner 0x570BC0. That pool's frame-driver site 0x500923 is
		// not gated (see the queue note above), so every impact effect - the gunblade R1 trigger explosion
		// among them - steps once per HOST frame.
		// 0x570BC0 separates cleanly: the UPDATE is 0x571BC0 integrating the SVECTOR at node+0x10 plus the
		// sprite step node+0x0C++, the DRAW is a fresh 0x5082B0(0xB4) work block linked through 0x571C80
		// and freed again, so drawing on every host frame allocates new primitives and never replays
		// recycled ones. The engine already has the gate we need: with BATTLE_FREEZE bit 0 set the function
		// draws and returns 0 without advancing node+0x0C (0x570C88). The bit is set only around this one
		// call - frame-wide it would also stop 0x502AB0 from calling 0x504290 and kill pose interpolation.
		constexpr uint32_t ADDR_IMPACT_FX = 0x570BC0;
		const uint8_t IMPACT_FX_PROLOGUE[6] = { 0x83, 0xEC, 0x08, 0x53, 0x56, 0x57 }; // sub esp,8 / push ebx,esi,edi
		constexpr uint32_t IMPACT_FX_STATE = 0x0C;   // +0x0C u16 sprite frame, +0x10 the SVECTOR 0x571BC0 steps
		constexpr uint32_t IMPACT_FX_STATE_LEN = 12; // in place - both restored, +0x0C..+0x17 inclusive
		typedef int (__cdecl *impact_fx_t)(uint8_t*);
		impact_fx_t orig_impact_fx = nullptr;

		int __cdecl impact_fx_hook(uint8_t* node)
		{
			if (orig_impact_fx == nullptr) return 0;
			if (node == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle || (ff8_interp::cfg().battle_debug & DBG_IMPACT_FX_OFF))
				return orig_impact_fx(node);
			update_tick();
			if (tick_real) return orig_impact_fx(node);
			uint8_t saved[IMPACT_FX_STATE_LEN];
			memcpy(saved, node + IMPACT_FX_STATE, sizeof(saved));
			uint32_t freeze = *BATTLE_FREEZE;
			*BATTLE_FREEZE = freeze | 1;
			int r = orig_impact_fx(node); // draws a fresh work block, returns without stepping the effect
			*BATTLE_FREEZE = freeze;
			memcpy(node + IMPACT_FX_STATE, saved, sizeof(saved)); // undo 0x571BC0's in-place integration
			return r;
		}
	}

	// --- shared with the sibling translation units of this module, see internal.h ---

	// Declared extern in internal.h, so this definition has external linkage despite being const.
	uint32_t* const BATTLE_FREEZE = (uint32_t*)0x1D96A9C;

	// Thin forwarders onto the anonymous namespace's tick state. The accessor is called tick_update()
	// and not update_tick() so it cannot become ambiguous with the internal update_tick() above.
	void tick_update() { update_tick(); }
	bool tick_is_real() { return tick_real; }
	float tick_alpha_now() { return tick_alpha; }
	float tick_alpha_lead_now() { return tick_alpha_lead; }
	uint32_t logic_tick_count() { return logic_ticks; }

	bool in_battle() { return ff8_interp::game_mode() == ff8_interp::GameMode::Battle; }

	void* make_trampoline(uint32_t target, const uint8_t* prologue, size_t prologue_len)
	{
		if (memcmp((const void*)target, prologue, prologue_len) != 0) return nullptr;
		uint8_t* t = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (t == nullptr) return nullptr;
		memcpy(t, prologue, prologue_len);
		t[prologue_len] = 0xE9; // jmp rel32 back to the rest of the original function
		*(uint32_t*)(t + prologue_len + 1) = (target + (uint32_t)prologue_len) - ((uint32_t)t + (uint32_t)prologue_len + 5);
		return (void*)t;
	}

	void animations_hook_init()
	{
		if (!ff8_interp::cfg().battle_enabled) return;

		if (!ff8_interp::is_us_exe())
		{
			ff8_interp::log_warning("%s: battle animation interpolation only verified on the US executable, skipping.\n", __func__);
			return;
		}

		if (ff8_interp::cfg().battle_fps > 0)
		{
			if (ff8_interp::cfg().battle_fps < 30 || ff8_interp::cfg().battle_fps > 1000)
			{
				ff8_interp::log_warning("%s: ff8_battle_fps out of range (30..1000), got %ld. Disabled.\n", __func__, ff8_interp::cfg().battle_fps);
				return;
			}
			battle_fps = (int)ff8_interp::cfg().battle_fps;
		}
		else switch (ff8_interp::cfg().host_limiter_fps) // FFNx: ff8_fps_limiter 30 / 60 fps
		{
		case 30: battle_fps = 30; break;
		case 60: battle_fps = 60; break;
		default:
			ff8_interp::log_info("%s: ff8_fps_limiter is not 30/60 fps and ff8_battle_fps is 0, battle animation interpolation disabled.\n", __func__);
			return;
		}

		original_read_animation = (read_animation_t)make_trampoline(ADDR_READ_ANIMATION, READ_ANIMATION_PROLOGUE, sizeof(READ_ANIMATION_PROLOGUE));
		original_seq_driver = (seq_driver_t)make_trampoline(ADDR_SEQ_DRIVER, SEQ_DRIVER_PROLOGUE, sizeof(SEQ_DRIVER_PROLOGUE));
		if (original_read_animation == nullptr || original_seq_driver == nullptr)
		{
			ff8_interp::log_warning("%s: engine prologue mismatch (read=%p seq=%p), battle animation interpolation disabled.\n", __func__, original_read_animation, original_seq_driver);
			return;
		}

		ff8_interp::hook_function(ADDR_READ_ANIMATION, (void*)read_animation_hook);
		ff8_interp::hook_function(ADDR_SEQ_DRIVER, (void*)seq_driver_hook);
		hook_installed = true;

		// Spell / GF effects and the ATB run from their own per-frame drivers.
		ff8_interp::hook_call(CS_EFFECT_TICK, (void*)effect_tick_hook);
		ff8_interp::hook_call(CS_ATB_TICK, (void*)atb_tick_hook);
		for (int i = 0; i < FX_LINKERS; i++)
		{
			orig_fx_linker[i] = (fx_linker_t)make_trampoline(FX_LINKER_ADDR[i], FX_LINKER_PROLOGUE[i], FX_LINKER_PROLOGUE_LEN[i]);
			if (orig_fx_linker[i] != nullptr) ff8_interp::hook_function(FX_LINKER_ADDR[i], FX_LINKER_HOOK[i]);
			else ff8_interp::log_warning("%s: display list linker %08X prologue mismatch, effects using it cannot be replayed.\n", __func__, FX_LINKER_ADDR[i]);
		}
		for (int i = 0; i < 4; i++) ff8_interp::hook_call(CS_UI_CLOCK[i], (void*)ui_clock_hook);
		ff8_interp::hook_call(CS_APPEAR_TICK, (void*)appear_tick_hook);
		ff8_interp::hook_call(CS_MANAGER_QUEUE, (void*)manager_queue_hook);
		orig_stage_texanim = (stage_texanim_t)make_trampoline(ADDR_STAGE_TEXANIM, STAGE_TEXANIM_PROLOGUE, sizeof(STAGE_TEXANIM_PROLOGUE));
		if (orig_stage_texanim != nullptr) ff8_interp::hook_function(ADDR_STAGE_TEXANIM, (void*)stage_texanim_hook);
		else ff8_interp::log_warning("%s: stage texture animation prologue mismatch, it stays at host rate.\n", __func__);
		ff8_interp::hook_call(CS_VICTORY_LOGIC, (void*)victory_logic_hook);
		orig_battle_state = (battle_state_t)make_trampoline(ADDR_BATTLE_STATE, BATTLE_STATE_PROLOGUE, sizeof(BATTLE_STATE_PROLOGUE));
		orig_fade_trigger = (fade_trigger_t)make_trampoline(ADDR_FADE_TRIGGER, FADE_TRIGGER_PROLOGUE, sizeof(FADE_TRIGGER_PROLOGUE));
		orig_fade_tick = (fade_tick_t)make_trampoline(ADDR_FADE_TICK, FADE_TICK_PROLOGUE, sizeof(FADE_TICK_PROLOGUE));
		if (orig_battle_state != nullptr && orig_fade_trigger != nullptr && orig_fade_tick != nullptr)
		{
			ff8_interp::hook_function(ADDR_BATTLE_STATE, (void*)battle_state_hook);
			ff8_interp::hook_function(ADDR_FADE_TRIGGER, (void*)fade_trigger_hook);
			ff8_interp::hook_function(ADDR_FADE_TICK, (void*)fade_tick_hook);
		}
		else ff8_interp::log_warning("%s: end-of-battle prologue mismatch (%p/%p/%p), the victory sequence stays at host rate.\n", __func__, orig_battle_state, orig_fade_trigger, orig_fade_tick);
		orig_caption_tick = (caption_tick_t)make_trampoline(ADDR_CAPTION_TICK, CAPTION_PROLOGUE, sizeof(CAPTION_PROLOGUE));
		if (orig_caption_tick != nullptr) ff8_interp::hook_function(ADDR_CAPTION_TICK, (void*)caption_tick_hook);
		else ff8_interp::log_warning("%s: caption prologue mismatch, battle text stays at host rate.\n", __func__);
		orig_ui_update = (ui_update_t)make_trampoline(ADDR_UI_UPDATE, UI_UPDATE_PROLOGUE, sizeof(UI_UPDATE_PROLOGUE));
		if (orig_ui_update != nullptr) ff8_interp::hook_function(ADDR_UI_UPDATE, (void*)ui_update_hook);
		else ff8_interp::log_warning("%s: window update prologue mismatch, battle text stays at host rate.\n", __func__);
		orig_gf_seq_tick = (gf_seq_tick_t)make_trampoline(ADDR_GF_SEQ_TICK, GF_SEQ_PROLOGUE, sizeof(GF_SEQ_PROLOGUE));
		if (orig_gf_seq_tick != nullptr) ff8_interp::hook_function(ADDR_GF_SEQ_TICK, (void*)gf_seq_tick_hook);
		else ff8_interp::log_warning("%s: summon sequence prologue mismatch, the summon entrance stays at host rate.\n", __func__);
		orig_apply_results = (apply_results_t)make_trampoline(ADDR_APPLY_RESULTS, APPLY_RESULTS_PROLOGUE, sizeof(APPLY_RESULTS_PROLOGUE));
		if (orig_apply_results != nullptr) ff8_interp::hook_function(ADDR_APPLY_RESULTS, (void*)apply_results_hook);
		else ff8_interp::log_warning("%s: action result prologue mismatch, a boosted summon can still land before its gauge.\n", __func__);
		orig_damage_tick = (damage_tick_t)make_trampoline(ADDR_DAMAGE_TICK, DAMAGE_TICK_PROLOGUE, sizeof(DAMAGE_TICK_PROLOGUE));
		if (orig_damage_tick != nullptr) ff8_interp::hook_function(ADDR_DAMAGE_TICK, (void*)damage_tick_hook);
		else ff8_interp::log_warning("%s: damage popup prologue mismatch, popups stay at host rate.\n", __func__);
		orig_impact_fx = (impact_fx_t)make_trampoline(ADDR_IMPACT_FX, IMPACT_FX_PROLOGUE, sizeof(IMPACT_FX_PROLOGUE));
		if (orig_impact_fx != nullptr) ff8_interp::hook_function(ADDR_IMPACT_FX, (void*)impact_fx_hook);
		else ff8_interp::log_warning("%s: impact effect prologue mismatch, hit explosions stay at host rate.\n", __func__);
		orig_magic_effect_tick = (magic_tick_t)make_trampoline(ADDR_MAGIC_EFFECT_TICK, MAGIC_EFFECT_PROLOGUE, sizeof(MAGIC_EFFECT_PROLOGUE));
		orig_magic_cast_tick = (magic_tick_t)make_trampoline(ADDR_MAGIC_CAST_TICK, MAGIC_CAST_PROLOGUE, sizeof(MAGIC_CAST_PROLOGUE));
		if (orig_magic_effect_tick != nullptr) ff8_interp::hook_function(ADDR_MAGIC_EFFECT_TICK, (void*)magic_effect_tick_hook);
		if (orig_magic_cast_tick != nullptr) ff8_interp::hook_function(ADDR_MAGIC_CAST_TICK, (void*)magic_cast_tick_hook);
		if (orig_magic_effect_tick == nullptr || orig_magic_cast_tick == nullptr)
			ff8_interp::log_warning("%s: magic handler prologue mismatch, spell effects cannot be held.\n", __func__);

		ff8_interp::hook_call(CS_BATTLE_CAMERA, (void*)battle_camera_hook);
		ff8_interp::hook_call(CS_PAD_REFRESH, (void*)pad_refresh_hook);
		ff8_interp::hook_call(CS_PAD_A, (void*)pad_a_hook);
		ff8_interp::hook_call(CS_PAD_B, (void*)pad_b_hook);
		ff8_interp::hook_call(CS_PAD_C, (void*)pad_c_hook);
		for (int i = 0; i < EDGE_SITES; i++) ff8_interp::hook_call(CS_EDGE_READ[i], EDGE_READ_HOOK[i]);
		for (int i = 0; i < 4; i++) ff8_interp::hook_call(CS_PAD_REPEAT[i], (void*)pad_repeat_hook);
		int clock_sites = 0;
		for (int i = 0; i < 4; i++) clock_sites += ff8_play_clock::patch_site(CS_PLAY_CLOCK[i], (void*)play_clock_hook, __func__) ? 1 : 0;
		orig_menu_update = (menu_update_t)make_trampoline(ADDR_MENU_UPDATE, MENU_UPDATE_PROLOGUE, sizeof(MENU_UPDATE_PROLOGUE));
		if (orig_menu_update != nullptr) ff8_interp::hook_function(ADDR_MENU_UPDATE, (void*)menu_update_hook);
		else ff8_interp::log_warning("%s: menu prologue mismatch, the battle menu stays at host rate.\n", __func__);
		{
			const uint8_t* p = (const uint8_t*)ADDR_READ_ANIMATION;
			ff8_interp::log_info("%s: bytes@508F90 after patch: %02x %02x %02x %02x %02x (hook=%p tramp=%p)\n", __func__, p[0], p[1], p[2], p[3], p[4], read_animation_hook, original_read_animation);
		}
		ff8_interp::log_info("%s: battle animation interpolation v74 enabled (battle %d fps, logic %d fps, play clock %d/4 sites).\n", __func__, battle_fps, LOGIC_FPS, clock_sites);

		// Sibling translation units of this module (internal.h).
		gf_pacing_hook_init();
		hud_blend_hook_init();
		fx_queues_hook_init();
		fx_blend_hook_init();
	}

	bool animations_installed() { return hook_installed; }

	void animations_reset()
	{
		states.clear();
		intro_held_frames = 0;
		entity_states.clear();
		pad_cache[0] = pad_cache[1] = pad_cache[2] = 0;
		pad_acc[0] = pad_acc[1] = pad_acc[2] = 0;
		pad_pub_frame[0] = pad_pub_frame[1] = pad_pub_frame[2] = 0xFFFFFFFF;
		ring_acc = 0.0;
		ring_frame = 0xFFFFFFFF;
		for (int i = 0; i < EDGE_SITES; i++) { edge_pending[i] = 0; edge_frame[i] = 0xFFFFFFFF; edge_tick[i] = 0; }
		edge_queue_n = 0;
		ring_step = false;
		menu_last_result = 0;
		summon_live_frame = 0xFFFFFFFF;
		holdable = nullptr;
		last_ctx = nullptr;
		registered_root = nullptr;
		fx_prims.clear();
		fx_prims_prev.clear();
		cam_pose_valid = false;
		cam_published_valid = false;
		blink_valid = false;
		blink_pending = false;
		blink_calls = blink_last_step = 0;
		logic_ticks = 0;
		end_hold_seconds = 0.0;
		victory_acc = 0.0;
		prim_stack_low = 0xFFFFFFFF;
		fx_record_target = nullptr;
		fx_defer_links = false;
		fx_flushes = 0;
		held_ctx = nullptr;
		held_frames = 0;
		tick_frame = 0xFFFFFFFF;
		tick_acc = 0.0;
		ff8_tick::clock_reset();

		// Sibling translation units of this module (internal.h).
		gf_pacing_reset();
		hud_blend_reset();
		fx_queues_reset();
		fx_blend_reset();
	}

	// Called once per host frame from ff8_interp::frame(): diagnostic heartbeat while the hook is being validated.
	void animations_on_frame()
	{
		if (!hook_installed) return;
		int mode = (int)ff8_interp::mode_id();
		if (mode != last_mode)
		{
			// Entering/leaving battle: forget every snapshot so stale poses/positions are never restored.
			animations_reset();
			last_mode = mode;
		}
		if (!ff8_interp::cfg().trace) return;
		if (ff8_interp::frame_counter() % 60 != 0) return;
		const uint8_t* p = (const uint8_t*)ADDR_READ_ANIMATION;
		ff8_interp::log_trace("ff8anim heartbeat f=%u mode=%d calls=%u bytes@508F90=%02x %02x %02x %02x %02x\n",
			ff8_interp::frame_counter(), ff8_interp::mode_id(), hook_calls, p[0], p[1], p[2], p[3], p[4]);
	}
}
