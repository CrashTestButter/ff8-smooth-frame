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

		inline int16_t lerp_angle(int16_t a, int16_t b, float t)
		{
			int d = ((int(b) - int(a) + 2048) & 4095) - 2048;
			float v = d * t;
			return (int16_t)(int(a) + (int)(v + (v >= 0.0f ? 0.5f : -0.5f)));
		}
		inline int16_t lerp_i16(int16_t a, int16_t b, float t)
		{
			float v = (int(b) - int(a)) * t;
			return (int16_t)(int(a) + (int)(v + (v >= 0.0f ? 0.5f : -0.5f)));
		}
	// 0x653DA0 decodes one packed pose per instance per call (frame = counter >> 4; 4 bytes per
	// bone: x = ((b3 & 3) << 8 | b0) << 2, y = ((b3 & 0xC) << 6 | b1) << 2, z = ((b3 & 0x30) << 4 | b2) << 2;
	// frame stride 4 + 4 * bones; root = two dwords at the frame start, GETA added to the second),
	// then builds the skeleton and submits the model. It is called once per frame, so we never call
	// it more than once: lookahead frames are decoded here from the animation data, and every host
	// frame the blend is fed through the engine's raw-pose mode (flag bit2: 8 bytes per bone
	// memcpy) with a scratch buffer as "frame 0".

	inline bool inst_active(uint8_t* inst)
	{
		return (uintptr_t)inst >= 0x10000 && (uintptr_t)inst < 0x7FFFFFFF && *(int32_t*)inst != -1 && (inst[INST_HIDDEN] & 1) == 0
			&& *(uint8_t**)(inst + INST_ANIM) != nullptr && *(uint16_t*)(inst + INST_BONE_COUNT) > 0
			&& (inst[INST_FLAGS] & 4) == 0;
	}
	// Mirror of the engine's packed decode (non-raw mode) into the raw 8-byte-per-bone layout.
	void decode_pose(const uint8_t* anim, int frame, int bones, std::vector<uint8_t>& out)
	{
		const uint8_t* p = anim + frame * (4 + 4 * bones);
		out.resize(8 + 8 * bones);
		memcpy(&out[0], p, 8); // root dwords (GETA is added by the engine)
		const uint8_t* b = p + 8;
		for (int i = 0; i < bones; i++, b += 4)
		{
			uint32_t d0 = (uint32_t(((b[3] & 0xC) << 6) | b[1]) << 18) | (uint32_t(((b[3] & 3) << 8) | b[0]) << 2);
			uint32_t d1 = uint32_t(((b[3] & 0x30) << 4) | b[2]) << 2;
			memcpy(&out[8 + 8 * i], &d0, 4);
			memcpy(&out[12 + 8 * i], &d1, 4);
		}
	}
	// Bone rotations. The engine builds each bone's local matrix as R = Rx(x) * Ry(y) * Rz(z)
	// (0x654060: standard right-handed elementary rotations, 12-bit angles, 4096 = full turn,
	// row-major product 0x56C090 out = a * b). Per-axis angle blending swings limbs through wrong
	// intermediate rotations when two poses are far apart (clip switch, loop seam), so poses are
	// blended as rotations: angles -> matrix -> quaternion, slerp, back to angles.
	struct Quat { float w, x, y, z; };
	constexpr float ANG = 6.2831853f / 4096.0f;
	inline Quat euler_to_quat(int ax, int ay, int az)
	{
		float ca = cosf(ax * ANG), sa = sinf(ax * ANG), cb = cosf(ay * ANG), sb = sinf(ay * ANG), cc = cosf(az * ANG), sc = sinf(az * ANG);
		float m00 = cb * cc, m01 = -cb * sc, m02 = sb;
		float m10 = ca * sc + sa * sb * cc, m11 = ca * cc - sa * sb * sc, m12 = -sa * cb;
		float m20 = sa * sc - ca * sb * cc, m21 = sa * cc + ca * sb * sc, m22 = ca * cb;
		Quat q;
		float tr = m00 + m11 + m22;
		if (tr > 0.0f)
		{
			float r = sqrtf(1.0f + tr), inv = 0.5f / r;
			q.w = 0.5f * r; q.x = (m21 - m12) * inv; q.y = (m02 - m20) * inv; q.z = (m10 - m01) * inv;
		}
		else if (m00 >= m11 && m00 >= m22)
		{
			float r = sqrtf(1.0f + m00 - m11 - m22), inv = 0.5f / r;
			q.x = 0.5f * r; q.w = (m21 - m12) * inv; q.y = (m01 + m10) * inv; q.z = (m02 + m20) * inv;
		}
		else if (m11 >= m22)
		{
			float r = sqrtf(1.0f - m00 + m11 - m22), inv = 0.5f / r;
			q.y = 0.5f * r; q.w = (m02 - m20) * inv; q.x = (m01 + m10) * inv; q.z = (m12 + m21) * inv;
		}
		else
		{
			float r = sqrtf(1.0f - m00 - m11 + m22), inv = 0.5f / r;
			q.z = 0.5f * r; q.w = (m10 - m01) * inv; q.x = (m02 + m20) * inv; q.y = (m12 + m21) * inv;
		}
		return q;
	}
	inline int16_t rad_to_ang(float r)
	{
		int v = (int)floorf(r / ANG + 0.5f);
		return (int16_t)(v & 4095);
	}
	inline void quat_to_euler(const Quat& q, int16_t* out)
	{
		float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
		float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
		float m00 = 1.0f - 2.0f * (yy + zz), m01 = 2.0f * (xy - wz), m02 = 2.0f * (xz + wy);
		float m11 = 1.0f - 2.0f * (xx + zz), m12 = 2.0f * (yz - wx);
		float m21 = 2.0f * (yz + wx), m22 = 1.0f - 2.0f * (xx + yy);
		if (m02 > 1.0f) m02 = 1.0f;
		if (m02 < -1.0f) m02 = -1.0f;
		float b = asinf(m02), a, c;
		if (fabsf(m02) < 0.9999f) { a = atan2f(-m12, m22); c = atan2f(-m01, m00); }
		else { a = atan2f(m21, m11); c = 0.0f; } // gimbal lock: fold z into x
		out[0] = rad_to_ang(a); out[1] = rad_to_ang(b); out[2] = rad_to_ang(c);
	}
	inline Quat slerp(Quat a, Quat b, float t)
	{
		float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
		if (d < 0.0f) { b.w = -b.w; b.x = -b.x; b.y = -b.y; b.z = -b.z; d = -d; }
		float wa, wb;
		if (d > 0.9995f) { wa = 1.0f - t; wb = t; }
		else
		{
			float th = acosf(d), s = sinf(th);
			wa = sinf((1.0f - t) * th) / s; wb = sinf(t * th) / s;
		}
		Quat q = { wa * a.w + wb * b.w, wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z };
		float n = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
		if (n > 0.0f) { q.w /= n; q.x /= n; q.y /= n; q.z /= n; }
		return q;
	}
	void blend_pose(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, float t, std::vector<uint8_t>& out)
	{
		out.resize(a.size());
		const int16_t* pa = (const int16_t*)a.data();
		const int16_t* pb = (const int16_t*)b.data();
		int16_t* po = (int16_t*)out.data();
		int n = (int)(a.size() / 2);
		for (int k = 0; k < 4 && k < n; k++) po[k] = lerp_i16(pa[k], pb[k], t); // root translation
		for (int k = 4; k + 3 < n; k += 4)
		{
			if (pa[k] == pb[k] && pa[k + 1] == pb[k + 1] && pa[k + 2] == pb[k + 2])
			{
				po[k] = pa[k]; po[k + 1] = pa[k + 1]; po[k + 2] = pa[k + 2];
			}
			else
			{
				Quat qa = euler_to_quat(pa[k] & 4095, pa[k + 1] & 4095, pa[k + 2] & 4095);
				Quat qb = euler_to_quat(pb[k] & 4095, pb[k + 1] & 4095, pb[k + 2] & 4095);
				quat_to_euler(slerp(qa, qb, t), po + k);
			}
			po[k + 3] = lerp_i16(pa[k + 3], pb[k + 3], t); // pad word
		}
	}

	}

	void PoseInterp::reset()
	{
		for (auto& c : chara) c.valid = false;
		clip_loops.clear();
	}

	void PoseInterp::animate(animate_fn orig, int p1, int p2, bool tick_real, float alpha, uint32_t tick_no)
	{
			if (tick_real)
			{
				for (int i = 0; i < CHARA_SLOTS; i++)
				{
					uint8_t* inst = table[i];
					CharaPose& cp = chara[i];
					if (!inst_active(inst)) { cp.valid = false; continue; }
					uint8_t* anim = *(uint8_t**)(inst + INST_ANIM);
					uint16_t counter = *(uint16_t*)(inst + INST_COUNTER);
					uint16_t nframes = *(uint16_t*)(inst + INST_FRAMES) >> 4;
					int bones = *(uint16_t*)(inst + INST_BONE_COUNT);
					uint16_t frame = counter >> 4;
					if (nframes == 0 || frame >= nframes) { cp.valid = false; continue; }
					int span = nframes * 16;
					bool same_anim = cp.valid && cp.anim == anim;
					bool advanced = same_anim && counter != cp.counter;
					if (same_anim && counter < cp.counter) clip_loops[anim] = true;
					if (advanced)
					{
						int d = (int(counter) - int(cp.counter)) % span;
						if (d < 0) d += span;
						if (d > 0 && d <= 16 * MAX_AHEAD) cp.delta = d;
					}
					else cp.delta = 16;
					std::swap(cp.last0, cp.poses[0]); // previous tick's content (hold detection)
					decode_pose(anim, frame, bones, cp.poses[0]);
					cp.kdist = 0;
					cp.nposes = 1;
					cp.counter = counter;
					cp.frame = frame;
					if (!same_anim)
					{
						// Clip switch: cross-fade from the pose shown on the last host frame to the new
						// clip's first frame over this tick (vanilla pops).
						cp.hold = 1;
						cp.anim = anim;
						if (cp.valid && cp.shown.size() == cp.poses[0].size() && cp.shown != cp.poses[0])
						{
							cp.poses[1] = cp.poses[0];
							cp.poses[0] = cp.shown;
							cp.kdist = 1;
							cp.nposes = 2;
						}
					}
					else if (!advanced)
					{
						// Counter unchanged (clip end hold or paused): the engine shows frame f, so do we.
						cp.hold = 1;
					}
					else
					{
						// Clips may repeat a pose on consecutive frames (authored below 30 fps): count the
						// ticks the content has been unchanged and look ahead for the next distinct pose.
						cp.hold = (cp.last0 == cp.poses[0]) ? (cp.hold < 64 ? cp.hold + 1 : 64) : 1;
						int ahead = (cp.delta + 15) / 16; // frames the blend must cover this tick
						if (ahead < 1) ahead = 1;
						if (ahead > MAX_AHEAD) ahead = MAX_AHEAD;
						bool loops = clip_loops.count(anim) != 0;
						for (int j = 1; j <= MAX_AHEAD && nframes > 1; j++)
						{
							if (!(j <= ahead || cp.kdist == 0)) break;
							if (frame + j >= nframes && !loops) break; // do not blend past the end of a clip that never wrapped
							decode_pose(anim, (frame + j) % nframes, bones, cp.poses[j]);
							cp.nposes = j + 1;
							if (cp.kdist == 0 && cp.poses[j] != cp.poses[0]) cp.kdist = j;
						}
					}
					cp.valid = true;
					if (ff8_interp::cfg().trace)
						ff8_interp::log_trace("ff8pose t=%u s=%d anim=%p ctr=%u n=%u b=%d d=%d hold=%d k=%d np=%d\n", tick_no, i, anim,
							counter, nframes, bones, cp.delta, cp.hold, cp.kdist, cp.nposes);
				}
			}
			// Display: feed blended raw poses through the engine (flag bit2, frame 0 of a scratch buffer).
			struct Saved { uint8_t flags; uint8_t* anim; uint16_t counter; bool used; } sv[CHARA_SLOTS] = {};
			for (int i = 0; i < CHARA_SLOTS; i++)
			{
				uint8_t* inst = table[i];
				CharaPose& cp = chara[i];
				if (!cp.valid || !inst_active(inst) || cp.anim != *(uint8_t**)(inst + INST_ANIM)) continue;
				if (cp.nposes < 2 || cp.kdist == 0) { cp.shown = cp.poses[0]; continue; } // engine's own decode is shown
				size_t need = (size_t)(8 + 8 * *(uint16_t*)(inst + INST_BONE_COUNT));
				bool ok = true;
				for (int j = 0; j < cp.nposes; j++) if (cp.poses[j].size() != need) ok = false;
				if (!ok) continue;
				float fpt = float(cp.delta) / 16.0f;            // frames per tick
				float sub = float(cp.counter & 15) / 16.0f;     // sub-frame at the tick
				if (cp.hold == 1 && cp.kdist == 1)
				{
					// distinct consecutive frames: piecewise through the decoded frames
					float phi = sub + fpt * alpha;
					int seg = (int)phi;
					if (seg > cp.nposes - 2) seg = cp.nposes - 2;
					if (seg < 0) seg = 0;
					float frac = phi - float(seg);
					if (frac > 1.0f) frac = 1.0f;
					blend_pose(cp.poses[seg], cp.poses[seg + 1], frac, cp.scratch);
				}
				else
				{
					// repeated pose: spread the motion over the whole hold (previous ticks + frames ahead)
					float head = float(cp.hold - 1) * fpt;
					float phase = (head + sub + fpt * alpha) / (head + float(cp.kdist));
					if (phase > 1.0f) phase = 1.0f;
					if (phase < 0.0f) phase = 0.0f;
					blend_pose(cp.poses[0], cp.poses[cp.kdist], phase, cp.scratch);
				}
				cp.shown = cp.scratch;
				sv[i] = { inst[INST_FLAGS], *(uint8_t**)(inst + INST_ANIM), *(uint16_t*)(inst + INST_COUNTER), true };
				inst[INST_FLAGS] |= 4;
				*(uint8_t**)(inst + INST_ANIM) = cp.scratch.data();
				*(uint16_t*)(inst + INST_COUNTER) = 0;
			}
			orig(p1, p2);
			for (int i = 0; i < CHARA_SLOTS; i++)
			{
				if (!sv[i].used) continue;
				uint8_t* inst = table[i];
				inst[INST_FLAGS] = sv[i].flags;
				*(uint8_t**)(inst + INST_ANIM) = sv[i].anim;
				*(uint16_t*)(inst + INST_COUNTER) = sv[i].counter;
			}
	}
}
