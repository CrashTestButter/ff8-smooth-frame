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

#include "chara_pose.h"

#include <algorithm>

#include "../host.h"

#include <string.h>
#include <cmath>

namespace ff8_chara
{
	namespace
	{
		constexpr int CHARA_SLOTS = PoseInterp::SLOTS;
		constexpr uint32_t INST_ANIM = 0x08;       // animation data pointer
		constexpr uint32_t INST_FRAMES = 0x0C;     // frame count, counter units (frames << 4)
		constexpr uint32_t INST_BONE_COUNT = 0x0E;
		constexpr uint32_t INST_COUNTER = 0x52;    // frame << 4 (low 4 bits sub-frame, ignored by the decoder)
		constexpr uint32_t INST_HIDDEN = 0x60;
		constexpr uint32_t INST_FLAGS = 0x72;      // bit2 = raw 8-byte poses
		constexpr int INST_FLAG_RAW_POSES = 4;
		constexpr uint32_t INST_BONES = 0x04;      // bone records, 0x40 each, record 0 = root
		constexpr uint32_t BONE_FLAGS = 0x34;      // byte, bit0 = the decoder skips this bone
		constexpr uint32_t INST_HELD_BONE[3] = { 0x70, 0x71, 0x73 }; // bone indices a script holds...
		constexpr uint8_t INST_HELD_BIT[3] = { 0x02, 0x04, 0x20 };   // ...while this bit of +0x60 is set

		__forceinline uint8_t*& instance_anim(uint8_t* instance) { return *(uint8_t**)(instance + INST_ANIM); }
		__forceinline uint16_t& instance_counter(uint8_t* instance) { return *(uint16_t*)(instance + INST_COUNTER); }
		__forceinline uint16_t& instance_bone_count(uint8_t* instance) { return *(uint16_t*)(instance + INST_BONE_COUNT); }


		// field v13: the decoders (0x533CD0 field, 0x653DA0 world) skip a bone whose record has bit0 at
		// +0x34 or that a script holds (+0x70/+0x71/+0x73 with +0x60 bits 2/4/0x20) and step their read
		// pointer by 4 for it - also in raw mode, where a bone is 8 bytes. A raw feed with a skipped bone
		// before the end therefore misread every later bone (half of one, half of the next): Squall's and
		// Zell's hands and feet shot up on the blended frames while they talked in the train to Timber.
		inline bool bone_skipped(const uint8_t* instance, int bone)
		{
			const uint8_t* bone_record = *(uint8_t* const*)(instance + INST_BONES) + 0x40 * (bone + 1);
			if (bone_record[BONE_FLAGS] & 1) return true;
			for (int k = 0; k < 3; k++)
				if (bone == instance[INST_HELD_BONE[k]] && (instance[INST_HIDDEN] & INST_HELD_BIT[k])) return true;
			return false;
		}

		inline int16_t lerp_angle(int16_t a, int16_t b, float t)
		{
			int delta = ((int(b) - int(a) + 2048) & 4095) - 2048;
			float scaled = delta * t;
			return (int16_t)(int(a) + (int)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f)));
		}
		inline int16_t lerp_i16(int16_t a, int16_t b, float t)
		{
			float scaled = (int(b) - int(a)) * t;
			return (int16_t)(int(a) + (int)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f)));
		}
	// 0x653DA0 decodes one packed pose per instance per call (frame = counter >> 4; 4 bytes per
	// bone: x = ((b3 & 3) << 8 | b0) << 2, y = ((b3 & 0xC) << 6 | b1) << 2, z = ((b3 & 0x30) << 4 | b2) << 2;
	// frame stride 4 + 4 * bones; root = two dwords at the frame start, GETA added to the second),
	// then builds the skeleton and submits the model. It is called once per frame, so we never call
	// it more than once: lookahead frames are decoded here from the animation data, and every host
	// frame the blend is fed through the engine's raw-pose mode (flag bit2: 8 bytes per bone
	// memcpy) with a scratch buffer as "frame 0".

	__forceinline bool instance_allocated(const uint8_t* instance)
	{
		return (uintptr_t)instance >= 0x10000 && (uintptr_t)instance < 0x7FFFFFFF && *(int32_t*)instance != -1;
	}
	inline bool instance_active(uint8_t* instance)
	{
		return instance_allocated(instance) && (instance[INST_HIDDEN] & 1) == 0
			&& *(uint8_t**)(instance + INST_ANIM) != nullptr && *(uint16_t*)(instance + INST_BONE_COUNT) > 0
			&& (instance[INST_FLAGS] & INST_FLAG_RAW_POSES) == 0;
	}
	// Mirror of the engine's packed decode (non-raw mode) into the raw 8-byte-per-bone layout.
	void decode_pose(const uint8_t* animation, int frame, int bone_count, std::vector<uint8_t>& out)
	{
		const uint8_t* frame_data = animation + frame * (4 + 4 * bone_count);
		out.resize(8 + 8 * bone_count);
		memcpy(&out[0], frame_data, 8); // root dwords (GETA is added by the engine)
		const uint8_t* packed_bone = frame_data + 8;
		for (int i = 0; i < bone_count; i++, packed_bone += 4)
		{
			uint32_t xy_word = (uint32_t(((packed_bone[3] & 0xC) << 6) | packed_bone[1]) << 18) | (uint32_t(((packed_bone[3] & 3) << 8) | packed_bone[0]) << 2);
			uint32_t z_word = uint32_t(((packed_bone[3] & 0x30) << 4) | packed_bone[2]) << 2;
			memcpy(&out[8 + 8 * i], &xy_word, 4);
			memcpy(&out[12 + 8 * i], &z_word, 4);
		}
	}
	// Bone rotations. The engine builds each bone's local matrix as R = Rx(x) * Ry(y) * Rz(z)
	// (0x654060: standard right-handed elementary rotations, 12-bit angles, 4096 = full turn,
	// row-major product 0x56C090 out = a * b). Per-axis angle blending swings limbs through wrong
	// intermediate rotations when two poses are far apart (clip switch, loop seam), so poses are
	// blended as rotations: angles -> matrix -> quaternion, slerp, back to angles.
	struct Quaternion { float w, x, y, z; };
	constexpr float RADIANS_PER_ANGLE_UNIT = 6.2831853f / 4096.0f;
	inline Quaternion euler_to_quaternion(int angle_x, int angle_y, int angle_z)
	{
		float cos_x = cosf(angle_x * RADIANS_PER_ANGLE_UNIT);
		float sin_x = sinf(angle_x * RADIANS_PER_ANGLE_UNIT);
		float cos_y = cosf(angle_y * RADIANS_PER_ANGLE_UNIT);
		float sin_y = sinf(angle_y * RADIANS_PER_ANGLE_UNIT);
		float cos_z = cosf(angle_z * RADIANS_PER_ANGLE_UNIT);
		float sin_z = sinf(angle_z * RADIANS_PER_ANGLE_UNIT);
		float m00 = cos_y * cos_z, m01 = -cos_y * sin_z, m02 = sin_y;
		float m10 = cos_x * sin_z + sin_x * sin_y * cos_z, m11 = cos_x * cos_z - sin_x * sin_y * sin_z, m12 = -sin_x * cos_y;
		float m20 = sin_x * sin_z - cos_x * sin_y * cos_z, m21 = sin_x * cos_z + cos_x * sin_y * sin_z, m22 = cos_x * cos_y;
		Quaternion q;
		float trace = m00 + m11 + m22;
		if (trace > 0.0f)
		{
			float root = sqrtf(1.0f + trace);
			float scale = 0.5f / root;
			q.w = 0.5f * root;
			q.x = (m21 - m12) * scale;
			q.y = (m02 - m20) * scale;
			q.z = (m10 - m01) * scale;
		}
		else if (m00 >= m11 && m00 >= m22)
		{
			float root = sqrtf(1.0f + m00 - m11 - m22);
			float scale = 0.5f / root;
			q.x = 0.5f * root;
			q.w = (m21 - m12) * scale;
			q.y = (m01 + m10) * scale;
			q.z = (m02 + m20) * scale;
		}
		else if (m11 >= m22)
		{
			float root = sqrtf(1.0f - m00 + m11 - m22);
			float scale = 0.5f / root;
			q.y = 0.5f * root;
			q.w = (m02 - m20) * scale;
			q.x = (m01 + m10) * scale;
			q.z = (m12 + m21) * scale;
		}
		else
		{
			float root = sqrtf(1.0f - m00 - m11 + m22);
			float scale = 0.5f / root;
			q.z = 0.5f * root;
			q.w = (m10 - m01) * scale;
			q.x = (m02 + m20) * scale;
			q.y = (m12 + m21) * scale;
		}
		return q;
	}
	inline int16_t radians_to_angle(float radians)
	{
		int angle = (int)floorf(radians / RADIANS_PER_ANGLE_UNIT + 0.5f);
		return (int16_t)(angle & 4095);
	}
	inline void quaternion_to_euler(const Quaternion& q, int16_t* out)
	{
		float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
		float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
		float m00 = 1.0f - 2.0f * (yy + zz), m01 = 2.0f * (xy - wz), m02 = 2.0f * (xz + wy);
		float m11 = 1.0f - 2.0f * (xx + zz), m12 = 2.0f * (yz - wx);
		float m21 = 2.0f * (yz + wx), m22 = 1.0f - 2.0f * (xx + yy);
		if (m02 > 1.0f) m02 = 1.0f;
		if (m02 < -1.0f) m02 = -1.0f;
		float radians_y = asinf(m02);
		float radians_x;
		float radians_z;
		if (fabsf(m02) < 0.9999f)
		{
			radians_x = atan2f(-m12, m22);
			radians_z = atan2f(-m01, m00);
		}
		else // gimbal lock: fold z into x
		{
			radians_x = atan2f(m21, m11);
			radians_z = 0.0f;
		}
		out[0] = radians_to_angle(radians_x);
		out[1] = radians_to_angle(radians_y);
		out[2] = radians_to_angle(radians_z);
	}
	inline Quaternion slerp(Quaternion a, Quaternion b, float t)
	{
		float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
		if (dot < 0.0f)
		{
			b.w = -b.w;
			b.x = -b.x;
			b.y = -b.y;
			b.z = -b.z;
			dot = -dot;
		}
		float weight_a, weight_b;
		if (dot > 0.9995f)
		{
			weight_a = 1.0f - t;
			weight_b = t;
		}
		else
		{
			float theta = acosf(dot);
			float sin_theta = sinf(theta);
			weight_a = sinf((1.0f - t) * theta) / sin_theta;
			weight_b = sinf(t * theta) / sin_theta;
		}
		Quaternion q = { weight_a * a.w + weight_b * b.w, weight_a * a.x + weight_b * b.x, weight_a * a.y + weight_b * b.y, weight_a * a.z + weight_b * b.z };
		float length = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
		if (length > 0.0f)
		{
			q.w /= length;
			q.x /= length;
			q.y /= length;
			q.z /= length;
		}
		return q;
	}
	// Largest bone rotation between two raw poses, in degrees (root translation not counted).
	float pose_max_angle(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int* worst_bone = nullptr)
	{
		const int16_t* words_a = (const int16_t*)a.data();
		const int16_t* words_b = (const int16_t*)b.data();
		int word_count = (int)(a.size() / 2);
		float worst_degrees = 0.0f;
		for (int k = 4; k + 3 < word_count; k += 4)
		{
			if (words_a[k] == words_b[k] && words_a[k + 1] == words_b[k + 1] && words_a[k + 2] == words_b[k + 2]) continue;
			Quaternion rotation_a = euler_to_quaternion(words_a[k] & 4095, words_a[k + 1] & 4095, words_a[k + 2] & 4095);
			Quaternion rotation_b = euler_to_quaternion(words_b[k] & 4095, words_b[k + 1] & 4095, words_b[k + 2] & 4095);
			float abs_dot = fabsf(rotation_a.w * rotation_b.w + rotation_a.x * rotation_b.x + rotation_a.y * rotation_b.y + rotation_a.z * rotation_b.z);
			if (abs_dot > 1.0f) abs_dot = 1.0f;
			float degrees = 2.0f * acosf(abs_dot) * (180.0f / 3.14159265f);
			if (degrees > worst_degrees)
			{
				worst_degrees = degrees;
				if (worst_bone) *worst_bone = k / 4 - 1;
			}
		}
		return worst_degrees;
	}
	// Clip switches with a bone turning further than this are cut, as the game does, instead of
	// cross-faded: a one-tick fade through a near half-turn reads as an extra move (Zell's boxing
	// stance -> walk turned his upper body every time he started walking). Scan of all chara.one
	// stand/walk/run switches: Zell and Rinoa models flip one or two bones by 170-180 degrees, every
	// other model stays below ~146 (run poses), so those keep the fade.
	constexpr float SWITCH_FADE_MAX_DEG = 150.0f;
	// field v12: cos(90 deg / 2) - |dot| of two bone quaternions below this = a turn over 90 degrees.
	constexpr float BONE_POP_COS = 0.70710678f;
	// field v12 kill bit, honoured in ff8_field_interp_debug and ff8_world_interp_debug (shared module).
	constexpr long DBG_BONE_POP_OFF = 8192;
	constexpr long DBG_RAW_SKIP_OFF = 16384;   // field v13: raw feed without the skipped-bone layout (pre-v13)
	void blend_pose(const std::vector<uint8_t>& from, const std::vector<uint8_t>& to, float t, std::vector<uint8_t>& out)
	{
		out.resize(from.size());
		const int16_t* from_words = (const int16_t*)from.data();
		const int16_t* to_words = (const int16_t*)to.data();
		int16_t* out_words = (int16_t*)out.data();
		int word_count = (int)(from.size() / 2);
		for (int k = 0; k < 4 && k < word_count; k++)
			out_words[k] = lerp_i16(from_words[k], to_words[k], t); // root translation
		for (int k = 4; k + 3 < word_count; k += 4)
		{
			if (from_words[k] == to_words[k] && from_words[k + 1] == to_words[k + 1] && from_words[k + 2] == to_words[k + 2])
			{
				out_words[k] = from_words[k];
				out_words[k + 1] = from_words[k + 1];
				out_words[k + 2] = from_words[k + 2];
			}
			else
			{
				Quaternion from_rotation = euler_to_quaternion(from_words[k] & 4095, from_words[k + 1] & 4095, from_words[k + 2] & 4095);
				Quaternion to_rotation = euler_to_quaternion(to_words[k] & 4095, to_words[k + 1] & 4095, to_words[k + 2] & 4095);
				// field v12: a bone turning more than 90 degrees in one step is a pop in the data (vanilla
				// shows it as one): switch at mid-step instead of sweeping it - the sweep swung hands and feet
				// through the air (Squall / Zell talking in the train to Timber; battle v86 does the same).
				const float abs_dot = fabsf(from_rotation.w * to_rotation.w + from_rotation.x * to_rotation.x + from_rotation.y * to_rotation.y + from_rotation.z * to_rotation.z);
				if (abs_dot < BONE_POP_COS && !((ff8_interp::cfg().field_debug | ff8_interp::cfg().world_debug) & DBG_BONE_POP_OFF))
				{
					const int16_t* nearer = (t < 0.5f) ? from_words : to_words;
					out_words[k] = nearer[k];
					out_words[k + 1] = nearer[k + 1];
					out_words[k + 2] = nearer[k + 2];
				}
				else
					quaternion_to_euler(slerp(from_rotation, to_rotation, t), out_words + k);
			}
			out_words[k + 3] = lerp_i16(from_words[k + 3], to_words[k + 3], t); // pad word
		}
	}

	}

	void PoseInterp::reset()
	{
		for (auto& character : characters)
			character.valid = false;
		clip_loops.clear();
	}

	void PoseInterp::animate(animate_fn original, int arg1, int arg2, bool is_logic_tick, float alpha, uint32_t tick_number)
	{
			if (is_logic_tick)
			{
				for (int i = 0; i < CHARA_SLOTS; i++)
				{
					uint8_t* instance = instance_table[i];
					CharaPose& character = characters[i];
					if (!instance_active(instance))
					{
						// field v11 trace: a slot that has a model and an animation but is skipped (hidden or
						// engine-raw) - the engine shows it unblended if it is visible.
						if (ff8_interp::cfg().trace && (uintptr_t)instance >= 0x10000 && (uintptr_t)instance < 0x7FFFFFFF && *(int32_t*)instance != -1
							&& instance_anim(instance) != nullptr)
							ff8_interp::log_trace("ff8pose skip t=%u s=%d anim=%p ctr=%u hidden=%d flag4=%d bones=%u\n", tick_number, i, instance_anim(instance),
								instance_counter(instance), instance[INST_HIDDEN] & 1, (instance[INST_FLAGS] & INST_FLAG_RAW_POSES) ? 1 : 0, instance_bone_count(instance));
						character.valid = false;
						continue;
					}
					uint8_t* animation = instance_anim(instance);
					uint16_t counter = instance_counter(instance);
					uint16_t frame_count = *(uint16_t*)(instance + INST_FRAMES) >> 4;
					int bone_count = instance_bone_count(instance);
					uint16_t frame = counter >> 4;
					if (frame_count == 0 || frame >= frame_count)
					{
						character.valid = false;
						continue;
					}
					int counter_span = frame_count * 16;
					bool same_anim = character.valid && character.animation == animation;
					bool advanced = same_anim && counter != character.counter;
					if (same_anim && counter < character.counter) clip_loops[animation] = true;
					if (advanced)
					{
						int counter_step = (int(counter) - int(character.counter)) % counter_span;
						if (counter_step < 0) counter_step += counter_span;
						if (counter_step > 0 && counter_step <= 16 * MAX_FRAMES_AHEAD) character.counter_delta = counter_step;
					}
					else character.counter_delta = 16;
					std::swap(character.previous_tick_pose, character.poses[0]); // previous tick's content (hold detection)
					decode_pose(animation, frame, bone_count, character.poses[0]);
					character.next_distinct_pose = 0;
					character.pose_count = 1;
					character.counter = counter;
					character.frame = frame;
					if (!same_anim)
					{
						// Clip switch: cross-fade from the pose shown on the last host frame to the new
						// clip's first frame over this tick (vanilla pops).
						character.hold_ticks = 1;
						character.animation = animation;
						if (character.valid && character.shown.size() == character.poses[0].size() && character.shown != character.poses[0])
						{
							float degrees = pose_max_angle(character.shown, character.poses[0]);
							if (ff8_interp::cfg().trace)
								ff8_interp::log_trace("ff8pose switch t=%u s=%d anim=%p maxdeg=%.0f %s\n", tick_number, i, animation, degrees,
									degrees > SWITCH_FADE_MAX_DEG ? "cut" : "fade");
							if (degrees <= SWITCH_FADE_MAX_DEG)
							{
								character.poses[1] = character.poses[0];
								character.poses[0] = character.shown;
								character.next_distinct_pose = 1;
								character.pose_count = 2;
							}
						}
					}
					else if (!advanced)
					{
						// Counter unchanged (clip end hold or paused): the engine shows frame f, so do we.
						character.hold_ticks = 1;
					}
					else
					{
						// Clips may repeat a pose on consecutive frames (authored below 30 fps): count the
						// ticks the content has been unchanged and look ahead for the next distinct pose.
						character.hold_ticks = (character.previous_tick_pose == character.poses[0]) ? (character.hold_ticks < 64 ? character.hold_ticks + 1 : 64) : 1;
						int frames_ahead = (character.counter_delta + 15) / 16; // frames the blend must cover this tick
						if (frames_ahead < 1) frames_ahead = 1;
						if (frames_ahead > MAX_FRAMES_AHEAD) frames_ahead = MAX_FRAMES_AHEAD;
						bool clip_has_looped = clip_loops.count(animation) != 0;
						for (int j = 1; j <= MAX_FRAMES_AHEAD && frame_count > 1; j++)
						{
							if (!(j <= frames_ahead || character.next_distinct_pose == 0)) break;
							if (frame + j >= frame_count && !clip_has_looped) break; // do not blend past the end of a clip that never wrapped
							decode_pose(animation, (frame + j) % frame_count, bone_count, character.poses[j]);
							character.pose_count = j + 1;
							if (character.next_distinct_pose == 0 && character.poses[j] != character.poses[0]) character.next_distinct_pose = j;
						}
					}
					character.valid = true;
					if (ff8_interp::cfg().trace)
					{
						// field v11: the step this tick blends across - worst bone turn and root move.
						float step_degrees = 0.0f;
						int root_step = 0;
						int step_bone = -1;
						if (character.pose_count >= 2 && character.poses[1].size() == character.poses[0].size() && character.poses[0].size() >= 8)
						{
							step_degrees = pose_max_angle(character.poses[0], character.poses[1], &step_bone);
							const int16_t* root_now = (const int16_t*)character.poses[0].data();
							const int16_t* root_next = (const int16_t*)character.poses[1].data();
							for (int k = 0; k < 3; k++)
								root_step = (std::max)(root_step, abs(int(root_next[k]) - int(root_now[k])));
						}
						ff8_interp::log_trace("ff8pose t=%u s=%d anim=%p ctr=%u n=%u b=%d d=%d hold=%d k=%d np=%d step=%.0f bone=%d root=%d\n", tick_number, i, animation,
							counter, frame_count, bone_count, character.counter_delta, character.hold_ticks, character.next_distinct_pose, character.pose_count, step_degrees, step_bone, root_step);
					}
				}
			}
			// Display: feed blended raw poses through the engine (flag bit2, frame 0 of a scratch buffer).
			struct SavedInstance
			{
				uint8_t flags;
				uint8_t* animation;
				uint16_t counter;
				bool used;
			};
			SavedInstance saved[CHARA_SLOTS] = {};
			for (int i = 0; i < CHARA_SLOTS; i++)
			{
				uint8_t* instance = instance_table[i];
				CharaPose& character = characters[i];
				if (!character.valid || !instance_active(instance) || character.animation != instance_anim(instance)) continue;
				if (character.pose_count < 2 || character.next_distinct_pose == 0) // engine's own decode is shown
				{
					character.shown = character.poses[0];
					continue;
				}
				size_t pose_size = (size_t)(8 + 8 * instance_bone_count(instance));
				bool sizes_match = true;
				for (int j = 0; j < character.pose_count; j++)
					if (character.poses[j].size() != pose_size) sizes_match = false;
				if (!sizes_match) continue;
				float frames_per_tick = float(character.counter_delta) / 16.0f;  // frames per tick
				float sub_frame = float(character.counter & 15) / 16.0f; // sub-frame at the tick
				if (character.hold_ticks == 1 && character.next_distinct_pose == 1)
				{
					// distinct consecutive frames: piecewise through the decoded frames
					float frame_position = sub_frame + frames_per_tick * alpha;
					int segment = (int)frame_position;
					if (segment > character.pose_count - 2) segment = character.pose_count - 2;
					if (segment < 0) segment = 0;
					float segment_t = frame_position - float(segment);
					if (segment_t > 1.0f) segment_t = 1.0f;
					blend_pose(character.poses[segment], character.poses[segment + 1], segment_t, character.scratch);
				}
				else
				{
					// repeated pose: spread the motion over the whole hold (previous ticks + frames ahead)
					float held_frames = float(character.hold_ticks - 1) * frames_per_tick;
					float phase = (held_frames + sub_frame + frames_per_tick * alpha) / (held_frames + float(character.next_distinct_pose));
					if (phase > 1.0f) phase = 1.0f;
					if (phase < 0.0f) phase = 0.0f;
					blend_pose(character.poses[0], character.poses[character.next_distinct_pose], phase, character.scratch);
				}
				character.shown = character.scratch;
				// Lay the pose out the way the decoder reads it: 8 root bytes, then 8 bytes per bone and 4 for
				// a bone it skips (field v13).
				const int bone_count = instance_bone_count(instance);
				const bool skip_layout = !((ff8_interp::cfg().field_debug | ff8_interp::cfg().world_debug) & DBG_RAW_SKIP_OFF);
				character.feed.resize(character.scratch.size());
				memcpy(character.feed.data(), character.scratch.data(), 8);
				size_t write_offset = 8;
				int skipped_bones = 0;
				for (int bone = 0; bone < bone_count; bone++)
				{
					if (skip_layout && bone_skipped(instance, bone))
					{
						memset(&character.feed[write_offset], 0, 4);
						write_offset += 4;
						skipped_bones++;
						continue;
					}
					memcpy(&character.feed[write_offset], &character.scratch[8 + 8 * bone], 8);
					write_offset += 8;
				}
				if (ff8_interp::cfg().trace && skipped_bones != character.fed_skipped_bones)
					ff8_interp::log_trace("ff8pose feed s=%d skipped bones %d -> %d\n", i, character.fed_skipped_bones, skipped_bones);
				character.fed_skipped_bones = skipped_bones;
				saved[i] = { instance[INST_FLAGS], instance_anim(instance), instance_counter(instance), true };
				instance[INST_FLAGS] |= INST_FLAG_RAW_POSES;
				instance_anim(instance) = character.feed.data();
				instance_counter(instance) = 0;
			}
			original(arg1, arg2);
			for (int i = 0; i < CHARA_SLOTS; i++)
			{
				if (!saved[i].used) continue;
				uint8_t* instance = instance_table[i];
				instance[INST_FLAGS] = saved[i].flags;
				instance_anim(instance) = saved[i].animation;
				instance_counter(instance) = saved[i].counter;
			}
	}
}
