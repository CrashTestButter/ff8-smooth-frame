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
// Engine layout (FF8 2000 v1.2 / Steam 2013):
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
#include <cmath>
#include <unordered_map>
#include <vector>
#include <new>
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
		constexpr uint32_t ADDR_SEQUENCE_DRIVER = 0x504290;          // void BattleEntity_RunSequence(BattleEntity*): per entity per logic frame, runs the sec-5 VM + Advance
		constexpr uint32_t ADDR_CURRENT_ENTITY = 0x01D98204;    // globals the driver sets first thing (its caller may read them)
		constexpr uint32_t ADDR_CURRENT_ANIM_CMD = 0x01D981F8;
		constexpr uint32_t ADDR_CURRENT_SEQUENCE = 0x01D98200;

		// First four instructions of the sequence driver (sub esp,8 / push ebx / push edi / mov edi,[esp+14h]).
		const uint8_t SEQUENCE_DRIVER_PROLOGUE[9] = {0x83, 0xEC, 0x08, 0x53, 0x57, 0x8B, 0x7C, 0x24, 0x14};

		// Entity world placement sources (int16), consumed every host frame by the entity task 0x502AB0
		// which rebuilds the entity matrix (+0x40) from them after running the sequence driver.
		constexpr uint32_t ENTITY_POS_X = 0x1C;
		constexpr uint32_t ENTITY_POS_Y_OFFSET = 0x1E;
		constexpr uint32_t ENTITY_POS_Z = 0x20;
		constexpr uint32_t ENTITY_POS_Y = 0x22;
		// Sequence VM working position (E5 0x0D/0x0E/0x0F "set position Z/X/Y"), int16 in the sequence
		// state block pointed to by entity+0x74; the entity task adds it into the world matrix too.
		constexpr uint32_t SEQUENCE_POS_Z = 0x24;
		constexpr uint32_t SEQUENCE_POS_X = 0x26;
		constexpr uint32_t SEQUENCE_POS_Y = 0x28;
		constexpr int POSITION_FIELDS = 7;
		constexpr int ENTITY_POS_SNAP = 6000; // per tick delta above this is a teleport, not motion

		constexpr uint32_t ENTITY_ANIM_CTX = 0x60;
		constexpr uint32_t ENTITY_ANIM_CMD = 0x6C;
		constexpr uint32_t ENTITY_SEQUENCE = 0x74;
		constexpr uint32_t ENTITY_WEAPON = 0x78;
		constexpr uint32_t WEAPON_ANIM_CTX = 0x00;
		constexpr uint32_t WEAPON_ANIM_CMD = 0x0C;

		// First three instructions of Battle_ReadAnimation (sub esp,8 / push ebx / mov ebx,[esp+14h]):
		// position independent, so they can be relocated into a trampoline.
		const uint8_t READ_ANIMATION_PROLOGUE[8] = {0x83, 0xEC, 0x08, 0x53, 0x8B, 0x5C, 0x24, 0x14};

		constexpr uint32_t POSE_FLAGS = 0x01;
		constexpr uint32_t POSE_ROOT = 0x08;
		constexpr uint32_t POSE_SLOTS = 0x10;
		constexpr uint32_t POSE_SLOT_SIZE = 0x30;
		constexpr uint32_t SLOT_ROTATION = 0x04;
		constexpr uint32_t SLOT_SCALE = 0x0A;

		constexpr uint32_t CMD_ANIM_ID = 0;
		constexpr uint32_t CMD_CURRENT_FRAME = 6;
		constexpr uint32_t CMD_TOTAL_FRAMES = 7;

		typedef int (__cdecl *read_animation_t)(uint8_t* ctx, uint8_t* cmd);
		typedef void (__cdecl *build_bone_matrices_t)(uint8_t* ctx);
		typedef void (__cdecl *sequence_driver_t)(uint8_t* entity);

		read_animation_t original_read_animation = nullptr; // trampoline into the unpatched function
		sequence_driver_t original_sequence_driver = nullptr;         // trampoline into the unpatched function
		build_bone_matrices_t build_bone_matrices = (build_bone_matrices_t)ADDR_BUILD_BONE_MATRICES;

		// Snapshot of everything ReadAnimation accumulates into a PoseBuf.
		struct Pose
		{
			uint8_t flags = 0;
			int16_t root[3] = {0, 0, 0};
			std::vector<int16_t> bones; // 6 per bone: rx ry rz sx sy sz
		};

		struct AnimationState
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
			int root_jump = 0; // v82: largest |root| component change from the shown pose to the snapped frame 0
		};

		std::unordered_map<uint8_t*, AnimationState> anim_states; // keyed by AnimCmd*
		int battle_fps = 30;              // host frames per second in battle
		constexpr int LOGIC_FPS = 15;     // native battle tick rate

		// Per host frame tick state, derived lazily from frame_counter: a logic tick fires whenever
		// 1/15 s worth of host frames has elapsed (works for any host rate, 144 included).
		uint32_t tick_frame = 0xFFFFFFFF;
		double tick_accumulator = 0.0;         // seconds of real time carried into the current logic period
		bool is_logic_tick = true;    // this host frame consumes a real animation frame
		float tick_alpha = 1.0f;  // blend between the last two decoded poses to show this host frame
		float tick_alpha_lead = 1.0f; // the pre-v71 formula, one host frame ahead (hud_blend only)
		uint32_t logic_ticks = 0; // number of logic ticks since the module was reset

		struct EntityState
		{
			bool valid = false;
			int16_t prev[POSITION_FIELDS] = {0};
			int16_t next[POSITION_FIELDS] = {0};
			// v102: what this module last wrote into the position fields, and a position someone else
			// wrote there since (an effect moving an entity whose sequence runs: Elvoret's entrance)
			bool written_valid = false;
			int16_t written[POSITION_FIELDS] = {0};
			bool external_pending = false;
			int16_t external[POSITION_FIELDS] = {0};
		};
		std::unordered_map<uint8_t*, EntityState> entity_states; // keyed by BattleEntity*
		int last_mode = -1;

		inline void capture_position(const uint8_t* entity, int16_t out[POSITION_FIELDS])
		{
			out[0] = *(const int16_t*)(entity + ENTITY_POS_X);
			out[1] = *(const int16_t*)(entity + ENTITY_POS_Y_OFFSET);
			out[2] = *(const int16_t*)(entity + ENTITY_POS_Z);
			out[3] = *(const int16_t*)(entity + ENTITY_POS_Y);
			const uint8_t* sequence = *(const uint8_t* const*)(entity + ENTITY_SEQUENCE);
			out[4] = sequence ? *(const int16_t*)(sequence + SEQUENCE_POS_Z) : 0;
			out[5] = sequence ? *(const int16_t*)(sequence + SEQUENCE_POS_X) : 0;
			out[6] = sequence ? *(const int16_t*)(sequence + SEQUENCE_POS_Y) : 0;
		}

		inline void restore_position(uint8_t* entity, const int16_t values[POSITION_FIELDS])
		{
			*(int16_t*)(entity + ENTITY_POS_X) = values[0];
			*(int16_t*)(entity + ENTITY_POS_Y_OFFSET) = values[1];
			*(int16_t*)(entity + ENTITY_POS_Z) = values[2];
			*(int16_t*)(entity + ENTITY_POS_Y) = values[3];
			uint8_t* sequence = *(uint8_t**)(entity + ENTITY_SEQUENCE);
			if (sequence == nullptr) return;
			*(int16_t*)(sequence + SEQUENCE_POS_Z) = values[4];
			*(int16_t*)(sequence + SEQUENCE_POS_X) = values[5];
			*(int16_t*)(sequence + SEQUENCE_POS_Y) = values[6];
		}

		constexpr double LOGIC_PERIOD = 1.0 / double(LOGIC_FPS);

		// Wall-clock gate: ff8_battle_fps only caps the render rate, the logic rate must not depend on it.
		void update_tick()
		{
			if (tick_frame == ff8_interp::frame_counter()) return;
			tick_frame = ff8_interp::frame_counter();
			double dt = ff8_tick::frame_delta();
			tick_accumulator += dt;
			is_logic_tick = tick_accumulator >= LOGIC_PERIOD;
			if (is_logic_tick)
			{
				logic_ticks++;
				tick_accumulator -= LOGIC_PERIOD;
				if (tick_accumulator > LOGIC_PERIOD) tick_accumulator = LOGIC_PERIOD; // after a stall, catch up over the next frames
				// v116: a stall of 100 ms or more (a synchronous load, e.g. the monster models in the first
				// second of the intro pan) drops the debt instead: the vanilla game just pauses, and the
				// catch-up fired two ticks in two frames, a visible leap of the intro camera (v115 trace:
				// 427 ms frame at 31.94 s, ticks at frames 2366/2367/2368).
				const bool drop_stall_debt = !(ff8_interp::cfg().battle_debug2 & DBG2_STALL_CATCHUP) && dt >= 0.1 * ff8_interp::speed();
				if (drop_stall_debt) tick_accumulator = 0.0;
			}
			// v71: the time since the last tick, tick_accumulator / P. `tick_accumulator` already holds this frame's dt, so
			// the pre-v71 (tick_accumulator + dt) / P counted it twice: everything drew one host frame ahead, alpha
			// was pinned to 1.0 on the last frame before every tick and the tick frame then jumped to
			// (r + dt) / P - an uneven step pair once per tick (audit F2). The old value is still kept
			// for the Renzokuken bar (tick_alpha_lead_now), and DBG_ALPHA_LEAD puts it back everywhere.
			tick_alpha_lead = float((tick_accumulator + dt) / LOGIC_PERIOD);
			if (tick_alpha_lead > 1.0f) tick_alpha_lead = 1.0f;
			if (tick_alpha_lead < 0.0f) tick_alpha_lead = 0.0f;
			tick_alpha = (ff8_interp::cfg().battle_debug & DBG_ALPHA_LEAD) ? tick_alpha_lead : float(tick_accumulator / LOGIC_PERIOD);
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

		void capture_pose(const uint8_t* pose, Pose& out)
		{
			int bone_count = pose[0];
			out.flags = pose[POSE_FLAGS];
			memcpy(out.root, pose + POSE_ROOT, sizeof(out.root));
			out.bones.resize(bone_count * 6);
			for (int i = 0; i < bone_count; i++)
			{
				const uint8_t* slot = pose + POSE_SLOTS + i * POSE_SLOT_SIZE;
				memcpy(&out.bones[i * 6], slot + SLOT_ROTATION, 3 * sizeof(int16_t));
				memcpy(&out.bones[i * 6 + 3], slot + SLOT_SCALE, 3 * sizeof(int16_t));
			}
		}

		void restore_pose(uint8_t* pose, const Pose& in)
		{
			int bone_count = pose[0];
			if ((int)in.bones.size() != bone_count * 6) return;
			pose[POSE_FLAGS] = in.flags;
			memcpy(pose + POSE_ROOT, in.root, sizeof(in.root));
			for (int i = 0; i < bone_count; i++)
			{
				uint8_t* slot = pose + POSE_SLOTS + i * POSE_SLOT_SIZE;
				memcpy(slot + SLOT_ROTATION, &in.bones[i * 6], 3 * sizeof(int16_t));
				memcpy(slot + SLOT_SCALE, &in.bones[i * 6 + 3], 3 * sizeof(int16_t));
			}
		}

		// Shortest arc on the 12 bit angle circle (4096 units per turn).
		inline int16_t lerp_angle(int16_t a, int16_t b, float t)
		{
			int delta = ((int(b) - int(a) + 2048) & 4095) - 2048;
			float step = delta * t;
			return (int16_t)(int(a) + (int)(step + (step >= 0.0f ? 0.5f : -0.5f)));
		}

		inline int16_t lerp_linear(int16_t a, int16_t b, float t)
		{
			float step = (int(b) - int(a)) * t;
			return (int16_t)(int(a) + (int)(step + (step >= 0.0f ? 0.5f : -0.5f)));
		}

		// v85: bones are blended as rotations. The engine builds a bone's matrix as
		// M = Ry(-ry) . Rx(-rx) . Rz(rz) (12-bit angles; the convention FF8UltimateEditor's
		// rotation3d.py verifies against the renderer). Per-axis blending is only right for small steps:
		// one orientation has more than one angle triple, and Selphie's nunchaku switches between them
		// (victory anim 12: 179 degrees on one axis for a 28 degree turn; Slot anim 30: 147 for 1), so
		// the per-axis blend swung the links through poses neither frame has. A bone whose step is
		// small on every axis keeps the per-axis lerp (identical result, no trig); a larger one turns
		// once, around one axis, the short way (slerp). Only a real turn beyond 90 degrees (v86; v85: 150) switches at
		// mid-tick like the engine. v84 cut on the per-axis
		// step, which also popped the bones that only changed triple.
		constexpr int BONE_SLERP_MIN = 256;        // 22.5 degrees on any axis: below, per-axis lerp
		// v86: cos(90 deg / 2) - true turns beyond 90 degrees in one logic frame are cut. Selphie's win pose
		// (r0win.dat section 7, weapon anim, 60 frames) whirls the links 100..179 degrees per frame from
		// frame 6 to ~50: the engine shows it as a strobe, a blend turns every step into a visible swing
		// (v85 at 150: the weapon "flung around" for three seconds). A step that big carries no usable
		// direction or speed, so it is shown the engine's way.
		constexpr float BONE_CUT_COS_HALF = 0.7071f;

		struct BoneQuat { float w, x, y, z; };
		constexpr float RADIANS_PER_ANGLE_UNIT = 6.2831853f / 4096.0f;

		BoneQuat matrix_to_quat(const float* m);
		BoneQuat bone_angles_to_quat(const int16_t* angles)
		{
			const float u = -angles[1] * RADIANS_PER_ANGLE_UNIT;
			const float v = -angles[0] * RADIANS_PER_ANGLE_UNIT;
			const float w = angles[2] * RADIANS_PER_ANGLE_UNIT;
			const float cu = cosf(u), su = sinf(u);
			const float cv = cosf(v), sv = sinf(v);
			const float cw = cosf(w), sw = sinf(w);
			// Ry(u) . Rx(v) . Rz(w)
			const float m00 = cu * cw + su * sv * sw, m01 = -cu * sw + su * sv * cw, m02 = su * cv;
			const float m10 = cv * sw, m11 = cv * cw, m12 = -sv;
			const float m20 = -su * cw + cu * sv * sw, m21 = su * sw + cu * sv * cw, m22 = cu * cv;
			const float m[9] = { m00, m01, m02, m10, m11, m12, m20, m21, m22 };
			return matrix_to_quat(m);
		}

		// Row-major 3x3 rotation -> unit quaternion (q maps to the same matrix).
		BoneQuat matrix_to_quat(const float* m)
		{
			const float m00 = m[0], m01 = m[1], m02 = m[2], m10 = m[3], m11 = m[4], m12 = m[5], m20 = m[6], m21 = m[7], m22 = m[8];
			BoneQuat q;
			const float trace = m00 + m11 + m22;
			if (trace > 0.0f)
			{
				float scale = sqrtf(1.0f + trace) * 2.0f;
				q.w = 0.25f * scale;
				q.x = (m21 - m12) / scale;
				q.y = (m02 - m20) / scale;
				q.z = (m10 - m01) / scale;
			}
			else if (m00 > m11 && m00 > m22)
			{
				float scale = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
				q.w = (m21 - m12) / scale;
				q.x = 0.25f * scale;
				q.y = (m01 + m10) / scale;
				q.z = (m02 + m20) / scale;
			}
			else if (m11 > m22)
			{
				float scale = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
				q.w = (m02 - m20) / scale;
				q.x = (m01 + m10) / scale;
				q.y = 0.25f * scale;
				q.z = (m12 + m21) / scale;
			}
			else
			{
				float scale = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
				q.w = (m10 - m01) / scale;
				q.x = (m02 + m20) / scale;
				q.y = (m12 + m21) / scale;
				q.z = 0.25f * scale;
			}
			return q;
		}

		// Back to the engine's triple, each angle written on the turn nearest `reference` (the per-axis
		// blend's value), so the numbers stay close to what the file holds.
		void quat_to_bone_angles(const BoneQuat& q, const int16_t* reference, int16_t* out)
		{
			const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
			const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
			const float m00 = 1 - 2 * (yy + zz), m01 = 2 * (xy - wz), m02 = 2 * (xz + wy);
			const float m10 = 2 * (xy + wz), m11 = 1 - 2 * (xx + zz), m12 = 2 * (yz - wx);
			const float m20 = 2 * (xz - wy), m22 = 1 - 2 * (xx + yy);
			float sin_v = -m12;
			if (sin_v > 1.0f) sin_v = 1.0f;
			if (sin_v < -1.0f) sin_v = -1.0f;
			const float v = asinf(sin_v);
			float u, w;
			// v91: the general branch is accurate in float down to |sin_v| = 0.999999 (cos v ~ 1.4e-3). At the
			// lock only y +- z is defined: keep z at the reference and solve y (v85-v90 folded z into y
			// below 0.9999, i.e. within 0.81 degrees of the lock - up to ~1.6 degrees off, a visible pop at
			// the frame boundary on bones that sit near x = +-90: Selphie's body bone 1 at x = -1016 raw;
			// worst found by the anim viewer 0.21 units, d4c009 anim 31 step 16).
			if (fabsf(sin_v) < 0.999999f)
			{
				u = atan2f(m02, m22);
				w = atan2f(m10, m11);
			}
			else
			{
				w = reference[2] * RADIANS_PER_ANGLE_UNIT;              // rz = w
				u = atan2f(-m20, m00) + (sin_v > 0.0f ? w : -w); // sv = +1: atan2 = u - w; sv = -1: u + w
			}
			const float raw_angles[3] = { -v / RADIANS_PER_ANGLE_UNIT, -u / RADIANS_PER_ANGLE_UNIT, w / RADIANS_PER_ANGLE_UNIT }; // rx = -v, ry = -u, rz = w
			for (int k = 0; k < 3; k++)
			{
				int rounded = (int)floorf(raw_angles[k] + 0.5f);
				rounded = int(reference[k]) + ((((rounded - int(reference[k])) % 4096) + 4096 + 2048) % 4096 - 2048);
				out[k] = (int16_t)rounded;
			}
		}

		// v90: small parts (weapons, <= 16 bones) are blended in MODEL space. Selphie's nunchaku is a
		// chain (grip 0; links 1 -> 3 -> 4 -> 5 -> 6; 2 = held stick, 6 = free stick) whose links twist
		// up to half a turn per tick against each other while the sticks barely move (anim 5, step
		// 12 -> 13: bone 6 turns 168 degrees relative to its parent but only 20 in the model). Any blend
		// of the LOCAL rotations (per axis, slerp, v86 per-bone cut, v87 whole-part step) mixes those
		// counter-turns; v87 also held every rotation to mid-tick while the grip moved, so the jump was
		// often bigger than the real motion. Here each bone's model-space orientation
		// (chain = parent chain * local, the engine's product) is slerped between the two frames and the
		// local rotation is rebuilt against the BLENDED parent. t = 0 / 1 give the frames exactly.
		// Found and checked in an offline animation viewer. Only a bone whose model-space turn is beyond 176 degrees, where the
		// short way is a coin toss, switches at mid-tick; its children are still blended on their own.
		constexpr int MODEL_SPACE_MAX_BONES = 16;
		constexpr float MODEL_CUT_COS_HALF = 0.0349f; // cos(176 deg / 2); Selphie anim 5: 6 -> 7 (179) cuts, 12 -> 13 (173) blends
		constexpr uint32_t SLOT_PARENT = 0x00;        // i16 parent bone, -1 = root

		inline BoneQuat quat_multiply(const BoneQuat& a, const BoneQuat& b)
		{
			return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
			         a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
			         a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			         a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
		}
		inline BoneQuat quat_conjugate(const BoneQuat& q) { return { q.w, -q.x, -q.y, -q.z }; }

		BoneQuat quat_slerp(BoneQuat qa, BoneQuat qb, float t, bool cut)
		{
			float dot = qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z;
			if (dot < 0.0f)
			{
				qb.w = -qb.w;
				qb.x = -qb.x;
				qb.y = -qb.y;
				qb.z = -qb.z;
				dot = -dot;
			}
			if (cut && dot < MODEL_CUT_COS_HALF) return (t < 0.5f) ? qa : qb;
			float weight_a = 1.0f - t, weight_b = t;
			if (dot < 0.9995f)
			{
				const float theta = acosf(dot), sin_theta = sinf(theta);
				weight_a = sinf((1.0f - t) * theta) / sin_theta;
				weight_b = sinf(t * theta) / sin_theta;
			}
			BoneQuat q = { weight_a * qa.w + weight_b * qb.w,
			               weight_a * qa.x + weight_b * qb.x,
			               weight_a * qa.y + weight_b * qb.y,
			               weight_a * qa.z + weight_b * qb.z };
			const float length = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
			if (length > 0.0f)
			{
				q.w /= length;
				q.x /= length;
				q.y /= length;
				q.z /= length;
			}
			return q;
		}

		// Writes the model-space blend of every bone's rotation; false (nothing written) when the
		// hierarchy is not parent-before-child, so the caller falls back to the local blend.
		bool write_model_space(uint8_t* pose, const Pose& a, const Pose& b, float t, int bone_count)
		{
			BoneQuat chain_a[MODEL_SPACE_MAX_BONES], chain_b[MODEL_SPACE_MAX_BONES], blended[MODEL_SPACE_MAX_BONES];
			int parent[MODEL_SPACE_MAX_BONES];
			for (int i = 0; i < bone_count; i++)
			{
				parent[i] = *(const int16_t*)(pose + POSE_SLOTS + i * POSE_SLOT_SIZE + SLOT_PARENT);
				if (parent[i] >= i) return false;
			}
			for (int i = 0; i < bone_count; i++)
			{
				const int16_t* angles_a = &a.bones[i * 6];
				const int16_t* angles_b = &b.bones[i * 6];
				const BoneQuat local_a = bone_angles_to_quat(angles_a), local_b = bone_angles_to_quat(angles_b);
				chain_a[i] = parent[i] >= 0 ? quat_multiply(chain_a[parent[i]], local_a) : local_a;
				chain_b[i] = parent[i] >= 0 ? quat_multiply(chain_b[parent[i]], local_b) : local_b;
				blended[i] = quat_slerp(chain_a[i], chain_b[i], t, true);
				const BoneQuat local = parent[i] >= 0 ? quat_multiply(quat_conjugate(blended[parent[i]]), blended[i]) : blended[i];
				int16_t* rotation = (int16_t*)(pose + POSE_SLOTS + i * POSE_SLOT_SIZE + SLOT_ROTATION);
				int16_t reference[3];
				for (int k = 0; k < 3; k++) reference[k] = lerp_angle(angles_a[k], angles_b[k], t);
				quat_to_bone_angles(local, reference, rotation);
			}
			return true;
		}

		void write_interpolated(uint8_t* pose, const Pose& a, const Pose& b, float t)
		{
			int bone_count = pose[0];
			if ((int)a.bones.size() != bone_count * 6 || (int)b.bones.size() != bone_count * 6) return;
			const bool rotation_blend = !(ff8_interp::cfg().battle_debug2 & DBG2_BONE_SLERP_OFF);
			const bool model_space = rotation_blend && bone_count <= MODEL_SPACE_MAX_BONES
				&& !(ff8_interp::cfg().battle_debug2 & DBG2_MODEL_SPACE_OFF);
			pose[POSE_FLAGS] = b.flags;
			for (int k = 0; k < 3; k++)
				((int16_t*)(pose + POSE_ROOT))[k] = lerp_linear(a.root[k], b.root[k], t);
			for (int i = 0; i < bone_count; i++)
			{
				uint8_t* slot = pose + POSE_SLOTS + i * POSE_SLOT_SIZE;
				int16_t* rotation = (int16_t*)(slot + SLOT_ROTATION);
				int16_t* scale = (int16_t*)(slot + SLOT_SCALE);
				const int16_t* bone_a = &a.bones[i * 6];
				const int16_t* bone_b = &b.bones[i * 6];
				bool large_step = false;
				for (int k = 0; k < 3; k++)
				{
					rotation[k] = lerp_angle(bone_a[k], bone_b[k], t);
					scale[k] = lerp_linear(bone_a[3 + k], bone_b[3 + k], t);
					const int angle_step = ((int(bone_b[k]) - int(bone_a[k]) + 2048) & 4095) - 2048;
					if (angle_step > BONE_SLERP_MIN || angle_step < -BONE_SLERP_MIN) large_step = true;
				}
				if (!large_step || !rotation_blend || model_space) continue;
				BoneQuat qa = bone_angles_to_quat(bone_a), qb = bone_angles_to_quat(bone_b);
				float dot = qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z;
				if (dot < 0.0f)
				{
					qb.w = -qb.w;
					qb.x = -qb.x;
					qb.y = -qb.y;
					qb.z = -qb.z;
					dot = -dot;
				}
				if (dot < BONE_CUT_COS_HALF)
				{
					// a real turn beyond 90 degrees in one step: show it as the engine does
					const int16_t* source = (t < 0.5f) ? bone_a : bone_b;
					for (int k = 0; k < 3; k++) rotation[k] = source[k];
					continue;
				}
				float weight_a = 1.0f - t, weight_b = t;
				if (dot < 0.9995f)
				{
					const float theta = acosf(dot), sin_theta = sinf(theta);
					weight_a = sinf((1.0f - t) * theta) / sin_theta;
					weight_b = sinf(t * theta) / sin_theta;
				}
				BoneQuat q = { weight_a * qa.w + weight_b * qb.w,
				               weight_a * qa.x + weight_b * qb.x,
				               weight_a * qa.y + weight_b * qb.y,
				               weight_a * qa.z + weight_b * qb.z };
				const float length = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
				if (length > 0.0f)
				{
					q.w /= length;
					q.x /= length;
					q.y /= length;
					q.z /= length;
				}
				int16_t reference[3] = { rotation[0], rotation[1], rotation[2] };
				quat_to_bone_angles(q, reference, rotation);
			}
			// v90: the loop above wrote the per-axis blend (scale, and the rotations as a fallback)
			if (model_space && !write_model_space(pose, a, b, t, bone_count))
			{
				// hierarchy not parent-before-child: v85 local slerp for this part
				for (int i = 0; i < bone_count; i++)
				{
					int16_t* rotation = (int16_t*)(pose + POSE_SLOTS + i * POSE_SLOT_SIZE + SLOT_ROTATION);
					const BoneQuat q = quat_slerp(bone_angles_to_quat(&a.bones[i * 6]), bone_angles_to_quat(&b.bones[i * 6]), t, false);
					int16_t reference[3] = { rotation[0], rotation[1], rotation[2] };
					quat_to_bone_angles(q, reference, rotation);
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
		bool poses_far_apart(const Pose& a, const Pose& b)
		{
			if (a.bones.size() != b.bones.size() || a.bones.empty()) return true;
			long total = 0;
			int count = 0;
			for (size_t i = 0; i < a.bones.size(); i += 6)
				for (int k = 0; k < 3; k++)
				{
					int difference = ((int(b.bones[i + k]) - int(a.bones[i + k]) + 2048) & 4095) - 2048;
					if (difference < 0) difference = -difference;
					if (difference > POSE_CUT_MAX) return true;
					total += difference;
					count++;
				}
			return count != 0 && total / count > POSE_CUT_MEAN;
		}

		// A freshly queued animation (frame 0 just decoded): start the blend from whatever pose was on
		// screen last so the change eases in over one logic tick, unless the two poses are far apart -
		// then keep the engine's hard cut. DBG_POSE_CUT_OFF always cross-fades (the pre-v54 behaviour).
		void snap(AnimationState& state, const uint8_t* pose, const uint8_t* cmd)
		{
			capture_pose(pose, state.next);
			state.root_jump = 0;
			if (state.shown_valid)
				for (int k = 0; k < 3; k++)
				{
					int jump = int(state.next.root[k]) - int(state.shown.root[k]);
					if (jump < 0) jump = -jump;
					if (jump > state.root_jump) state.root_jump = jump;
				}
			if (state.shown_valid && state.shown.bones.size() == state.next.bones.size()
				&& ((ff8_interp::cfg().battle_debug & DBG_POSE_CUT_OFF) || !poses_far_apart(state.shown, state.next)))
				state.prev = state.shown;
			else
			{
				state.prev = state.next;
				// v83: a cut is about the LIMBS. The root is a translation that the engine hands to the
				// sequence position in the same tick (run -> strike: root ~-1655 -> 0 while the position
				// moves -1655), so keep it blending from the shown root:
				// position lerp + root lerp = lerp of their sum, continuous without knowing the axes.
				// Only a root jump the size of a teleport (ENTITY_POS_SNAP) stays a cut.
				if (state.shown_valid && state.root_jump <= ENTITY_POS_SNAP && !(ff8_interp::cfg().battle_debug2 & DBG2_ROOT_BLEND_OFF))
					memcpy(state.prev.root, state.shown.root, sizeof(state.prev.root));
			}
			state.valid = true;
			state.anim_id = cmd[CMD_ANIM_ID];
			state.last_host_frame = 0xFFFFFFFF;
			state.last_result = 0;
			state.snap_frame = ff8_interp::frame_counter();
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
				auto it = anim_states.find(cmd);
				if (it != anim_states.end() && it->second.valid)
				{
					AnimationState& finished = it->second;
					// v83: the animation is over - hold its last frame. prev still held the frame BEFORE the
					// last one, so every held host frame in between (interpolate_part) re-blended
					// frame n-1 -> n and this restore snapped back to n on the tick: a dying enemy's final
					// pose twitched at 15 Hz for the whole death fade. Not on the host frame of the last
					// real read itself (several callers per frame), that blend is still running.
					if (finished.last_host_frame != ff8_interp::frame_counter() && !(ff8_interp::cfg().battle_debug2 & DBG2_HOLD_LAST_OFF))
					{
						finished.prev = finished.next;
						finished.shown = finished.next;
						finished.shown_valid = true;
					}
					// Make sure the final decoded pose is what stays on screen.
					restore_pose(pose, finished.next);
					build_bone_matrices(ctx);
				}
				int result = original_read_animation(ctx, cmd);
				if (ff8_interp::cfg().trace)
					ff8_interp::log_trace("ff8anim f=%u cmd=%p id=%d %d/%d finished -> %d\n", ff8_interp::frame_counter(), cmd, cmd[CMD_ANIM_ID], cmd[CMD_CURRENT_FRAME], cmd[CMD_TOTAL_FRAMES], result);
				return result;
			}

			AnimationState& state = anim_states[cmd];

			// Frame 0 read right after (pre_)Battle_ReadAnimation reset the command: never gate it,
			// it is the absolute base pose of a freshly queued animation.
			if (cmd[CMD_CURRENT_FRAME] == 0)
			{
				int result = original_read_animation(ctx, cmd);
				snap(state, pose, cmd);
				update_tick();
				if (tick_alpha < 1.0f && !gf_siren_exact_pose() && state.prev.bones.size() == state.next.bones.size()) // v75: Siren keeps the exact pose (gf_siren.cpp)
				{
					write_interpolated(pose, state.prev, state.next, tick_alpha);
					build_bone_matrices(ctx);
				}
				capture_pose(pose, state.shown);
				state.shown_valid = true;
				if (ff8_interp::cfg().trace)
					ff8_interp::log_trace("ff8anim f=%u cmd=%p id=%d frame0 total=%d bones=%d flags=%02x -> %d\n", ff8_interp::frame_counter(), cmd, cmd[CMD_ANIM_ID], cmd[CMD_TOTAL_FRAMES], pose[0], cmd[1], result);
				return result;
			}

			if (!state.valid || state.anim_id != cmd[CMD_ANIM_ID])
				snap(state, pose, cmd);

			// Serviced already on this host frame (several callers per frame): keep the answer.
			if (state.last_host_frame == ff8_interp::frame_counter())
			{
				if (ff8_interp::cfg().trace)
					ff8_interp::log_trace("ff8anim f=%u cmd=%p id=%d %d/%d repeat -> %d\n", ff8_interp::frame_counter(), cmd, cmd[CMD_ANIM_ID], cmd[CMD_CURRENT_FRAME], cmd[CMD_TOTAL_FRAMES], state.last_result);
				return state.last_result;
			}
			state.last_host_frame = ff8_interp::frame_counter();

			update_tick();
			if (is_logic_tick)
			{
				// Deltas accumulate in place: put the exact last decoded pose back before decoding.
				restore_pose(pose, state.next);
				// Frame 0 and frame 1 usually decode in the same host frame: keep the cross-fade start.
				if (state.snap_frame != ff8_interp::frame_counter()) state.prev = state.next;
				state.last_result = original_read_animation(ctx, cmd);
				capture_pose(pose, state.next);
			}
			else
			{
				state.last_result = 0;
			}

			// Show A -> B spread over the host frames of this logic tick.
			// v75: inside Siren's paced tick keep the exact decoded pose - its recorded display list is replay-blended (gf_siren.cpp).
			float t = gf_siren_exact_pose() ? 1.0f : tick_alpha;
			if (t < 1.0f)
				write_interpolated(pose, state.prev, state.next, t);
			else
				restore_pose(pose, state.next);
			build_bone_matrices(ctx);
			capture_pose(pose, state.shown);
			state.shown_valid = true;

			if (ff8_interp::cfg().trace)
				ff8_interp::log_trace("ff8anim f=%u cmd=%p id=%d %d/%d %s t=%.2f flags=%02x -> %d\n", ff8_interp::frame_counter(), cmd, cmd[CMD_ANIM_ID], cmd[CMD_CURRENT_FRAME], cmd[CMD_TOTAL_FRAMES], is_logic_tick ? "READ" : "interp", t, cmd[1], state.last_result);
			return state.last_result;
		}

		// Skipped logic frame for one entity part: show the pose between the last two decoded frames.
		void interpolate_part(uint8_t* ctx, uint8_t* cmd)
		{
			uint8_t* pose = pose_of(ctx);
			if (pose == nullptr || pose[0] == 0) return;
			auto it = anim_states.find(cmd);
			if (it == anim_states.end() || !it->second.valid || it->second.anim_id != cmd[CMD_ANIM_ID]) return;
			AnimationState& state = it->second;
			float t = tick_alpha;
			if (t < 1.0f)
				write_interpolated(pose, state.prev, state.next, t);
			else
				restore_pose(pose, state.next);
			build_bone_matrices(ctx);
			capture_pose(pose, state.shown);
			state.shown_valid = true;
		}

		// Replacement for BattleEntity_RunSequence (0x504290): the sec-5 sequence VM, wait counters,
		// hit/damage triggers and the animation advance all live here, so it must run at 15 fps.
		inline int16_t lerp_i16(int16_t a, int16_t b, float t) { return lerp_linear(a, b, t); }

		void write_interpolated_position(uint8_t* entity, EntityState& entity_state, float t)
		{
			int16_t values[POSITION_FIELDS];
			for (int k = 0; k < POSITION_FIELDS; k++)
				values[k] = (t >= 1.0f) ? entity_state.next[k] : lerp_i16(entity_state.prev[k], entity_state.next[k], t);
			restore_position(entity, values);
			memcpy(entity_state.written, values, sizeof(values));
			entity_state.written_valid = true;
		}

		// v102: positions written OUTSIDE the sequence driver. The driver hook captured an entity's position
		// right after its own sequence ran and, on every later frame, wrote lerp(prev, next) back (and
		// restored `next` before the next tick's driver). An effect that moves an entity whose sequence
		// keeps running - Elvoret's entrance (effect 17) lowering it from above while it plays its idle -
		// writes the position after that capture, so its move was overwritten every frame: Elvoret stood at
		// its final place from the start ("snaps into position"). Now a field that no longer holds what
		// this module wrote last is someone else's write: it becomes the logic position at the next tick
		// and is blended like any other move (one tick behind, as everything). (Paused entities: v89.)
		void position_note_external(uint8_t* entity, EntityState& entity_state)
		{
			if (!entity_state.valid || !entity_state.written_valid || (ff8_interp::cfg().battle_debug2 & DBG2_EXTERNAL_POSITION_OFF)) return;
			int16_t current[POSITION_FIELDS];
			capture_position(entity, current);
			if (memcmp(current, entity_state.written, sizeof(current)) == 0) return;
			if (ff8_interp::cfg().trace)
				ff8_interp::log_trace("ff8extpos f=%u ent=%p tick=%d ent(x,yo,z,y)=(%d,%d,%d,%d) seq=(%d,%d,%d) d_from_ours=(%d,%d,%d,%d | %d,%d,%d)\n", ff8_interp::frame_counter(), entity, (int)is_logic_tick,
					current[0], current[1], current[2], current[3], current[4], current[5], current[6],
					current[0] - entity_state.written[0], current[1] - entity_state.written[1], current[2] - entity_state.written[2], current[3] - entity_state.written[3], current[4] - entity_state.written[4], current[5] - entity_state.written[5], current[6] - entity_state.written[6]);
			memcpy(entity_state.external, current, sizeof(current));
			entity_state.external_pending = true;
			memcpy(entity_state.written, current, sizeof(current)); // until we write again, this is what the fields hold
		}

		// Before an effect tree runs: every entity whose fields still hold our blend gets its logic
		// position back, so an effect that reads or moves positions sees the engine's values.
		void position_restore_logic_all()
		{
			if (ff8_interp::cfg().battle_debug2 & DBG2_EXTERNAL_POSITION_OFF) return;
			for (auto& entry : entity_states)
			{
				EntityState& entity_state = entry.second;
				if (!entity_state.valid || !entity_state.written_valid) continue;
				// paused entities (the task skips their driver) belong to the v89 paused blend
				const uint8_t* entity = entry.first;
				const uint8_t* sequence = *(const uint8_t* const*)(entity + ENTITY_SEQUENCE);
				const bool driver_skipped = (entity[0] & 0x08) || ((entity[0x08] & 0x18) && sequence != nullptr && (sequence[0x2C] & 0x40));
				if (driver_skipped) continue;
				position_note_external(entry.first, entity_state);
				const int16_t* logic = entity_state.external_pending ? entity_state.external : entity_state.next;
				restore_position(entry.first, logic);
				memcpy(entity_state.written, logic, sizeof(entity_state.written));
			}
		}

		// v82 root-motion hand-off. A run animation carries the character's travel in its pose ROOT
		// (Squall's attack run, anim 32: root ~-1655 by frame 14 while the sequence position stays 0).
		// When the sequence queues the next animation, pre_Battle_ReadAnimation zeroes the root and the
		// sec-5 VM moves the sequence position by the same amount IN THE SAME TICK (ff8pos d=-1655 with
		// ff8anim id=34 frame0). The engine's two frames are the same place on screen. Blending only the
		// position (0 -> -1655 over the tick) while the pose shows the new frame 0 (root ~0) draws him
		// back at his start position on the tick frame, then zipping forward: the v81 "teleport back for
		// one frame before the leap". A hand-off is shown as the engine's cut for pose AND position.
		constexpr int ROOT_HANDOFF_MIN = 256; // root jump on a frame-0 snap that counts as a hand-off

		void cut_pose(uint8_t* ctx, uint8_t* cmd)
		{
			auto it = anim_states.find(cmd);
			if (it == anim_states.end() || !it->second.valid || it->second.snap_frame != ff8_interp::frame_counter()) return;
			AnimationState& state = it->second;
			state.prev = state.next;
			uint8_t* pose = pose_of(ctx);
			if (pose == nullptr || pose[0] == 0 || (int)state.next.bones.size() != pose[0] * 6) return;
			restore_pose(pose, state.next);
			build_bone_matrices(ctx);
			state.shown = state.next;
			state.shown_valid = true;
		}

		// True when this tick's body frame-0 snap jumped the pose root: the position change of the same
		// tick is the engine moving that travel from the root into the sequence position.
		// v83: superseded by the root blend in snap(); only runs with DBG2_ROOT_BLEND_OFF (the cut itself
		// showed a one-tick step, and fired on plain motion too: enemy anim 8 -> 9 at d=-612 per tick).
		bool root_handoff(uint8_t* entity)
		{
			if (ff8_interp::cfg().battle_debug2 & DBG2_ROOT_HANDOFF_OFF) return false;
			if (!(ff8_interp::cfg().battle_debug2 & DBG2_ROOT_BLEND_OFF)) return false;
			auto it = anim_states.find(entity + ENTITY_ANIM_CMD);
			return it != anim_states.end() && it->second.valid && it->second.snap_frame == ff8_interp::frame_counter()
				&& it->second.root_jump > ROOT_HANDOFF_MIN;
		}


		// ---- v91: weapon ends held by a hand (grip) ---------------------------------------------------
		// Found in an offline animation viewer. Selphie holds her
		// nunchaku with both hands: one stick hangs off the weapon root, the far one (d5w028 bone 6) sits
		// behind four chain links (1, 3, 4, 5) in her other hand. Body and weapon are separate parts,
		// blended separately, so between two logic frames the far stick leaves the hand (1.0..2.4 units;
		// no rotation blend can close a loop that runs through two parts). Per logic tick, for every
		// weapon end behind single-child links, the body bone in whose frame the end sits still from
		// frame A to frame B (moves <= 0.15 mesh units, turns <= 5 degrees) is taken as the hand holding
		// it (hands differ per animation: anim 5 bone 23, the win pose bone 24). Every frame, the end's
		// pose relative to that hand is blended between A and B and carried by the BLENDED hand; the links
		// are bent towards it with FABRIK (first link's origin fixed), each link turned the least way so it
		// keeps its roll, and the end takes the target rotation. Frames A and B themselves are untouched.
		// v92: WHICH end is held by WHICH hand in WHICH steps is no longer guessed per tick (v91 took any
		// body bone the end kept still against, without contact: Quistis' whip tip "held" by her torso or
		// thigh, and the links included the handle, so the whip jumped and left her hands). It comes from
		// GRIP_TABLE, made offline by an animation viewer:
		// hands = body bones with a mesh and no children; a step f -> f+1 is held when the end keeps still
		// against the hand (<= 0.15 mesh units, <= 5 degrees) AND its segment passes within 0.5 units of
		// the hand's mesh at both frames; one hand per animation (most held steps); links stop at the
		// root's only child (the handle). The loop step last -> 0 is never held.
		// Engine facts used: PoseBuf slot +0x10 MATRIX (3x3 i16 / 4096, t i32 at +0x24), child origin =
		// parent origin + parent Z column * parent length (0x508C90); the weapon is drawn with the entity
		// matrix times the weapon block's attach matrix (+0x24, 0x502D40) - the grip only runs while that
		// is identity, i.e. weapon model space = body model space, as in the files.
		struct BoneMatrix
		{
			float rotation[9];
			float translation[3];
		};
		// v100: the ENGINE builds a bone's local rotation as Ry(ry) . Rx(rx) . Rz(rz) with standard
		// elementary rotations (0x56CE30 -> 0x56D090 / 0x56CFB0 / 0x56D020: identity, then Z, X, Y
		// premultiplied). bone_angles_to_quat / quat_to_bone_angles use the editor's Ry(-ry) . Rx(-rx) . Rz(rz), which is the
		// engine matrix conjugated by T = diag(-1, -1, 1) (a 180-degree turn about Z; the editor's root is
		// T * root as well). Blending is the same in both frames (write_interpolated, write_model_space
		// convert in and out with the same pair), but the grip works on the LIVE engine matrices: every
		// rotation it hands to quat_to_bone_angles must go through T first. v91-v99 skipped that, so the grip wrote
		// its links and the held end turned 180 degrees about Z - the whip / nunchaku jump, although its
		// position corrections were tiny.
		inline void conjugate_by_t(const float* in, float* out) // T * m * T: negate the entries with exactly one z index
		{
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					out[i * 3 + j] = ((i == 2) != (j == 2)) ? -in[i * 3 + j] : in[i * 3 + j];
		}
		constexpr int GRIP_MAX_BODY_BONES = 64;
		constexpr int GRIP_MAX_ENDS = 4;
		constexpr uint32_t WEAPON_ATTACH = 0x24;   // MATRIX in the weapon block

		// The end bone's pose in the hand bone's frame: unit quaternion + origin in RAW units (bone-length
		// units, before the model scale).
		struct GripPose { float qw, qx, qy, qz, px, py, pz; };
		struct GripEnd
		{
			int end;
			int hand;
			int link_count;
			int links[MODEL_SPACE_MAX_BONES];
			GripPose hand_to_end_a, hand_to_end_b;
		};
		struct GripState
		{
			uint32_t tick = 0xFFFFFFFF;
			int end_count = 0;
			GripEnd ends[GRIP_MAX_ENDS];
			uint32_t traced_tick = 0;
			int last_frame = -1;     // weapon cmd frame at the previous tick
			uint8_t last_anim = 0xFF; // weapon cmd anim at the previous tick
			// v95 diagnostics, per tick (trace): plain blend tip -> target, biggest FABRIK joint move,
			// |rel_A.p - rel_B.p|, tip -> hand distance at A, frames applied
			float max_drift = 0.0f;
			float max_move = 0.0f;
			float hand_to_end_change = 0.0f;
			float end_reach_a = 0.0f;
			int frames_applied = 0;
			int step_from = 0, step_to = 0;
		};

		// Generated offline by an animation viewer over every dXc / dXw pair and the r0win win poses
		// (weapon variants and body models agree). Key: weapon bones, body bones, animation id (or the
		// win pose, matched by its frame count), frame count, end bone. held_steps: bit f = step f -> f+1.
		constexpr uint8_t GRIP_ANIM_WIN = 0xFF;
		struct GripRow
		{
			uint8_t weapon_bones, body_bones, anim, frames, end, hand, link_count;
			uint8_t links[8];
			uint64_t held_steps;
		};
		const GripRow GRIP_TABLE[] = {
			{ 7, 25, 0, 12, 6, 23, 4, { 1, 3, 4, 5 }, 0x00000000000007FFull },
			{ 7, 25, 1, 15, 6, 23, 4, { 1, 3, 4, 5 }, 0x0000000000003FFFull },
			{ 7, 25, 3, 9, 6, 23, 4, { 1, 3, 4, 5 }, 0x00000000000000FFull },
			{ 7, 25, 4, 6, 6, 23, 4, { 1, 3, 4, 5 }, 0x000000000000001Full },
			{ 7, 25, 5, 15, 6, 23, 4, { 1, 3, 4, 5 }, 0x0000000000003FFFull },
			{ 7, 25, 6, 13, 6, 23, 4, { 1, 3, 4, 5 }, 0x0000000000000FFFull },
			{ 7, 25, 7, 27, 6, 23, 4, { 1, 3, 4, 5 }, 0x0000000003FFFFFFull },
			{ 7, 25, 8, 8, 6, 23, 4, { 1, 3, 4, 5 }, 0x000000000000007Full },
			{ 7, 25, 9, 8, 6, 23, 4, { 1, 3, 4, 5 }, 0x000000000000007Full },
			{ 7, 25, 10, 12, 6, 23, 4, { 1, 3, 4, 5 }, 0x00000000000007FFull },
			{ 7, 25, 11, 9, 6, 23, 4, { 1, 3, 4, 5 }, 0x00000000000000FFull },
			{ 7, 25, 12, 12, 6, 23, 4, { 1, 3, 4, 5 }, 0x00000000000007FFull },
			{ 7, 25, 13, 17, 6, 24, 4, { 1, 3, 4, 5 }, 0x0000000000000020ull },
			{ 7, 25, 14, 40, 6, 24, 4, { 1, 3, 4, 5 }, 0x0000001801FCBFFFull },
			{ 7, 25, 15, 12, 6, 23, 4, { 1, 3, 4, 5 }, 0x00000000000007FFull },
			{ 7, 25, 16, 9, 6, 23, 4, { 1, 3, 4, 5 }, 0x00000000000000FFull },
			{ 7, 25, 17, 6, 6, 23, 4, { 1, 3, 4, 5 }, 0x000000000000001Full },
			{ 7, 25, 18, 19, 6, 23, 4, { 1, 3, 4, 5 }, 0x000000000003FFFFull },
			{ 7, 25, 19, 3, 6, 23, 4, { 1, 3, 4, 5 }, 0x0000000000000003ull },
			{ 7, 25, 20, 8, 6, 23, 4, { 1, 3, 4, 5 }, 0x000000000000007Full },
			{ 7, 25, 21, 3, 6, 23, 4, { 1, 3, 4, 5 }, 0x0000000000000003ull },
			{ 7, 25, 22, 7, 6, 23, 4, { 1, 3, 4, 5 }, 0x000000000000003Full },
			{ 7, 25, 23, 10, 6, 24, 4, { 1, 3, 4, 5 }, 0x00000000000001A0ull },
			{ 7, 25, 24, 3, 6, 24, 4, { 1, 3, 4, 5 }, 0x0000000000000003ull },
			{ 7, 25, 25, 12, 6, 24, 4, { 1, 3, 4, 5 }, 0x00000000000003FFull },
			{ 7, 25, 26, 3, 6, 23, 4, { 1, 3, 4, 5 }, 0x0000000000000003ull },
			{ 7, 25, 27, 3, 6, 23, 4, { 1, 3, 4, 5 }, 0x0000000000000003ull },
			{ 7, 25, 28, 8, 6, 24, 4, { 1, 3, 4, 5 }, 0x000000000000007Full },
			{ 7, 25, 29, 30, 6, 23, 4, { 1, 3, 4, 5 }, 0x000000001E00007Full },
			{ 7, 25, 30, 33, 6, 23, 4, { 1, 3, 4, 5 }, 0x00000000FFFFFFFFull },
			{ 7, 25, 31, 2, 6, 23, 4, { 1, 3, 4, 5 }, 0x0000000000000001ull },
			{ 7, 25, 32, 7, 6, 23, 4, { 1, 3, 4, 5 }, 0x000000000000003Full },
			{ 7, 25, GRIP_ANIM_WIN, 60, 6, 24, 4, { 1, 3, 4, 5 }, 0x07FFFFFFFFFFFFFFull },
			{ 9, 27, 3, 12, 8, 25, 6, { 2, 3, 4, 5, 6, 7 }, 0x00000000000007FFull },
			{ 9, 27, 4, 9, 8, 25, 6, { 2, 3, 4, 5, 6, 7 }, 0x00000000000000FFull },
			{ 9, 27, 18, 29, 8, 26, 6, { 2, 3, 4, 5, 6, 7 }, 0x000000000FF80000ull },
			{ 9, 27, 19, 2, 8, 26, 6, { 2, 3, 4, 5, 6, 7 }, 0x0000000000000001ull },
			{ 9, 27, 20, 5, 8, 26, 6, { 2, 3, 4, 5, 6, 7 }, 0x0000000000000002ull },
			{ 9, 27, 21, 6, 8, 26, 6, { 2, 3, 4, 5, 6, 7 }, 0x000000000000001Full },
			{ 9, 27, 23, 10, 8, 26, 6, { 2, 3, 4, 5, 6, 7 }, 0x0000000000000062ull },
			{ 9, 27, 24, 4, 8, 26, 6, { 2, 3, 4, 5, 6, 7 }, 0x0000000000000007ull },
			{ 9, 27, 26, 3, 8, 26, 6, { 2, 3, 4, 5, 6, 7 }, 0x0000000000000003ull },
			{ 9, 27, 29, 4, 8, 26, 6, { 2, 3, 4, 5, 6, 7 }, 0x0000000000000007ull },
			{ 9, 27, 30, 8, 8, 26, 6, { 2, 3, 4, 5, 6, 7 }, 0x0000000000000014ull },
			{ 9, 27, 31, 5, 8, 26, 6, { 2, 3, 4, 5, 6, 7 }, 0x0000000000000008ull },
			{ 9, 27, GRIP_ANIM_WIN, 60, 8, 26, 6, { 2, 3, 4, 5, 6, 7 }, 0x0613FE7E001FC000ull },
		};
		constexpr int GRIP_ROW_COUNT = (int)(sizeof(GRIP_TABLE) / sizeof(GRIP_TABLE[0]));
		std::unordered_map<uint8_t*, GripState> grip_states; // keyed by BattleEntity*

		BoneMatrix slot_matrix(const uint8_t* pose, int bone)
		{
			const uint8_t* slot = pose + POSE_SLOTS + bone * POSE_SLOT_SIZE;
			const int16_t* matrix = (const int16_t*)(slot + 0x10);
			const int32_t* translation = (const int32_t*)(slot + 0x24);
			BoneMatrix result;
			for (int k = 0; k < 9; k++) result.rotation[k] = matrix[k] / 4096.0f;
			for (int k = 0; k < 3; k++) result.translation[k] = float(translation[k]);
			for (int c = 0; c < 3; c++) // unit columns: model scale and bone scale out
			{
				const float length = sqrtf(result.rotation[c] * result.rotation[c] + result.rotation[3 + c] * result.rotation[3 + c] + result.rotation[6 + c] * result.rotation[6 + c]);
				if (length > 0.0f)
				{
					result.rotation[c] /= length;
					result.rotation[3 + c] /= length;
					result.rotation[6 + c] /= length;
				}
			}
			return result;
		}
		inline void mat3_multiply(const float* a, const float* b, float* out)
		{
			float result[9];
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					result[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
			memcpy(out, result, sizeof(result));
		}
		inline void mat3_transpose_multiply(const float* a, const float* b, float* out) // a^T * b
		{
			float result[9];
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					result[i * 3 + j] = a[i] * b[j] + a[3 + i] * b[3 + j] + a[6 + i] * b[6 + j];
			memcpy(out, result, sizeof(result));
		}
		inline void mat3_apply(const float* m, const float* v, float* out)
		{
			float result[3];
			for (int i = 0; i < 3; i++) result[i] = m[i * 3] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2];
			memcpy(out, result, sizeof(result));
		}
		inline void quat_to_matrix(const BoneQuat& q, float* m)
		{
			const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
			const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
			m[0] = 1 - 2 * (yy + zz);
			m[1] = 2 * (xy - wz);
			m[2] = 2 * (xz + wy);
			m[3] = 2 * (xy + wz);
			m[4] = 1 - 2 * (xx + zz);
			m[5] = 2 * (yz - wx);
			m[6] = 2 * (xz - wy);
			m[7] = 2 * (yz + wx);
			m[8] = 1 - 2 * (xx + yy);
		}
		// The end's pose in the hand's frame: rotation H^T E, position H^T (e - h); packed r[9] + p[3].
		void relative_pose(const BoneMatrix& hand, const BoneMatrix& end, float* out)
		{
			mat3_transpose_multiply(hand.rotation, end.rotation, out);
			const float offset[3] = { end.translation[0] - hand.translation[0], end.translation[1] - hand.translation[1], end.translation[2] - hand.translation[2] };
			float hand_transposed[9];
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					hand_transposed[i * 3 + j] = hand.rotation[j * 3 + i];
			mat3_apply(hand_transposed, offset, out + 9);
		}
		bool weapon_attach_identity(const uint8_t* weapon)
		{
			const int16_t* matrix = (const int16_t*)(weapon + WEAPON_ATTACH);
			const int32_t* translation = (const int32_t*)(weapon + WEAPON_ATTACH + 0x14);
			for (int i = 0; i < 9; i++) if (matrix[i] != ((i % 4 == 0) ? 4096 : 0)) return false;
			return translation[0] == 0 && translation[1] == 0 && translation[2] == 0;
		}

		// v98: frames A and B through our own float copy of the engine FK (0x508C90) on the stored poses,
		// in the engine's space and raw units: chain = parent chain * local (M = Ry(-ry) Rx(-rx) Rz(rz)),
		// child origin = parent origin + parent chain * (0, 0, parent length), root origin = stored root * 16
		// (the engine scales the root by scale >> 8 and the lengths by scale / 4096). Bone scale channels
		// are ignored. v91-v96 ran the engine FK on the live PoseBuf for this (restore A / B, build, back to
		// the blend): with it the whip jumped, without the grip it was calm. v97 took the anim viewer's
		// per-frame table instead, but the viewer places part roots in the editor's display axes (x and y
		// negated), so the hand-to-end offset, which runs through both parts' roots, came out ~500 engine
		// units off and the grip dragged the weapon away from the hand.
		void raw_forward_kinematics(const Pose& pose, const uint8_t* live, int bones, int bone, float* rotation_out, float* origin_out)
		{
			float chain[GRIP_MAX_BODY_BONES][9], origin[GRIP_MAX_BODY_BONES][3];
			for (int i = 0; i <= bone; i++)
			{
				const int parent = *(const int16_t*)(live + POSE_SLOTS + i * POSE_SLOT_SIZE + SLOT_PARENT);
				float local[9];
				quat_to_matrix(bone_angles_to_quat(&pose.bones[i * 6]), local);
				if (parent < 0 || parent >= i)
				{
					memcpy(chain[i], local, sizeof(local));
					origin[i][0] = -16.0f * pose.root[0];
					origin[i][1] = -16.0f * pose.root[1];
					origin[i][2] = 16.0f * pose.root[2];
				}
				else
				{
					mat3_multiply(chain[parent], local, chain[i]);
					const float length = (float)*(const int16_t*)(live + POSE_SLOTS + parent * POSE_SLOT_SIZE + 0x02);
					for (int k = 0; k < 3; k++)
						origin[i][k] = origin[parent][k] + chain[parent][k * 3 + 2] * length;
				}
			}
			// v101: kept in the editor's frame (bone_angles_to_quat convention, root mirrored like the editor's T * root),
			// the frame the whole grip now works in
			memcpy(rotation_out, chain[bone], 9 * sizeof(float));
			memcpy(origin_out, origin[bone], 3 * sizeof(float));
			(void)bones;
		}

		GripPose raw_hand_to_end_pose(const Pose& body, const uint8_t* body_pose, int hand, const Pose& weapon, const uint8_t* weapon_pose, int end)
		{
			float hand_rotation[9], hand_origin[3], end_rotation[9], end_origin[3];
			raw_forward_kinematics(body, body_pose, body_pose[0], hand, hand_rotation, hand_origin);
			raw_forward_kinematics(weapon, weapon_pose, weapon_pose[0], end, end_rotation, end_origin);
			float relative_rotation[9];
			mat3_transpose_multiply(hand_rotation, end_rotation, relative_rotation);
			const float offset[3] = { end_origin[0] - hand_origin[0], end_origin[1] - hand_origin[1], end_origin[2] - hand_origin[2] };
			float hand_transposed[9];
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					hand_transposed[i * 3 + j] = hand_rotation[j * 3 + i];
			float relative_position[3];
			mat3_apply(hand_transposed, offset, relative_position);
			const BoneQuat relative_quat = matrix_to_quat(relative_rotation);
			return { relative_quat.w, relative_quat.x, relative_quat.y, relative_quat.z, relative_position[0], relative_position[1], relative_position[2] };
		}

		// Once per logic tick: the table rows for this animation and step, and the end's pose in the hand
		// at frames A and B. Nothing is written to the live poses.
		void grip_find(GripState& grip, const uint8_t* body_pose, const AnimationState& body_state, const uint8_t* weapon_pose, const AnimationState& weapon_state,
			uint8_t anim, int frames, int from, int to)
		{
			grip.end_count = 0;
			const int body_bones = body_pose[0], weapon_bones = weapon_pose[0];
			for (int i = 0; i < weapon_bones; i++)
				if (*(const int16_t*)(weapon_pose + POSE_SLOTS + i * POSE_SLOT_SIZE + SLOT_PARENT) >= i) return;
			for (int i = 0; i < body_bones; i++)
				if (*(const int16_t*)(body_pose + POSE_SLOTS + i * POSE_SLOT_SIZE + SLOT_PARENT) >= i) return;
			bool exact_row_exists = false;
			for (int row_index = 0; row_index < GRIP_ROW_COUNT; row_index++)
				if (GRIP_TABLE[row_index].weapon_bones == weapon_bones && GRIP_TABLE[row_index].body_bones == body_bones
					&& GRIP_TABLE[row_index].anim == anim && GRIP_TABLE[row_index].frames == frames)
					exact_row_exists = true;
			for (int row_index = 0; row_index < GRIP_ROW_COUNT && grip.end_count < GRIP_MAX_ENDS; row_index++)
			{
				const GripRow& row = GRIP_TABLE[row_index];
				if (row.weapon_bones != weapon_bones || row.body_bones != body_bones || row.frames != frames) continue;
				if (exact_row_exists ? row.anim != anim : row.anim != GRIP_ANIM_WIN) continue;
				// held across every step from -> to (one step normally, two with the FAST flag)
				if (from < 0 || to != from + 1) continue; // v101: single steps only, as the viewer (FAST double steps are not held)
				bool held = true;
				for (int step = from; step < to; step++)
					if (step >= 64 || !((row.held_steps >> step) & 1)) held = false;
				if (!held || row.end >= weapon_bones || row.hand >= body_bones) continue;
				GripEnd& held_end = grip.ends[grip.end_count++];
				held_end.end = row.end;
				held_end.hand = row.hand;
				held_end.link_count = row.link_count;
				for (int k = 0; k < row.link_count; k++)
					held_end.links[k] = row.links[k];
				held_end.hand_to_end_a = raw_hand_to_end_pose(body_state.prev, body_pose, row.hand, weapon_state.prev, weapon_pose, row.end);
				held_end.hand_to_end_b = raw_hand_to_end_pose(body_state.next, body_pose, row.hand, weapon_state.next, weapon_pose, row.end);
			}
		}

		void grip_apply(uint8_t* entity)
		{
			if (ff8_interp::cfg().battle_debug2 & (DBG2_GRIP_OFF | DBG2_MODEL_SPACE_OFF | DBG2_BONE_SLERP_OFF)) return;
			uint8_t* weapon = *(uint8_t**)(entity + ENTITY_WEAPON);
			if (weapon == nullptr) return;
			if (!weapon_attach_identity(weapon))
			{
				static uint8_t* reported_weapon = nullptr;
				if (ff8_interp::cfg().trace && reported_weapon != weapon)
				{
					reported_weapon = weapon;
					const int16_t* matrix = (const int16_t*)(weapon + WEAPON_ATTACH);
					const int32_t* translation = (const int32_t*)(weapon + WEAPON_ATTACH + 0x14);
					ff8_interp::log_trace("ff8grip ent=%p weapon attach not identity (%d %d %d / %d %d %d / %d %d %d, t %d %d %d): grip off for it\n", entity,
						matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5], matrix[6], matrix[7], matrix[8], translation[0], translation[1], translation[2]);
				}
				return;
			}
			uint8_t* body_ctx = entity + ENTITY_ANIM_CTX;
			uint8_t* weapon_ctx = weapon + WEAPON_ANIM_CTX;
			uint8_t* body_pose = pose_of(body_ctx);
			uint8_t* weapon_pose = pose_of(weapon_ctx);
			if (body_pose == nullptr || weapon_pose == nullptr || body_pose[0] == 0 || weapon_pose[0] == 0) return;
			if (body_pose[0] > GRIP_MAX_BODY_BONES || weapon_pose[0] > MODEL_SPACE_MAX_BONES) return;
			auto body_entry = anim_states.find(entity + ENTITY_ANIM_CMD);
			auto weapon_entry = anim_states.find(weapon + WEAPON_ANIM_CMD);
			if (body_entry == anim_states.end() || weapon_entry == anim_states.end()) return;
			AnimationState& body_state = body_entry->second;
			AnimationState& weapon_state = weapon_entry->second;
			if (!body_state.valid || !weapon_state.valid
				|| body_state.anim_id != entity[ENTITY_ANIM_CMD + CMD_ANIM_ID]
				|| weapon_state.anim_id != weapon[WEAPON_ANIM_CMD + CMD_ANIM_ID])
				return;
			if ((int)body_state.prev.bones.size() != body_pose[0] * 6 || (int)body_state.next.bones.size() != body_pose[0] * 6) return;
			if ((int)weapon_state.prev.bones.size() != weapon_pose[0] * 6 || (int)weapon_state.next.bones.size() != weapon_pose[0] * 6) return;
			const float t = tick_alpha;
			if (t >= 1.0f || gf_siren_exact_pose()) return;

			GripState& grip = grip_states[entity];
			if (grip.tick != logic_ticks)
			{
				if (ff8_interp::cfg().trace && grip.end_count > 0 && grip.frames_applied > 0)
					ff8_interp::log_trace("ff8grip tick ent=%p anim=%d step %d->%d frames=%d drift(blend tip->target)=%.0f fabrik_move=%.0f relAB=%.0f tip_hand_A=%.0f\n",
						entity, grip.last_anim, grip.step_from, grip.step_to, grip.frames_applied, grip.max_drift, grip.max_move, grip.hand_to_end_change, grip.end_reach_a);
				grip.max_drift = grip.max_move = grip.hand_to_end_change = grip.end_reach_a = 0.0f;
				grip.frames_applied = 0;
				grip.tick = logic_ticks;
				// step: the frame the weapon showed at the previous tick -> the frame it shows now; a new
				// animation or a loop back is not a held step. v96: cmd [6] is the frame just read PLUS ONE
				// (trace: the last frame of 12 reads as 12/12, frame 1 after a restart as 2/12), except right
				// after a (re)queue, where frame 0 shows as 0. v92-v95 used cmd [6] itself: every held-step
				// lookup was one step late (Quistis anim 31: the grip ran on 2->3, the table holds 3->4).
				const uint8_t anim = weapon[WEAPON_ANIM_CMD + CMD_ANIM_ID];
				const int raw_frame = weapon[WEAPON_ANIM_CMD + CMD_CURRENT_FRAME];
				const int current_frame = raw_frame > 0 ? raw_frame - 1 : 0;
				const int from = (grip.last_anim == anim && grip.last_frame >= 0) ? grip.last_frame : current_frame - 1;
				grip.last_anim = anim;
				grip.last_frame = current_frame;
				grip.step_from = from;
				grip.step_to = current_frame;
				grip_find(grip, body_pose, body_state, weapon_pose, weapon_state, anim, weapon[WEAPON_ANIM_CMD + CMD_TOTAL_FRAMES], from, current_frame);
				if (grip.end_count > 0)
				{
					const GripPose& pose_a = grip.ends[0].hand_to_end_a;
					const GripPose& pose_b = grip.ends[0].hand_to_end_b;
					grip.hand_to_end_change = sqrtf((pose_a.px - pose_b.px) * (pose_a.px - pose_b.px)
						+ (pose_a.py - pose_b.py) * (pose_a.py - pose_b.py)
						+ (pose_a.pz - pose_b.pz) * (pose_a.pz - pose_b.pz));
					grip.end_reach_a = sqrtf(pose_a.px * pose_a.px + pose_a.py * pose_a.py + pose_a.pz * pose_a.pz);
				}
				if (ff8_interp::cfg().trace && grip.end_count > 0 && grip.traced_tick != logic_ticks)
				{
					grip.traced_tick = logic_ticks;
					ff8_interp::log_trace("ff8grip f=%u ent=%p anim=%d end=%d hand=%d links=%d (%d grips)\n", ff8_interp::frame_counter(), entity,
						entity[ENTITY_ANIM_CMD + CMD_ANIM_ID], grip.ends[0].end, grip.ends[0].hand, grip.ends[0].link_count, grip.end_count);
				}
			}
			if (grip.end_count == 0) return;

			// v101: the whole solve in floats from the blended angles the parts hold now, in the editor's
			// frame and raw units - exactly the anim viewer's holdOne (grip.js). v91-v100 read the LIVE engine
			// matrices instead (rotations in 1/4096, positions in whole engine units, a link ~112 units long)
			// and converted conventions and units: each step ~1 % off, which along the chain to an end ~4 mesh
			// units from the hand came out as a per-frame wobble of +-0.1 mesh units (v100 dump vs viewer:
			// body and pre-grip weapon identical, the held end jittering) - the whip / nunchaku jump.
			const Pose* current_body = nullptr;
			Pose body_now, weapon_now;
			capture_pose(body_pose, body_now);
			capture_pose(weapon_pose, weapon_now);
			current_body = &body_now;
			const int weapon_bones = weapon_pose[0];
			float weapon_chain[MODEL_SPACE_MAX_BONES][9], weapon_origin[MODEL_SPACE_MAX_BONES][3];
			for (int i = 0; i < weapon_bones; i++)
				raw_forward_kinematics(weapon_now, weapon_pose, weapon_bones, i, weapon_chain[i], weapon_origin[i]);
			bool changed = false;
			for (int grip_index = 0; grip_index < grip.end_count; grip_index++)
			{
				const GripEnd& held_end = grip.ends[grip_index];
				float hand_rotation[9], hand_origin[3];
				raw_forward_kinematics(*current_body, body_pose, body_pose[0], held_end.hand, hand_rotation, hand_origin);
				// target: the blended hand carrying the blended relative pose
				const BoneQuat qa = { held_end.hand_to_end_a.qw, held_end.hand_to_end_a.qx, held_end.hand_to_end_a.qy, held_end.hand_to_end_a.qz };
				const BoneQuat qb = { held_end.hand_to_end_b.qw, held_end.hand_to_end_b.qx, held_end.hand_to_end_b.qy, held_end.hand_to_end_b.qz };
				float relative_rotation[9];
				quat_to_matrix(quat_slerp(qa, qb, t, false), relative_rotation);
				const float relative_position[3] = { held_end.hand_to_end_a.px + (held_end.hand_to_end_b.px - held_end.hand_to_end_a.px) * t,
				                                     held_end.hand_to_end_a.py + (held_end.hand_to_end_b.py - held_end.hand_to_end_a.py) * t,
				                                     held_end.hand_to_end_a.pz + (held_end.hand_to_end_b.pz - held_end.hand_to_end_a.pz) * t };
				float target_rotation[9], target_position[3];
				mat3_multiply(hand_rotation, relative_rotation, target_rotation);
				mat3_apply(hand_rotation, relative_position, target_position);
				for (int k = 0; k < 3; k++) target_position[k] += hand_origin[k];

				// joints: link origins + the end's origin, from the model-space blend
				const int link_count = held_end.link_count;
				float joints[MODEL_SPACE_MAX_BONES + 1][3], old_joints[MODEL_SPACE_MAX_BONES + 1][3];
				float link_length[MODEL_SPACE_MAX_BONES];
				for (int k = 0; k < link_count; k++)
					memcpy(joints[k], weapon_origin[held_end.links[k]], sizeof(joints[k]));
				memcpy(joints[link_count], weapon_origin[held_end.end], sizeof(joints[link_count]));
				memcpy(old_joints, joints, sizeof(joints));
				{
					const float dx = joints[link_count][0] - target_position[0];
					const float dy = joints[link_count][1] - target_position[1];
					const float dz = joints[link_count][2] - target_position[2];
					const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
					if (grip_index == 0 && distance > grip.max_drift) grip.max_drift = distance;
				}
				float reach = 0.0f;
				for (int k = 0; k < link_count; k++)
				{
					const float dx = joints[k + 1][0] - joints[k][0];
					const float dy = joints[k + 1][1] - joints[k][1];
					const float dz = joints[k + 1][2] - joints[k][2];
					link_length[k] = sqrtf(dx * dx + dy * dy + dz * dz);
					reach += link_length[k];
				}
				auto towards = [](const float* from, const float* to, float length, float* out) {
					const float direction[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
					const float distance = sqrtf(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
					float result[3];
					for (int k = 0; k < 3; k++)
						result[k] = from[k] + (distance > 1e-6f ? direction[k] / distance * length : 0.0f);
					memcpy(out, result, sizeof(result));
				};
				const float root[3] = { joints[0][0], joints[0][1], joints[0][2] };
				const float root_to_target[3] = { target_position[0] - root[0], target_position[1] - root[1], target_position[2] - root[2] };
				if (sqrtf(root_to_target[0] * root_to_target[0] + root_to_target[1] * root_to_target[1] + root_to_target[2] * root_to_target[2]) >= reach)
				{
					for (int k = 0; k < link_count; k++)
						towards(joints[k], target_position, link_length[k], joints[k + 1]); // out of reach: straight
				}
				else
				{
					// tolerance: the viewer's 1e-5 mesh units = 0.02 raw
					for (int iteration = 0; iteration < 32; iteration++)
					{
						const float error_x = joints[link_count][0] - target_position[0];
						const float error_y = joints[link_count][1] - target_position[1];
						const float error_z = joints[link_count][2] - target_position[2];
						if (error_x * error_x + error_y * error_y + error_z * error_z <= 0.02f * 0.02f) break;
						memcpy(joints[link_count], target_position, sizeof(target_position));
						for (int k = link_count - 1; k > 0; k--)
							towards(joints[k + 1], joints[k], link_length[k], joints[k]);
						for (int k = 0; k < link_count; k++)
							towards(joints[k], joints[k + 1], link_length[k], joints[k + 1]);
					}
				}
				if (grip_index == 0)
				{
					for (int k = 1; k <= link_count; k++)
					{
						const float dx = joints[k][0] - old_joints[k][0];
						const float dy = joints[k][1] - old_joints[k][1];
						const float dz = joints[k][2] - old_joints[k][2];
						const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
						if (distance > grip.max_move) grip.max_move = distance;
					}
					grip.frames_applied++;
				}

				// new model rotations: each link turned the least way from its old direction to the new one
				float new_rotation[MODEL_SPACE_MAX_BONES + 1][9];
				for (int k = 0; k < link_count; k++)
				{
					float old_direction[3], new_direction[3];
					for (int c = 0; c < 3; c++)
					{
						old_direction[c] = old_joints[k + 1][c] - old_joints[k][c];
						new_direction[c] = joints[k + 1][c] - joints[k][c];
					}
					const float old_length = sqrtf(old_direction[0] * old_direction[0] + old_direction[1] * old_direction[1] + old_direction[2] * old_direction[2]);
					const float new_length = sqrtf(new_direction[0] * new_direction[0] + new_direction[1] * new_direction[1] + new_direction[2] * new_direction[2]);
					if (old_length <= 0.0f || new_length <= 0.0f)
					{
						memcpy(new_rotation[k], weapon_chain[held_end.links[k]], sizeof(new_rotation[k]));
						continue;
					}
					for (int c = 0; c < 3; c++)
					{
						old_direction[c] /= old_length;
						new_direction[c] /= new_length;
					}
					const float dot = old_direction[0] * new_direction[0] + old_direction[1] * new_direction[1] + old_direction[2] * new_direction[2];
					BoneQuat q;
					if (dot > -0.9999f)
					{
						q = { 1.0f + dot,
						      old_direction[1] * new_direction[2] - old_direction[2] * new_direction[1],
						      old_direction[2] * new_direction[0] - old_direction[0] * new_direction[2],
						      old_direction[0] * new_direction[1] - old_direction[1] * new_direction[0] };
						const float quat_length = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
						q.w /= quat_length;
						q.x /= quat_length;
						q.y /= quat_length;
						q.z /= quat_length;
					}
					else q = { 1.0f, 0.0f, 0.0f, 0.0f };
					float turn_matrix[9];
					quat_to_matrix(q, turn_matrix);
					mat3_multiply(turn_matrix, weapon_chain[held_end.links[k]], new_rotation[k]);
				}
				memcpy(new_rotation[link_count], target_rotation, sizeof(target_rotation));

				// back to local rotations: local = parent^T * child, near the blended triple (editor frame,
				// the frame quat_to_bone_angles works in - no conversion)
				for (int k = 0; k <= link_count; k++)
				{
					const int bone = (k < link_count) ? held_end.links[k] : held_end.end;
					const int parent = *(const int16_t*)(weapon_pose + POSE_SLOTS + bone * POSE_SLOT_SIZE + SLOT_PARENT);
					float local[9];
					if (k == 0)
					{
						if (parent < 0) memcpy(local, new_rotation[0], sizeof(local));
						else mat3_transpose_multiply(weapon_chain[parent], new_rotation[0], local);
					}
					else mat3_transpose_multiply(new_rotation[k - 1], new_rotation[k], local);
					int16_t* rotation = (int16_t*)(weapon_pose + POSE_SLOTS + bone * POSE_SLOT_SIZE + SLOT_ROTATION);
					int16_t reference[3] = { rotation[0], rotation[1], rotation[2] };
					quat_to_bone_angles(matrix_to_quat(local), reference, rotation);
				}
				changed = true;
			}
			if (changed) build_bone_matrices(weapon_ctx);
		}

		// v99: pose dump for comparing the game with the anim viewer (trace only). For entities that have
		// a row in GRIP_TABLE (Selphie, Quistis), every drawn frame: animation, the stored frames A -> B
		// being blended (frame index = cmd[6] - 1, see grip_apply), t, and the rotation triples (+ root)
		// the mod hands the engine for every body and weapon bone, i.e. what gets drawn.
		struct DumpState
		{
			uint32_t tick = 0xFFFFFFFF;
			int last_frame = -1;
			uint8_t anim = 0xFF;
			int from = -1, to = -1;
		};
		std::unordered_map<uint8_t*, DumpState> dump_states;
		void pose_dump(uint8_t* entity)
		{
			if (!ff8_interp::cfg().trace) return;
			uint8_t* weapon = *(uint8_t**)(entity + ENTITY_WEAPON);
			if (weapon == nullptr) return;
			const uint8_t* body_pose = pose_of(entity + ENTITY_ANIM_CTX);
			const uint8_t* weapon_pose = pose_of(weapon + WEAPON_ANIM_CTX);
			if (body_pose == nullptr || weapon_pose == nullptr) return;
			bool known = false;
			for (int row = 0; row < GRIP_ROW_COUNT && !known; row++)
				known = GRIP_TABLE[row].weapon_bones == weapon_pose[0] && GRIP_TABLE[row].body_bones == body_pose[0];
			if (!known) return;
			DumpState& dump = dump_states[entity];
			const uint8_t anim = weapon[WEAPON_ANIM_CMD + CMD_ANIM_ID];
			const int raw_frame = weapon[WEAPON_ANIM_CMD + CMD_CURRENT_FRAME];
			const int current_frame = raw_frame > 0 ? raw_frame - 1 : 0;
			if (dump.tick != logic_ticks)
			{
				dump.tick = logic_ticks;
				dump.from = (dump.anim == anim && dump.last_frame >= 0) ? dump.last_frame : current_frame;
				dump.to = current_frame;
				dump.anim = anim;
				dump.last_frame = current_frame;
			}
			char line[4096];
			auto dump_part = [&](const char* tag, const uint8_t* pose) {
				int length = snprintf(line, sizeof(line), "ff8dump f=%u ent=%p part=%s anim=%d total=%d from=%d to=%d t=%.4f root=%d,%d,%d rot=",
					ff8_interp::frame_counter(), entity, tag, anim, weapon[WEAPON_ANIM_CMD + CMD_TOTAL_FRAMES], dump.from, dump.to, tick_alpha,
					((const int16_t*)(pose + POSE_ROOT))[0], ((const int16_t*)(pose + POSE_ROOT))[1], ((const int16_t*)(pose + POSE_ROOT))[2]);
				for (int i = 0; i < pose[0] && length < (int)sizeof(line) - 32; i++)
				{
					const int16_t* rotation = (const int16_t*)(pose + POSE_SLOTS + i * POSE_SLOT_SIZE + SLOT_ROTATION);
					length += snprintf(line + length, sizeof(line) - length, "%d,%d,%d;", rotation[0], rotation[1], rotation[2]);
				}
				ff8_interp::log_trace("%s\n", line);
			};
			dump_part("B", body_pose);
			dump_part("W", weapon_pose);
		}

		// v118: the weapon's attach transform. The entity task rebuilds the weapon block's attach matrix
		// (+0x24) every frame from three rotation angles at +0x14 and three translation shorts at +0x1C
		// (0x502CE2..0x502D04, after the driver), and the weapon is drawn with entity matrix x attach
		// matrix. The sequence writes those six on logic ticks only, so a weapon that leaves the hand -
		// Rinoa's Blaster Edge flying to the target and back - moved in 15 Hz steps while her body was
		// blended. Blended here like the entity position: tick capture, lerp on held frames (angles on the
		// shortest arc), the logic values put back before the next tick's driver; a field that no longer
		// holds our blend was written by someone else and is kept.
		constexpr uint32_t WEAPON_ATTACH_ROT = 0x14;   // 3 x i16 angle (4096 / turn)
		constexpr uint32_t WEAPON_ATTACH_POS = 0x1C;   // 3 x i16
		constexpr int WEAPON_ATTACH_FIELDS = 6;
		constexpr int WEAPON_ATTACH_SNAP_DISTANCE = 2048;             // a jump this far per tick is a reposition: snap
		struct WeaponAttachState
		{
			bool valid = false;
			uint8_t* weapon = nullptr;
			int16_t prev[WEAPON_ATTACH_FIELDS], next[WEAPON_ATTACH_FIELDS], written[WEAPON_ATTACH_FIELDS];
			bool written_valid = false;
		};
		std::unordered_map<uint8_t*, WeaponAttachState> weapon_attach_states; // keyed by BattleEntity*

		inline void read_weapon_attach(const uint8_t* weapon, int16_t* values)
		{
			memcpy(values, weapon + WEAPON_ATTACH_ROT, 6);
			memcpy(values + 3, weapon + WEAPON_ATTACH_POS, 6);
		}
		inline void write_weapon_attach(uint8_t* weapon, const int16_t* values)
		{
			memcpy(weapon + WEAPON_ATTACH_ROT, values, 6);
			memcpy(weapon + WEAPON_ATTACH_POS, values + 3, 6);
		}
		inline bool weapon_attach_blend_off() { return (ff8_interp::cfg().battle_debug2 & DBG2_WEAPON_ATTACH_OFF) != 0; }

		// Before the tick's driver: the logic values back, unless someone else wrote the fields.
		void weapon_attach_before_tick(uint8_t* entity, uint8_t* weapon)
		{
			auto it = weapon_attach_states.find(entity);
			if (it == weapon_attach_states.end() || !it->second.valid || it->second.weapon != weapon || !it->second.written_valid) return;
			int16_t current[WEAPON_ATTACH_FIELDS];
			read_weapon_attach(weapon, current);
			if (memcmp(current, it->second.written, sizeof(current)) == 0)
				write_weapon_attach(weapon, it->second.next);
			it->second.written_valid = false;
		}

		void weapon_attach_after_tick(uint8_t* entity, uint8_t* weapon)
		{
			WeaponAttachState& attach_state = weapon_attach_states[entity];
			int16_t now[WEAPON_ATTACH_FIELDS];
			read_weapon_attach(weapon, now);
			if (!attach_state.valid || attach_state.weapon != weapon) memcpy(attach_state.prev, now, sizeof(now));
			else memcpy(attach_state.prev, attach_state.next, sizeof(now));
			memcpy(attach_state.next, now, sizeof(now));
			attach_state.valid = true;
			attach_state.weapon = weapon;
			bool cut = false;
			for (int k = 3; k < 6; k++)
			{
				const int delta = int(attach_state.next[k]) - int(attach_state.prev[k]);
				if (delta > WEAPON_ATTACH_SNAP_DISTANCE || delta < -WEAPON_ATTACH_SNAP_DISTANCE) cut = true;
			}
			if (cut) memcpy(attach_state.prev, attach_state.next, sizeof(now));
			if (ff8_interp::cfg().trace && memcmp(attach_state.prev, attach_state.next, sizeof(now)) != 0)
				ff8_interp::log_trace("ff8watt f=%u ent=%p rot=(%d,%d,%d) pos=(%d,%d,%d) d=(%d,%d,%d | %d,%d,%d)%s\n", ff8_interp::frame_counter(), entity,
					now[0], now[1], now[2], now[3], now[4], now[5], now[0] - attach_state.prev[0], now[1] - attach_state.prev[1], now[2] - attach_state.prev[2],
					now[3] - attach_state.prev[3], now[4] - attach_state.prev[4], now[5] - attach_state.prev[5], cut ? " SNAP" : "");
		}

		void weapon_attach_blend(uint8_t* entity, uint8_t* weapon, float t)
		{
			auto it = weapon_attach_states.find(entity);
			if (it == weapon_attach_states.end() || !it->second.valid || it->second.weapon != weapon) return;
			WeaponAttachState& attach_state = it->second;
			if (memcmp(attach_state.prev, attach_state.next, sizeof(attach_state.prev)) == 0) // still: nothing to blend, leave the fields alone
				return;
			if (attach_state.written_valid)
			{
				int16_t current[WEAPON_ATTACH_FIELDS];
				read_weapon_attach(weapon, current);
				if (memcmp(current, attach_state.written, sizeof(current)) != 0) // someone else's value: keep it
				{
					attach_state.written_valid = false;
					return;
				}
			}
			int16_t values[WEAPON_ATTACH_FIELDS];
			for (int k = 0; k < 3; k++)
				values[k] = lerp_angle(attach_state.prev[k], attach_state.next[k], t);
			for (int k = 3; k < 6; k++)
				values[k] = lerp_i16(attach_state.prev[k], attach_state.next[k], t);
			write_weapon_attach(weapon, values);
			memcpy(attach_state.written, values, sizeof(values));
			attach_state.written_valid = true;
		}

		void __cdecl seq_driver_hook(uint8_t* entity)
		{
			update_tick();
			EntityState& entity_state = entity_states[entity];
			uint8_t* const attach_weapon = weapon_attach_blend_off() ? nullptr : *(uint8_t**)(entity + ENTITY_WEAPON);
			position_note_external(entity, entity_state); // v102
			if (is_logic_tick)
			{
				// The logic must see the exact last tick's position, not the displayed blend; v102: or the
				// position an effect wrote since (then it is this tick's starting point).
				if (entity_state.valid)
					restore_position(entity, entity_state.external_pending ? entity_state.external : entity_state.next);
				entity_state.external_pending = false;
				if (attach_weapon != nullptr) weapon_attach_before_tick(entity, attach_weapon); // v118
				original_sequence_driver(entity);
				if (attach_weapon != nullptr) weapon_attach_after_tick(entity, attach_weapon);
				int16_t now[POSITION_FIELDS];
				capture_position(entity, now);
				if (!entity_state.valid)
				{
					memcpy(entity_state.prev, now, sizeof(now));
					entity_state.valid = true;
				}
				else
				{
					memcpy(entity_state.prev, entity_state.next, sizeof(entity_state.prev));
				}
				memcpy(entity_state.next, now, sizeof(now));
				int16_t previous_before_cuts[POSITION_FIELDS];
				memcpy(previous_before_cuts, entity_state.prev, sizeof(previous_before_cuts)); // for the trace: the cuts below overwrite prev
				const bool moved = memcmp(entity_state.next, entity_state.prev, sizeof(entity_state.next)) != 0;
				bool snapped = false, handoff = false;
				for (int k = 0; k < POSITION_FIELDS; k++)
				{
					int delta = int(entity_state.next[k]) - int(entity_state.prev[k]);
					if (delta > ENTITY_POS_SNAP || delta < -ENTITY_POS_SNAP)
					{
						memcpy(entity_state.prev, entity_state.next, sizeof(entity_state.prev));
						snapped = true;
						break;
					}
				}
				// v83: a teleport cuts the position, so a pose snapped on the same tick must not keep
				// blending its root from the old place (snap() carries the shown root on a cut).
				if (snapped)
				{
					uint8_t* weapon = *(uint8_t**)(entity + ENTITY_WEAPON);
					if (weapon != nullptr) cut_pose(weapon + WEAPON_ANIM_CTX, weapon + WEAPON_ANIM_CMD);
					cut_pose(entity + ENTITY_ANIM_CTX, entity + ENTITY_ANIM_CMD);
				}
				// v82: root-motion hand-off (see cut_pose): cut position, body and weapon pose together.
				if (!snapped && moved && root_handoff(entity))
				{
					memcpy(entity_state.prev, entity_state.next, sizeof(entity_state.prev));
					handoff = true;
					uint8_t* weapon = *(uint8_t**)(entity + ENTITY_WEAPON);
					if (weapon != nullptr) cut_pose(weapon + WEAPON_ANIM_CTX, weapon + WEAPON_ANIM_CMD);
					cut_pose(entity + ENTITY_ANIM_CTX, entity + ENTITY_ANIM_CMD);
				}
				if (ff8_interp::cfg().trace && (moved || snapped))
					ff8_interp::log_trace("ff8pos f=%u ent=%p ent(x,yo,z,y)=(%d,%d,%d,%d) seq(z,x,y)=(%d,%d,%d) d=(%d,%d,%d) rot=(%d,%d,%d)%s\n", ff8_interp::frame_counter(), entity,
						entity_state.next[0], entity_state.next[1], entity_state.next[2], entity_state.next[3], entity_state.next[4], entity_state.next[5], entity_state.next[6],
						entity_state.next[4] - previous_before_cuts[4], entity_state.next[5] - previous_before_cuts[5], entity_state.next[6] - previous_before_cuts[6],
						*(int16_t*)(entity + 0x0C), *(int16_t*)(entity + 0x0E), *(int16_t*)(entity + 0x10), snapped ? " SNAP" : handoff ? " HANDOFF" : "");
				write_interpolated_position(entity, entity_state, tick_alpha);
				if (attach_weapon != nullptr) weapon_attach_blend(entity, attach_weapon, tick_alpha); // v118: the tick frame shows the blend too
				grip_apply(entity); // v91: after both parts are blended and built
				pose_dump(entity);  // v99
				return;
			}
			// Keep the driver's "current entity" globals coherent for whoever reads them after the call.
			*(uint8_t**)ADDR_CURRENT_ENTITY = entity;
			*(uint8_t**)ADDR_CURRENT_ANIM_CMD = entity + ENTITY_ANIM_CMD;
			*(uint8_t**)ADDR_CURRENT_SEQUENCE = *(uint8_t**)(entity + ENTITY_SEQUENCE);

			uint8_t* weapon = *(uint8_t**)(entity + ENTITY_WEAPON);
			if (weapon != nullptr) interpolate_part(weapon + WEAPON_ANIM_CTX, weapon + WEAPON_ANIM_CMD);
			interpolate_part(entity + ENTITY_ANIM_CTX, entity + ENTITY_ANIM_CMD);
			if (entity_state.valid) write_interpolated_position(entity, entity_state, tick_alpha);
			if (attach_weapon != nullptr) weapon_attach_blend(entity, attach_weapon, tick_alpha); // v118
			grip_apply(entity); // v91
			pose_dump(entity);  // v99
		}

		// ---- v87: entities whose sequence is paused ------------------------------------------------
		// The entity task 0x502AB0 runs the sequence driver only while the sequence is not paused
		// (seq state +0x2C bit 6) and the freeze bit 0x1D96A9C & 0x201 is clear; otherwise it only
		// rebuilds the bone matrices (`push esi / call 0x508F60` at 0x502BD5) and then, like always,
		// builds the world matrix from rotation +0x0C..+0x10, position +0x1C..+0x22, scale
		// +0x94..+0x9A and the sequence position. An effect that throws an entity around while its
		// sequence is paused (Elvoret's entrance blowing Biggs and Wedge off the tower, effect 17)
		// writes those fields on logic ticks only (the effect tree is skipped in between), and nothing
		// blended them: seq_driver_hook never runs for a paused entity. So the drawn entity moved in
		// 15 Hz steps. This hook blends the same fields between the values of the last two logic ticks
		// (one tick behind, like every other entity) right before the world matrix is built, and puts
		// the real values back wherever they are still the blend: at the next call and before the
		// effect tick runs. A field someone else changed in the meantime is taken as the new real value.
		// Only the per-entity pause flags count; the freeze bit belongs to the GF holds.
		// v89: the v88 hook point (the bone-rebuild call 0x508F60 at 0x502BD5) and its test (sequence
		// flag 0x40 only) missed Biggs and Wedge: the task skips the driver when entity flag [e] & 8 is
		// set (always for state [e+4] == 0x8F, which also sets it at 0x502AC6 and skips the bone call),
		// or when status [e+8] & 0x18 together with the sequence flag 0x40. The hook now sits on the
		// rotation-matrix call every placed entity passes (`call 0x56CE30(e+0x0C, e+0x40)` at 0x502BF0,
		// right before the world matrix is built from the fields) and tests exactly that branch.
		constexpr uint32_t CALL_SITE_ENTITY_ROTATION_MATRIX = 0x502BF0;
		constexpr uint32_t FN_ENTITY_ROTATION_MATRIX = 0x56CE30;
		constexpr int PAUSED_FIELDS = 14;  // rot 3, pos 4, scale 4, sequence pos 3
		constexpr int PAUSED_ANGLES = 3;   // the first three are 12-bit angles
		typedef void (__cdecl *entity_rotation_matrix_t)(uint8_t* rotation, uint8_t* matrix);
		entity_rotation_matrix_t original_entity_rotation_matrix = (entity_rotation_matrix_t)FN_ENTITY_ROTATION_MATRIX;

		struct PausedState
		{
			bool valid = false;
			bool blended = false;
			uint32_t tick = 0xFFFFFFFF;
			int16_t prev[PAUSED_FIELDS] = {};
			int16_t next[PAUSED_FIELDS] = {};
			int16_t real[PAUSED_FIELDS] = {};
			int16_t written[PAUSED_FIELDS] = {};
		};
		std::unordered_map<uint8_t*, PausedState> paused_states; // keyed by BattleEntity*

		int16_t* paused_field(uint8_t* entity, int field_index)
		{
			if (field_index < 3) return (int16_t*)(entity + 0x0C + 2 * field_index);
			if (field_index < 7) return (int16_t*)(entity + ENTITY_POS_X + 2 * (field_index - 3));
			if (field_index < 11) return (int16_t*)(entity + 0x94 + 2 * (field_index - 7));
			uint8_t* sequence = *(uint8_t**)(entity + ENTITY_SEQUENCE);
			return sequence ? (int16_t*)(sequence + SEQUENCE_POS_Z + 2 * (field_index - 11)) : nullptr;
		}

		void paused_unblend(uint8_t* entity, PausedState& paused_state)
		{
			if (!paused_state.blended) return;
			for (int k = 0; k < PAUSED_FIELDS; k++)
			{
				int16_t* field = paused_field(entity, k);
				if (field != nullptr && *field == paused_state.written[k]) *field = paused_state.real[k];
			}
			paused_state.blended = false;
		}

		void paused_unblend_all()
		{
			for (auto& entry : paused_states)
				paused_unblend(entry.first, entry.second);
		}

		// The task's own test for skipping the sequence driver (0x502B9D..0x502BAF), without the freeze bit.
		inline bool entity_paused(const uint8_t* entity)
		{
			if (entity[0] & 0x08) return true;
			if (!(entity[0x08] & 0x18)) return false;
			const uint8_t* sequence = *(const uint8_t* const*)(entity + ENTITY_SEQUENCE);
			return sequence != nullptr && (sequence[0x2C] & 0x40) != 0;
		}

		void paused_blend(uint8_t* entity)
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle) return;
			if (ff8_interp::cfg().battle_debug2 & DBG2_PAUSED_BLEND_OFF) return;
			const bool paused = entity_paused(entity);
			auto it = paused_states.find(entity);
			if (!paused)
			{
				if (it != paused_states.end())
				{
					paused_unblend(entity, it->second);
					paused_states.erase(it);
				}
				return;
			}
			update_tick();
			PausedState& paused_state = paused_states[entity];
			paused_unblend(entity, paused_state);
			int16_t current[PAUSED_FIELDS];
			for (int k = 0; k < PAUSED_FIELDS; k++)
			{
				int16_t* field = paused_field(entity, k);
				current[k] = field ? *field : 0;
			}
			if (!paused_state.valid)
			{
				memcpy(paused_state.prev, current, sizeof(current));
				memcpy(paused_state.next, current, sizeof(current));
				paused_state.tick = logic_ticks;
				paused_state.valid = true;
			}
			else if (paused_state.tick != logic_ticks)
			{
				// one sample per logic tick
				memcpy(paused_state.prev, paused_state.next, sizeof(current));
				memcpy(paused_state.next, current, sizeof(current));
				paused_state.tick = logic_ticks;
				bool snapped = false;
				for (int k = PAUSED_ANGLES; k < PAUSED_FIELDS; k++)
				{
					const int delta = int(paused_state.next[k]) - int(paused_state.prev[k]);
					if (delta > ENTITY_POS_SNAP || delta < -ENTITY_POS_SNAP) snapped = true;
				}
				if (snapped) memcpy(paused_state.prev, paused_state.next, sizeof(current));
				if (ff8_interp::cfg().trace && memcmp(paused_state.prev, paused_state.next, sizeof(current)) != 0)
					ff8_interp::log_trace("ff8paused f=%u ent=%p rot=(%d,%d,%d) pos=(%d,%d,%d,%d) scale=(%d,%d,%d,%d) seq=(%d,%d,%d)%s\n", ff8_interp::frame_counter(), entity,
						current[0], current[1], current[2], current[3], current[4], current[5], current[6], current[7], current[8], current[9], current[10], current[11], current[12], current[13], snapped ? " SNAP" : "");
			}
			// A paused entity must not snap back when its sequence resumes: keep the driver's pair on the
			// real place.
			auto entity_entry = entity_states.find(entity);
			if (entity_entry != entity_states.end() && entity_entry->second.valid)
			{
				const int16_t position[POSITION_FIELDS] = { current[3], current[4], current[5], current[6], current[11], current[12], current[13] };
				memcpy(entity_entry->second.prev, position, sizeof(position));
				memcpy(entity_entry->second.next, position, sizeof(position));
			}
			const float t = tick_alpha;
			for (int k = 0; k < PAUSED_FIELDS; k++)
			{
				int16_t* field = paused_field(entity, k);
				if (field == nullptr) continue;
				const int16_t value = (k < PAUSED_ANGLES) ? lerp_angle(paused_state.prev[k], paused_state.next[k], t)
				                                          : lerp_linear(paused_state.prev[k], paused_state.next[k], t);
				paused_state.real[k] = current[k];
				paused_state.written[k] = value;
				if (value != current[k])
				{
					*field = value;
					paused_state.blended = true;
				}
			}
		}

		void __cdecl entity_rotmat_hook(uint8_t* rotation, uint8_t* matrix)
		{
			paused_blend(rotation - 0x0C); // rotation = entity + 0x0C
			original_entity_rotation_matrix(rotation, matrix);
		}

		// ---- magic / GF effect tick and ATB -------------------------------------------------------
		// Every active spell or GF effect is one task tree whose root pointer lives in 0x1D96AAC; the
		// battle frame driver 0x500900 ticks it once per frame through ExecuteTaskQueue (call site
		// 0x50093A, cdecl, returns 0 when the effect is done). The tick also DRAWS: the effect rebuilds
		// its geometry into the battle render list every frame, there is no separate render pass, so
		// skipping it drops the effect for that frame. Instead the tick runs only on logic frames and the
		// primitives it linked are re-issued on the frames in between, so the effect advances at 15 fps
		// while it is still drawn at the host rate. Command type = byte +1 of the action data.
		constexpr uint32_t CALL_SITE_EFFECT_TICK = 0x50093A;
		constexpr uint32_t FN_EXEC_TASK_QUEUE = 0x508420;
		uint8_t** const BATTLE_ACTION_DATA = (uint8_t**)0x1D99A50;
		// Action data (*0x1D99A50): +1 command type, +2 anim state, +4 command arg, +6 effect id.
		// Command types: 0x00 physical, 0x02 magic cast, 0x06 draw (casting from the Draw menu also
		// runs the magic path), 0x26/0xF4/0xFE GF cinematic, 0xEC/0xF5 Gilgamesh.
		constexpr uint32_t ACT_COMMAND_TYPE = 1;
		constexpr uint32_t ACT_ANIM_STATE = 2;
		constexpr uint32_t ACT_EFFECT_ID = 6;
		inline bool command_is_gf(uint8_t command)
		{
			return command == 0x26 || command == 0xF4 || command == 0xFE || command == 0xEC || command == 0xF5;
		}
		// A spell effect is playing when the action carries an effect id and is not a physical hit or a
		// summon cinematic. The command type alone is not enough: a spell cast straight from the Draw
		// menu keeps command type 0x06 and still plays its effect through this tick, and stocking with Draw
		// rides the same path - skipping and replaying rolls nothing back, so neither needs an exclusion.
		inline bool holdable_action(uint8_t* action)
		{
			if (action == nullptr) return false;
			uint8_t command = action[ACT_COMMAND_TYPE];
			uint16_t effect = *(uint16_t*)(action + ACT_EFFECT_ID);
			// Physical attacks were excluded here on the assumption that they carry no effect. They do:
			// impact flashes and the gunblade trigger explosion ride a physical action, and stayed at
			// host rate because of this rule. Only the absence of an effect id disqualifies an action.
			if (effect == 0) return false;
			// v85: an animation-VM tree with a gf_vm row (Elvoret's death) is held there, drawn live.
			if (gf_vm_paces_effect(effect)) return false;
			// Summon cinematics hang when their tick is skipped (Quezacotl freezes as it spawns), so they
			// stay at host rate until the cinematic path gets its own treatment via its paused flag.
			// v75: Siren (effect 95) is an ordinary effect-framework tree and is safe to skip whole (gf_siren.cpp).
			if (command_is_gf(command)) return gf_siren_holdable(action);
			return true;
		}
		// ExecuteTaskQueue(queue) walks a linked list: queue[0] = head node, queue[1] = tail; each node
		// has its next pointer at +4 and its tick function at +8, and its context follows inline.
		// ATB gauges + GF countdown: pure logic, one call site in the HUD phase 0x4A84E0.
		constexpr uint32_t CALL_SITE_ATB_TICK = 0x4A87D6;
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
		magic_tick_t original_magic_effect_tick = nullptr;
		magic_tick_t original_magic_cast_tick = nullptr;
		void* registered_root = nullptr;
		uint32_t recorded_tick_count = 0;

		typedef int (__cdecl *exec_task_queue_t)(void*);
		typedef void (__cdecl *atb_tick_t)();

		exec_task_queue_t original_exec_task_queue = (exec_task_queue_t)FN_EXEC_TASK_QUEUE;
		atb_tick_t original_atb_tick = (atb_tick_t)FN_ATB_TICK;
		void* holdable_ctx = nullptr;   // the tree a real tick has already run, under a magic cast
		void* last_effect_ctx = nullptr;
		void* held_effect_ctx = nullptr;   // the tree currently being skipped, and for how many frames
		uint32_t held_frames = 0;

		bool range_writable(void* address, size_t size)
		{
			MEMORY_BASIC_INFORMATION memory_info;
			uint8_t* cursor = (uint8_t*)address;
			uint8_t* end = cursor + size;
			while (cursor < end)
			{
				if (VirtualQuery(cursor, &memory_info, sizeof(memory_info)) == 0) return false;
				if (memory_info.State != MEM_COMMIT) return false;
				if (memory_info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
				if ((memory_info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) return false;
				cursor = (uint8_t*)memory_info.BaseAddress + memory_info.RegionSize;
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
		// FX_LINKER_COUNT (= 5) now lives in internal.h, together with FX_PRIMITIVE_WORDS and FxCall, because
		// fx_blend.cpp has to see the same shape. Every linker in the array is plain cdecl and proven
		// in game.
		// 0x45CEE0 and 0x45D080 are deliberately NOT in it: both open with a register push, so their calling
		// convention is not certain, and hooking 0x45D080 crashed inside it on battle entry (opt-in removed
		// in v59). That only matters if some effect draws exclusively through them.
		const uint32_t FX_LINKER_ADDRESSES[FX_LINKER_COUNT] = { 0x45C7A0, 0x45C860, 0x45C870, 0x45C8E0, 0x45D310 };
		const uint8_t FX_LINKER_PROLOGUE[FX_LINKER_COUNT][7] = {
			{ 0xA1, 0x28, 0x88, 0xCA, 0x01, 0x00, 0x00 },
			{ 0x8B, 0x0D, 0x28, 0x88, 0xCA, 0x01, 0x00 },
			{ 0xA1, 0x28, 0x88, 0xCA, 0x01, 0x00, 0x00 },
			{ 0xA1, 0x28, 0x88, 0xCA, 0x01, 0x00, 0x00 },
			{ 0x83, 0xEC, 0x08, 0x53, 0x55, 0x56, 0x00 },
		};
		const size_t FX_LINKER_PROLOGUE_LENGTH[FX_LINKER_COUNT] = { 5, 6, 5, 5, 6 };

		// Ghidra types these void, but 0x45C7A0 / 0x45C870 / 0x45C8E0 all leave the bump cursor in EAX
		// (`mov eax,[0x1CA8828] … add eax,0x18 … mov [0x1CA8828],eax; ret`, and the bounds-check exit
		// returns the unchanged cursor). A review of all 2852 call sites found no caller that reads it,
		// but the deferral is the one place where our code can return WITHOUT the engine's linker having
		// run, so the value is carried through rather than left to chance: the wrappers return uint32_t,
		// pass the trampoline's EAX straight out, and the deferred path returns the cursor - exactly what
		// the engine leaves when it links nothing.
		uint32_t* const OT_ARENA_CURSOR = (uint32_t*)0x1CA8828;
		typedef uint32_t (__cdecl *fx_linker_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
		fx_linker_t original_fx_linker[FX_LINKER_COUNT] = {};

		// A recorded link call, plus a copy of the primitive packet as it looked on that logic frame.
		// The packet's first word is the PSX tag: high byte = length in words, low 24 bits = the next
		// pointer the linker patches. Keeping the copy lets a skipped frame draw a blend of the last
		// two logic frames instead of repeating the newest one, which is what makes particle motion
		// smooth rather than stepped. `FxCall` and `FX_PRIMITIVE_WORDS` are declared in internal.h so that
		// fx_blend.cpp can read both recorded lists in place instead of being handed a copy of each.
		std::vector<FxCall> fx_prims, fx_prims_previous, fx_prims_two_ticks_ago; // v109: + the tick before prev
		bool fx_replaying = false;
		constexpr size_t FX_MAX_PRIMITIVES = 16384;

		// Per-frame memory (v78). A recorded link call is replayed on LATER host frames, so the address it
		// hands the linker must still hold the same packet then. Two engine allocators hand out memory that
		// is rebuilt every host frame, and a primitive from either must never be linked in place on a
		// replay (the walker 0x45D080 reads the chain pointer out of the PRIMITIVE's word 0, so a reused
		// address cross-links two buckets or loops the walk, and garbage there faults at 0x45D0FB):
		//  1. the LIFO stack 0x5082B0 (cursor 0x1D999C4, rewind 0x5082D0). Its base is the CONSTANT
		//     0x1A77238: 0x5082F0 sets the cursor to 0x401000(0) = `lea eax,[0*4+0x1A77238]` (verified at
		//     install). Pre-v78 calibrated the base as the lowest cursor ever seen, which sits above the
		//     true base whenever every observed link happened with a block allocated (audit, row 7). The
		//     range is [base, highest cursor seen): memory above the live cursor is freed stack, not safe.
		//  2. the global packet area: cursor 0x1D8E054, rebased EVERY host frame by 0x5007A9..0x5007F1 to
		//     *0xB6D070 + idx*0x1C000 (limit +0x1C000 in 0x1D969A8), or *0xB6D094 + idx*0x24000 when
		//     0x1D96A9C & 0x80; idx = *0x1D96A80 & 0xFF, which nothing in FF8_EN.exe writes (55 reads,
		//     no store), so on PC the area restarts at the same address every frame. Entity models and
		//     shadows (0x502D40 -> 0x5088A0 / 0x5099D0), the HUD and 194 of the 343 MagicList_Logic
		//     modules write packets there - Sleep (eff 145, 0x6185D0) draws its target's model and
		//     shadow through it (0x619770 -> 0x5088A0, 0x619880 -> 0x619B70), and the replay of those
		//     addresses on the next frame was the v77 hard crash.
		// Such a call is still recorded when the packet could be copied: the replay then links a stable
		// copy owned by this module (fx_stable_copy) instead of the engine's address. Without a usable
		// copy (or with the ordering-table bucket itself in per-frame memory) it is not recorded, and the
		// tree falls back to running every host frame (fx_unpaced_ctx) - unless DBG_FX_UNPACED_OFF.
		uint32_t* const PRIM_STACK_CURSOR = (uint32_t*)0x1D999C4;
		constexpr uint32_t PRIM_STACK_BASE = 0x1A77238;
		uint32_t prim_stack_base = 0;           // PRIM_STACK_BASE once verified at install, else calibrated
		uint32_t prim_stack_low = 0xFFFFFFFF;   // calibration fallback only (bytes at 0x401000 differ)
		uint32_t prim_stack_high = 0;           // highest cursor seen
		bool packet_area_verified = false;         // 0x5007B7..0x5007F7 bytes match, the packet-area rule is on
		uint32_t* const PACKET_BASE_A = (uint32_t*)0xB6D070;  // stride 0x1C000
		uint32_t* const PACKET_BASE_B = (uint32_t*)0xB6D094;  // stride 0x24000 (0x1D96A9C & 0x80)
		uint8_t* const PACKET_BUFFER_INDEX = (uint8_t*)0x1D96A80;  // low byte of the dword 0x5007B0 masks with 0xFF
		uint8_t* const PACKET_MODE_FLAGS = (uint8_t*)0x1D96A9C; // bit 0x80 selects base B (0x5007A9)

		inline bool in_primitive_stack(uint32_t address)
		{
			const uint32_t cursor = *PRIM_STACK_CURSOR;
			if (cursor == 0) return false;
			if (cursor > prim_stack_high) prim_stack_high = cursor;
			uint32_t base = prim_stack_base;
			if (base == 0)
			{
				if (cursor < prim_stack_low) prim_stack_low = cursor;
				base = prim_stack_low;
			}
			return address >= base && address < prim_stack_high;
		}
		inline bool in_packet_area(uint32_t address)
		{
			if (!packet_area_verified) return false;
			// Exactly the half 0x5007A9..0x5007F7 selects for this frame. The static images of the two base
			// pointers are 0x1901018 and 0x199B018, and the next region pointers (*0xB6D074 = 0x1937018,
			// *0xB6D098 = 0x19AB018) sit closer than two strides, so no wider range is assumed.
			const bool use_base_b = (*PACKET_MODE_FLAGS & 0x80) != 0;
			const uint32_t base = use_base_b ? *PACKET_BASE_B : *PACKET_BASE_A;
			const uint32_t stride = use_base_b ? 0x24000 : 0x1C000;
			if (base == 0) return false;
			const uint32_t start = base + uint32_t(*PACKET_BUFFER_INDEX) * stride;
			return address >= start && address - start < stride;
		}
		// Memory that is rebuilt within a host frame or on the next one.
		inline bool prim_is_volatile(uint32_t address) { return in_primitive_stack(address) || in_packet_area(address); }

		// Stable copies for the replay of per-frame packets. Blocks are never freed or moved (the ordering
		// table may point into any of them until the walk at the end of the frame); the bump cursor is
		// reset once per host frame, which is after that frame's walk.
		constexpr size_t FX_COPY_BLOCK_WORDS = 0x10000; // 256 KB per block
		constexpr size_t FX_COPY_MAX_BLOCKS = 32;       // 8 MB ceiling; a full list needs < 2 MB
		std::vector<uint32_t*> fx_copy_blocks;
		size_t fx_copy_block_index = 0;
		size_t fx_copy_words_used = 0;
		uint32_t fx_copy_frame = 0xFFFFFFFF;
		uint32_t* fx_stable_copy(const uint32_t* source, int words)
		{
			if (words <= 0 || size_t(words) > FX_PRIMITIVE_WORDS) return nullptr;
			const uint32_t frame = ff8_interp::frame_counter();
			if (frame != fx_copy_frame)
			{
				fx_copy_frame = frame;
				fx_copy_block_index = 0;
				fx_copy_words_used = 0;
			}
			if (fx_copy_block_index < fx_copy_blocks.size() && fx_copy_words_used + size_t(words) > FX_COPY_BLOCK_WORDS)
			{
				fx_copy_block_index++;
				fx_copy_words_used = 0;
			}
			if (fx_copy_block_index >= fx_copy_blocks.size())
			{
				if (fx_copy_blocks.size() >= FX_COPY_MAX_BLOCKS) return nullptr;
				uint32_t* block = new (std::nothrow) uint32_t[FX_COPY_BLOCK_WORDS];
				if (block == nullptr) return nullptr;
				fx_copy_blocks.push_back(block);
				fx_copy_block_index = fx_copy_blocks.size() - 1;
				fx_copy_words_used = 0;
			}
			uint32_t* destination = fx_copy_blocks[fx_copy_block_index] + fx_copy_words_used;
			fx_copy_words_used += size_t(words);
			memcpy(destination, source, size_t(words) * 4);
			return destination;
		}

		// v80: the GTE SZ FIFO (SZ0..SZ3). 0x45C7A0 copies it into the link node (quads SZ0..3, polygons
		// SZ1..3, anything else zeros) and the renderer takes each vertex's depth from there. Replayed fn 0
		// calls used to link whatever the GTE last computed - one stale depth for every replayed polygon,
		// so an entity model redrawn by a spell (Sleep 0x619B70) lost triangles to the depth test.
		uint32_t* const GTE_SZ_FIFO = (uint32_t*)0x1CA8A50;

		// v78 bookkeeping for the tree being recorded / replayed.
		uint32_t fx_unsafe_calls = 0;     // this tick: calls from per-frame memory that could not be recorded
		uint32_t fx_unsafe_site = 0;      // first such call's game call site
		uint32_t fx_copied = 0, fx_stale = 0, fx_dropped_volatile = 0; // trace counters
		void* fx_unpaced_ctx = nullptr;   // tree that runs every host frame because it cannot be replayed
		void* fx_stale_logged_ctx = nullptr;
		void* fx_unsafe_logged_ctx = nullptr;
		bool fx_prev_recorded = false;    // the previous link call of the tick being recorded was recorded

		std::vector<FxCall>* fx_record_target = nullptr;

		// Set while a logic frame's effect tick is being recorded AND its link calls are being held back,
		// so that the list which reaches the ordering table is the interpolated one issued after the tick
		// (design rule 2.1: blend on the logic frame too, or the effect jumps a tick ahead and snaps back).
		// It is cleared the moment a call arrives that cannot be recorded: such a call goes straight to
		// the engine, so everything held back has to be issued first or it would end up behind it in the
		// depth bucket.
		bool fx_defer_links = false;
		uint32_t fx_flushes = 0; // times a non-recordable call forced the held-back list out mid-tick
		void fx_replay_list(const std::vector<FxCall>& list, const std::vector<FxCall>* previous, int slot, bool allow_blend);

		// `call_site` is the game's own call site (captured in the wrappers below, before anything can clobber
		// it). It is recorded per call purely as a diagnostic: it names which emitter produced each
		// packet, which is the only way to tell from a log whether a given piece of an effect is drawn
		// through the effect tree at all.
		// v105: effect model draws re-run live on held frames. X-ATM092's defeat explosion (effect 276,
		// module 0x684EE0) draws the whole machine itself: task code calls 0x685750(entity, part mask) at
		// 0x685592 (entity = *0x2508278, mask in the task node), which takes the entity's world matrix
		// (+0x40) and pose, draws the model (renderer 0x685860, only the masked parts) and its shadow, and
		// resets its own per-bone flags - a pure draw of the entity's current state. Replaying its ~900
		// recorded triangles (face-identity blending is ambiguous on a machine of identical panels) showed
		// the whole machine at 15 Hz as soon as the explosions began. On a tick the call now runs normally
		// but its packets are not recorded; on the held frames the call is repeated live after the replay,
		// so the machine is drawn from the pose and position this module blends for that frame.
		constexpr uint32_t CALL_SITE_XATM_MODEL_DRAW = 0x685592;
		constexpr uint32_t FN_XATM_MODEL_DRAW = 0x685750;
		typedef void (__cdecl *model_draw_t)(uint8_t* entity, uint8_t* mask);
		model_draw_t original_xatm_model_draw = (model_draw_t)FN_XATM_MODEL_DRAW;
		// v114: a second kind - Granaldo's Raldo kick (effect 19, module 0x8A32A0) moves the kicked Raldo
		// by writing its position and world-matrix translation (0x8A9453..0x8A94A9), then draws it with
		// 0x8A94E0(ctx 0x2726910) at 0x8A94AC: model, weapon and shadow of BattleEntitySlotData[ctx+0xAC]
		// from the entity's matrix and pose into the per-frame packet area (566 packets per tick, replayed
		// at 15 Hz). Live on held frames, it draws from the matrix the entity task built from the
		// blended position (v102).
		constexpr uint32_t CALL_SITE_RALDO_DRAW = 0x8A94AC;
		constexpr uint32_t FN_RALDO_DRAW = 0x8A94E0;
		typedef void (__cdecl *ctx_draw_t)(uint8_t* ctx);
		ctx_draw_t original_raldo_draw = (ctx_draw_t)FN_RALDO_DRAW;
		struct LiveDraw
		{
			model_draw_t draw_model;
			uint8_t* entity;
			uint8_t* mask;
			ctx_draw_t draw_context;
		};
		std::vector<LiveDraw> fx_live_draws, fx_live_draws_recording;
		void* fx_live_ctx = nullptr;
		bool fx_live_bypass = false; // the linker calls of a live draw are not recorded
		bool fx_live_recording = false;

		void __cdecl xatm_model_draw_hook(uint8_t* entity, uint8_t* mask)
		{
			if (!fx_live_recording || (ff8_interp::cfg().battle_debug2 & DBG2_FX_LIVE_DRAW_OFF))
			{
				original_xatm_model_draw(entity, mask);
				return;
			}
			fx_live_draws_recording.push_back({ original_xatm_model_draw, entity, mask, nullptr });
			fx_live_bypass = true;
			original_xatm_model_draw(entity, mask);
			fx_live_bypass = false;
		}

		void __cdecl raldo_draw_hook(uint8_t* ctx)
		{
			if (!fx_live_recording || (ff8_interp::cfg().battle_debug2 & DBG2_FX_LIVE_DRAW_OFF))
			{
				original_raldo_draw(ctx);
				return;
			}
			fx_live_draws_recording.push_back({ nullptr, nullptr, nullptr, original_raldo_draw });
			fx_live_draws_recording.back().entity = ctx;
			fx_live_bypass = true;
			original_raldo_draw(ctx);
			fx_live_bypass = false;
		}

		inline uint32_t fx_linker_common(int linker, uint32_t call_site, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
		{
			if (fx_live_bypass)
			{
				// v105: drawn live on every frame, never replayed; a following 0x45C860 must not patch
				// a replayed node with its flags either.
				fx_prev_recorded = false;
				return original_fx_linker[linker](a0, a1, a2, a3, a4, a5, a6, a7);
			}
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
			if (linker != 4 && fx_record_target != nullptr && !fx_replaying && fx_record_target->size() < FX_MAX_PRIMITIVES)
			{
				FxCall call = { linker, { a0, a1, a2, a3, a4, a5, a6, a7 }, (uint32_t*)a1, 0, {}, 0, false, {} };
				if (linker == 0) memcpy(call.sz_fifo, (const void*)GTE_SZ_FIFO, sizeof(call.sz_fifo)); // v80: the depth fn 0 links
				// v78: which calls are safe to replay. 0x45C860 (fn 1) is `void f(u16)`: it stores its one
				// argument into the flags word of the node the PREVIOUS call created (`[cursor-4]`) and has
				// no primitive at all - a1 is whatever the caller left on its stack, and pre-v78 fed it to
				// the transient test, VirtualQuery and a packet copy. It is recorded as it is, and the replay
				// re-issues it only right after its predecessor actually linked.
				bool safe = true;
				bool skip_quietly = false;
				if (linker == 1)
				{
					call.primitive = nullptr;
					// Its node belongs to a call that was not recorded: replaying it would patch whichever
					// node the replay linked last. It went to the engine in place (a non-recorded call ends
					// any deferral), which is all it is for.
					if (!fx_prev_recorded) skip_quietly = true;
				}
				else
				{
					const bool primitive_volatile = prim_is_volatile(a1);
					const bool bucket_volatile = prim_is_volatile(a0); // the bucket the node is chained into
					bool readable = false;
					// The primitive is the second argument for every other linker in this family.
					if (call.primitive != nullptr && range_writable(call.primitive, 4))
					{
						readable = true;
						// v78: a zero-length packet (a pure chain marker) is copied too, as its tag alone; fx_blend
						// still sees it as `words < 2`, i.e. no copy, exactly as before.
						int words = int(call.primitive[0] >> 24);
						if (words < FX_PRIMITIVE_WORDS && range_writable(call.primitive, (words + 1) * 4))
						{
							call.words = words + 1;
							memcpy(call.copy, call.primitive, call.words * 4);
						}
					}
					call.is_volatile = primitive_volatile;
					// Per-frame packet without a complete copy, a bucket in per-frame memory, or a primitive
					// that cannot even be read: nothing about it may be issued on a later frame.
					if (!readable || bucket_volatile || (primitive_volatile && call.words < 1)) safe = false;
				}
				if (skip_quietly) {}
				else if (safe)
				{
					// Diagnostic only; not captured when the blending is off, so a 512 run records exactly the
					// FxCall v59 recorded. (The intrinsic in the wrapper is a plain read of the return-address
					// slot and has no side effect of its own.)
					call.call_site = blend_on ? call_site : 0;
					fx_record_target->push_back(call);
					recorded = true;
				}
				else
				{
					if (fx_unsafe_calls++ == 0) fx_unsafe_site = call_site;
				}
			}
			if (fx_record_target != nullptr && !fx_replaying) fx_prev_recorded = recorded;
			if (fx_defer_links)
			{
				// Issued after the tick, from the blended replay in effect_tick_hook. EAX must still look
				// like a linker return, and "linked nothing" is what the engine's own bounds-check exit
				// leaves: the unchanged cursor.
				if (recorded) return *OT_ARENA_CURSOR;
				// Not recordable (the ordering-table renderer, a per-frame packet without a copy, an overlong
				// list, a 0x45C860 whose node was not recorded): it has
				// to keep its place, so flush what is held back - verbatim, the list is still incomplete -
				// and let the rest of this tick through unchanged.
				fx_defer_links = false;
				fx_flushes++;
				if (fx_record_target != nullptr) fx_replay_list(*fx_record_target, nullptr, 0, false);
			}
			return original_fx_linker[linker](a0, a1, a2, a3, a4, a5, a6, a7);
		}
		uint32_t __cdecl fx_linker0(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
		{
			return fx_linker_common(0, (uint32_t)(uintptr_t)_ReturnAddress(), a0, a1, a2, a3, a4, a5, a6, a7);
		}
		uint32_t __cdecl fx_linker1(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
		{
			return fx_linker_common(1, (uint32_t)(uintptr_t)_ReturnAddress(), a0, a1, a2, a3, a4, a5, a6, a7);
		}
		uint32_t __cdecl fx_linker2(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
		{
			return fx_linker_common(2, (uint32_t)(uintptr_t)_ReturnAddress(), a0, a1, a2, a3, a4, a5, a6, a7);
		}
		uint32_t __cdecl fx_linker3(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
		{
			return fx_linker_common(3, (uint32_t)(uintptr_t)_ReturnAddress(), a0, a1, a2, a3, a4, a5, a6, a7);
		}
		uint32_t __cdecl fx_linker4(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
		{
			return fx_linker_common(4, (uint32_t)(uintptr_t)_ReturnAddress(), a0, a1, a2, a3, a4, a5, a6, a7);
		}
		void* const FX_LINKER_HOOK[FX_LINKER_COUNT] = { (void*)fx_linker0, (void*)fx_linker1, (void*)fx_linker2, (void*)fx_linker3, (void*)fx_linker4 };

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
			uint32_t bytes_left = OT_ARENA_END - cursor;
			return bytes_left > OT_ARENA_MARGIN + nodes * OT_NODE_BYTES;
		}

		// Issue a recorded list into the ordering table. With `allow_blend` (and a previous list to pair
		// it against) fx_blend.cpp interpolates the vertex dwords of every call it can pair and this links
		// its scratch copies instead of the effect's own packets - v60, the rebuilt version of the path
		// that was removed in v25. fx_blend_prepare() runs before the first link call goes out, which is
		// the only moment its scratch may grow, and nothing the ordering table points at is ever resized.
		// When the two lists do not pair (different length, a different linker or primitive type at some
		// index, a packet that did not move with the rest) it returns false and this stays the verbatim
		// replay it has always been.
		void fx_replay_list(const std::vector<FxCall>& list, const std::vector<FxCall>* previous, int slot, bool allow_blend)
		{
			// No bulk pre-check (v68). The per-call ot_arena_room(1) below already stops short with the
			// full margin, and refusing the whole list up front threw away an entire deferred logic
			// frame's geometry - something the engine itself never does, since its linkers bounds-check
			// per call and drop only the overflow.
			if (list.empty()) return;
			const bool blend = allow_blend && previous != nullptr
				&& (fx_blend_set_two_ticks_ago(previous == &fx_prims_previous ? fx_prims_two_ticks_ago.data() : nullptr,
				                       previous == &fx_prims_previous ? fx_prims_two_ticks_ago.size() : 0), true)
				&& fx_blend_prepare(slot, previous->data(), previous->size(), list.data(), list.size(), tick_alpha);
			fx_replaying = true;
			bool previous_linked = false; // 0x45C860 may only follow a call that really linked a node
			uint32_t saved_depths[4];     // v80: the engine's own GTE depth state, restored after the replay
			memcpy(saved_depths, (const void*)GTE_SZ_FIFO, sizeof(saved_depths));
			for (size_t i = 0; i < list.size(); i++)
			{
				const FxCall& call = list[i];
				if (original_fx_linker[call.linker] == nullptr)
				{
					previous_linked = false;
					continue;
				}
				if (!ot_arena_room(1)) break; // the arena filled up mid-replay (per call, v68; no bulk check)
				if (call.linker == 1)
				{
					// No primitive: a u16 written into the node its predecessor created.
					if (previous_linked)
						original_fx_linker[1](call.args[0], call.args[1], call.args[2], call.args[3], call.args[4], call.args[5], call.args[6], call.args[7]);
					previous_linked = false;
					continue;
				}
				previous_linked = false;
				// v78: never hand the linker memory that does not survive to this frame's walk.
				if (prim_is_volatile(call.args[0])) // bucket in per-frame memory
				{
					fx_dropped_volatile++;
					continue;
				}
				uint32_t primitive_arg = 0;
				if (blend)
				{
					// 0 = no interpolated copy for this call (the command byte is not one of the blendable
					// types, or the packet is shorter than its own field mask). A scratch copy is our memory.
					primitive_arg = fx_blend_arg(slot, i);
				}
				if (primitive_arg == 0)
				{
					if (call.is_volatile || prim_is_volatile(call.args[1]))
					{
						// Per-frame engine packet: the address now holds whatever was drawn there this frame.
						// Link a stable copy of the packet as it was recorded instead, never the address.
						const uint32_t* copy = call.words >= 1 ? fx_stable_copy(call.copy, call.words) : nullptr;
						if (copy == nullptr)
						{
							fx_dropped_volatile++;
							continue;
						}
						primitive_arg = (uint32_t)(uintptr_t)copy;
						fx_copied++;
					}
					else
					{
						// Effect-owned packet, linked in place as since v45. The tree is held, so nothing may have
						// rewritten it: a changed length byte or command type means the memory was reused (the
						// v77 crash class) - skip it rather than let the walker read someone else's chain.
						if (call.words >= 1)
						{
							const uint32_t* live_packet = call.primitive;
							if ((live_packet[0] >> 24) != (call.copy[0] >> 24)
								|| (call.words >= 2 && ((live_packet[1] >> 24) & 0xFC) != ((call.copy[1] >> 24) & 0xFC)))
							{
								fx_stale++;
								continue;
							}
						}
						primitive_arg = call.args[1];
					}
				}
				if (call.linker == 0) memcpy((void*)GTE_SZ_FIFO, call.sz_fifo, sizeof(call.sz_fifo)); // v80: the recorded vertex depths
				original_fx_linker[call.linker](call.args[0], primitive_arg, call.args[2], call.args[3], call.args[4], call.args[5], call.args[6], call.args[7]);
				previous_linked = true;
			}
			memcpy((void*)GTE_SZ_FIFO, saved_depths, sizeof(saved_depths));
			fx_replaying = false;
		}
		void fx_replay() { fx_replay_list(fx_prims, &fx_prims_previous, 0, true); }

		// Damage and heal popups animate over ten frames and draw inside their own tick, like the
		// effects. The task node is small and self-contained, so a bounded snapshot of it is safe here
		// (unlike the effect trees): the popup draws every frame and steps at the logic rate.
		constexpr uint32_t ADDR_DAMAGE_TICK = 0x5069B0;
		const uint8_t DAMAGE_TICK_PROLOGUE[6] = { 0x53, 0x55, 0x56, 0x57, 0x6A, 0x18 }; // push ebx/ebp/esi/edi, push 18h
		constexpr size_t DAMAGE_NODE_SIZE = 0x20; // highest field the tick touches is +0x1C
		typedef int (__cdecl *damage_tick_t)(uint8_t*);
		damage_tick_t original_damage_tick = nullptr;

		int __cdecl damage_tick_hook(uint8_t* node)
		{
			if (node == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
				return original_damage_tick(node);
			update_tick();
			if (is_logic_tick) return original_damage_tick(node);
			uint8_t saved[DAMAGE_NODE_SIZE];
			memcpy(saved, node, DAMAGE_NODE_SIZE);
			int result = original_damage_tick(node); // draws this frame
			memcpy(node, saved, DAMAGE_NODE_SIZE);
			return result & ~2; // the restore undid the completion the held tick reported
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
		const uint32_t CALL_SITE_UI_CLOCK[4] = { 0x47D09D, 0x47D0A7, 0x47D0B1, 0x47D141 };
		typedef void (__cdecl *ui_clock_t)();
		ui_clock_t original_ui_clock = (ui_clock_t)FN_UI_CLOCK;

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
				original_ui_clock();
				return;
			}
			update_tick();
			uint8_t* ui_ctx_before = *(uint8_t**)0x1D6D490;
			uint8_t phase_before = ((uintptr_t)ui_ctx_before > 0x10000) ? ui_ctx_before[0x2C] : 0;
			original_ui_clock(); // always: it refreshes the context pointer and runs the input latch
			uint8_t* ui_ctx = *(uint8_t**)0x1D6D490;
			if ((uintptr_t)ui_ctx <= 0x10000) return;
			if (!blink_valid)
			{
				blink_phase = ui_ctx[0x2C];
				blink_valid = true;
				blink_calls = blink_last_step = 0;
				return;
			}
			if (ui_ctx == ui_ctx_before && ui_ctx[0x2C] != phase_before) blink_pending = true; // the engine stepped it this pass
			if (is_logic_tick) blink_calls++;
			uint32_t window = *(uint32_t*)0x1D6D620;
			if (window < 1) window = 1;
			if (blink_calls - blink_last_step >= window)
			{
				blink_phase++;
				blink_pending = false;
				blink_last_step = blink_calls;
			}
			ui_ctx[0x2C] = blink_phase;
		}

		// Results screens (EXP / AP / items). They live in their own module whose logic driver 0x4A3EE0 is
		// called once per rendered frame from 0x4A3E5E; nothing in the battle module reaches it. The module
		// is written for 60 logic ticks per second, so this drives it from a wall-clock accumulator instead
		// of the host frame rate. OFF by default (DBG_RESULTS_GATE_ON opts in): the module is not actually running
		// at host rate - its main loop 0x4A2690 busy-waits on its own 60 fps timer (0x1D2BB80), which the
		// FFNx limiter never replaces - so this gate only matters once that busy-wait is neutralised.
		constexpr uint32_t CALL_SITE_VICTORY_LOGIC = 0x4A3E5E;
		constexpr uint32_t FN_VICTORY_LOGIC = 0x4A3EE0;
		constexpr double VICTORY_PERIOD = 1.0 / 60.0;
		typedef void (__cdecl *victory_logic_t)();
		victory_logic_t original_victory_logic = (victory_logic_t)FN_VICTORY_LOGIC;
		double victory_accumulator = 0.0;

		void __cdecl victory_logic_hook()
		{
			if (!(ff8_interp::cfg().battle_debug & DBG_RESULTS_GATE_ON))
			{
				original_victory_logic();
				return;
			}
			victory_accumulator += ff8_tick::frame_delta();
			if (victory_accumulator < VICTORY_PERIOD) return;
			victory_accumulator -= VICTORY_PERIOD;
			if (victory_accumulator > VICTORY_PERIOD) victory_accumulator = VICTORY_PERIOD; // never burst after a stall
			original_victory_logic();
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
		battle_state_t original_battle_state = nullptr;
		fade_trigger_t original_fade_trigger = nullptr;
		fade_tick_t original_fade_tick = nullptr;
		double end_hold_seconds = 0.0;

		void __cdecl battle_state_hook()
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
			{
				original_battle_state();
				return;
			}
			update_tick();
			int8_t countdown_before = *BATTLE_END_COUNTDOWN;
			original_battle_state();
			int8_t countdown_after = *BATTLE_END_COUNTDOWN;
			if (countdown_before <= 0) // idle: nothing to pace
			{
				end_hold_seconds = 0.0;
				return;
			}
			if (countdown_after != (int8_t)(countdown_before - 1)) return;
			if (is_logic_tick)
			{
				// v76 (audit F5): a tick that really stepped the countdown proves it is not stuck, so the
				// watchdog restarts. Before, it summed ALL held time: a 60-tick victory / game-over fade
				// (4 s, set by 0x47DFC0) tripped it after ~2.8 s and its last ~18 ticks ran at host rate.
				if (!(ff8_interp::cfg().battle_debug & DBG_END_WATCHDOG_OFF)) end_hold_seconds = 0.0;
				return;
			}
			end_hold_seconds += ff8_tick::frame_delta();
			if (end_hold_seconds > END_HOLD_MAX_SECONDS) return; // never block the exit
			*BATTLE_END_COUNTDOWN = countdown_before;
		}

		void __cdecl fade_trigger_hook(int slot)
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
			{
				original_fade_trigger(slot);
				return;
			}
			update_tick();
			if (is_logic_tick) original_fade_trigger(slot); // pure logic, nothing drawn
		}

		int __cdecl fade_tick_hook(uint8_t* node)
		{
			if (node == nullptr || original_fade_tick == nullptr) return 0;
			// node[0x11] == 0 selects the end-of-battle fade; the other flavour (0x501CE0: from 0 to 0x80
			// over +0x0E steps) is the intro fade started by task 0x3EB (0x506DE0) and by task 3.
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
				return original_fade_tick(node);
			const bool intro = node[0x11] != 0;
			if (intro && (ff8_interp::cfg().battle_debug & DBG_INTRO_PACE_OFF))
				return original_fade_tick(node);
			update_tick();
			if (is_logic_tick) return original_fade_tick(node);
			uint8_t saved_payload[6];
			memcpy(saved_payload, node + 0x0C, sizeof(saved_payload)); // payload only: +0 flags, +4 next, +8 callback
			// v72: the intro flavour calls 0x47D910 (0x4A6CB0(0x1000) / 0x4A6CC0(1) behind 0x1CFF6E2 & 4)
			// when its step counter +0x0C equals 5, before incrementing it. Held on 5, every held frame
			// would fire it again, so a held frame shows step 4 instead (one step, ~66 ms, behind).
			if (intro && *(int16_t*)(node + 0x0C) == 5)
				*(int16_t*)(node + 0x0C) = 4;
			int result = original_fade_tick(node);              // still draws this frame
			memcpy(node + 0x0C, saved_payload, sizeof(saved_payload));
			return result & ~2;
		}

		// Battle stage texture animation (the beach waves, waterfalls, conveyors, blinking lights).
		// `BS_UpdateTextureAnimation 0x50CB20` blits rectangles inside video memory every frame from the
		// stage's .X animation block. It has many call sites, so it is hooked as a function: prologue
		// `53 55 8B 6C 24 10` (6 bytes, clean boundary, no relative call), cdecl with two arguments
		// (callers do `add esp,8` and it ends in a plain ret). Skipping it between logic frames simply
		// leaves the last blit on screen, so nothing flickers.
		constexpr uint32_t ADDR_STAGE_TEXTURE_ANIMATION = 0x50CB20;
		const uint8_t STAGE_TEXTURE_ANIMATION_PROLOGUE[6] = { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x10 };
		typedef void (__cdecl *stage_texture_animation_t)(int, int);
		stage_texture_animation_t original_stage_texture_animation = nullptr;

		void __cdecl stage_texanim_hook(int a, int b)
		{
			if (original_stage_texture_animation == nullptr) return;
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
			{
				original_stage_texture_animation(a, b);
				return;
			}
			update_tick();
			if (is_logic_tick) original_stage_texture_animation(a, b);
		}

		// Battle intro. Each entity's appear/entry state machine is stepped by 0x50C110 from the entity
		// task, once per HOST frame, which is why the party's entry animations and their sounds run ahead
		// of the rest of the intro. The state machine draws nothing of its own, but most of its states do
		// write the model colour, so a skipped frame cannot simply return (see appear_tick_hook below);
		// the first step of a freshly set state still runs immediately so nothing is delayed by a frame.
		constexpr uint32_t CALL_SITE_APPEAR_TICK = 0x502B74;
		constexpr uint32_t FN_APPEAR_TICK = 0x50C110;
		// The battle flags dword. Bit 0 is FF8's own "frozen" flag: every fade and every effect runner
		// still writes its visuals while it is set but holds its own counter (verified at 0x570C88 and in
		// 0x50C410/0x50C4D0/0x50C560; 0x50C230/0x50C330 only half, see v72 in the hook). Nothing in the battle module ever writes bit 0,
		// so it is ours to drive - but strictly around a single call, never for a whole frame: 0x502AB0
		// tests it too and would stop calling 0x504290, which is where our own pose interpolation lives.
		// (BATTLE_FREEZE itself is defined below, outside this anonymous namespace, and declared in
		// internal.h so the sibling translation units can drive the same bit.)
		typedef int (__cdecl *appear_tick_t)(uint8_t*);
		appear_tick_t original_appear_tick = (appear_tick_t)FN_APPEAR_TICK;

		int __cdecl appear_tick_hook(uint8_t* entity)
		{
			if (entity == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
				return original_appear_tick(entity);
			update_tick();
			if (is_logic_tick || entity[6] == 0) return original_appear_tick(entity);
			if (ff8_interp::cfg().battle_debug & DBG_DEATH_FADE_SKIP) return 0; // pre-v58 behaviour: skip the tick entirely
			// States 5, 8 and 9 (removal 0x50C5F0, 0x50C670, 0x50C6D0) do not test the freeze bit, so
			// they keep the plain skip - otherwise a corpse would be removed at host rate.
			if (entity[5] == 5 || entity[5] == 8 || entity[5] == 9) return 0;
			// v72: the fade-in handlers 0x50C230 (states 1/2) and 0x50C330 (states 0xA/0xC) honour the
			// freeze bit only HALF way: they open with an UNCONDITIONAL `inc byte [e+6]` (0x50C245 /
			// 0x50C346) and only the second +1 at the end sits behind `test [0x1D96A9C],1`
			// (0x50C30D / 0x50C3E9). Vanilla ramps +2 per tick (15 steps = 8 ticks, 0.53 s); v58-v71
			// added +1 on every held host frame, so at 144 Hz the entry fade finished in ~1.4 ticks and
			// monsters popped in (and their intro sequence moved on) far ahead of the camera pan. Hold
			// e[6] across the frozen call and feed the handler the value the last tick drew (h - 2 ->
			// pre-inc h - 1 < 15), so a held frame redraws that tick's colour and can never complete the
			// state early. The other states only increment behind the bit; DBG_APPEAR_RAMP_OFF = v71.
			const uint8_t appear_state = entity[5];
			const bool pre_increment = (appear_state == 1 || appear_state == 2 || appear_state == 0x0A || appear_state == 0x0C)
				&& !(ff8_interp::cfg().battle_debug & DBG_APPEAR_RAMP_OFF);
			const uint8_t ramp = entity[6];
			if (pre_increment) entity[6] = ramp >= 2 ? uint8_t(ramp - 2) : 0;
			// One instruction before our call site, 0x502B5C -> 0x50A0A0 rewrites the model colour
			// e[0x28..0x2B] back to DAT_00B8B7D8 every HOST frame. The gated fade 0x50C560 is a pure
			// function of the ramp counter e[6] and only ran on logic ticks, so one frame in ten showed
			// the faded colour and the other nine the full-bright default - the reported flicker-glow.
			// Under the freeze bit the fade rewrites the colour on every host frame while e[6] is held,
			// so the fade looks continuous and still ramps at the logic rate.
			uint32_t saved_freeze = *BATTLE_FREEZE;
			*BATTLE_FREEZE = saved_freeze | 1;
			int result = original_appear_tick(entity);
			*BATTLE_FREEZE = saved_freeze;
			if (pre_increment)
			{
				entity[6] = ramp;
				if (ff8_interp::cfg().trace && ramp <= 2)
					ff8_interp::log_trace("ff8appear f=%u ent=%p state=%d ramp held at %d (pre-inc handler)\n", ff8_interp::frame_counter(), entity, appear_state, ramp);
			}
			return result;
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
		constexpr uint32_t CALL_SITE_MANAGER_QUEUE = 0x50090B;       // call 0x508420(*0x1D96AA4)
		uint32_t* const BATTLE_INTRO_STATE = (uint32_t*)0x1D27B04;
		typedef int (__cdecl *manager_queue_t)(void*);
		manager_queue_t original_manager_queue = (manager_queue_t)FN_EXEC_TASK_QUEUE;
		uint32_t intro_held_frames = 0;

		int __cdecl manager_queue_hook(void* queue)
		{
			if (queue == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle
				|| (ff8_interp::cfg().battle_debug & DBG_INTRO_PACE_OFF) || *BATTLE_INTRO_STATE >= 3)
				return original_manager_queue(queue);
			update_tick();
			if (is_logic_tick)
			{
				if (ff8_interp::cfg().trace && intro_held_frames != 0 && (logic_ticks % 15) == 0)
					ff8_interp::log_trace("ff8intro f=%u manager queue on tick, intro state %u, held %u frames so far\n", ff8_interp::frame_counter(), *BATTLE_INTRO_STATE, intro_held_frames);
				return original_manager_queue(queue);
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
		caption_tick_t original_caption_tick = nullptr;

		int __cdecl caption_tick_hook(uint8_t* node)
		{
			if (original_caption_tick == nullptr) return 0;
			if (node == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
				return original_caption_tick(node);
			update_tick();
			if (is_logic_tick) return original_caption_tick(node);
			uint8_t saved_timer = node[CAPTION_TIMER];
			int result = original_caption_tick(node);
			node[CAPTION_TIMER] = saved_timer;
			return result & ~2; // the restore undid the completion a held frame would have reported
		}

		// Battle interface. `BattleUI_UpdateAllWindows 0x4B9C80` is the per-frame update pass over the
		// nine window slots (typewriter captions, menus, gauges); `BattleUI_DrawAllWindows 0x4B9DB0` is a
		// separate draw pass. Gating the update alone keeps every window drawn on every frame, and since
		// the HUD update calls it four times per rendered frame, running it only on logic frames yields
		// the 4 x 15 = 60 interface ticks per second the engine was written for.
		constexpr uint32_t ADDR_UI_UPDATE = 0x4B9C80;
		const uint8_t UI_UPDATE_PROLOGUE[8] = { 0x83, 0xEC, 0x10, 0xA1, 0x90, 0xD4, 0xD6, 0x01 }; // sub esp,10h / mov eax,[1D6D490]
		typedef void (__cdecl *ui_update_t)();
		ui_update_t original_ui_update = nullptr;

		void __cdecl ui_update_hook()
		{
			if (original_ui_update == nullptr) return;
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
			{
				original_ui_update();
				return;
			}
			update_tick();
			if (!is_logic_tick) return;
			original_ui_update();
			// v66: capture the Renzokuken trigger bar's movers for hud_blend.cpp. is_logic_tick is constant
			// for a whole host frame, so this runs on all FOUR HUD passes of a logic-tick frame, exactly
			// as the 15 fps engine ran them per rendered frame; the capture wants the state the LAST of
			// them leaves behind, so it is called after every one of them. See the marked block in
			// internal.h - it is inert unless the bar owns window slot 6.
			hud_blend_after_update();
		}

		// v77: CURSOR SPRITES (the pointing hand / fingers and the marker over the active character).
		// They are not drawn by the window draw pass: every window UPDATE callback registers the
		// sprites it wants for this frame through BattleUI_RegisterCursorSprite 0x4A8F10 -> 0x4A8130
		// into the per-frame table 0x1D6D4B0 (8 entries x 0x2C, slot = 0x1D74EA0 + n, one pair per HUD
		// pass) or 0x4A8250 -> 0x1D6D640 (2 x 0x2C), and BattleUI_DrawCursorSprites 0x4A78E0 - called once
		// per rendered frame at 0x47D14B, right after the 4th HUD pass - draws every active entry and
		// then CLEARS both tables (and the UI cursor block bytes 0x1A79073 / 0x1A79087, pass index
		// 0x1D74EA8 / 0x1D74EA0 / 0x1D74EA4). Since the window update 0x4B9C80 runs on logic ticks only,
		// nothing registers on a held frame: the table is empty and the cursors are drawn on ~1 host
		// frame in 10 - the fast "flicker/spin" of the hand and the active-character marker that was
		// reported since v5x. Fix: on a tick frame snapshot the registered entries just before the draw;
		// on a held frame, if nothing registered itself, put the snapshot back before the draw, so every
		// host frame draws what the last logic frame registered (vanilla: one frame = one tick). The
		// draw allocates its primitives fresh from the UI buffer each call (0x4B77C0 / 0x49D6E0), so a
		// repeated draw is safe. Entries registered by the engine on a held frame win (none are known:
		// all 85 0x4A8F10 callers but the -1 "hide" in 0x4A8870 are window updates).
		constexpr uint32_t CALL_SITE_CURSOR_DRAW = 0x47D14B;   // call 0x4A78E0, void __cdecl(void)
		constexpr uint32_t FN_CURSOR_DRAW = 0x4A78E0;
		uint8_t* const CURSOR_TABLE_A = (uint8_t*)0x1D6D4B0;
		constexpr size_t CURSOR_TABLE_A_ENTRIES = 8;
		uint8_t* const CURSOR_TABLE_B = (uint8_t*)0x1D6D640;
		constexpr size_t CURSOR_TABLE_B_ENTRIES = 2;
		constexpr size_t CURSOR_ENTRY_SIZE = 0x2C;          // byte +0 = active
		uint8_t* const CURSOR_UI_BYTES[2] = { (uint8_t*)0x1A79073, (uint8_t*)0x1A79087 }; // 0x1A78C88 + 0x3EB / 0x3FF, cleared by the draw
		typedef void (__cdecl *cursor_draw_t)();
		cursor_draw_t original_cursor_draw = (cursor_draw_t)FN_CURSOR_DRAW;
		uint8_t cursor_snapshot_a[CURSOR_TABLE_A_ENTRIES * CURSOR_ENTRY_SIZE] = {};
		uint8_t cursor_snapshot_b[CURSOR_TABLE_B_ENTRIES * CURSOR_ENTRY_SIZE] = {};
		uint8_t cursor_snapshot_ui_bytes[2] = {};
		bool cursor_snapshot_valid = false;
		uint32_t cursor_replays = 0;
		uint32_t cursor_last_frame = 0xFFFFFFFF; // host frame of the previous draw call

		inline bool cursor_tables_empty()
		{
			for (size_t i = 0; i < CURSOR_TABLE_A_ENTRIES; i++)
				if (CURSOR_TABLE_A[i * CURSOR_ENTRY_SIZE] != 0) return false;
			for (size_t i = 0; i < CURSOR_TABLE_B_ENTRIES; i++)
				if (CURSOR_TABLE_B[i * CURSOR_ENTRY_SIZE] != 0) return false;
			return true;
		}

		void __cdecl cursor_draw_hook()
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle || (ff8_interp::cfg().battle_debug & DBG_CURSOR_HOLD_OFF))
			{
				original_cursor_draw();
				return;
			}
			update_tick();
			// A gap (the 4-pass HUD state was left, a pause, a menu) drops the snapshot: never redraw a stale list.
			if (cursor_last_frame + 1 != ff8_interp::frame_counter()) cursor_snapshot_valid = false;
			cursor_last_frame = ff8_interp::frame_counter();
			if (is_logic_tick)
			{
				memcpy(cursor_snapshot_a, CURSOR_TABLE_A, sizeof(cursor_snapshot_a));
				memcpy(cursor_snapshot_b, CURSOR_TABLE_B, sizeof(cursor_snapshot_b));
				cursor_snapshot_ui_bytes[0] = *CURSOR_UI_BYTES[0];
				cursor_snapshot_ui_bytes[1] = *CURSOR_UI_BYTES[1];
				cursor_snapshot_valid = true;
			}
			else if (cursor_snapshot_valid && cursor_tables_empty())
			{
				memcpy(CURSOR_TABLE_A, cursor_snapshot_a, sizeof(cursor_snapshot_a));
				memcpy(CURSOR_TABLE_B, cursor_snapshot_b, sizeof(cursor_snapshot_b));
				*CURSOR_UI_BYTES[0] = cursor_snapshot_ui_bytes[0];
				*CURSOR_UI_BYTES[1] = cursor_snapshot_ui_bytes[1];
				cursor_replays++;
				if (ff8_interp::cfg().trace && (cursor_replays % 600) == 1)
					ff8_interp::log_trace("ff8cursor f=%u held frame: cursor sprites of the last tick redrawn (%u replays)\n", ff8_interp::frame_counter(), cursor_replays);
			}
			original_cursor_draw();
		}

		// The real battle camera. `updateBattleCamera 0x504060` (call site 0x500988 in the frame driver)
		// steps the camera byte-code sequence, ticks its own task queue and writes the live camera pose:
		// position at 0xB8B7F0 and look-at at 0xB8B7F8, each three int16 plus padding, with the rest
		// pose at 0xB8B800 and a roll/zoom short at 0x1D8E038. The battle render (0x502D40, 0x5033E0)
		// reads that pose. Running it per host frame is what swept the camera about ten times too fast.
		// (0x500FD0, chased for several rounds before this, only animates camera dummies and does not
		// own the view.)
		constexpr uint32_t CALL_SITE_BATTLE_CAMERA = 0x500988;
		constexpr uint32_t FN_BATTLE_CAMERA = 0x504060;
		int16_t* const CAMERA_POSE = (int16_t*)0xB8B7F0;   // 8 int16: position xyz + pad, look-at xyz + pad
		int16_t* const CAMERA_ROLL = (int16_t*)0x1D8E038;   // projection distance (zoom), despite the name
		// v117: the real roll. 0x5033E0 (render) builds Rz(this) and turns the camera position and look-at
		// by it before the view matrix; 0x504060 writes it with the pose. 12-bit angle, 0 = no roll.
		int16_t* const CAMERA_Z_ROLL = (int16_t*)0x1D977A2;
		constexpr int CAMERA_POSE_WORDS = 8;
		typedef void (__cdecl *battle_camera_t)();
		battle_camera_t original_battle_camera = (battle_camera_t)FN_BATTLE_CAMERA;

		struct CameraPose
		{
			int16_t words[CAMERA_POSE_WORDS];
			int16_t roll;
			int16_t z_roll;
		};
		CameraPose camera_pose_prev, camera_pose_next;
		bool camera_pose_valid = false;
		CameraPose camera_published;             // what this hook last wrote to the pose (to spot foreign writes)
		bool camera_published_valid = false;
		uint32_t camera_foreign_held_frames = 0;     // v115 trace: held frames whose pose someone else changed

		void __cdecl battle_camera_hook()
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
			{
				original_battle_camera();
				return;
			}
			update_tick();
			// A summon cinematic writes the pose itself (18 direct stores in 0x6C3940) and gf_pacing
			// blends it; the engine's own camera block is idle meanwhile, so blending here only
			// republished the pre-summon pose over the cinematic's every frame (v61-v63: camera stuck
			// facing off to the side). The original still ticks the camera task queue once per tick.
			if (gf_cinematic_owns_camera())
			{
				if (is_logic_tick) original_battle_camera();
				camera_pose_valid = false;
				return;
			}
			if (is_logic_tick)
			{
				// The update may exit without writing the pose: never let it capture our blend. But only put
				// `next` back when the pose is still exactly what we published: a spell effect ticked earlier
				// in this frame (0x50093A runs before 0x500988) may have written the camera itself, and
				// restoring over that erased every effect-driven camera move (v57-v68: Blizzard's camera
				// lagging behind the spell, the same failure the GF cinematic had).
				const bool pose_is_ours = camera_published_valid && memcmp(CAMERA_POSE, camera_published.words, sizeof(camera_published.words)) == 0
					&& *CAMERA_ROLL == camera_published.roll && *CAMERA_Z_ROLL == camera_published.z_roll;
				if (camera_pose_valid && pose_is_ours)
				{
					memcpy(CAMERA_POSE, camera_pose_next.words, sizeof(camera_pose_next.words));
					*CAMERA_ROLL = camera_pose_next.roll;
					*CAMERA_Z_ROLL = camera_pose_next.z_roll;
				}
				original_battle_camera();
				CameraPose now;
				memcpy(now.words, CAMERA_POSE, sizeof(now.words));
				now.roll = *CAMERA_ROLL;
				now.z_roll = *CAMERA_Z_ROLL;
				// A cut (a new shot) moves the camera far in one tick: snap instead of sweeping through it.
				bool cut = !camera_pose_valid;
				for (int i = 0; i < 3 && !cut; i++)
					if (abs(int(now.words[i]) - int(camera_pose_next.words[i])) > 2048 || abs(int(now.words[4 + i]) - int(camera_pose_next.words[4 + i])) > 2048)
						cut = true;
				if (!cut && abs(((int(now.z_roll) - int(camera_pose_next.z_roll) + 2048) & 4095) - 2048) > 1024) // v117: roll jump > 90 deg = new shot
					cut = true;
				if (ff8_interp::cfg().trace)
				{
					// v115: one line per tick while the camera moves (intro orbit diagnosis).
					int max_delta = 0;
					if (camera_pose_valid)
						for (int i = 0; i < 3; i++)
						{
							max_delta = (std::max)(max_delta, abs(int(now.words[i]) - int(camera_pose_next.words[i])));
							max_delta = (std::max)(max_delta, abs(int(now.words[4 + i]) - int(camera_pose_next.words[4 + i])));
						}
					if (max_delta != 0 || cut || camera_foreign_held_frames != 0 || now.z_roll != camera_pose_next.z_roll)
						ff8_interp::log_trace("ff8cam f=%u intro=%u mode=%d ours=%d cut=%d dmax=%d foreign_held=%u pos=(%d,%d,%d) at=(%d,%d,%d) zoom=%d roll=%d\n",
							ff8_interp::frame_counter(), *BATTLE_INTRO_STATE, int(*(const int16_t*)0x1D97718), int(pose_is_ours), int(cut), max_delta, camera_foreign_held_frames,
							now.words[0], now.words[1], now.words[2], now.words[4], now.words[5], now.words[6], now.roll, now.z_roll);
					camera_foreign_held_frames = 0;
				}
				camera_pose_prev = cut ? now : camera_pose_next;
				camera_pose_next = now;
				camera_pose_valid = true;
				// Fall through and blend on this frame too. Leaving the freshly captured pose on screen
				// here and blending only on the frames in between makes every logic frame jump ahead and
				// the next frame snap back - the same jitter the world map had before its display pass
				// ran on every frame.
			}
			if (!camera_pose_valid) return;
			// v115 trace: on a held frame the pose should still be what we published last frame.
			if (!is_logic_tick && camera_published_valid
				&& (memcmp(CAMERA_POSE, camera_published.words, sizeof(camera_published.words)) != 0 || *CAMERA_ROLL != camera_published.roll || *CAMERA_Z_ROLL != camera_published.z_roll))
				camera_foreign_held_frames++;
			// Pure output: the update rewrites it from the sequence state on the next logic frame, so a
			// blended value here reaches only the renderer and cannot feed back.
			int16_t blended[CAMERA_POSE_WORDS];
			for (int i = 0; i < CAMERA_POSE_WORDS; i++)
				blended[i] = lerp_i16(camera_pose_prev.words[i], camera_pose_next.words[i], tick_alpha);
			memcpy(CAMERA_POSE, blended, sizeof(blended));
			*CAMERA_ROLL = lerp_i16(camera_pose_prev.roll, camera_pose_next.roll, tick_alpha);
			memcpy(camera_published.words, blended, sizeof(blended));
			camera_published.roll = *CAMERA_ROLL;
			*CAMERA_Z_ROLL = (ff8_interp::cfg().battle_debug2 & DBG2_CAMERA_ROLL_OFF) ? camera_pose_next.z_roll
			                                                                   : lerp_angle(camera_pose_prev.z_roll, camera_pose_next.z_roll, tick_alpha);
			camera_published.z_roll = *CAMERA_Z_ROLL;
			camera_published_valid = true;
		}

		// Summon action sequence 0x50B2A0: a state machine on its task byte +0xD. States 0..3 are the
		// caster stepping into position and vanishing (no geometry of its own, it only sets animations
		// and waits); state 3 registers the cinematic tree and from state 4 on the cinematic runs and
		// draws. Pacing the early states fixes the rushed entrance while leaving the cinematic, which
		// hangs if its tick is skipped, running exactly as before.
		constexpr uint32_t ADDR_GF_SEQUENCE_TICK = 0x50B2A0;
		const uint8_t GF_SEQUENCE_PROLOGUE[6] = { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C }; // push ebx/ebp, mov ebp,[esp+0Ch]
		constexpr uint8_t GF_STATE_CINEMATIC = 4;
		constexpr uint32_t GF_TASK_STATE = 0x0D;
		typedef int (__cdecl *gf_sequence_tick_t)(uint8_t*);
		gf_sequence_tick_t original_gf_sequence_tick = nullptr;

		// v71: "a summon cinematic is live", for apply_results_hook. Every summon - Quezacotl and all the
		// GFs gf_pacing does not pace - goes through this one task, and from state 4 on (state 3 has just
		// registered the cinematic tree and falls through into 4) until the task ends (state 9 returns 2)
		// the cinematic runs. The task is called every host frame then (the summon is not holdable, the
		// effect tick runs it at host rate), so a stamp of the last frame it was seen in state >= 4 is
		// live within one frame's grace, like gf_cinematic_owns_camera(). The end of the task, a state
		// below 4 (a new summon's entrance) and animations_reset() clear it explicitly.
		uint32_t summon_live_frame = 0xFFFFFFFF;
		inline void summon_mark(const uint8_t* node, int result)
		{
			if (result == 2 || node[GF_TASK_STATE] < GF_STATE_CINEMATIC) summon_live_frame = 0xFFFFFFFF;
			else summon_live_frame = ff8_interp::frame_counter();
		}
		inline bool summon_cinematic_live()
		{
			return summon_live_frame != 0xFFFFFFFF && (ff8_interp::frame_counter() - summon_live_frame) <= 1;
		}

		int __cdecl gf_seq_tick_hook(uint8_t* node)
		{
			if (node == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
				return original_gf_sequence_tick(node);
			int result;
			if (node[GF_TASK_STATE] >= GF_STATE_CINEMATIC) // cinematic draws, leave it
				result = original_gf_sequence_tick(node);
			else
			{
				update_tick();
				if (!is_logic_tick) // still running, just not advanced on this frame
				{
					summon_mark(node, 0);
					return 0;
				}
				result = original_gf_sequence_tick(node);
			}
			summon_mark(node, result);
			return result;
		}

		// GF summon damage. A summon cinematic applies its result on exactly one timeline frame (frame
		// 0x159 of the inner timeline 0x6C3940 calls Battle_ApplyActionResultToAllTargets 0x506BA0), while
		// the Boost gauge only publishes its multiplier from the window-6 update callback 0x56DD70, which
		// runs at the logic rate. With the cinematic tree still ticking at host rate the damage frame
		// arrives ~10x early, before the gauge reaches state 5, so 0x4850A0 has never run and TARGET_DATA
		// 0x1D28344 still holds the pre-boost damage - which is literally 0 for a boostable summon
		// (0x48D9FE zeroes the multiplier byte 0x1D2A23A at resolve time). Settling a pending gauge here
		// restores vanilla ordering. Interim fix: the cinematic's own pacing is still open.
		constexpr uint32_t ADDR_APPLY_RESULTS = 0x506BA0;
		const uint8_t APPLY_RESULTS_PROLOGUE[5] = { 0x57, 0x8B, 0x7C, 0x24, 0x0C }; // push edi / mov edi,[esp+0Ch]
		constexpr uint32_t FN_PUBLISH_BOOST = 0x4850A0; // void __cdecl (int): publishes the boost and recomputes every target
		uint16_t* const BOOST_VALUE = (uint16_t*)0x209CEF0;  // what the minigame accumulated, capped 0xFA
		uint8_t* const BOOST_ENABLED = (uint8_t*)0x209CEF8;  // non-zero while a boostable summon is armed
		uint8_t* const BOOST_STATE = (uint8_t*)0x209CEFB;    // 0x56DD70's state: <5 pending, 5 publish, 6 published
		uint8_t** const CURRENT_ACTION = (uint8_t**)0x1D99A50;   // -> the action being played: cmd id at +1, TARGET_DATA at +8, target count at +0x10
		constexpr uint8_t CMD_BOOSTABLE_SUMMON = 0xFE;       // the only command that arms the gauge (both 0x56DCE0 call sites, 0x50AC32 / 0x50B4C5, guard on it)
		typedef void (__cdecl *apply_results_t)(int, int);
		apply_results_t original_apply_results = nullptr;

		void __cdecl apply_results_hook(int target_data, int target_count)
		{
			if (original_apply_results == nullptr) return;
			if (ff8_interp::cfg().battle_debug & DBG_GF_BOOST_OFF)
			{
				original_apply_results(target_data, target_count);
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
			const uint8_t* action = *CURRENT_ACTION;
			const bool cinematic_live = gf_cinematic_owns_camera() || gf_cinematic_live()
				|| (!(ff8_interp::cfg().battle_debug & DBG_BOOST_ANY_GF_OFF) && summon_cinematic_live());
			if (cinematic_live && action != nullptr && action[1] == CMD_BOOSTABLE_SUMMON && *BOOST_STATE < 5)
			{
				if (ff8_interp::cfg().trace)
					ff8_interp::log_trace("ff8gf boost settle f=%u value=%u enabled=%u state=%u\n", ff8_interp::frame_counter(), (unsigned)*BOOST_VALUE, (unsigned)*BOOST_ENABLED, (unsigned)*BOOST_STATE);
				// Exactly what 0x56DD70 case 5 does, and idempotent: the state byte guards it.
				uint16_t boost = *BOOST_VALUE;
				if (boost == 0) boost = 100; // no Square presses -> the neutral multiplier, never 0
				((void(__cdecl*)(int))FN_PUBLISH_BOOST)((int)boost);
				*BOOST_STATE = 6;
				// Disarm the gauge block like the engine's own case 5 does. Leaving it armed let the NEXT
				// action re-enter here (0x1D99A50 still pointed at the summon): Blizzard cast after
				// Quezacotl was republished against the summon's targets and did 0 damage (v63).
				*BOOST_ENABLED = 0;
				*(uint8_t*)0x209CEF9 = 0;
			}
			else if (ff8_interp::cfg().trace && action != nullptr && action[1] == CMD_BOOSTABLE_SUMMON)
				ff8_interp::log_trace("ff8gf boost no-settle f=%u live=%d state=%u enabled=%u\n", ff8_interp::frame_counter(), (int)cinematic_live, (unsigned)*BOOST_STATE, (unsigned)*BOOST_ENABLED);
			original_apply_results(target_data, target_count);
		}

		int __cdecl magic_effect_tick_hook(uint8_t* task)
		{
			uint32_t root_before = *EFFECT_ROOT;
			int result = original_magic_effect_tick(task);
			if (*EFFECT_ROOT != root_before && *EFFECT_ROOT != 0)
			{
				registered_root = (void*)*EFFECT_ROOT;
				if (ff8_interp::cfg().trace)
					ff8_interp::log_trace("ff8fx registered effect root %p (effect tick)\n", registered_root);
			}
			return result;
		}
		int __cdecl magic_cast_tick_hook(uint8_t* task)
		{
			uint32_t root_before = *EFFECT_ROOT;
			int result = original_magic_cast_tick(task);
			if (*EFFECT_ROOT != root_before && *EFFECT_ROOT != 0)
			{
				registered_root = (void*)*EFFECT_ROOT;
				if (ff8_interp::cfg().trace)
					ff8_interp::log_trace("ff8fx registered effect root %p (cast tick)\n", registered_root);
			}
			return result;
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
			if (ctx == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
				return original_exec_task_queue(ctx);
			update_tick();
			paused_unblend_all(); // v87: the effect sees the real places of the entities it moves
			position_restore_logic_all(); // v102: the same for entities whose sequence runs
			uint8_t* action = *BATTLE_ACTION_DATA;
			if (ff8_interp::cfg().trace)
			{
				static void* last_trace_ctx = nullptr;
				static uint8_t last_command = 0xFF;
				static uint16_t last_effect = 0xFFFF;
				uint8_t command = action ? action[1] : 0xFF;
				uint16_t effect = action ? *(uint16_t*)(action + ACT_EFFECT_ID) : 0xFFFF;
				if (ctx != last_trace_ctx || command != last_command || effect != last_effect)
				{
					ff8_interp::log_trace("ff8fx f=%u ctx=%p cmd=%02x eff=%u anim=%02x arg=%d hold=%p act=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
						ff8_interp::frame_counter(), ctx, command,
						action ? *(uint16_t*)(action + ACT_EFFECT_ID) : 0, action ? action[ACT_ANIM_STATE] : 0,
						action ? *(int16_t*)(action + 4) : 0, holdable_ctx,
						action ? action[0] : 0, action ? action[1] : 0, action ? action[2] : 0, action ? action[3] : 0,
						action ? action[4] : 0, action ? action[5] : 0, action ? action[6] : 0, action ? action[7] : 0,
						action ? action[8] : 0, action ? action[9] : 0, action ? action[10] : 0, action ? action[11] : 0,
						action ? action[16] : 0, action ? action[17] : 0, action ? action[18] : 0, action ? action[19] : 0);
					last_trace_ctx = ctx;
					last_command = command;
					last_effect = effect;
				}
			}
			bool spell_effect = holdable_action(action);
			// v70 (DBG_RENZOKUKEN_SYNC_OFF restores the old rule): a spell tree is paced from its very first
			// frame. Before, a tree only became `holdable_ctx` on the SECOND tick that ran it (ctx ==
			// last_effect_ctx), so from registration (the cast tick runs at host rate, usually off-tick) until
			// then it ran on every host frame - ~10-19 effect frames in one or two ticks. The Renzokuken
			// modules (e.g. eff 160 = 0x5FF080, main task 0x5FF180) set Squall's sequence on their frame
			// 1 and read the trigger buckets the sequence's opcode (0x504E50 -> 0x4BA4C0 / 0x4BA560)
			// writes to 0x1D98214 on frame 2; the sequence runs only on ticks, so frame 2 read the
			// buckets of Squall's PREVIOUS gunblade attack, and the finisher variant (action+4 == -2)
			// links its finisher when the frame counter equals that stale sum: an instant Rough Divide.
			const bool pace_from_first_frame = !(ff8_interp::cfg().battle_debug & DBG_RENZOKUKEN_SYNC_OFF);
			// v78: the unpaced fallback belongs to one tree; the effect root only ever holds one.
			if (fx_unpaced_ctx != nullptr && ctx != fx_unpaced_ctx) fx_unpaced_ctx = nullptr;
			if (is_logic_tick)
			{
				// A tree is only held once a real tick has run it at least once: its first tick builds
				// the task list. Record the primitives this tick links so the skipped frames can
				// re-issue them. v70: a fresh tree's first tick is recorded too (without deferral - there
				// is no pair yet), so its skipped frames replay it instead of drawing nothing.
				const bool fresh = pace_from_first_frame && spell_effect && ctx != holdable_ctx;
				if (fresh)
				{
					fx_prims.clear();
					fx_prims_previous.clear();
					fx_prims_two_ticks_ago.clear();
				}
				// v78: a tree that cannot be replayed safely runs every host frame and is not recorded.
				bool record = spell_effect && (ctx == holdable_ctx || fresh) && ctx != fx_unpaced_ctx;
				if (record)
				{
					fx_prims_two_ticks_ago.swap(fx_prims_previous);
					fx_prims_previous.swap(fx_prims);
					fx_prims.clear();
					fx_unsafe_calls = 0;
					fx_unsafe_site = 0;
					fx_prev_recorded = false;
					fx_record_target = &fx_prims;
					fx_live_draws_recording.clear();
					fx_live_recording = true;
					// Hold this tick's link calls back so the ordering table gets the same interpolated list
					// the frames in between get (rule 2.1); without it the logic frame would jump a whole tick
					// ahead of the models and the camera and the next frame would snap back.
					// v63: only while blending is actually succeeding. If the pair is falling back there is
					// nothing to substitute, so holding the calls back would add risk for no gain - the tick
					// then runs exactly as it did in v59, engine-drawn and in place.
					fx_defer_links = !fresh && fx_blend_defer_ok();
				}
				int result = original_exec_task_queue(ctx);
				if (record)
				{
					fx_live_recording = false;
					fx_live_draws.swap(fx_live_draws_recording);
					fx_live_ctx = ctx;
					fx_record_target = nullptr;
					const bool deferred = fx_defer_links; // cleared early if a call could not be held back
					fx_defer_links = false;
					if (deferred) fx_replay_list(fx_prims, &fx_prims_previous, 0, true);
					if (ff8_interp::cfg().trace && (recorded_tick_count++ % 15) == 0)
					{
						int calls_per_linker[FX_LINKER_COUNT] = {};
						unsigned volatile_calls = 0;
						for (const FxCall& call : fx_prims)
						{
							calls_per_linker[call.linker]++;
							if (call.is_volatile) volatile_calls++;
						}
						ff8_interp::log_trace("ff8fx rec n=%u by linker %d/%d/%d/%d/%d deferred=%d flushes=%u | vol=%u copied=%u stale=%u dropped=%u unsafe=%u\n",
							(unsigned)fx_prims.size(), calls_per_linker[0], calls_per_linker[1], calls_per_linker[2], calls_per_linker[3], calls_per_linker[4], (int)deferred, fx_flushes,
							volatile_calls, fx_copied, fx_stale, fx_dropped_volatile, fx_unsafe_calls);
					}
					// v78: this tick linked something from per-frame memory that no replay can reproduce
					// safely (no complete packet copy, or its bucket is per-frame memory). Replaying the
					// rest would leave that geometry out on 9 frames of 10; run the tree every host frame
					// instead (unpaced, as every tree was before v58) - the one thing it cannot do is crash.
					if (fx_unsafe_calls > 0 && fx_unpaced_ctx != ctx)
					{
						const bool keep_paced = (ff8_interp::cfg().battle_debug & DBG_FX_UNPACED_OFF) != 0;
						if (fx_unsafe_logged_ctx != ctx)
						{
							ff8_interp::log_info("ff8fx eff=%u ctx=%p: %u link call(s) from per-frame memory without a usable copy (first site %08x) - %s\n",
								action ? (unsigned)*(uint16_t*)(action + ACT_EFFECT_ID) : 0u, ctx, fx_unsafe_calls, fx_unsafe_site,
								keep_paced ? "kept paced, those calls are missing on held frames (bit 1073741824)" : "running this tree every host frame (unpaced)");
							fx_unsafe_logged_ctx = ctx;
						}
						if (!keep_paced)
						{
							fx_unpaced_ctx = ctx;
							fx_prims.clear();
							fx_prims_previous.clear();
							fx_prims_two_ticks_ago.clear();
						}
					}
				}
				void* previous_holdable = holdable_ctx;
				holdable_ctx = (spell_effect && (ctx == last_effect_ctx || pace_from_first_frame)) ? ctx : nullptr;
				// A different tree: neither recorded list describes it any more, and pairing lists that
				// belong to two different effects would interpolate unrelated geometry. (A fresh tree
				// already cleared them before recording its first tick.)
				if (holdable_ctx != previous_holdable && !fresh)
				{
					fx_prims.clear();
					fx_prims_previous.clear();
					fx_prims_two_ticks_ago.clear();
				}
				last_effect_ctx = ctx;
				// v70: the tree ended (the driver clears the root on 0 and stops calling us). Forget it,
				// or the next cast of the same module - same static queue address - would be taken
				// for a held tree and replay the previous cast's primitives on its first frames.
				if (pace_from_first_frame && result == 0)
				{
					holdable_ctx = nullptr;
					last_effect_ctx = nullptr;
					fx_prims.clear();
					fx_prims_previous.clear();
					fx_prims_two_ticks_ago.clear();
					fx_live_draws.clear();
					fx_live_ctx = nullptr;
				}
				if (result == 0)
				{
					if (ctx == fx_unpaced_ctx) fx_unpaced_ctx = nullptr;
					fx_unsafe_logged_ctx = fx_stale_logged_ctx = nullptr;
				}
				return result;
			}
			// v70: a spell tree registered off-tick waits for the next tick instead of running at host
			// rate until its second tick. Nothing has run or been recorded yet, so there is nothing to
			// replay; returning non-zero keeps the root alive (0x500942).
			if (pace_from_first_frame && spell_effect && ctx != holdable_ctx) return 1;
			if (!spell_effect || ctx != holdable_ctx) return original_exec_task_queue(ctx);
			// v78 unpaced fallback: the tree cannot be replayed safely, so it runs on this frame too.
			if (ctx == fx_unpaced_ctx)
			{
				int result = original_exec_task_queue(ctx);
				if (result == 0)
				{
					holdable_ctx = nullptr;
					last_effect_ctx = nullptr;
					fx_unpaced_ctx = nullptr;
					fx_unsafe_logged_ctx = fx_stale_logged_ctx = nullptr;
					fx_prims.clear();
					fx_prims_previous.clear();
					fx_prims_two_ticks_ago.clear();
				}
				return result;
			}
			// Skip the effect entirely between logic frames. Nothing advances and nothing is restored, so no
			// state can be left inconsistent (the snapshot hold that used to sit here hung the game). The cost
			// is that the effect contributes no geometry on these frames: it is drawn at the logic rate until
			// the emitted prims are replayed instead.
			if (ctx != held_effect_ctx)
			{
				held_effect_ctx = ctx;
				held_frames = 0;
			}
			held_frames++;
			if (ff8_interp::cfg().trace && (held_frames % 120) == 0)
				ff8_interp::log_trace("ff8fx skip ctx=%p eff=%u frames=%u prims=%u\n", ctx, *(uint16_t*)(action + ACT_EFFECT_ID), held_frames, (unsigned)fx_prims.size());
			const uint32_t stale_before = fx_stale;
			fx_replay();
			if (fx_live_ctx == ctx)
			{
				for (const LiveDraw& live_draw : fx_live_draws) // v105 / v114
				{
					if (live_draw.draw_context) live_draw.draw_context(live_draw.entity);
					else live_draw.draw_model(live_draw.entity, live_draw.mask);
				}
			}
			if (fx_stale != stale_before && fx_stale_logged_ctx != ctx)
			{
				ff8_interp::log_info("ff8fx eff=%u ctx=%p: %u recorded packet(s) were rewritten while the tree was held (memory reused) - skipped on replay\n",
					action ? (unsigned)*(uint16_t*)(action + ACT_EFFECT_ID) : 0u, ctx, fx_stale - stale_before);
				fx_stale_logged_ctx = ctx;
			}
			return 1;
		}

		// Pad polling inside the HUD phase 0x4A84E0. It runs every host frame while the menu state
		// machine steps at the logic rate, so a held button produces an edge on many menu steps:
		// confirm repeats into the next menu and the one after. Reads are cached between logic ticks,
		// which is what the engine sees in vanilla (one poll per logic frame): the engine's own
		// current/previous copy then reports one edge per press.
		constexpr uint32_t CALL_SITE_PAD_REFRESH = 0x4A84F8;
		constexpr uint32_t FN_PAD_REFRESH = 0x49E9C0;
		constexpr uint32_t CALL_SITE_PAD_A = 0x4A84FF;
		constexpr uint32_t FN_PAD_A = 0x49ED30;
		constexpr uint32_t CALL_SITE_PAD_B = 0x4A851B;
		constexpr uint32_t FN_PAD_B = 0x49ED80;
		constexpr uint32_t CALL_SITE_PAD_C = 0x4A8537;
		constexpr uint32_t FN_PAD_C = 0x49EDE0;

		typedef void (__cdecl *pad_refresh_t)();
		typedef uint32_t (__cdecl *pad_read_t)(int, int, int);
		pad_refresh_t original_pad_refresh = (pad_refresh_t)FN_PAD_REFRESH;
		pad_read_t original_pad_a = (pad_read_t)FN_PAD_A;
		pad_read_t original_pad_b = (pad_read_t)FN_PAD_B;
		pad_read_t original_pad_c = (pad_read_t)FN_PAD_C;
		uint32_t pad_cache[3] = {};
		uint32_t pad_accumulated[3] = {};
		uint32_t pad_publish_frame[3] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF }; // host frame of the last publish

		inline bool pad_bypass()
		{
			return ff8_interp::game_mode() != ff8_interp::GameMode::Battle;
		}
		// The pad is still read every host frame and the presses are accumulated, so a tap that begins
		// and ends between two logic frames is not lost - that matters for the gunblade R1 trigger and
		// for quick menu presses. What the engine sees is one stable value per logic frame, so its own
		// current/previous comparison still yields exactly one edge per press.
		inline uint32_t pad_step(int channel, uint32_t now)
		{
			pad_accumulated[channel] |= now;
			// Publish once per host frame. The battle loop runs FOUR HUD passes per rendered frame in
			// state 0x1CDBFE0 == 3 and the menu consumes the pad on every pass, so the EDGE channels
			// (1 = repeat word, 2 = pressed) may be delivered exactly once per tick frame: returning the
			// cached word to passes 2-4 (v56-v62) consumed every press four times - the cursor wrapped a
			// four-entry list back to Attack, cancel cycled back to the same character and the Renzokuken
			// R1 fired four times. The LEVEL channel (0 = held) stays stable across the passes.
			if (is_logic_tick && pad_publish_frame[channel] != ff8_interp::frame_counter())
			{
				pad_publish_frame[channel] = ff8_interp::frame_counter();
				pad_cache[channel] = pad_accumulated[channel];
				pad_accumulated[channel] = 0;
				return pad_cache[channel];
			}
			return channel == 0 ? pad_cache[channel] : 0;
		}
		// Auto-repeat of a held button. 0x49E9C0 calls 0x49EC90 once per button group (four call sites)
		// every time the ring advances; that function counts a signed byte at port+0xC+group down from
		// 0x1D2B2F0 (0x10, initial delay) and reloads it from 0x1D2B2F1 (5, interval) after each pulse.
		// Natively the ring advances 60 times a second (4 HUD passes per 15 fps frame): first repeat after
		// 0.28 s, then 10 pulses a second. Under FFNx the ring advances on every host frame (the four HUD
		// passes are not gated), so at 144 fps the countdown runs ~10x too fast and a tap held a few frames
		// overshoots. The input latch 0x4A8E30 is unrelated (v40/v54 proved it). Fix: feed a 60 Hz
		// wall-clock budget once per host frame and let the countdown step only when a period is due.
		constexpr uint32_t CALL_SITE_PAD_REPEAT[4] = { 0x49EC05, 0x49EC1C, 0x49EC33, 0x49EC4A };
		constexpr uint32_t FN_PAD_REPEAT = 0x49EC90;
		constexpr double RING_PERIOD = 1.0 / 60.0;
		typedef uint16_t (__cdecl *pad_repeat_t)(int, int, uint16_t, uint16_t, int);
		pad_repeat_t original_pad_repeat = (pad_repeat_t)FN_PAD_REPEAT;
		double ring_budget = 0.0;            // wall-clock budget (seconds) not yet spent on a countdown step
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
		constexpr uint32_t CALL_SITE_EDGE_READ[EDGE_SITES] = { 0x4BA6F2, 0x56DD93, 0x4AD8D5, 0x4AF8F2 };
		constexpr uint32_t FN_EDGE_READ = 0x4A8420;
		constexpr uint32_t FN_PAD_REMAP = 0x4A2D60;
		typedef uint32_t (__cdecl *edge_read_t)(int);
		typedef uint32_t (__cdecl *pad_remap_t)(uint32_t);
		edge_read_t original_edge_read = (edge_read_t)FN_EDGE_READ;
		uint32_t edge_pending[EDGE_SITES] = {};
		uint32_t edge_frame[EDGE_SITES] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
		uint32_t edge_tick[EDGE_SITES] = {};
		constexpr int EDGE_QUEUE_DEPTH = 8; // same depth as the Duel history
		uint32_t edge_queue[EDGE_QUEUE_DEPTH] = {};
		int edge_queue_count = 0;
		inline bool duel_edge_off() { return (ff8_interp::cfg().battle_debug & (DBG_EDGE_ACCUMULATE_OFF | DBG_DUEL_EDGE_OFF)) != 0; }
		inline uint32_t edge_read(int site, int port)
		{
			if (pad_bypass() || port != 0 || (ff8_interp::cfg().battle_debug & DBG_EDGE_ACCUMULATE_OFF))
				return original_edge_read(port);
			update_tick();
			if (!is_logic_tick || edge_frame[site] == ff8_interp::frame_counter()) return 0; // delivered once per tick frame
			const bool stale = edge_tick[site] + 1 < logic_ticks;
			edge_frame[site] = ff8_interp::frame_counter();
			edge_tick[site] = logic_ticks;
			const uint32_t raw_edges = edge_pending[site];
			edge_pending[site] = 0;
			if (stale || raw_edges == 0) return 0;
			return ((pad_remap_t)FN_PAD_REMAP)(raw_edges) & 0xFFFF;
		}
		uint32_t __cdecl edge_read0(int port) { return edge_read(0, port); }
		uint32_t __cdecl edge_read1(int port) { return edge_read(1, port); }
		uint32_t __cdecl edge_read2(int port) { return edge_read(2, port); }
		// Site 3 (Duel), 0x49EDE0(port, back) signature: anything but the live port-0 read passes through.
		uint32_t __cdecl edge_read3(int port, int back)
		{
			if (pad_bypass() || port != 0 || back != 0 || duel_edge_off())
				return original_pad_c(port, back, 0);
			update_tick();
			if (!is_logic_tick) return 0;
			if (edge_frame[EDGE_RAW_SITE] != ff8_interp::frame_counter()) // first poll of this tick frame
			{
				const bool stale = edge_tick[EDGE_RAW_SITE] + 1 < logic_ticks;
				edge_frame[EDGE_RAW_SITE] = ff8_interp::frame_counter();
				edge_tick[EDGE_RAW_SITE] = logic_ticks;
				if (stale) edge_queue_count = 0;
			}
			if (edge_queue_count == 0) return 0;
			const uint32_t raw_edges = edge_queue[0];
			for (int i = 1; i < edge_queue_count; i++)
				edge_queue[i - 1] = edge_queue[i];
			edge_queue_count--;
			return raw_edges;
		}
		void* const EDGE_READ_HOOK[EDGE_SITES] = { (void*)edge_read0, (void*)edge_read1, (void*)edge_read2, (void*)edge_read3 };

		// v76: two counters that live INLINE in the HUD pass 0x4A84E0 (4 passes per host frame, so
		// 576 steps/s at 144 Hz instead of the native 60) and have no call of their own to gate. Each is
		// corrected from the nearest call site that runs right before it in the same pass.
		//
		// ESCAPE HOLD. 0x4A85CC..0x4A85E4: while the level word ctx+0x10 has both L2+R2 (bits 0x3) the
		// dword 0x1D6D638 is incremented (and 0x1D74E98 = 1), otherwise zeroed. The escape roll
		// Battle_TickEscapeRoll 0x486130 (tail of the paced ATB tick 0x4842B0, `jmp` at 0x484485) reads it
		// through 0x4A71A0 and rolls the run-away chance only when counter % 60 == 0, i.e. once per second
		// natively. At host rate the counter races past 60 between two ATB ticks and the roll fires only
		// when a multiple of 60 happens to land on a tick frame: irregular, from 0.1 s to well over 1 s.
		// Gate: the call 0x47D8E0 (Battle_IsPaused) at 0x4A85BD is the last thing before the increment;
		// on a held frame, when the increment is about to happen (not paused, both buttons held), the
		// counter is pre-decremented so the inline `inc` leaves it unchanged. Tick frames count all four
		// passes, exactly as a 15 fps frame did.
		//
		// ANALOG STICK REPEAT. 0x4A8604..0x4A8753 (only when no d-pad bit is held): the stick angle
		// drives a press/repeat state ctx+0x26 (7 = first press, 3 = repeat pulse, 1 = held, 8 = new
		// sector) with a signed countdown ctx+0x27 (0x1D2B2F0 = 16 initial delay, 0x1D2B2F1 = 5 interval)
		// decremented on EVERY pass - the analog twin of the d-pad repeat 0x49EC90 that v57 paced - and
		// the resulting direction bits are ORed into ctx+0x10 (level), +0x12 (repeat) and +0x14 (press).
		// Two host-rate faults: the countdown runs ~10x fast (no initial delay), and the press / pulse
		// bits written on a held frame are overwritten by the next pass before the tick-gated window
		// update 0x4B9C80 ever sees them. Gate, two call sites:
		//  * 0x4A8624, call 0x49F0A0(0, 3, 0) (the Y axis read, right before the countdown at 0x4A86CD):
		//    on a pass without a 60 Hz ring step (ring_step, the v57 budget) the countdown byte is
		//    pre-incremented when the state is live, so the inline `dec` leaves it where it was;
		//  * 0x4A87B2, call 0x47D8E0 (after the OR, before 0x4B9C80 at 0x4A87D1): on held frames the
		//    +0x12 / +0x14 bits (only analog bits then - pad_step hands the edge channels 0 off-tick) are
		//    collected, and ORed back into +0x12 / +0x14 on the first pass of the next tick frame.
		constexpr uint32_t CALL_SITE_HUD_ESCAPE = 0x4A85BD;     // call 0x47D8E0, before the escape-hold increment
		constexpr uint32_t CALL_SITE_HUD_ANALOG_Y = 0x4A8624;   // call 0x49F0A0(0, 3, 0), before the analog countdown
		constexpr uint32_t CALL_SITE_HUD_ANALOG_ACC = 0x4A87B2; // call 0x47D8E0, after the analog OR, before 0x4B9C80
		constexpr uint32_t FN_BATTLE_PAUSED = 0x47D8E0;  // return *(u8*)0x1D28DE9
		constexpr uint32_t FN_PAD_AXIS = 0x49F0A0;       // int __cdecl (port, axis, ?)
		typedef int (__cdecl *battle_paused_t)();
		typedef int (__cdecl *pad_axis_t)(int, int, int);
		battle_paused_t original_battle_paused = (battle_paused_t)FN_BATTLE_PAUSED;
		pad_axis_t original_pad_axis = (pad_axis_t)FN_PAD_AXIS;
		uint8_t** const UI_CTX = (uint8_t**)0x1D6D490;
		int32_t* const ESCAPE_HOLD = (int32_t*)0x1D6D638;
		uint16_t analog_repeat_bits = 0;
		uint16_t analog_press_bits = 0;
		uint32_t analog_publish_frame = 0xFFFFFFFF;

		int __cdecl hud_escape_hook()
		{
			const int paused = original_battle_paused();
			if (pad_bypass() || (ff8_interp::cfg().battle_debug & DBG_ESCAPE_HOLD_OFF)) return paused;
			update_tick();
			if (is_logic_tick || paused != 0) return paused;
			uint8_t* ui_ctx = *UI_CTX;
			if (ui_ctx != nullptr && (ui_ctx[0x10] & 3) == 3) // the inline inc at 0x4A85DD undoes it
				(*ESCAPE_HOLD)--;
			return paused;
		}

		int __cdecl hud_analog_y_hook(int port, int axis, int c)
		{
			const int result = original_pad_axis(port, axis, c);
			if (pad_bypass() || (ff8_interp::cfg().battle_debug & DBG_ANALOG_REPEAT_OFF)) return result;
			update_tick();
			if (ring_step) return result; // this pass may count down (decided by pad_refresh_hook, same pass)
			uint8_t* ui_ctx = *UI_CTX;
			if (ui_ctx != nullptr && ui_ctx[0x26] != 0 && (int8_t)ui_ctx[0x27] < 0x7F) // the inline dec at 0x4A86D0 undoes it
				ui_ctx[0x27]++;
			return result;
		}

		int __cdecl hud_analog_acc_hook()
		{
			const int paused = original_battle_paused();
			if (pad_bypass() || (ff8_interp::cfg().battle_debug & DBG_ANALOG_REPEAT_OFF)) return paused;
			update_tick();
			uint8_t* ui_ctx = *UI_CTX;
			if (ui_ctx == nullptr) return paused;
			uint16_t* repeat_word = (uint16_t*)(ui_ctx + 0x12);
			uint16_t* press_word = (uint16_t*)(ui_ctx + 0x14);
			if (!is_logic_tick)
			{
				analog_repeat_bits |= *repeat_word & 0xF000;   // only the stick's direction bits (0x4A873C mask)
				analog_press_bits |= *press_word & 0xF000;
				return paused;
			}
			if (analog_publish_frame != ff8_interp::frame_counter())
			{
				analog_publish_frame = ff8_interp::frame_counter();
				*repeat_word |= analog_repeat_bits;
				*press_word |= analog_press_bits;
				analog_repeat_bits = analog_press_bits = 0;
			}
			return paused;
		}

		void __cdecl pad_refresh_hook()
		{
			if (pad_bypass())
			{
				original_pad_refresh();
				return;
			}
			update_tick();
			// One countdown step per 60 Hz period, decided per ring advance. The cap of four periods means a
			// stall, or the opt-in HUD gate (one advance per logic frame), grants at most the native four steps.
			if (ring_frame != ff8_interp::frame_counter())
			{
				ring_frame = ff8_interp::frame_counter();
				ring_budget += ff8_tick::frame_delta();
				if (ring_budget > 4 * RING_PERIOD) ring_budget = 4 * RING_PERIOD;
			}
			ring_step = ring_budget >= RING_PERIOD;
			if (ring_step) ring_budget -= RING_PERIOD;
			original_pad_refresh(); // cheap, and the reads below need fresh hardware state; the ring must advance every pass (edges)
			// Collect the press edges of this ring advance for the consumers that read the ring directly.
			if (!(ff8_interp::cfg().battle_debug & DBG_EDGE_ACCUMULATE_OFF))
			{
				const uint32_t edges = original_pad_c(0, 0, 0);
				for (int i = 0; i < EDGE_RAW_SITE; i++)
					edge_pending[i] |= edges;
				// The Duel reads only the low byte of the raw mask; a queue full of backlog merges into its last entry.
				if ((edges & 0xFF) != 0 && !(ff8_interp::cfg().battle_debug & DBG_DUEL_EDGE_OFF))
				{
					if (edge_queue_count < EDGE_QUEUE_DEPTH) edge_queue[edge_queue_count++] = edges;
					else edge_queue[EDGE_QUEUE_DEPTH - 1] |= edges;
				}
			}
		}
		uint32_t __cdecl pad_a_hook(int a, int b, int c)
		{
			if (pad_bypass()) return original_pad_a(a, b, c);
			update_tick();
			return pad_step(0, original_pad_a(a, b, c));
		}
		uint32_t __cdecl pad_b_hook(int a, int b, int c)
		{
			if (pad_bypass()) return original_pad_b(a, b, c);
			update_tick();
			return pad_step(1, original_pad_b(a, b, c));
		}
		uint32_t __cdecl pad_c_hook(int a, int b, int c)
		{
			if (pad_bypass()) return original_pad_c(a, b, c);
			update_tick();
			return pad_step(2, original_pad_c(a, b, c));
		}
		// Replaces the four 0x49EC90 calls (cdecl; the caller cleans all four at once with add esp,50h, so
		// the hook must not touch the stack). On a non-step pass a press edge or a changed button set still
		// reaches the engine so it sets or restarts the counter; a held, unchanged set returns 0 with the
		// byte untouched - exactly what the original returns on a non-expired step. DBG_PAD_REPEAT_OFF disables it.
		uint16_t __cdecl pad_repeat_hook(int big, int port, uint16_t previous_buttons, uint16_t current_buttons, int group)
		{
			if (pad_bypass() || (ff8_interp::cfg().battle_debug & DBG_PAD_REPEAT_OFF) || ring_step)
				return original_pad_repeat(big, port, previous_buttons, current_buttons, group);
			uint16_t group_mask = *(uint16_t*)(port + 0x10 + group * 2); // group button mask, as read by 0x49EC90
			if (((previous_buttons & current_buttons) & group_mask) == 0 || ((previous_buttons ^ current_buttons) & group_mask) != 0)
				return original_pad_repeat(big, port, previous_buttons, current_buttons, group);
			return 0;
		}

		// Battle command menu state machine, registered as the menu window's UPDATE callback (the draw
		// callback is separate), so gating it leaves the menu on screen and only slows its stepping.
		constexpr uint32_t ADDR_MENU_UPDATE = 0x4BB9E0;
		const uint8_t MENU_UPDATE_PROLOGUE[5] = { 0x83, 0xEC, 0x14, 0x53, 0x55 }; // sub esp,14h / push ebx / push ebp
		typedef int (__cdecl *menu_update_t)();
		menu_update_t original_menu_update = nullptr;
		int menu_last_result = 0;

		int __cdecl menu_update_hook()
		{
			if (original_menu_update == nullptr) return 0;
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle) return original_menu_update();
			update_tick();
			if (is_logic_tick) menu_last_result = original_menu_update();
			return menu_last_result;
		}

		// v76: 0x4842B0 also publishes "the ATB ran this frame" in the byte 0x1D28DEB (cleared at
		// 0x4842BD unless 0x1D280C3 is set, set at 0x4842EE when the gauges advanced). The battle state
		// tick 0x47D490 case 4 tests it at 0x47D7CD - on EVERY host frame, it is not gated - and then runs
		// computeTimerStatus 0x483470 (site 0x47D7F1: the 14 per-status timers at 0x1D27B64 + slot*0xD0,
		// i.e. the Doom / gradual-petrify numbers 0x50A410 draws, the Poison / Regen trigger and every
		// timed status expiry) and 0x482F80 (site 0x47D7F6: Angelo cooldown 0x1D28DE4 and the random
		// Angelo / Gilgamesh / Odin rolls). Skipping 0x4842B0 on held frames left the tick frame's 1 in
		// place, so both ran ~9.6x per logic tick. Held frames now do the clear the skipped call would
		// have done. HUD passes 1-3 (0x47D0A2..B6) run before the director (0x47D113), so on a held frame
		// 0x47D7CD sees 0 exactly as it saw the value of that frame's own ATB run in vanilla.
		uint8_t* const ATB_RAN = (uint8_t*)0x1D28DEB;
		uint8_t* const ATB_HALT = (uint8_t*)0x1D280C3;

		void __cdecl atb_tick_hook()
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle)
			{
				original_atb_tick();
				return;
			}
			update_tick();
			if (is_logic_tick)
			{
				original_atb_tick();
				return;
			}
			if (!(ff8_interp::cfg().battle_debug & DBG_STATUS_TIMER_OFF) && *ATB_HALT == 0) // what 0x4842BD does
				*ATB_RAN = 0;
		}

		// v76: Battle_RunFrameCallbacks 0x482D50 (site 0x47D7FB in 0x47D490 case 4, every host frame)
		// walks the callback list 0x1D28C44 (16-byte entries, registered through 0x482C90). Its members
		// are logic only, nothing draws: 0x48ACD0 (AI opcodes 0x1B/0x1F "wait N frames": counts +8 down
		// while holding the ATB with 0x1D280C2 = 0 / 0x1D28DFD = 1 - scripted boss pauses were ~10x
		// short), 0x48AC90 (AI message after a delay, +8 countdown), 0x48AC60 (one-shot message),
		// 0x48E620 (GF-charge poll), 0x47E030 (end-fade trigger, already tick-gated by
		// fade_trigger_hook). Pure logic, so the call itself runs on logic ticks only. The reaper
		// 0x482DC0 at 0x47D800 stays at host rate (idempotent).
		constexpr uint32_t CALL_SITE_FRAME_CALLBACKS = 0x47D7FB;
		constexpr uint32_t FN_FRAME_CALLBACKS = 0x482D50;
		typedef void (__cdecl *frame_callbacks_t)();
		frame_callbacks_t original_frame_callbacks = (frame_callbacks_t)FN_FRAME_CALLBACKS;

		void __cdecl frame_callbacks_hook()
		{
			if (ff8_interp::game_mode() != ff8_interp::GameMode::Battle || (ff8_interp::cfg().battle_debug & DBG_FRAME_CALLBACK_OFF))
			{
				original_frame_callbacks();
				return;
			}
			update_tick();
			if (is_logic_tick) original_frame_callbacks();
		}

		// Play clock 0x4701B0 (save play time 0x1CFE928 + event countdown 0x1CFE92C, one 1/60 s step per
		// call). The battle main loop 0x47CF60 calls it four times per frame, unconditionally, before
		// the HUD passes: 4 x 15 = 60 steps/s natively, 576/s at 144 Hz (9.6x). v71 hands every call to
		// the shared 60 Hz wall-clock budget (play_clock.h); DBG_PLAY_CLOCK_OFF restores host rate.
		constexpr uint32_t CALL_SITE_PLAY_CLOCK[4] = { 0x47D076, 0x47D07B, 0x47D080, 0x47D085 };
		void __cdecl play_clock_hook()
		{
			ff8_play_clock::run(!(ff8_interp::cfg().battle_debug & DBG_PLAY_CLOCK_OFF));
		}

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
		impact_fx_t original_impact_fx = nullptr;

		int __cdecl impact_fx_hook(uint8_t* node)
		{
			if (original_impact_fx == nullptr) return 0;
			if (node == nullptr || ff8_interp::game_mode() != ff8_interp::GameMode::Battle || (ff8_interp::cfg().battle_debug & DBG_IMPACT_FX_OFF))
				return original_impact_fx(node);
			update_tick();
			if (is_logic_tick) return original_impact_fx(node);
			uint8_t saved_state[IMPACT_FX_STATE_LEN];
			memcpy(saved_state, node + IMPACT_FX_STATE, sizeof(saved_state));
			uint32_t saved_freeze = *BATTLE_FREEZE;
			*BATTLE_FREEZE = saved_freeze | 1;
			int result = original_impact_fx(node); // draws a fresh work block, returns without stepping the effect
			*BATTLE_FREEZE = saved_freeze;
			memcpy(node + IMPACT_FX_STATE, saved_state, sizeof(saved_state)); // undo 0x571BC0's in-place integration
			return result;
		}
	}

	// --- shared with the sibling translation units of this module, see internal.h ---

	// Declared extern in internal.h, so this definition has external linkage despite being const.
	uint32_t* const BATTLE_FREEZE = (uint32_t*)0x1D96A9C;

	// Thin forwarders onto the anonymous namespace's tick state. The accessor is called tick_update()
	// and not update_tick() so it cannot become ambiguous with the internal update_tick() above.
	void tick_update() { update_tick(); }
	bool is_logic_tick_now() { return is_logic_tick; }
	float tick_alpha_now() { return tick_alpha; }
	float tick_alpha_lead_now() { return tick_alpha_lead; }
	uint32_t logic_tick_count() { return logic_ticks; }

	bool in_battle() { return ff8_interp::game_mode() == ff8_interp::GameMode::Battle; }

	void* make_trampoline(uint32_t target, const uint8_t* prologue, size_t prologue_length)
	{
		if (memcmp((const void*)target, prologue, prologue_length) != 0) return nullptr;
		uint8_t* trampoline = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (trampoline == nullptr) return nullptr;
		memcpy(trampoline, prologue, prologue_length);
		trampoline[prologue_length] = 0xE9; // jmp rel32 back to the rest of the original function
		*(uint32_t*)(trampoline + prologue_length + 1) = (target + (uint32_t)prologue_length) - ((uint32_t)trampoline + (uint32_t)prologue_length + 5);
		return (void*)trampoline;
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
		case 30:
			battle_fps = 30;
			break;
		case 60:
			battle_fps = 60;
			break;
		default:
			ff8_interp::log_info("%s: ff8_fps_limiter is not 30/60 fps and ff8_battle_fps is 0, battle animation interpolation disabled.\n", __func__);
			return;
		}

		original_read_animation = (read_animation_t)make_trampoline(ADDR_READ_ANIMATION, READ_ANIMATION_PROLOGUE, sizeof(READ_ANIMATION_PROLOGUE));
		original_sequence_driver = (sequence_driver_t)make_trampoline(ADDR_SEQUENCE_DRIVER, SEQUENCE_DRIVER_PROLOGUE, sizeof(SEQUENCE_DRIVER_PROLOGUE));
		if (original_read_animation == nullptr || original_sequence_driver == nullptr)
		{
			ff8_interp::log_warning("%s: engine prologue mismatch (read=%p seq=%p), battle animation interpolation disabled.\n", __func__, original_read_animation, original_sequence_driver);
			return;
		}

		ff8_interp::hook_function(ADDR_READ_ANIMATION, (void*)read_animation_hook);
		ff8_interp::hook_function(ADDR_SEQUENCE_DRIVER, (void*)seq_driver_hook);
		hook_installed = true;

		// Spell / GF effects and the ATB run from their own per-frame drivers.
		ff8_interp::hook_call(CALL_SITE_EFFECT_TICK, (void*)effect_tick_hook);
		ff8_interp::hook_call(CALL_SITE_ENTITY_ROTATION_MATRIX, (void*)entity_rotmat_hook); // v89 (v88: 0x502BD5)
		ff8_interp::hook_call(CALL_SITE_XATM_MODEL_DRAW, (void*)xatm_model_draw_hook); // v105
		ff8_interp::hook_call(CALL_SITE_RALDO_DRAW, (void*)raldo_draw_hook);           // v114
		ff8_interp::hook_call(CALL_SITE_ATB_TICK, (void*)atb_tick_hook);
		{
			// v78: the two per-frame allocators the replay must never link in place (prim_is_volatile).
			// 0x401000: mov eax,[esp+4] / lea eax,[eax*4+0x1A77238] / ret - what 0x5082F0 resets the stack to.
			static const uint8_t STACK_BASE_FN[12] = { 0x8B, 0x44, 0x24, 0x04, 0x8D, 0x04, 0x85, 0x38, 0x72, 0xA7, 0x01, 0xC3 };
			// 0x5082F0: push 0 / call 0x401000 / add esp,4 / mov [0x1D999C4],eax.
			static const uint8_t STACK_RESET_FN[8] = { 0x6A, 0x00, 0xE8, 0x09, 0x8D, 0xEF, 0xFF, 0x83 };
			if (memcmp((const void*)0x401000, STACK_BASE_FN, sizeof(STACK_BASE_FN)) == 0
				&& memcmp((const void*)0x5082F0, STACK_RESET_FN, sizeof(STACK_RESET_FN)) == 0)
				prim_stack_base = PRIM_STACK_BASE;
			else
				ff8_interp::log_warning("%s: 0x401000 / 0x5082F0 bytes differ, the transient stack base is calibrated at runtime instead of 0x1A77238.\n", __func__);
			// 0x5007B7: mov edx,[0xB6D094] ... 0x5007D0: add ecx,0x24000 ... 0x5007D8: mov edx,[0xB6D070] ... 0x5007F7: add ecx,0x1C000.
			static const uint8_t PACKET_B_BASE_LOAD[6] = { 0x8B, 0x15, 0x94, 0xD0, 0xB6, 0x00 };
			static const uint8_t PACKET_B_STRIDE_ADD[6] = { 0x81, 0xC1, 0x00, 0x40, 0x02, 0x00 };
			static const uint8_t PACKET_A_BASE_LOAD[6] = { 0x8B, 0x15, 0x70, 0xD0, 0xB6, 0x00 };
			static const uint8_t PACKET_A_STRIDE_ADD[6] = { 0x81, 0xC1, 0x00, 0xC0, 0x01, 0x00 };
			packet_area_verified = memcmp((const void*)0x5007B7, PACKET_B_BASE_LOAD, 6) == 0 && memcmp((const void*)0x5007D0, PACKET_B_STRIDE_ADD, 6) == 0
				&& memcmp((const void*)0x5007D8, PACKET_A_BASE_LOAD, 6) == 0 && memcmp((const void*)0x5007F7, PACKET_A_STRIDE_ADD, 6) == 0;
			if (!packet_area_verified)
				ff8_interp::log_warning("%s: packet-area setup 0x5007B7..0x5007FD differs; spell trees drawing into the global packet area are not protected.\n", __func__);
		}
		for (int i = 0; i < FX_LINKER_COUNT; i++)
		{
			original_fx_linker[i] = (fx_linker_t)make_trampoline(FX_LINKER_ADDRESSES[i], FX_LINKER_PROLOGUE[i], FX_LINKER_PROLOGUE_LENGTH[i]);
			if (original_fx_linker[i] != nullptr) ff8_interp::hook_function(FX_LINKER_ADDRESSES[i], FX_LINKER_HOOK[i]);
			else ff8_interp::log_warning("%s: display list linker %08X prologue mismatch, effects using it cannot be replayed.\n", __func__, FX_LINKER_ADDRESSES[i]);
		}
		for (int i = 0; i < 4; i++) ff8_interp::hook_call(CALL_SITE_UI_CLOCK[i], (void*)ui_clock_hook);
		ff8_interp::hook_call(CALL_SITE_APPEAR_TICK, (void*)appear_tick_hook);
		ff8_interp::hook_call(CALL_SITE_MANAGER_QUEUE, (void*)manager_queue_hook);
		original_stage_texture_animation = (stage_texture_animation_t)make_trampoline(ADDR_STAGE_TEXTURE_ANIMATION, STAGE_TEXTURE_ANIMATION_PROLOGUE, sizeof(STAGE_TEXTURE_ANIMATION_PROLOGUE));
		if (original_stage_texture_animation != nullptr) ff8_interp::hook_function(ADDR_STAGE_TEXTURE_ANIMATION, (void*)stage_texanim_hook);
		else ff8_interp::log_warning("%s: stage texture animation prologue mismatch, it stays at host rate.\n", __func__);
		ff8_interp::hook_call(CALL_SITE_VICTORY_LOGIC, (void*)victory_logic_hook);
		original_battle_state = (battle_state_t)make_trampoline(ADDR_BATTLE_STATE, BATTLE_STATE_PROLOGUE, sizeof(BATTLE_STATE_PROLOGUE));
		original_fade_trigger = (fade_trigger_t)make_trampoline(ADDR_FADE_TRIGGER, FADE_TRIGGER_PROLOGUE, sizeof(FADE_TRIGGER_PROLOGUE));
		original_fade_tick = (fade_tick_t)make_trampoline(ADDR_FADE_TICK, FADE_TICK_PROLOGUE, sizeof(FADE_TICK_PROLOGUE));
		if (original_battle_state != nullptr && original_fade_trigger != nullptr && original_fade_tick != nullptr)
		{
			ff8_interp::hook_function(ADDR_BATTLE_STATE, (void*)battle_state_hook);
			ff8_interp::hook_function(ADDR_FADE_TRIGGER, (void*)fade_trigger_hook);
			ff8_interp::hook_function(ADDR_FADE_TICK, (void*)fade_tick_hook);
		}
		else ff8_interp::log_warning("%s: end-of-battle prologue mismatch (%p/%p/%p), the victory sequence stays at host rate.\n", __func__, original_battle_state, original_fade_trigger, original_fade_tick);
		original_caption_tick = (caption_tick_t)make_trampoline(ADDR_CAPTION_TICK, CAPTION_PROLOGUE, sizeof(CAPTION_PROLOGUE));
		if (original_caption_tick != nullptr) ff8_interp::hook_function(ADDR_CAPTION_TICK, (void*)caption_tick_hook);
		else ff8_interp::log_warning("%s: caption prologue mismatch, battle text stays at host rate.\n", __func__);
		original_ui_update = (ui_update_t)make_trampoline(ADDR_UI_UPDATE, UI_UPDATE_PROLOGUE, sizeof(UI_UPDATE_PROLOGUE));
		if (original_ui_update != nullptr) ff8_interp::hook_function(ADDR_UI_UPDATE, (void*)ui_update_hook);
		else ff8_interp::log_warning("%s: window update prologue mismatch, battle text stays at host rate.\n", __func__);
		original_gf_sequence_tick = (gf_sequence_tick_t)make_trampoline(ADDR_GF_SEQUENCE_TICK, GF_SEQUENCE_PROLOGUE, sizeof(GF_SEQUENCE_PROLOGUE));
		if (original_gf_sequence_tick != nullptr) ff8_interp::hook_function(ADDR_GF_SEQUENCE_TICK, (void*)gf_seq_tick_hook);
		else ff8_interp::log_warning("%s: summon sequence prologue mismatch, the summon entrance stays at host rate.\n", __func__);
		original_apply_results = (apply_results_t)make_trampoline(ADDR_APPLY_RESULTS, APPLY_RESULTS_PROLOGUE, sizeof(APPLY_RESULTS_PROLOGUE));
		if (original_apply_results != nullptr) ff8_interp::hook_function(ADDR_APPLY_RESULTS, (void*)apply_results_hook);
		else ff8_interp::log_warning("%s: action result prologue mismatch, a boosted summon can still land before its gauge.\n", __func__);
		original_damage_tick = (damage_tick_t)make_trampoline(ADDR_DAMAGE_TICK, DAMAGE_TICK_PROLOGUE, sizeof(DAMAGE_TICK_PROLOGUE));
		if (original_damage_tick != nullptr) ff8_interp::hook_function(ADDR_DAMAGE_TICK, (void*)damage_tick_hook);
		else ff8_interp::log_warning("%s: damage popup prologue mismatch, popups stay at host rate.\n", __func__);
		original_impact_fx = (impact_fx_t)make_trampoline(ADDR_IMPACT_FX, IMPACT_FX_PROLOGUE, sizeof(IMPACT_FX_PROLOGUE));
		if (original_impact_fx != nullptr) ff8_interp::hook_function(ADDR_IMPACT_FX, (void*)impact_fx_hook);
		else ff8_interp::log_warning("%s: impact effect prologue mismatch, hit explosions stay at host rate.\n", __func__);
		original_magic_effect_tick = (magic_tick_t)make_trampoline(ADDR_MAGIC_EFFECT_TICK, MAGIC_EFFECT_PROLOGUE, sizeof(MAGIC_EFFECT_PROLOGUE));
		original_magic_cast_tick = (magic_tick_t)make_trampoline(ADDR_MAGIC_CAST_TICK, MAGIC_CAST_PROLOGUE, sizeof(MAGIC_CAST_PROLOGUE));
		if (original_magic_effect_tick != nullptr) ff8_interp::hook_function(ADDR_MAGIC_EFFECT_TICK, (void*)magic_effect_tick_hook);
		if (original_magic_cast_tick != nullptr) ff8_interp::hook_function(ADDR_MAGIC_CAST_TICK, (void*)magic_cast_tick_hook);
		if (original_magic_effect_tick == nullptr || original_magic_cast_tick == nullptr)
			ff8_interp::log_warning("%s: magic handler prologue mismatch, spell effects cannot be held.\n", __func__);

		ff8_interp::hook_call(CALL_SITE_BATTLE_CAMERA, (void*)battle_camera_hook);
		ff8_interp::hook_call(CALL_SITE_PAD_REFRESH, (void*)pad_refresh_hook);
		ff8_interp::hook_call(CALL_SITE_PAD_A, (void*)pad_a_hook);
		ff8_interp::hook_call(CALL_SITE_PAD_B, (void*)pad_b_hook);
		ff8_interp::hook_call(CALL_SITE_PAD_C, (void*)pad_c_hook);
		for (int i = 0; i < EDGE_SITES; i++) ff8_interp::hook_call(CALL_SITE_EDGE_READ[i], EDGE_READ_HOOK[i]);
		for (int i = 0; i < 4; i++) ff8_interp::hook_call(CALL_SITE_PAD_REPEAT[i], (void*)pad_repeat_hook);
		ff8_interp::hook_call(CALL_SITE_HUD_ESCAPE, (void*)hud_escape_hook);         // v76
		ff8_interp::hook_call(CALL_SITE_FRAME_CALLBACKS, (void*)frame_callbacks_hook); // v76
		ff8_interp::hook_call(CALL_SITE_CURSOR_DRAW, (void*)cursor_draw_hook);       // v77
		ff8_interp::hook_call(CALL_SITE_HUD_ANALOG_Y, (void*)hud_analog_y_hook);     // v76
		ff8_interp::hook_call(CALL_SITE_HUD_ANALOG_ACC, (void*)hud_analog_acc_hook); // v76
		int clock_sites = 0;
		for (int i = 0; i < 4; i++)
			clock_sites += ff8_play_clock::patch_site(CALL_SITE_PLAY_CLOCK[i], (void*)play_clock_hook, __func__) ? 1 : 0;
		original_menu_update = (menu_update_t)make_trampoline(ADDR_MENU_UPDATE, MENU_UPDATE_PROLOGUE, sizeof(MENU_UPDATE_PROLOGUE));
		if (original_menu_update != nullptr) ff8_interp::hook_function(ADDR_MENU_UPDATE, (void*)menu_update_hook);
		else ff8_interp::log_warning("%s: menu prologue mismatch, the battle menu stays at host rate.\n", __func__);
		{
			const uint8_t* code_bytes = (const uint8_t*)ADDR_READ_ANIMATION;
			ff8_interp::log_info("%s: bytes@508F90 after patch: %02x %02x %02x %02x %02x (hook=%p tramp=%p)\n", __func__, code_bytes[0], code_bytes[1], code_bytes[2], code_bytes[3], code_bytes[4], read_animation_hook, original_read_animation);
		}
		ff8_interp::log_info("%s: battle animation interpolation v118 enabled (battle %d fps, logic %d fps, play clock %d/4 sites).\n", __func__, battle_fps, LOGIC_FPS, clock_sites);

		// Sibling translation units of this module (internal.h).
		gf_pacing_hook_init();
		gf_vm_hook_init(); // after gf_pacing: it refuses a tick someone else already hooked
		gf_siren_hook_init();
		hud_blend_hook_init();
		fx_queues_hook_init();
		fx_blend_hook_init();
		battle_misc_hook_init();
	}

	bool animations_installed() { return hook_installed; }

	void animations_reset()
	{
		anim_states.clear();
		intro_held_frames = 0;
		entity_states.clear();
		paused_states.clear();
		grip_states.clear();
		dump_states.clear();
		pad_cache[0] = pad_cache[1] = pad_cache[2] = 0;
		pad_accumulated[0] = pad_accumulated[1] = pad_accumulated[2] = 0;
		pad_publish_frame[0] = pad_publish_frame[1] = pad_publish_frame[2] = 0xFFFFFFFF;
		ring_budget = 0.0;
		ring_frame = 0xFFFFFFFF;
		for (int i = 0; i < EDGE_SITES; i++)
		{
			edge_pending[i] = 0;
			edge_frame[i] = 0xFFFFFFFF;
			edge_tick[i] = 0;
		}
		edge_queue_count = 0;
		ring_step = false;
		analog_repeat_bits = analog_press_bits = 0;
		analog_publish_frame = 0xFFFFFFFF;
		cursor_snapshot_valid = false;
		cursor_replays = 0;
		cursor_last_frame = 0xFFFFFFFF;
		menu_last_result = 0;
		summon_live_frame = 0xFFFFFFFF;
		holdable_ctx = nullptr;
		last_effect_ctx = nullptr;
		registered_root = nullptr;
		fx_prims.clear();
		fx_prims_previous.clear();
		fx_prims_two_ticks_ago.clear();
		camera_pose_valid = false;
		camera_published_valid = false;
		blink_valid = false;
		blink_pending = false;
		blink_calls = blink_last_step = 0;
		logic_ticks = 0;
		end_hold_seconds = 0.0;
		victory_accumulator = 0.0;
		prim_stack_low = 0xFFFFFFFF;
		prim_stack_high = 0;
		fx_unsafe_calls = 0;
		fx_unsafe_site = 0;
		fx_prev_recorded = false;
		fx_unpaced_ctx = nullptr;
		fx_stale_logged_ctx = nullptr;
		fx_unsafe_logged_ctx = nullptr;
		fx_record_target = nullptr;
		fx_defer_links = false;
		fx_flushes = 0;
		held_effect_ctx = nullptr;
		held_frames = 0;
		tick_frame = 0xFFFFFFFF;
		tick_accumulator = 0.0;
		ff8_tick::clock_reset();

		// Sibling translation units of this module (internal.h).
		gf_pacing_reset();
		gf_vm_reset();
		gf_siren_reset();
		hud_blend_reset();
		fx_queues_reset();
		fx_blend_reset();
		battle_misc_reset();
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
		const uint8_t* code_bytes = (const uint8_t*)ADDR_READ_ANIMATION;
		ff8_interp::log_trace("ff8anim heartbeat f=%u mode=%d calls=%u bytes@508F90=%02x %02x %02x %02x %02x\n",
			ff8_interp::frame_counter(), ff8_interp::mode_id(), hook_calls, code_bytes[0], code_bytes[1], code_bytes[2], code_bytes[3], code_bytes[4]);
	}
}
