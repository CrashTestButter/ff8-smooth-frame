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

// Magic primitive blending (v60). Shares the battle module's tick state through internal.h; gated by
// DBG_FX_BLEND_OFF.
//
// animations.cpp records every display-list link call a spell effect makes on a logic frame and
// re-issues them on the host frames in between, so particles step at 15 fps. This file turns that
// hold into motion: it pairs the previous recorded list against the current one and, for every call
// it can pair, builds an interpolated COPY of the packet in scratch storage of its own. animations.cpp
// then links the copy instead of the effect's own packet - on the logic frame as well as between them
// (design rule 2.1), so effects lag by the same one tick as the models and the camera.
//
// Three things killed the v25 attempt and are the reason this file looks the way it does:
//  * these effects flip their packet arena every frame, so two recorded lists never share a primitive
//    ADDRESS - they are offset by one CONSTANT delta across the whole list, which is the pairing test;
//  * word 1's colour is recomputed every frame, so it is not part of the identity test either (only
//    its top byte, the GPU command, is) and it is never interpolated;
//  * the scratch was one shared vector that up to four replays per frame resized while the ordering
//    table still held pointers into it. Here a scratch block is never resized and never freed at all:
//    a grow allocates a NEW block and keeps the old one, so no address the ordering table was handed
//    can ever become invalid. Growth is bounded (doubling, capped at FX_BLEND_MAX_PRIMS packets).

#include "internal.h"

#include "../host.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace ff8_battle_anim
{
	namespace
	{
		// One slot per recorded list. Only slot 0 is used today (the spell effect root); the sprite
		// queues are not replayed at all, see the v59 note in animations.cpp.
		constexpr int FX_BLEND_SLOTS = 4;
		// Matches FX_MAX_PRIMS in animations.cpp: a longer list is never recorded, so never blended.
		constexpr size_t FX_BLEND_MAX_PRIMS = 16384;
		constexpr size_t FX_BLEND_FIRST_PRIMS = 512; // first allocation, then doubled as needed

		// Only these three linkers take a primitive as their second argument and are safe to blend:
		// 0x45C7A0 (fn 0), 0x45C870 (fn 2) and 0x45C8E0 (fn 3). 0x45C860 (fn 1) takes a bare u16 that
		// it writes into the node the previous call created, and fn 4 (0x45D310) is the ordering-table
		// renderer and is never even recorded. Both are passed through with their recorded arguments.
		inline bool linker_blends(int fn) { return fn == 0 || fn == 2 || fn == 3; }

		// Which packet dwords hold a packed (x, y) pair of int16 screen coordinates, by GPU command
		// byte (packet word 1 >> 24, i.e. prim[7] - the same byte 0x45C7A0 itself reads and masks with
		// 0xFC). Everything the table does not name is left at its current-frame value: the tag, the
		// command + colour word, the UV/CLUT/tpage words and the packed per-vertex RGB.
		const uint8_t VTX_F3[]  = { 2, 3, 4 };        // flat triangle        0x20-0x23
		const uint8_t VTX_F4[]  = { 2, 3, 4, 5 };     // flat quad            0x28-0x2B
		const uint8_t VTX_FT3[] = { 2, 4, 6 };        // flat textured tri    0x24-0x27
		const uint8_t VTX_FT4[] = { 2, 4, 6, 8 };     // flat textured quad   0x2C-0x2F
		const uint8_t VTX_G3[]  = { 2, 4, 6 };        // gouraud triangle     0x30-0x33
		const uint8_t VTX_G4[]  = { 2, 4, 6, 8 };     // gouraud quad         0x38-0x3B
		const uint8_t VTX_GT3[] = { 2, 5, 8 };        // gouraud textured tri 0x34-0x37
		const uint8_t VTX_GT4[] = { 2, 5, 8, 11 };    // gouraud textured quad 0x3C-0x3F
		// Sprites and tiles carry their position in ONE dword and nothing else that may be touched:
		// SPRT is tag / code+rgb / xy / uv+clut [/ w+h], TILE is tag / code+rgb / xy [/ w+h]. The UV +
		// CLUT word and the width/height word must never be interpolated - a blended CLUT picks a wrong
		// palette and a blended w/h resizes the sprite. v66 had no entry for these at all, so the
		// 0x64E8C1 / 0x64F065 emitters (the ice shards) produced nothing but `novtx` and were the part
		// of the effect that kept stepping.
		const uint8_t VTX_XY[]  = { 2 };

		// Refreshed once per prepare so the hot path does not re-read the config word per primitive.
		bool rect_off = false;

		int vertex_dwords(uint8_t cmd, const uint8_t** out)
		{
			const uint32_t type = cmd & 0xFC;
			if (rect_off && type >= 0x60) { *out = nullptr; return 0; } // DBG_FX_RECT_OFF
			switch (type)
			{
			case 0x20: *out = VTX_F3;  return 3;
			case 0x24: *out = VTX_FT3; return 3;
			case 0x28: *out = VTX_F4;  return 4;
			case 0x2C: *out = VTX_FT4; return 4;
			case 0x30: *out = VTX_G3;  return 3;
			case 0x34: *out = VTX_GT3; return 3;
			case 0x38: *out = VTX_G4;  return 4;
			case 0x3C: *out = VTX_GT4; return 4;
			case 0x60: *out = VTX_XY;  return 1; // TILE, variable size   0x60-0x63
			case 0x64: *out = VTX_XY;  return 1; // SPRT, variable size   0x64-0x67
			case 0x68: *out = VTX_XY;  return 1; // TILE 1x1              0x68-0x6B
			case 0x6C: *out = VTX_XY;  return 1; // SPRT 1x1              0x6C-0x6F
			case 0x70: *out = VTX_XY;  return 1; // TILE 8x8              0x70-0x73
			case 0x74: *out = VTX_XY;  return 1; // SPRT 8x8              0x74-0x77
			case 0x78: *out = VTX_XY;  return 1; // TILE 16x16            0x78-0x7B
			case 0x7C: *out = VTX_XY;  return 1; // SPRT 16x16            0x7C-0x7F
			// Lines are deliberately absent: LINE_F2 (0x40) puts its vertices at 2,3 but LINE_G2 (0x50)
			// at 2,4, and the poly-line forms are variable length terminated by 0x55555555, so one wrong
			// guess would drag a colour word through the vertex blender. Add them only with a dump.
			default:   *out = nullptr; return 0;
			}
		}

		// Two int16 packed in one dword, blended separately. Both ends are int16 and t is clamped to
		// 0..1 by update_tick(), so the result can never leave the int16 range.
		inline uint32_t blend_xy(uint32_t a, uint32_t b, float t)
		{
			const int ax = (int16_t)(uint16_t)(a & 0xFFFF), ay = (int16_t)(uint16_t)(a >> 16);
			const int bx = (int16_t)(uint16_t)(b & 0xFFFF), by = (int16_t)(uint16_t)(b >> 16);
			const int x = ax + (int)lrintf(float(bx - ax) * t);
			const int y = ay + (int)lrintf(float(by - ay) * t);
			return (uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)y << 16);
		}

		// Scratch packets. The ordering table keeps raw pointers into these until the frame is drawn
		// and the walker 0x45D080 dereferences them, so a block is never resized, moved or freed: a
		// grow allocates a new block and the old ones are simply kept. Not even fx_blend_reset()
		// releases them - see the note there.
		struct Scratch
		{
			std::vector<std::unique_ptr<uint32_t[]>> blocks; // [0..n-2] retired, back() is live
			uint32_t* words = nullptr;
			size_t prims = 0;   // capacity in packets, each FX_PRIM_WORDS dwords
		};

		struct Slot
		{
			Scratch scratch;
			std::vector<uint8_t> use_scratch; // per call: 1 = link the interpolated copy
			bool ok = false;                  // this host frame's pair was accepted
			uint32_t frame = 0xFFFFFFFF;      // frame_counter of the accepted pair
		};
		Slot slots[FX_BLEND_SLOTS];

		// Reserve capacity for `prims` packets. Only ever grows, and only from fx_blend_prepare(),
		// which animations.cpp calls before the first link call of that list in this host frame.
		bool scratch_reserve(Scratch& s, size_t prims)
		{
			if (prims <= s.prims) return true;
			if (prims > FX_BLEND_MAX_PRIMS) return false;
			size_t want = s.prims != 0 ? s.prims : FX_BLEND_FIRST_PRIMS;
			while (want < prims) want *= 2;
			if (want > FX_BLEND_MAX_PRIMS) want = FX_BLEND_MAX_PRIMS;
			std::unique_ptr<uint32_t[]> block(new (std::nothrow) uint32_t[want * FX_PRIM_WORDS]);
			if (!block) return false;
			s.words = block.get();
			s.prims = want;
			s.blocks.push_back(std::move(block)); // the previous block stays alive, see above
			return true;
		}
		// --- diagnostics -------------------------------------------------------------------------
		enum FbReason
		{
			FB_OFF = 0,   // bit 512 set, or one of the two lists missing
			FB_EMPTY,     // nothing recorded this tick
			FB_NOVTX,     // not one primitive in the list has interpolatable vertices
			FB_NOPAIR,    // runs exist but none of them could be paired against the previous tick
			FB_ALLOC,     // scratch refused (longer than FX_BLEND_MAX_PRIMS, or out of memory)
			FB_COUNT
		};
		const char* const FB_NAME[FB_COUNT] = { "off", "empty", "novtx", "nopair", "alloc" };

		uint32_t stat_blended = 0, stat_fallback = 0, stat_prims = 0, stat_ms = 0;
		uint32_t stat_reason[FB_COUNT] = {};
		uint32_t stat_nocopy = 0;    // entries with no usable packet copy
		uint32_t stat_nonblend = 0;  // entries passed through: no primitive, or an unblendable command
		uint32_t blend_streak = 0;   // consecutive successful prepares, drives fx_blend_defer_ok()
		uint32_t blend_miss = 0;     // consecutive failures since the last success
		bool blend_armed = false;    // hysteresis state, see fx_blend_defer_ok()
		constexpr uint32_t FX_BLEND_ARM_LIMIT = 2;
		constexpr uint32_t FX_BLEND_MISS_LIMIT = 30; // ~3 logic ticks at 144 fps

		// Detail of the most recent list.
		int last_reason = FB_OFF;
		uint32_t last_runs = 0, last_pruns = 0, last_paired = 0;
		uint32_t last_ents = 0, last_blent = 0, last_delta = 0;

		// v66 diagnostics. `moved` answers the only question that matters once pairing works: do the
		// two recorded packets actually differ? If a whole list pairs and blends but nothing moved, the
		// geometry on screen is not coming from these packets. `alpha_*` answers the other one: is the
		// blend factor really sweeping 0..1 across the host frames of a logic period, or is it stuck?
		// All four accumulate over the whole trace second, i.e. over every host frame's rescan of the
		// list - divide by the frame rate to get a per-list figure.
		uint32_t stat_moved = 0;      // paired entries whose masked vertex dwords differ
		uint32_t stat_blent = 0;      // paired entries actually written to scratch (the denominator)
		uint32_t stat_snapped = 0;    // paired entries rejected by FX_BLEND_SNAP
		int32_t stat_movemax = 0;     // largest single-axis movement seen, in screen units
		float alpha_min = 2.0f, alpha_max = -1.0f;
		uint64_t alpha_seen = 0;      // bit k = an alpha in [k/64, (k+1)/64) was used
		// One paired packet, dumped so the field mask can be checked against the real bytes.
		uint8_t dump_cmd = 0;
		int dump_words = 0, dump_nv = 0;
		uint8_t dump_idx[4] = {};
		uint32_t dump_prev[4] = {}, dump_cur[4] = {}, dump_out[4] = {};
		// ...and the whole packet, both frames, so the field mask can be checked against the real
		// layout rather than against libgpu's documentation: a masked dword must read as a plausible
		// pair of signed screen coordinates, an unmasked one as a colour, a UV pair or a tpage word.
		uint32_t dump_all_prev[12] = {}, dump_all_cur[12] = {};
		int dump_all_n = 0;
		uint8_t hist_cmd[8] = {};
		uint32_t hist_n[8] = {};
		int hist_used = 0;
		// Which game call sites produced the packets of the most recent list. This is the diagnostic
		// that says whether a given piece of an effect is drawn through the effect tree at all: a part
		// of the picture that steps while these packets blend is being drawn by something else, and its
		// emitter will simply not appear here.
		uint32_t site_addr[6] = {};
		uint32_t site_n[6] = {};
		int site_used = 0;

		void site_add(uint32_t ret)
		{
			for (int i = 0; i < site_used; i++) if (site_addr[i] == ret) { site_n[i]++; return; }
			if (site_used >= (int)(sizeof(site_addr) / sizeof(site_addr[0]))) return;
			site_addr[site_used] = ret;
			site_n[site_used] = 1;
			site_used++;
		}

		void hist_add(uint8_t cmd)
		{
			for (int i = 0; i < hist_used; i++) if (hist_cmd[i] == cmd) { hist_n[i]++; return; }
			if (hist_used >= (int)(sizeof(hist_cmd) / sizeof(hist_cmd[0]))) return;
			hist_cmd[hist_used] = cmd;
			hist_n[hist_used] = 1;
			hist_used++;
		}

		void trace_once_per_second()
		{
			if (!ff8_interp::cfg().trace) return;
			const uint32_t now = GetTickCount();
			if (stat_ms == 0) { stat_ms = now; return; }
			if (now - stat_ms < 1000) return;
			ff8_interp::log_trace("ff8fx blend lists=%u fallback=%u prims=%u | off=%u empty=%u novtx=%u nopair=%u alloc=%u\n",
				stat_blended, stat_fallback, stat_prims,
				stat_reason[FB_OFF], stat_reason[FB_EMPTY], stat_reason[FB_NOVTX],
				stat_reason[FB_NOPAIR], stat_reason[FB_ALLOC]);
			ff8_interp::log_trace("ff8fx blend last=%s runs=%u/%u paired=%u ents=%u/%u nocopy=%u skip=%u delta=%08x\n",
				FB_NAME[last_reason], last_runs, last_pruns, last_paired, last_blent, last_ents,
				stat_nocopy, stat_nonblend, last_delta);
			char cmds[160];
			int o = 0;
			for (int i = 0; i < hist_used && o < (int)sizeof(cmds) - 16; i++)
				o += snprintf(cmds + o, sizeof(cmds) - (size_t)o, "%02x:%u ", hist_cmd[i], hist_n[i]);
			cmds[o] = 0;
			ff8_interp::log_trace("ff8fx blend cmds %s\n", cmds);
			char sites[192];
			int t = 0;
			for (int i = 0; i < site_used && t < (int)sizeof(sites) - 24; i++)
				t += snprintf(sites + t, sizeof(sites) - (size_t)t, "%08x:%u ", site_addr[i], site_n[i]);
			sites[t] = 0;
			ff8_interp::log_trace("ff8fx blend sites %s\n", sites);
			int nalpha = 0;
			for (int b = 0; b < 64; b++) if (alpha_seen & (1ull << b)) nalpha++;
			ff8_interp::log_trace("ff8fx blend alpha min=%.3f max=%.3f distinct=%d | moved=%u/%u snapped=%u maxdelta=%d\n",
				alpha_min > 1.0f ? -1.0f : alpha_min, alpha_max, nalpha,
				stat_moved, stat_blent, stat_snapped, stat_movemax);
			if (dump_nv > 0)
			{
				char dmp[192];
				int d = 0;
				for (int v = 0; v < dump_nv && d < (int)sizeof(dmp) - 40; v++)
					d += snprintf(dmp + d, sizeof(dmp) - (size_t)d, "[%u] %08x>%08x=%08x ",
						dump_idx[v], dump_prev[v], dump_cur[v], dump_out[v]);
				dmp[d] = 0;
				ff8_interp::log_trace("ff8fx blend pkt cmd=%02x words=%d %s\n", dump_cmd, dump_words, dmp);
				char raw[224];
				int q = 0;
				for (int v = 0; v < dump_all_n && q < (int)sizeof(raw) - 12; v++)
					q += snprintf(raw + q, sizeof(raw) - (size_t)q, "%08x ", dump_all_prev[v]);
				raw[q] = 0;
				ff8_interp::log_trace("ff8fx blend pkt prev %s\n", raw);
				q = 0;
				for (int v = 0; v < dump_all_n && q < (int)sizeof(raw) - 12; v++)
					q += snprintf(raw + q, sizeof(raw) - (size_t)q, "%08x ", dump_all_cur[v]);
				raw[q] = 0;
				ff8_interp::log_trace("ff8fx blend pkt cur  %s\n", raw);
			}
			alpha_min = 2.0f;
			alpha_max = -1.0f;
			alpha_seen = 0;
			stat_moved = 0;
			stat_blent = 0;
			stat_snapped = 0;
			stat_movemax = 0;
			stat_blended = 0;
			stat_fallback = 0;
			stat_nocopy = 0;
			stat_nonblend = 0;
			memset(stat_reason, 0, sizeof(stat_reason));
			stat_ms = now;
		}

		bool fall_back(FbReason r)
		{
			stat_fallback++;
			stat_reason[r]++;
			last_reason = r;
			blend_streak = 0;
			if (++blend_miss >= FX_BLEND_MISS_LIMIT) blend_armed = false;
			trace_once_per_second();
			return false;
		}

		// --- sub-list pairing (v65) ---------------------------------------------------------------
		// A spell emits a different number of packets on every tick - the v64 trace caught 0xD5, then
		// 0x32, then 0x7F on consecutive ticks - so whole-list index pairing (v60..v64) could never
		// match and every list fell back with reason `len`. Instead each recorded list is cut into
		// RUNS: a maximal span of consecutive blendable entries that share a key and sit at a constant
		// stride in memory, which is exactly one emitter's packet array (0x572200 fills the caller's
		// buffer with `param_4 += 5` per primitive, so one call yields one contiguous, evenly spaced
		// run). Runs are paired by (key, occurrence order), entries inside a paired run by index over
		// the common length, and everything unpaired - a particle that spawned this tick, one that
		// expired, a whole emitter that came or went - is issued exactly as recorded.
		//
		// Key = linker index + packet word count + command TYPE. The command byte's low two bits are
		// the semi-transparency and texture-blend flags, which a spell flips between frames (v64 saw
		// 0x24 pair with 0x26), so only the top six bits identify the primitive - the same 0xFC mask
		// 0x45C7A0 itself applies. Environment packets such as 0xE2 (texture window) have no vertex
		// mask, so they never enter a run and can never break one.
		struct Ent
		{
			uint32_t idx;   // index in the recorded list
			uint32_t key;
			uint32_t addr;  // the primitive pointer the linker was called with
		};
		struct Run
		{
			uint32_t key;
			uint32_t first; // index of the run's first entry in the Ent array
			uint32_t n;
			uint32_t base;  // addr of the first entry
			int32_t stride;
		};
		constexpr uint32_t NO_PAIR = 0xFFFFFFFF;
		// A paired packet that moved further than this in one logic tick is not the same particle
		// moving, it is a slot being reused (a particle died and another took its place) or a mispair
		// inside a run. Interpolating it draws a streak right across the picture, so it is issued
		// verbatim instead - the same "snap on a large delta" rule the entity positions and the battle
		// camera already use. The v66 log reported maxdelta=5023 screen units, which is exactly this.
		constexpr int FX_BLEND_SNAP = 2048;

		// Reused between frames so a 200-packet list does not allocate four vectors per host frame.
		// None of these is ever pointed at by the ordering table, so resizing them is free of the
		// hazard that governs the scratch packets.
		std::vector<Ent> ent_cur, ent_prev;
		std::vector<Run> run_cur, run_prev;
		std::vector<uint32_t> run_pair;

		inline uint32_t ent_key(const FxCall& c)
		{
			return (uint32_t)c.fn
				| ((uint32_t)c.words << 4)
				| ((uint32_t)((c.copy[1] >> 24) & 0xFC) << 12);
		}

		// Cut one recorded list into runs. `stats` is set for the current list only, so the counters
		// and the command histogram describe one tick and not two.
		void build_runs(const FxCall* list, size_t n, std::vector<Ent>& ents, std::vector<Run>& runs, bool stats)
		{
			ents.clear();
			runs.clear();
			for (size_t i = 0; i < n; i++)
			{
				const FxCall& c = list[i];
				// 0x45C860 carries no primitive; it is re-issued verbatim and in place, which is what
				// keeps it patching the node its predecessor created.
				if (!linker_blends(c.fn)) { if (stats) stat_nonblend++; continue; }
				if (c.words < 2) { if (stats) stat_nocopy++; continue; }
				const uint8_t cmd = (uint8_t)(c.copy[1] >> 24);
				if (stats) { hist_add(cmd); site_add(c.ret); }
				const uint8_t* mask = nullptr;
				const int nv = vertex_dwords(cmd, &mask);
				if (nv == 0 || mask[nv - 1] >= c.words) { if (stats) stat_nonblend++; continue; }

				const Ent e = { (uint32_t)i, ent_key(c), c.a[1] };
				bool extended = false;
				if (!runs.empty())
				{
					Run& r = runs.back();
					if (r.key == e.key)
					{
						if (r.n == 1)
						{
							const int32_t stride = (int32_t)(e.addr - r.base);
							if (stride != 0) { r.stride = stride; r.n = 2; extended = true; }
						}
						else if (r.base + (uint32_t)((int32_t)r.n * r.stride) == e.addr)
						{
							r.n++;
							extended = true;
						}
					}
				}
				if (!extended)
				{
					const Run r = { e.key, (uint32_t)ents.size(), 1, e.addr, 0 };
					runs.push_back(r);
				}
				ents.push_back(e);
			}
		}
	}

	bool fx_blend_enabled()
	{
		return (ff8_interp::cfg().battle_debug & DBG_FX_BLEND_OFF) == 0;
	}

	bool fx_blend_defer_ok()
	{
		// Holding a logic tick's link calls back is only worth anything if the list that replaces them
		// is actually interpolated, so the predictor arms after two consecutive successful prepares.
		// It DISARMS only after FX_BLEND_MISS_LIMIT consecutive failures (v67): the decision changes
		// where the drawn state sits in time - deferred, the logic frame shows prev->cur at that
		// frame's alpha, the same one-tick lag the models and the camera carry; not deferred, it shows
		// the raw current list, a whole tick ahead of them. Flipping that per frame, which is what the
		// v66 build did whenever one list fell back, moves the whole effect back and forth in time by
		// up to a full tick and is what made the timing feel wrong against the sound.
		// The predictor stays warm because the host frames between ticks call fx_blend_prepare() too.
		if (ff8_interp::cfg().battle_debug & DBG_FX_DEFER_OFF) return false;
		return fx_blend_enabled() && blend_armed;
	}

	bool fx_blend_prepare(int slot, const FxCall* prev, size_t prev_n, const FxCall* cur, size_t cur_n, float alpha)
	{
		if (slot < 0 || slot >= FX_BLEND_SLOTS) return false;
		Slot& s = slots[slot];
		// Idempotent within one host frame: the linker patches word 0 of every packet it links, so
		// rebuilding a scratch packet that is already in this frame's ordering table would clobber the
		// chain pointer it wrote there.
		if (s.ok && s.frame == ff8_interp::frame_counter()) return true;
		s.ok = false;
		rect_off = (ff8_interp::cfg().battle_debug & DBG_FX_RECT_OFF) != 0;
		stat_prims = (uint32_t)cur_n;
		if (!fx_blend_enabled() || prev == nullptr || cur == nullptr) return fall_back(FB_OFF);
		if (cur_n == 0) return fall_back(FB_EMPTY);

		if (alpha < alpha_min) alpha_min = alpha;
		if (alpha > alpha_max) alpha_max = alpha;
		{
			int bucket = (int)(alpha * 64.0f);
			if (bucket < 0) bucket = 0;
			if (bucket > 63) bucket = 63;
			alpha_seen |= 1ull << bucket;
		}
		hist_used = 0;
		site_used = 0;
		last_delta = 0;
		dump_nv = 0;
		build_runs(cur, cur_n, ent_cur, run_cur, true);
		build_runs(prev, prev_n, ent_prev, run_prev, false);
		last_runs = (uint32_t)run_cur.size();
		last_pruns = (uint32_t)run_prev.size();
		last_ents = (uint32_t)ent_cur.size();
		last_paired = 0;
		last_blent = 0;
		if (run_cur.empty()) return fall_back(FB_NOVTX);
		if (!scratch_reserve(s.scratch, cur_n)) return fall_back(FB_ALLOC);

		// Pair run a of the current list with the a-th previous run carrying the same key. Runs are
		// few (one per emitter call), so the quadratic scan is cheaper than building a map.
		run_pair.assign(run_cur.size(), NO_PAIR);
		for (size_t a = 0; a < run_cur.size(); a++)
		{
			uint32_t want = 0;
			for (size_t b = 0; b < a; b++) if (run_cur[b].key == run_cur[a].key) want++;
			uint32_t seen = 0;
			for (size_t b = 0; b < run_prev.size(); b++)
			{
				if (run_prev[b].key != run_cur[a].key) continue;
				if (seen == want) { run_pair[a] = (uint32_t)b; break; }
				seen++;
			}
		}

		s.use_scratch.assign(cur_n, 0);
		for (size_t a = 0; a < run_cur.size(); a++)
		{
			if (run_pair[a] == NO_PAIR) continue;
			const Run& rc = run_cur[a];
			const Run& rp = run_prev[run_pair[a]];
			last_paired++;
			if (last_delta == 0) last_delta = rc.base - rp.base;
			const uint32_t n = rc.n < rp.n ? rc.n : rp.n;
			for (uint32_t t = 0; t < n; t++)
			{
				const size_t ic = ent_cur[rc.first + t].idx;
				const size_t ip = ent_prev[rp.first + t].idx;
				const FxCall& c = cur[ic];
				const FxCall& p = prev[ip];
				// The key already fixes the linker, the word count and the command type, so the mask
				// below is valid for both sides; this only guards against a truncated packet.
				const uint8_t* mask = nullptr;
				const int nv = vertex_dwords((uint8_t)(c.copy[1] >> 24), &mask);
				if (nv == 0 || mask[nv - 1] >= c.words || c.words != p.words) continue;
				uint32_t* dst = s.scratch.words + ic * FX_PRIM_WORDS;
				// Start from the CURRENT packet, so the tag, the command word and everything the mask
				// does not name (colour, UVs, CLUT, tpage, packed RGB) are this logic frame's values,
				// then move the vertices back toward the previous frame by 1 - alpha.
				// Reject the pair before writing anything if any vertex teleported: a reused slot or a
				// mispair inside a run would otherwise be drawn as a streak across the screen.
				bool moved = false, snap = false;
				int worst = 0;
				for (int v = 0; v < nv; v++)
				{
					const int d = mask[v];
					if (p.copy[d] == c.copy[d]) continue;
					moved = true;
					const int dx = (int)(int16_t)(uint16_t)(c.copy[d] & 0xFFFF) - (int)(int16_t)(uint16_t)(p.copy[d] & 0xFFFF);
					const int dy = (int)(int16_t)(uint16_t)(c.copy[d] >> 16) - (int)(int16_t)(uint16_t)(p.copy[d] >> 16);
					const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
					const int m = adx > ady ? adx : ady;
					if (m > worst) worst = m;
					if (m > FX_BLEND_SNAP) snap = true;
				}
				if (worst > stat_movemax) stat_movemax = worst;
				if (snap) { stat_snapped++; continue; }
				memcpy(dst, c.copy, size_t(c.words) * 4);
				for (int v = 0; v < nv; v++)
				{
					const int d = mask[v];
					dst[d] = blend_xy(p.copy[d], c.copy[d], alpha);
				}
				if (moved) stat_moved++;
				if (dump_nv == 0)
				{
					// One paired packet per host frame, so the field mask can be checked against the
					// bytes the effect really wrote.
					dump_cmd = (uint8_t)(c.copy[1] >> 24);
					dump_words = c.words;
					dump_nv = nv < 4 ? nv : 4;
					for (int v = 0; v < dump_nv; v++)
					{
						dump_idx[v] = mask[v];
						dump_prev[v] = p.copy[mask[v]];
						dump_cur[v] = c.copy[mask[v]];
						dump_out[v] = dst[mask[v]];
					}
					dump_all_n = c.words < 12 ? c.words : 12;
					for (int v = 0; v < dump_all_n; v++)
					{
						dump_all_prev[v] = p.copy[v];
						dump_all_cur[v] = c.copy[v];
					}
				}
				s.use_scratch[ic] = 1;
				last_blent++;
				stat_blent++;
			}
		}
		// Nothing paired: every emitter is new this tick, so there is nothing to interpolate from.
		if (last_blent == 0) return fall_back(FB_NOPAIR);

		s.ok = true;
		s.frame = ff8_interp::frame_counter();
		stat_blended++;
		blend_miss = 0;
		if (++blend_streak >= FX_BLEND_ARM_LIMIT) blend_armed = true;
		trace_once_per_second();
		return true;
	}

	uint32_t fx_blend_arg(int slot, size_t i)
	{
		if (slot < 0 || slot >= FX_BLEND_SLOTS) return 0;
		const Slot& s = slots[slot];
		if (!s.ok || s.frame != ff8_interp::frame_counter() || i >= s.use_scratch.size() || s.use_scratch[i] == 0) return 0;
		return (uint32_t)(uintptr_t)(s.scratch.words + i * FX_PRIM_WORDS);
	}

	void fx_blend_hook_init()
	{
		// No hook of its own: animations.cpp already owns the linkers and drives this file from its
		// recording and replay path.
	}

	void fx_blend_reset()
	{
		for (int i = 0; i < FX_BLEND_SLOTS; i++)
		{
			slots[i].ok = false;
			slots[i].frame = 0xFFFFFFFF;
			slots[i].use_scratch.clear();
			// The scratch blocks themselves deliberately survive a reset. animations_reset() runs from
			// the once-per-frame driver hook on a battle mode change, and freeing a block there would be
			// the only moment in this file where memory an ordering table may still name goes away - the
			// exact shape of the v25 use-after-free. They are bounded (one block per growth step, at most
			// FX_BLEND_MAX_PRIMS packets) and the next battle reuses them without reallocating.
		}
		stat_blended = 0;
		stat_fallback = 0;
		stat_prims = 0;
		stat_ms = 0;
		stat_nocopy = 0;
		stat_nonblend = 0;
		blend_streak = 0;
		blend_miss = 0;
		blend_armed = false;
		memset(stat_reason, 0, sizeof(stat_reason));
		last_reason = FB_OFF;
		last_runs = last_pruns = last_paired = 0;
		last_ents = last_blent = last_delta = 0;
		stat_moved = 0;
		stat_blent = 0;
		stat_snapped = 0;
		stat_movemax = 0;
		alpha_min = 2.0f;
		alpha_max = -1.0f;
		alpha_seen = 0;
		dump_nv = 0;
		dump_all_n = 0;
		hist_used = 0;
		site_used = 0;
		ent_cur.clear();
		ent_prev.clear();
		run_cur.clear();
		run_prev.clear();
		run_pair.clear();
	}
}
