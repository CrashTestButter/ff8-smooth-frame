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
//    can ever become invalid. Growth is bounded (doubling, capped at FX_BLEND_MAX_PRIMITIVES packets).

#include "internal.h"

#include "../host.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <memory>
#include <algorithm>
#include <new>
#include <utility>
#include <vector>
#include <unordered_map>

namespace ff8_battle_anim
{
	namespace
	{
		// One slot per recorded list. Only slot 0 is used today (the spell effect root); the sprite
		// queues are not replayed at all, see the v59 note in animations.cpp.
		constexpr int FX_BLEND_SLOTS = 4;
		// Matches FX_MAX_PRIMITIVES in animations.cpp: a longer list is never recorded, so never blended.
		constexpr size_t FX_BLEND_MAX_PRIMITIVES = 16384;
		constexpr size_t FX_BLEND_FIRST_PRIMITIVES = 512; // first allocation, then doubled as needed

		// Only these three linkers take a primitive as their second argument and are safe to blend:
		// 0x45C7A0 (fn 0), 0x45C870 (fn 2) and 0x45C8E0 (fn 3). 0x45C860 (fn 1) takes a bare u16 that
		// it writes into the node the previous call created, and fn 4 (0x45D310) is the ordering-table
		// renderer and is never even recorded. Both are passed through with their recorded arguments.
		inline bool linker_blends(int linker) { return linker == 0 || linker == 2 || linker == 3; }

		// Which packet dwords hold a packed (x, y) pair of int16 screen coordinates, by GPU command
		// byte (packet word 1 >> 24, i.e. prim[7] - the same byte 0x45C7A0 itself reads and masks with
		// 0xFC). Everything the table does not name is left at its current-frame value: the tag, the
		// command + colour word, the UV/CLUT/tpage words and the packed per-vertex RGB.
		const uint8_t VERTEX_DWORDS_F3[]  = { 2, 3, 4 };        // flat triangle        0x20-0x23
		const uint8_t VERTEX_DWORDS_F4[]  = { 2, 3, 4, 5 };     // flat quad            0x28-0x2B
		const uint8_t VERTEX_DWORDS_FT3[] = { 2, 4, 6 };        // flat textured tri    0x24-0x27
		const uint8_t VERTEX_DWORDS_FT4[] = { 2, 4, 6, 8 };     // flat textured quad   0x2C-0x2F
		const uint8_t VERTEX_DWORDS_G3[]  = { 2, 4, 6 };        // gouraud triangle     0x30-0x33
		const uint8_t VERTEX_DWORDS_G4[]  = { 2, 4, 6, 8 };     // gouraud quad         0x38-0x3B
		const uint8_t VERTEX_DWORDS_GT3[] = { 2, 5, 8 };        // gouraud textured tri 0x34-0x37
		const uint8_t VERTEX_DWORDS_GT4[] = { 2, 5, 8, 11 };    // gouraud textured quad 0x3C-0x3F
		// Sprites and tiles carry their position in ONE dword and nothing else that may be touched:
		// SPRT is tag / code+rgb / xy / uv+clut [/ w+h], TILE is tag / code+rgb / xy [/ w+h]. The UV +
		// CLUT word and the width/height word must never be interpolated - a blended CLUT picks a wrong
		// palette and a blended w/h resizes the sprite. v66 had no entry for these at all, so the
		// 0x64E8C1 / 0x64F065 emitters (the ice shards) produced nothing but `novtx` and were the part
		// of the effect that kept stepping.
		const uint8_t VERTEX_DWORDS_XY[]  = { 2 };

		// Refreshed once per prepare so the hot path does not re-read the config word per primitive.
		bool rectangles_off = false;

		int vertex_dwords(uint8_t command, const uint8_t** out_mask)
		{
			const uint32_t type = command & 0xFC;
			if (rectangles_off && type >= 0x60) // DBG_FX_RECTANGLE_OFF
			{
				*out_mask = nullptr;
				return 0;
			}
			switch (type)
			{
			case 0x20: *out_mask = VERTEX_DWORDS_F3;  return 3;
			case 0x24: *out_mask = VERTEX_DWORDS_FT3; return 3;
			case 0x28: *out_mask = VERTEX_DWORDS_F4;  return 4;
			case 0x2C: *out_mask = VERTEX_DWORDS_FT4; return 4;
			case 0x30: *out_mask = VERTEX_DWORDS_G3;  return 3;
			case 0x34: *out_mask = VERTEX_DWORDS_GT3; return 3;
			case 0x38: *out_mask = VERTEX_DWORDS_G4;  return 4;
			case 0x3C: *out_mask = VERTEX_DWORDS_GT4; return 4;
			case 0x60: *out_mask = VERTEX_DWORDS_XY;  return 1; // TILE, variable size   0x60-0x63
			case 0x64: *out_mask = VERTEX_DWORDS_XY;  return 1; // SPRT, variable size   0x64-0x67
			case 0x68: *out_mask = VERTEX_DWORDS_XY;  return 1; // TILE 1x1              0x68-0x6B
			case 0x6C: *out_mask = VERTEX_DWORDS_XY;  return 1; // SPRT 1x1              0x6C-0x6F
			case 0x70: *out_mask = VERTEX_DWORDS_XY;  return 1; // TILE 8x8              0x70-0x73
			case 0x74: *out_mask = VERTEX_DWORDS_XY;  return 1; // SPRT 8x8              0x74-0x77
			case 0x78: *out_mask = VERTEX_DWORDS_XY;  return 1; // TILE 16x16            0x78-0x7B
			case 0x7C: *out_mask = VERTEX_DWORDS_XY;  return 1; // SPRT 16x16            0x7C-0x7F
			// Lines are deliberately absent: LINE_F2 (0x40) puts its vertices at 2,3 but LINE_G2 (0x50)
			// at 2,4, and the poly-line forms are variable length terminated by 0x55555555, so one wrong
			// guess would drag a colour word through the vertex blender. Add them only with a dump.
			default:   *out_mask = nullptr; return 0;
			}
		}

		__forceinline int packed_x(uint32_t xy) { return (int16_t)(uint16_t)(xy & 0xFFFF); }
		__forceinline int packed_y(uint32_t xy) { return (int16_t)(uint16_t)(xy >> 16); }
		__forceinline uint32_t pack_xy(int x, int y) { return (uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)y << 16); }

		__forceinline uint8_t command_byte(uint32_t command_word) { return (uint8_t)(command_word >> 24); }
		__forceinline uint32_t command_type(uint32_t command_word) { return (command_word >> 24) & 0xFC; }

		__forceinline float wrapped_angle(float angle)
		{
			if (angle > 3.14159265f) angle -= 6.2831853f;
			if (angle < -3.14159265f) angle += 6.2831853f;
			return angle;
		}

		// Two int16 packed in one dword, blended separately. Both ends are int16 and t is clamped to
		// 0..1 by update_tick(), so the result can never leave the int16 range.
		inline uint32_t blend_xy(uint32_t from, uint32_t to, float t)
		{
			const int from_x = packed_x(from);
			const int from_y = packed_y(from);
			const int to_x = packed_x(to);
			const int to_y = packed_y(to);
			const int x = from_x + (int)lrintf(float(to_x - from_x) * t);
			const int y = from_y + (int)lrintf(float(to_y - from_y) * t);
			return pack_xy(x, y);
		}

		// Scratch packets. The ordering table keeps raw pointers into these until the frame is drawn
		// and the walker 0x45D080 dereferences them, so a block is never resized, moved or freed: a
		// grow allocates a new block and the old ones are simply kept. Not even fx_blend_reset()
		// releases them - see the note there.
		struct Scratch
		{
			std::vector<std::unique_ptr<uint32_t[]>> blocks; // [0..n-2] retired, back() is live
			uint32_t* words = nullptr;
			size_t packet_capacity = 0; // capacity in packets, each FX_PRIMITIVE_WORDS dwords
		};

		struct Slot
		{
			Scratch scratch;
			std::vector<uint8_t> use_scratch;     // per call: 1 = link the interpolated copy
			bool accepted = false;                // this host frame's pair was accepted
			uint32_t accepted_frame = 0xFFFFFFFF; // frame_counter of the accepted pair
		};
		Slot slots[FX_BLEND_SLOTS];

		// Reserve capacity for `packet_count` packets. Only ever grows, and only from fx_blend_prepare(),
		// which animations.cpp calls before the first link call of that list in this host frame.
		bool scratch_reserve(Scratch& scratch, size_t packet_count)
		{
			if (packet_count <= scratch.packet_capacity) return true;
			if (packet_count > FX_BLEND_MAX_PRIMITIVES) return false;
			size_t new_capacity = scratch.packet_capacity != 0 ? scratch.packet_capacity : FX_BLEND_FIRST_PRIMITIVES;
			while (new_capacity < packet_count)
				new_capacity *= 2;
			if (new_capacity > FX_BLEND_MAX_PRIMITIVES) new_capacity = FX_BLEND_MAX_PRIMITIVES;
			std::unique_ptr<uint32_t[]> block(new (std::nothrow) uint32_t[new_capacity * FX_PRIMITIVE_WORDS]);
			if (!block) return false;
			scratch.words = block.get();
			scratch.packet_capacity = new_capacity;
			scratch.blocks.push_back(std::move(block)); // the previous block stays alive, see above
			return true;
		}
		// --- diagnostics -------------------------------------------------------------------------
		enum FallbackReason
		{
			FALLBACK_OFF = 0,   // bit 512 set, or one of the two lists missing
			FALLBACK_EMPTY,     // nothing recorded this tick
			FALLBACK_NOVTX,     // not one primitive in the list has interpolatable vertices
			FALLBACK_NOPAIR,    // runs exist but none of them could be paired against the previous tick
			FALLBACK_ALLOC,     // scratch refused (longer than FX_BLEND_MAX_PRIMITIVES, or out of memory)
			FALLBACK_REASON_COUNT
		};
		const char* const FALLBACK_NAMES[FALLBACK_REASON_COUNT] = { "off", "empty", "novtx", "nopair", "alloc" };

		uint32_t stat_blended_lists = 0, stat_fallbacks = 0, stat_list_prims = 0, stat_window_start = 0;
		uint32_t stat_reasons[FALLBACK_REASON_COUNT] = {};
		uint32_t stat_no_copy = 0;        // entries with no usable packet copy
		uint32_t stat_passed_through = 0; // entries passed through: no primitive, or an unblendable command
		uint32_t blend_streak = 0;        // consecutive successful prepares, drives fx_blend_defer_ok()
		uint32_t blend_misses = 0;        // consecutive failures since the last success
		bool blend_armed = false;         // hysteresis state, see fx_blend_defer_ok()
		constexpr uint32_t FX_BLEND_ARM_LIMIT = 2;
		constexpr uint32_t FX_BLEND_MISS_LIMIT = 30; // ~3 logic ticks at 144 fps

		// Detail of the most recent list.
		int last_reason = FALLBACK_OFF;
		uint32_t last_runs_current = 0, last_runs_previous = 0, last_runs_paired = 0;
		uint32_t last_entries = 0, last_entries_blended = 0, last_address_delta = 0;

		// v66 diagnostics. `moved` answers the only question that matters once pairing works: do the
		// two recorded packets actually differ? If a whole list pairs and blends but nothing moved, the
		// geometry on screen is not coming from these packets. `alpha_*` answers the other one: is the
		// blend factor really sweeping 0..1 across the host frames of a logic period, or is it stuck?
		// All four accumulate over the whole trace second, i.e. over every host frame's rescan of the
		// list - divide by the frame rate to get a per-list figure.
		uint32_t stat_moved = 0;            // paired entries whose masked vertex dwords differ
		uint32_t stat_entries_blended = 0;  // paired entries actually written to scratch (the denominator)
		uint32_t stat_snapped = 0;          // paired entries rejected by FX_BLEND_SNAP
		uint32_t stat_incoherent = 0;       // v104: entries of runs drawn verbatim as incoherent (FX_RUN_INCOHERENT)
		uint32_t stat_proximity_paired = 0; // v108: entries paired by proximity
		uint32_t stat_identity_paired = 0, stat_identity_ambiguous = 0; // v81: is_volatile entries blended by identity / left verbatim (count mismatch)
		int32_t stat_max_move = 0;          // largest single-axis movement seen, in screen units
		float alpha_min = 2.0f, alpha_max = -1.0f;
		uint64_t alpha_seen = 0;            // bit k = an alpha in [k/64, (k+1)/64) was used
		// One paired packet, dumped so the field mask can be checked against the real bytes.
		uint8_t dump_command = 0;
		int dump_words = 0, dump_vertex_count = 0;
		uint8_t dump_vertex_word[4] = {};
		uint32_t dump_previous[4] = {}, dump_current[4] = {}, dump_blended[4] = {};
		// ...and the whole packet, both frames, so the field mask can be checked against the real
		// layout rather than against libgpu's documentation: a masked dword must read as a plausible
		// pair of signed screen coordinates, an unmasked one as a colour, a UV pair or a tpage word.
		uint32_t dump_packet_previous[12] = {}, dump_packet_current[12] = {};
		int dump_packet_words = 0;
		uint8_t histogram_command[8] = {};
		uint32_t histogram_count[8] = {};
		int histogram_used = 0;
		// Which game call sites produced the packets of the most recent list. This is the diagnostic
		// that says whether a given piece of an effect is drawn through the effect tree at all: a part
		// of the picture that steps while these packets blend is being drawn by something else, and its
		// emitter will simply not appear here.
		uint32_t site_address[6] = {};
		uint32_t site_count[6] = {};
		int site_used = 0;

		void site_add(uint32_t call_site)
		{
			for (int i = 0; i < site_used; i++)
			{
				if (site_address[i] == call_site)
				{
					site_count[i]++;
					return;
				}
			}
			if (site_used >= (int)(sizeof(site_address) / sizeof(site_address[0]))) return;
			site_address[site_used] = call_site;
			site_count[site_used] = 1;
			site_used++;
		}

		void histogram_add(uint8_t command)
		{
			for (int i = 0; i < histogram_used; i++)
			{
				if (histogram_command[i] == command)
				{
					histogram_count[i]++;
					return;
				}
			}
			if (histogram_used >= (int)(sizeof(histogram_command) / sizeof(histogram_command[0]))) return;
			histogram_command[histogram_used] = command;
			histogram_count[histogram_used] = 1;
			histogram_used++;
		}

		void trace_once_per_second()
		{
			if (!ff8_interp::cfg().trace) return;
			const uint32_t now = GetTickCount();
			if (stat_window_start == 0)
			{
				stat_window_start = now;
				return;
			}
			if (now - stat_window_start < 1000) return;
			ff8_interp::log_trace("ff8fx blend lists=%u fallback=%u prims=%u | off=%u empty=%u novtx=%u nopair=%u alloc=%u\n",
				stat_blended_lists, stat_fallbacks, stat_list_prims,
				stat_reasons[FALLBACK_OFF], stat_reasons[FALLBACK_EMPTY], stat_reasons[FALLBACK_NOVTX],
				stat_reasons[FALLBACK_NOPAIR], stat_reasons[FALLBACK_ALLOC]);
			ff8_interp::log_trace("ff8fx blend last=%s runs=%u/%u paired=%u ents=%u/%u nocopy=%u skip=%u delta=%08x | id=%u ambig=%u\n",
				FALLBACK_NAMES[last_reason], last_runs_current, last_runs_previous, last_runs_paired, last_entries_blended, last_entries,
				stat_no_copy, stat_passed_through, last_address_delta, stat_identity_paired, stat_identity_ambiguous);
			char commands[160];
			int commands_length = 0;
			for (int i = 0; i < histogram_used && commands_length < (int)sizeof(commands) - 16; i++)
				commands_length += snprintf(commands + commands_length, sizeof(commands) - (size_t)commands_length, "%02x:%u ", histogram_command[i], histogram_count[i]);
			commands[commands_length] = 0;
			ff8_interp::log_trace("ff8fx blend cmds %s\n", commands);
			char sites[192];
			int sites_length = 0;
			for (int i = 0; i < site_used && sites_length < (int)sizeof(sites) - 24; i++)
				sites_length += snprintf(sites + sites_length, sizeof(sites) - (size_t)sites_length, "%08x:%u ", site_address[i], site_count[i]);
			sites[sites_length] = 0;
			ff8_interp::log_trace("ff8fx blend sites %s\n", sites);
			int distinct_alphas = 0;
			for (int bucket = 0; bucket < 64; bucket++)
			{
				if (alpha_seen & (1ull << bucket)) distinct_alphas++;
			}
			ff8_interp::log_trace("ff8fx blend alpha min=%.3f max=%.3f distinct=%d | moved=%u/%u snapped=%u maxdelta=%d incoherent=%u nnpaired=%u\n",
				alpha_min > 1.0f ? -1.0f : alpha_min, alpha_max, distinct_alphas,
				stat_moved, stat_entries_blended, stat_snapped, stat_max_move, stat_incoherent, stat_proximity_paired);
			if (dump_vertex_count > 0)
			{
				char vertices[192];
				int vertices_length = 0;
				for (int v = 0; v < dump_vertex_count && vertices_length < (int)sizeof(vertices) - 40; v++)
					vertices_length += snprintf(vertices + vertices_length, sizeof(vertices) - (size_t)vertices_length, "[%u] %08x>%08x=%08x ",
						dump_vertex_word[v], dump_previous[v], dump_current[v], dump_blended[v]);
				vertices[vertices_length] = 0;
				ff8_interp::log_trace("ff8fx blend pkt cmd=%02x words=%d %s\n", dump_command, dump_words, vertices);
				char packet[224];
				int packet_length = 0;
				for (int w = 0; w < dump_packet_words && packet_length < (int)sizeof(packet) - 12; w++)
					packet_length += snprintf(packet + packet_length, sizeof(packet) - (size_t)packet_length, "%08x ", dump_packet_previous[w]);
				packet[packet_length] = 0;
				ff8_interp::log_trace("ff8fx blend pkt prev %s\n", packet);
				packet_length = 0;
				for (int w = 0; w < dump_packet_words && packet_length < (int)sizeof(packet) - 12; w++)
					packet_length += snprintf(packet + packet_length, sizeof(packet) - (size_t)packet_length, "%08x ", dump_packet_current[w]);
				packet[packet_length] = 0;
				ff8_interp::log_trace("ff8fx blend pkt cur  %s\n", packet);
			}
			alpha_min = 2.0f;
			alpha_max = -1.0f;
			alpha_seen = 0;
			stat_moved = 0;
			stat_entries_blended = 0;
			stat_snapped = 0;
			stat_incoherent = 0;
			stat_proximity_paired = 0;
			stat_max_move = 0;
			stat_identity_paired = stat_identity_ambiguous = 0;
			stat_blended_lists = 0;
			stat_fallbacks = 0;
			stat_no_copy = 0;
			stat_passed_through = 0;
			memset(stat_reasons, 0, sizeof(stat_reasons));
			stat_window_start = now;
		}

		bool fall_back(FallbackReason reason)
		{
			stat_fallbacks++;
			stat_reasons[reason]++;
			last_reason = reason;
			blend_streak = 0;
			if (++blend_misses >= FX_BLEND_MISS_LIMIT) blend_armed = false;
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
		struct RunEntry
		{
			uint32_t list_index; // index in the recorded list
			uint32_t key;
			uint32_t address;    // the primitive pointer the linker was called with
		};
		struct Run
		{
			uint32_t key;
			uint32_t first_entry;  // index of the run's first entry in the RunEntry array
			uint32_t count;
			uint32_t base_address; // address of the first entry
			int32_t stride;
		};
		__forceinline uint32_t run_list_index(const std::vector<RunEntry>& entries, const Run& run, uint32_t position)
		{
			return entries[run.first_entry + position].list_index;
		}
		__forceinline const FxCall& run_call(const FxCall* list, const std::vector<RunEntry>& entries, const Run& run, uint32_t position)
		{
			return list[entries[run.first_entry + position].list_index];
		}
		constexpr uint32_t NO_PAIR = 0xFFFFFFFF;
		// A paired packet that moved further than this in one logic tick is not the same particle
		// moving, it is a slot being reused (a particle died and another took its place) or a mispair
		// inside a run. Interpolating it draws a streak right across the picture, so it is issued
		// verbatim instead - the same "snap on a large delta" rule the entity positions and the battle
		// camera already use. The v66 log reported maxdelta=5023 screen units, which is exactly this.
		constexpr int FX_BLEND_SNAP = 2048;
		// v104: an emitter run where at least 1 in FX_RUN_INCOHERENT pairs teleports (and at least two do)
		// is not moving geometry but geometry rebuilt every tick in the same slots: Thundaga's bolts
		// (GT3 0x34 from 0x6D437B) are regenerated at random each tick, so pairing slot k with slot k
		// blends unrelated segments - the long stretched triangles at the end of the cast ("clipping").
		// The per-pair snap only caught the worst of them. Such a run is drawn as the engine drew it
		// (verbatim, stepped at 15 Hz) for that tick.
		constexpr uint32_t FX_RUN_INCOHERENT = 8;

		// v108: polygon centre + radius of a recorded packet (screen units), false when it has < 3 vertices.
		bool polygon_centre(const FxCall& call, float& centre_x, float& centre_y, float& radius)
		{
			const uint8_t* mask = nullptr;
			const int vertex_count = vertex_dwords(command_byte(call.copy[1]), &mask);
			if (vertex_count < 3 || mask[vertex_count - 1] >= call.words) return false;
			centre_x = centre_y = 0.0f;
			for (int v = 0; v < vertex_count; v++)
			{
				centre_x += packed_x(call.copy[mask[v]]);
				centre_y += packed_y(call.copy[mask[v]]);
			}
			centre_x /= vertex_count;
			centre_y /= vertex_count;
			radius = 0.0f;
			for (int v = 0; v < vertex_count; v++)
			{
				const float dx = packed_x(call.copy[mask[v]]) - centre_x;
				const float dy = packed_y(call.copy[mask[v]]) - centre_y;
				const float distance = sqrtf(dx * dx + dy * dy);
				if (distance > radius) radius = distance;
			}
			return true;
		}
		inline float far_limit(float radius) { return (4.0f * radius + 64.0f) > 128.0f ? (4.0f * radius + 64.0f) : 128.0f; }
		// v109: the tick before the previous one, for trajectories. A polygon that moves further than
		// far_limit in one tick is still the same piece when the tick before held a polygon of the same
		// emitter (call site, command type, size) at about "previous position minus this step": a chunk
		// in flight (Thundaga's dirt flies ~500 units a tick). Respawns and mispairs have no such
		// predecessor and stay stepped (v107).
		struct EarlierPolygon
		{
			uint64_t key;
			float x, y, radius;
		};
		std::vector<EarlierPolygon> polygons_two_ticks_ago;
		inline uint64_t polygon_key(const FxCall& call) { return ((uint64_t)call.call_site << 16) | ((call.copy[1] >> 24) & 0xFC) << 8 | (call.words & 0xFF); }
		void set_list_two_ticks_ago(const FxCall* list, size_t count)
		{
			polygons_two_ticks_ago.clear();
			if (list == nullptr || (ff8_interp::cfg().battle_debug2 & DBG2_FX_TRAJECTORY_OFF)) return;
			for (size_t i = 0; i < count; i++)
			{
				float x, y, radius;
				if (list[i].words >= 2 && polygon_centre(list[i], x, y, radius))
					polygons_two_ticks_ago.push_back({ polygon_key(list[i]), x, y, radius });
			}
		}
		bool trajectory_ok(const FxCall& call, float previous_x, float previous_y, float step_x, float step_y, float radius)
		{
			if (polygons_two_ticks_ago.empty()) return false;
			const uint64_t key = polygon_key(call);
			const float predicted_x = previous_x - step_x;
			const float predicted_y = previous_y - step_y;
			// v112: strict. v109 took 0.35 |v| (at least 2r + 32): among many smoke puffs some puff was
			// always near "previous position minus this step", so X-ATM092's Clash dust was paired from
			// the party to its feet again. Now: same emitter key, similar size (radius within 30 %), and
			// within 0.15 |v| (at least r / 2 + 16) - a ballistic chunk changes its step far less per tick.
			const float speed = sqrtf(step_x * step_x + step_y * step_y);
			float tolerance = 0.15f * speed;
			if (tolerance < 0.5f * radius + 16.0f) tolerance = 0.5f * radius + 16.0f;
			for (const EarlierPolygon& earlier : polygons_two_ticks_ago)
			{
				if (earlier.key == key
					&& earlier.radius > 0.7f * radius && earlier.radius < 1.3f * radius
					&& (earlier.x - predicted_x) * (earlier.x - predicted_x) + (earlier.y - predicted_y) * (earlier.y - predicted_y) <= tolerance * tolerance)
					return true;
			}
			return false;
		}
		// v110: a run whose polygons mostly share corners with each other is a connected surface (Thundaga's
		// ground ripple, G4 0x3A from 0x573003: 100 quads of one mesh). Its slots are stable, so list order
		// is the right pairing; proximity pairing (v108) paired quads with their neighbours and tore it.
		// v111: "shares a corner" alone also caught smoke puffs that spawn stacked on the same spot (all
		// four corners identical) - X-ATM092's head-attack smoke was list-paired again and jumped back to
		// the feet. A surface's polygons share SOME corners with a neighbour (an edge or a point), stacked
		// duplicates share all of them; only the first counts.
		// v113: a surface also keeps its neighbourhoods: quads t and u that share an edge/corner in this tick
		// shared it in the previous one too (same run positions). Clash's dust squares (FT4 0x2E,
		// 0x5721CF, ~130 units) often lie edge to edge as well, but their neighbours change every tick as
		// puffs come and go; v110/v111 took them for a surface, list-paired them, and the dust gathered at
		// X-ATM092's feet again. A run is a surface when at least half of its polygons have such a
		// stable neighbour.
		std::unordered_map<uint32_t, std::vector<uint32_t>> corner_owners; // corner -> polygons (run-local)
		std::unordered_map<uint32_t, int> shared_corners;                  // neighbour -> shared corners
		std::vector<std::vector<uint32_t>> links_previous;                 // per prev polygon: partial-share neighbours
		void run_links(const FxCall* list, const std::vector<RunEntry>& entries, const Run& run, std::vector<std::vector<uint32_t>>& links)
		{
			links.assign(run.count, std::vector<uint32_t>());
			corner_owners.clear();
			for (uint32_t i = 0; i < run.count; i++)
			{
				const FxCall& call = run_call(list, entries, run, i);
				const uint8_t* mask = nullptr;
				const int vertex_count = vertex_dwords(command_byte(call.copy[1]), &mask);
				if (vertex_count < 3 || mask[vertex_count - 1] >= call.words) continue;
				for (int v = 0; v < vertex_count; v++)
				{
					std::vector<uint32_t>& owners = corner_owners[call.copy[mask[v]]];
					if (owners.empty() || owners.back() != i) owners.push_back(i);
				}
			}
			for (uint32_t i = 0; i < run.count; i++)
			{
				const FxCall& call = run_call(list, entries, run, i);
				const uint8_t* mask = nullptr;
				const int vertex_count = vertex_dwords(command_byte(call.copy[1]), &mask);
				if (vertex_count < 3 || mask[vertex_count - 1] >= call.words) continue;
				shared_corners.clear();
				for (int v = 0; v < vertex_count; v++)
				{
					for (uint32_t owner : corner_owners[call.copy[mask[v]]])
					{
						if (owner != i) shared_corners[owner]++;
					}
				}
				for (const auto& neighbour_and_corners : shared_corners)
				{
					const uint32_t& neighbour = neighbour_and_corners.first;
					const int& corner_count = neighbour_and_corners.second;
					if (corner_count < vertex_count) links[i].push_back(neighbour);
				}
			}
		}
		std::vector<std::vector<uint32_t>> links_current;
		bool run_is_mesh(const FxCall* current, const std::vector<RunEntry>& current_entries, const Run& current_run,
		                 const FxCall* previous, const std::vector<RunEntry>& previous_entries, const Run& previous_run)
		{
			run_links(current, current_entries, current_run, links_current);
			run_links(previous, previous_entries, previous_run, links_previous);
			uint32_t stable_polygons = 0;
			const uint32_t polygon_count = current_run.count;
			for (uint32_t i = 0; i < current_run.count; i++)
			{
				if (i >= previous_run.count) continue;
				bool neighbour_kept = false;
				for (uint32_t neighbour : links_current[i])
				{
					for (uint32_t previous_neighbour : links_previous[i])
					{
						if (previous_neighbour == neighbour)
						{
							neighbour_kept = true;
							break;
						}
					}
					if (neighbour_kept) break;
				}
				if (neighbour_kept) stable_polygons++;
			}
			return polygon_count > 0 && stable_polygons * 2 >= polygon_count;
		}
		bool current_run_is_mesh = false; // v110: the run being blended is a connected surface (no per-polygon rules)
		struct ProximityCandidate
		{
			float distance;
			uint32_t current_position, previous_position;
		};
		std::vector<ProximityCandidate> proximity_candidates;
		std::vector<uint32_t> proximity_pair;
		std::vector<uint8_t> proximity_previous_used;

		// Reused between frames so a 200-packet list does not allocate four vectors per host frame.
		// None of these is ever pointed at by the ordering table, so resizing them is free of the
		// hazard that governs the scratch packets.
		std::vector<RunEntry> entries_current, entries_previous;
		std::vector<Run> runs_current, runs_previous;
		std::vector<uint32_t> run_pair;

		inline uint32_t entry_key(const FxCall& call)
		{
			return (uint32_t)call.linker
				| ((uint32_t)call.words << 4)
				| ((uint32_t)command_type(call.copy[1]) << 12);
		}

		// Cut one recorded list into runs. `collect_stats` is set for the current list only, so the
		// counters and the command histogram describe one tick and not two.
		void build_runs(const FxCall* list, size_t count, std::vector<RunEntry>& entries, std::vector<Run>& runs, bool collect_stats)
		{
			entries.clear();
			runs.clear();
			for (size_t i = 0; i < count; i++)
			{
				const FxCall& call = list[i];
				// 0x45C860 carries no primitive; it is re-issued verbatim and in place, which is what
				// keeps it patching the node its predecessor created.
				if (!linker_blends(call.linker))
				{
					if (collect_stats) stat_passed_through++;
					continue;
				}
				// v79: never blend a packet from per-frame engine memory (FxCall::is_volatile). Those are entity-model
				// redraws inside a spell tree (Sleep 0x619B70, Cure 0x8D7976 ...): the emitter advances its
				// packet pointer only for FRONT-FACING triangles (GTE NCLIP > 0) and inserts an optional G3
				// overlay after some of them, so the array is compacted differently on every tick and index
				// k is not the same triangle on two ticks. Pairing by index interpolated vertices of unrelated
				// triangles - the v78 "target's triangles vanish briefly". They replay verbatim from the
				// stable copy (the tick's pose, complete), like any unpaired entry.
				// v81: they are paired by face identity instead (build_identities / DBG2_FX_VOLATILE_BLEND_OFF).
				if (call.is_volatile)
				{
					if (collect_stats) stat_passed_through++;
					continue;
				}
				if (call.words < 2)
				{
					if (collect_stats) stat_no_copy++;
					continue;
				}
				const uint8_t command = command_byte(call.copy[1]);
				if (collect_stats)
				{
					histogram_add(command);
					site_add(call.call_site);
				}
				const uint8_t* mask = nullptr;
				const int vertex_count = vertex_dwords(command, &mask);
				if (vertex_count == 0 || mask[vertex_count - 1] >= call.words)
				{
					if (collect_stats) stat_passed_through++;
					continue;
				}

				const RunEntry entry = { (uint32_t)i, entry_key(call), call.args[1] };
				bool extended = false;
				if (!runs.empty())
				{
					Run& run = runs.back();
					if (run.key == entry.key)
					{
						if (run.count == 1)
						{
							const int32_t stride = (int32_t)(entry.address - run.base_address);
							if (stride != 0)
							{
								run.stride = stride;
								run.count = 2;
								extended = true;
							}
						}
						else if (run.base_address + (uint32_t)((int32_t)run.count * run.stride) == entry.address)
						{
							run.count++;
							extended = true;
						}
					}
				}
				if (!extended)
				{
					const Run run = { entry.key, (uint32_t)entries.size(), 1, entry.address, 0 };
					runs.push_back(run);
				}
				entries.push_back(entry);
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

	namespace
	{
		// v81: identity pairing for packets in per-frame engine memory (FxCall::is_volatile). Those are entity
		// models that a spell tree redraws itself (Sleep 0x619770 -> 0x619B70, Cure's emitter at
		// 0x8D7976 ...): the emitter walks the model's faces in face order but advances its output only
		// for front-facing ones (NCLIP > 0) and may insert an untextured overlay (G3 0x32 / G4 0x3A) right
		// after a face, so list index k is not the same face on two ticks (v79). A face is identified
		// instead by what does not move: the emitter call site, the command type and its texture words
		// (UV+CLUT, UV+tpage, UV2[, UV3]). Faces that share all of that (mirrored limbs) are told apart by
		// their order, which is the face order - but only when BOTH ticks have the same number of them;
		// otherwise every face of that key is left verbatim. An overlay inherits the key of the face it
		// directly follows. Untextured packets that follow no face (the 0x5088A0 shadow) key on call site
		// + type, again only with equal counts. Every pair still goes through the FX_BLEND_SNAP guard.
		struct IdentityEntry
		{
			uint64_t base_key;
			uint64_t full_key;
			uint32_t list_index;
		};
		std::vector<IdentityEntry> identities_current, identities_previous;
		std::unordered_map<uint64_t, uint32_t> identity_count_current, identity_count_previous; // base key -> count
		std::unordered_map<uint64_t, uint32_t> identity_previous_index;                        // full key -> prev index

		inline uint64_t mix(uint64_t hash, uint64_t value)
		{
			hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
			return hash * 0xFF51AFD7ED558CCDull;
		}
		// Texture words by packet layout; 0 = untextured.
		int texture_words(uint32_t type, int* offsets)
		{
			switch (type)
			{
			case 0x24: offsets[0] = 3; offsets[1] = 5; offsets[2] = 7; return 3;                   // FT3
			case 0x2C: offsets[0] = 3; offsets[1] = 5; offsets[2] = 7; offsets[3] = 9; return 4;  // FT4
			case 0x34: offsets[0] = 3; offsets[1] = 6; offsets[2] = 9; return 3;                   // GT3
			case 0x3C: offsets[0] = 3; offsets[1] = 6; offsets[2] = 9; offsets[3] = 12; return 4; // GT4
			default: return 0;
			}
		}
		void build_identities(const FxCall* list, size_t count, std::vector<IdentityEntry>& identities, std::unordered_map<uint64_t, uint32_t>& counts)
		{
			identities.clear();
			counts.clear();
			uint64_t face_key = 0;        // key of the face the previous entry was, 0 = none
			for (size_t i = 0; i < count; i++)
			{
				const FxCall& call = list[i];
				const uint64_t previous_face_key = face_key;
				face_key = 0;
				if (!call.is_volatile || !linker_blends(call.linker) || call.words < 2) continue;
				const uint32_t type = command_type(call.copy[1]);
				const uint8_t* mask = nullptr;
				const int vertex_count = vertex_dwords((uint8_t)type, &mask);
				if (vertex_count == 0 || mask[vertex_count - 1] >= call.words) continue;
				uint64_t base_key = mix(mix(mix(0x51ED270B27A0C1F3ull, call.call_site), type), (uint64_t)call.linker << 8 | (uint64_t)call.words);
				int texture_offsets[4];
				const int texture_count = texture_words(type, texture_offsets);
				bool textures_fit = true;
				for (int k = 0; k < texture_count; k++)
				{
					if (texture_offsets[k] >= call.words) textures_fit = false;
				}
				if (texture_count > 0 && textures_fit)
				{
					for (int k = 0; k < texture_count; k++)
						base_key = mix(base_key, call.copy[texture_offsets[k]]);
					face_key = base_key;
				}
				else if (previous_face_key != 0)
					base_key = mix(mix(previous_face_key, 0x0E7E41A7ull), type); // overlay of the face just before it
				const uint32_t occurrence = counts[base_key]++;
				identities.push_back({ base_key, mix(base_key, occurrence), (uint32_t)i });
			}
		}
	}

	bool fx_blend_prepare(int slot, const FxCall* previous, size_t previous_count, const FxCall* current, size_t current_count, float alpha)
	{
		if (slot < 0 || slot >= FX_BLEND_SLOTS) return false;
		Slot& state = slots[slot];
		// Idempotent within one host frame: the linker patches word 0 of every packet it links, so
		// rebuilding a scratch packet that is already in this frame's ordering table would clobber the
		// chain pointer it wrote there.
		if (state.accepted && state.accepted_frame == ff8_interp::frame_counter()) return true;
		state.accepted = false;
		rectangles_off = (ff8_interp::cfg().battle_debug & DBG_FX_RECTANGLE_OFF) != 0;
		stat_list_prims = (uint32_t)current_count;
		if (!fx_blend_enabled() || previous == nullptr || current == nullptr) return fall_back(FALLBACK_OFF);
		if (current_count == 0) return fall_back(FALLBACK_EMPTY);

		if (alpha < alpha_min) alpha_min = alpha;
		if (alpha > alpha_max) alpha_max = alpha;
		{
			int bucket = (int)(alpha * 64.0f);
			if (bucket < 0) bucket = 0;
			if (bucket > 63) bucket = 63;
			alpha_seen |= 1ull << bucket;
		}
		histogram_used = 0;
		site_used = 0;
		last_address_delta = 0;
		dump_vertex_count = 0;
		build_runs(current, current_count, entries_current, runs_current, true);
		build_runs(previous, previous_count, entries_previous, runs_previous, false);
		last_runs_current = (uint32_t)runs_current.size();
		last_runs_previous = (uint32_t)runs_previous.size();
		last_entries = (uint32_t)entries_current.size();
		last_runs_paired = 0;
		last_entries_blended = 0;
		const bool identity_pairing_on = !(ff8_interp::cfg().battle_debug2 & DBG2_FX_VOLATILE_BLEND_OFF);
		identities_current.clear();
		identities_previous.clear();
		if (identity_pairing_on)
		{
			build_identities(current, current_count, identities_current, identity_count_current);
			build_identities(previous, previous_count, identities_previous, identity_count_previous);
		}
		if (runs_current.empty() && identities_current.empty()) return fall_back(FALLBACK_NOVTX);
		if (!scratch_reserve(state.scratch, current_count)) return fall_back(FALLBACK_ALLOC);

		// Pair run i of the current list with the i-th previous run carrying the same key. Runs are
		// few (one per emitter call), so the quadratic scan is cheaper than building a map.
		run_pair.assign(runs_current.size(), NO_PAIR);
		for (size_t i = 0; i < runs_current.size(); i++)
		{
			uint32_t occurrence = 0;
			for (size_t j = 0; j < i; j++)
			{
				if (runs_current[j].key == runs_current[i].key) occurrence++;
			}
			uint32_t seen = 0;
			for (size_t j = 0; j < runs_previous.size(); j++)
			{
				if (runs_previous[j].key != runs_current[i].key) continue;
				if (seen == occurrence)
				{
					run_pair[i] = (uint32_t)j;
					break;
				}
				seen++;
			}
		}

		state.use_scratch.assign(current_count, 0);
		// One pair (current entry current_index, previous entry previous_index) -> this frame's
		// interpolated scratch packet.
		auto blend_one = [&](size_t current_index, size_t previous_index) -> bool
		{
				const FxCall& current_call = current[current_index];
				const FxCall& previous_call = previous[previous_index];
				// The key already fixes the linker, the word count and the command type, so the mask
				// below is valid for both sides; this only guards against a truncated packet.
				const uint8_t* mask = nullptr;
				const int vertex_count = vertex_dwords(command_byte(current_call.copy[1]), &mask);
				if (vertex_count == 0 || mask[vertex_count - 1] >= current_call.words || current_call.words != previous_call.words) return false;
				uint32_t* out_packet = state.scratch.words + current_index * FX_PRIMITIVE_WORDS;
				// Start from the CURRENT packet, so the tag, the command word and everything the mask
				// does not name (colour, UVs, CLUT, tpage, packed RGB) are this logic frame's values,
				// then move the vertices back toward the previous frame by 1 - alpha.
				// Reject the pair before writing anything if any vertex teleported: a reused slot or a
				// mispair inside a run would otherwise be drawn as a streak across the screen.
				bool moved = false;
				bool snap = false;
				int worst_move = 0;
				for (int v = 0; v < vertex_count; v++)
				{
					const int word = mask[v];
					if (previous_call.copy[word] == current_call.copy[word]) continue;
					moved = true;
					const int dx = packed_x(current_call.copy[word]) - packed_x(previous_call.copy[word]);
					const int dy = packed_y(current_call.copy[word]) - packed_y(previous_call.copy[word]);
					const int abs_dx = dx < 0 ? -dx : dx;
					const int abs_dy = dy < 0 ? -dy : dy;
					const int axis_move = abs_dx > abs_dy ? abs_dx : abs_dy;
					if (axis_move > worst_move) worst_move = axis_move;
					if (axis_move > FX_BLEND_SNAP) snap = true;
				}
				if (worst_move > stat_max_move) stat_max_move = worst_move;
				// v107: a polygon that moves many times its own size in one tick is not a particle moving
				// but one that just appeared (a reused slot) or flew further than any in-between can show:
				// Thundaga's last dirt chunks (~25 units across) jump ~500 per tick. Blended, they slid
				// across the screen "super fast and distorted"; they are drawn as the engine drew them.
				if (!snap && vertex_count >= 3 && !current_run_is_mesh && !(ff8_interp::cfg().battle_debug2 & DBG2_FX_FAR_MOVE_OFF))
				{
					float previous_centre_x = 0, previous_centre_y = 0;
					float current_centre_x = 0, current_centre_y = 0;
					for (int v = 0; v < vertex_count; v++)
					{
						const int word = mask[v];
						previous_centre_x += packed_x(previous_call.copy[word]);
						previous_centre_y += packed_y(previous_call.copy[word]);
						current_centre_x += packed_x(current_call.copy[word]);
						current_centre_y += packed_y(current_call.copy[word]);
					}
					previous_centre_x /= vertex_count;
					previous_centre_y /= vertex_count;
					current_centre_x /= vertex_count;
					current_centre_y /= vertex_count;
					float radius = 0.0f;
					for (int v = 0; v < vertex_count; v++)
					{
						const int word = mask[v];
						const float dx = packed_x(current_call.copy[word]) - current_centre_x;
						const float dy = packed_y(current_call.copy[word]) - current_centre_y;
						const float distance = sqrtf(dx * dx + dy * dy);
						if (distance > radius) radius = distance;
					}
					const float step_x = current_centre_x - previous_centre_x;
					const float step_y = current_centre_y - previous_centre_y;
					const float step_length = sqrtf(step_x * step_x + step_y * step_y);
					if (step_length > 128.0f && step_length > 4.0f * radius + 64.0f
						&& !trajectory_ok(current_call, previous_centre_x, previous_centre_y, step_x, step_y, radius))
						snap = true;
				}
				if (snap)
				{
					stat_snapped++;
					return false;
				}
				memcpy(out_packet, current_call.copy, size_t(current_call.words) * 4);
				// v106: a polygon (3-4 vertices) is blended as a shape: its centre moves on a straight
				// line, each corner turns about the centre the short way and its distance to the centre is
				// lerped. Thundaga's flying dirt (GT3 0x34 from 0x6D437B) tumbles fast; lerping each corner
				// on its own cut the corners across the middle and collapsed / warped the chunks between
				// ticks. Without rotation this is the plain lerp (same result as before).
				bool rigid = vertex_count >= 3 && !current_run_is_mesh && !(ff8_interp::cfg().battle_debug2 & DBG2_FX_RIGID_OFF);
				float previous_centre_x = 0, previous_centre_y = 0;
				float current_centre_x = 0, current_centre_y = 0;
				if (rigid)
				{
					for (int v = 0; v < vertex_count; v++)
					{
						const int word = mask[v];
						previous_centre_x += packed_x(previous_call.copy[word]);
						previous_centre_y += packed_y(previous_call.copy[word]);
						current_centre_x += packed_x(current_call.copy[word]);
						current_centre_y += packed_y(current_call.copy[word]);
					}
					previous_centre_x /= vertex_count;
					previous_centre_y /= vertex_count;
					current_centre_x /= vertex_count;
					current_centre_y /= vertex_count;
					// only polygons that really turn: shared corners of a model's faces stay on the plain
					// lerp (per-face turning would open cracks), tumbling debris turns far more than this
					float turn = 0.0f;
					for (int v = 0; v < vertex_count; v++)
					{
						const int word = mask[v];
						const float previous_offset_x = packed_x(previous_call.copy[word]) - previous_centre_x;
						const float previous_offset_y = packed_y(previous_call.copy[word]) - previous_centre_y;
						const float current_offset_x = packed_x(current_call.copy[word]) - current_centre_x;
						const float current_offset_y = packed_y(current_call.copy[word]) - current_centre_y;
						const float angle_delta = wrapped_angle(atan2f(current_offset_y, current_offset_x) - atan2f(previous_offset_y, previous_offset_x));
						turn += fabsf(angle_delta);
					}
					if (turn / vertex_count < 0.349f) rigid = false; // 20 degrees
				}
				for (int v = 0; v < vertex_count; v++)
				{
					const int word = mask[v];
					if (!rigid)
					{
						out_packet[word] = blend_xy(previous_call.copy[word], current_call.copy[word], alpha);
						continue;
					}
					const float previous_offset_x = packed_x(previous_call.copy[word]) - previous_centre_x;
					const float previous_offset_y = packed_y(previous_call.copy[word]) - previous_centre_y;
					const float current_offset_x = packed_x(current_call.copy[word]) - current_centre_x;
					const float current_offset_y = packed_y(current_call.copy[word]) - current_centre_y;
					const float previous_length = sqrtf(previous_offset_x * previous_offset_x + previous_offset_y * previous_offset_y);
					const float current_length = sqrtf(current_offset_x * current_offset_x + current_offset_y * current_offset_y);
					float offset_x, offset_y;
					if (previous_length < 0.5f || current_length < 0.5f)
					{
						offset_x = previous_offset_x + (current_offset_x - previous_offset_x) * alpha;
						offset_y = previous_offset_y + (current_offset_y - previous_offset_y) * alpha;
					}
					else
					{
						const float previous_angle = atan2f(previous_offset_y, previous_offset_x);
						const float angle_delta = wrapped_angle(atan2f(current_offset_y, current_offset_x) - previous_angle);
						const float angle = previous_angle + angle_delta * alpha;
						const float length = previous_length + (current_length - previous_length) * alpha;
						offset_x = cosf(angle) * length;
						offset_y = sinf(angle) * length;
					}
					const int x = (int)lrintf(previous_centre_x + (current_centre_x - previous_centre_x) * alpha + offset_x);
					const int y = (int)lrintf(previous_centre_y + (current_centre_y - previous_centre_y) * alpha + offset_y);
					out_packet[word] = pack_xy(x, y);
				}
				if (moved) stat_moved++;
				if (dump_vertex_count == 0)
				{
					// One paired packet per host frame, so the field mask can be checked against the
					// bytes the effect really wrote.
					dump_command = command_byte(current_call.copy[1]);
					dump_words = current_call.words;
					dump_vertex_count = vertex_count < 4 ? vertex_count : 4;
					for (int v = 0; v < dump_vertex_count; v++)
					{
						dump_vertex_word[v] = mask[v];
						dump_previous[v] = previous_call.copy[mask[v]];
						dump_current[v] = current_call.copy[mask[v]];
						dump_blended[v] = out_packet[mask[v]];
					}
					dump_packet_words = current_call.words < 12 ? current_call.words : 12;
					for (int w = 0; w < dump_packet_words; w++)
					{
						dump_packet_previous[w] = previous_call.copy[w];
						dump_packet_current[w] = current_call.copy[w];
					}
				}
				state.use_scratch[current_index] = 1;
				last_entries_blended++;
				stat_entries_blended++;
				return true;
		};
		for (size_t run_index = 0; run_index < runs_current.size(); run_index++)
		{
			if (run_pair[run_index] == NO_PAIR) continue;
			const Run& current_run = runs_current[run_index];
			const Run& previous_run = runs_previous[run_pair[run_index]];
			last_runs_paired++;
			if (last_address_delta == 0) last_address_delta = current_run.base_address - previous_run.base_address;
			const uint32_t common_count = current_run.count < previous_run.count ? current_run.count : previous_run.count;
			// v108: pair by proximity when the list order does not fit. X-ATM092's head attack: the smoke
			// emitter's puffs at its legs and around the flung party share one run; as puffs come and go
			// the order shifts and slot k paired a puff at the legs with one at the player - blended, the
			// smoke jumped between them. If more than 1 in 16 index pairs move further than far_limit,
			// every current puff takes the nearest free previous puff within its far_limit instead
			// (closest pairs first); the rest is drawn as stored.
			current_run_is_mesh = run_is_mesh(current, entries_current, current_run, previous, entries_previous, previous_run);
			if (!(ff8_interp::cfg().battle_debug2 & DBG2_FX_PROXIMITY_PAIR_OFF) && current_run.count > 1 && previous_run.count > 1 && !current_run_is_mesh)
			{
				uint32_t far_pairs = 0, checked_pairs = 0;
				for (uint32_t i = 0; i < common_count; i++)
				{
					float current_x, current_y, current_radius;
					float previous_x, previous_y, previous_radius;
					if (!polygon_centre(run_call(current, entries_current, current_run, i), current_x, current_y, current_radius)
						|| !polygon_centre(run_call(previous, entries_previous, previous_run, i), previous_x, previous_y, previous_radius))
						continue;
					checked_pairs++;
					if (sqrtf((current_x - previous_x) * (current_x - previous_x) + (current_y - previous_y) * (current_y - previous_y)) > far_limit(current_radius)
						&& !trajectory_ok(run_call(current, entries_current, current_run, i), previous_x, previous_y, current_x - previous_x, current_y - previous_y, current_radius))
						far_pairs++;
				}
				if (checked_pairs > 0 && far_pairs * 16 > checked_pairs)
				{
					proximity_candidates.clear();
					for (uint32_t i = 0; i < current_run.count; i++)
					{
						float current_x, current_y, current_radius;
						if (!polygon_centre(run_call(current, entries_current, current_run, i), current_x, current_y, current_radius)) continue;
						const float limit = far_limit(current_radius);
						for (uint32_t j = 0; j < previous_run.count; j++)
						{
							float previous_x, previous_y, previous_radius;
							if (!polygon_centre(run_call(previous, entries_previous, previous_run, j), previous_x, previous_y, previous_radius)) continue;
							const float distance = sqrtf((current_x - previous_x) * (current_x - previous_x) + (current_y - previous_y) * (current_y - previous_y));
							if (distance <= limit) proximity_candidates.push_back({ distance, i, j }); // v112: no trajectory here (v109 let dust pair across the scene)
						}
					}
					std::sort(proximity_candidates.begin(), proximity_candidates.end(), [](const ProximityCandidate& a, const ProximityCandidate& b) { return a.distance < b.distance; });
					proximity_pair.assign(current_run.count, NO_PAIR);
					proximity_previous_used.assign(previous_run.count, 0);
					for (const ProximityCandidate& candidate : proximity_candidates)
					{
						if (proximity_pair[candidate.current_position] == NO_PAIR && !proximity_previous_used[candidate.previous_position])
						{
							proximity_pair[candidate.current_position] = candidate.previous_position;
							proximity_previous_used[candidate.previous_position] = 1;
						}
					}
					for (uint32_t i = 0; i < current_run.count; i++)
					{
						if (proximity_pair[i] != NO_PAIR)
						{
							blend_one(run_list_index(entries_current, current_run, i), run_list_index(entries_previous, previous_run, proximity_pair[i]));
							stat_proximity_paired++;
						}
					}
					continue;
				}
			}
			if (!(ff8_interp::cfg().battle_debug2 & DBG2_FX_COHERENCE_OFF))
			{
				uint32_t snapped_pairs = 0;
				for (uint32_t i = 0; i < common_count; i++)
				{
					const FxCall& current_call = run_call(current, entries_current, current_run, i);
					const FxCall& previous_call = run_call(previous, entries_previous, previous_run, i);
					const uint8_t* mask = nullptr;
					const int vertex_count = vertex_dwords(command_byte(current_call.copy[1]), &mask);
					if (vertex_count == 0 || mask[vertex_count - 1] >= current_call.words || current_call.words != previous_call.words) continue;
					for (int v = 0; v < vertex_count; v++)
					{
						const int word = mask[v];
						const int dx = packed_x(current_call.copy[word]) - packed_x(previous_call.copy[word]);
						const int dy = packed_y(current_call.copy[word]) - packed_y(previous_call.copy[word]);
						if (dx > FX_BLEND_SNAP || dx < -FX_BLEND_SNAP || dy > FX_BLEND_SNAP || dy < -FX_BLEND_SNAP)
						{
							snapped_pairs++;
							break;
						}
					}
				}
				if (snapped_pairs >= 2 && snapped_pairs * FX_RUN_INCOHERENT >= common_count) // verbatim this tick
				{
					stat_incoherent += common_count;
					continue;
				}
			}
			for (uint32_t i = 0; i < common_count; i++)
				blend_one(run_list_index(entries_current, current_run, i), run_list_index(entries_previous, previous_run, i));
		}
		// v81: per-frame-memory packets (entity model redraws) by stable identity, see build_identities().
		current_run_is_mesh = true; // v110: model faces (identity pairing) are one connected surface
		if (!identities_current.empty() && !identities_previous.empty())
		{
			identity_previous_index.clear();
			for (const IdentityEntry& entry : identities_previous)
				identity_previous_index[entry.full_key] = entry.list_index;
			for (const IdentityEntry& entry : identities_current)
			{
				auto previous_match = identity_previous_index.find(entry.full_key);
				if (previous_match == identity_previous_index.end()) continue;
				auto count_in_current = identity_count_current.find(entry.base_key);
				auto count_in_previous = identity_count_previous.find(entry.base_key);
				if (count_in_previous == identity_count_previous.end() || count_in_current->second != count_in_previous->second)
				{
					stat_identity_ambiguous++;
					continue;
				}
				if (blend_one(entry.list_index, previous_match->second)) stat_identity_paired++;
			}
		}
		// Nothing paired: every emitter is new this tick, so there is nothing to interpolate from.
		if (last_entries_blended == 0) return fall_back(FALLBACK_NOPAIR);

		state.accepted = true;
		state.accepted_frame = ff8_interp::frame_counter();
		stat_blended_lists++;
		blend_misses = 0;
		if (++blend_streak >= FX_BLEND_ARM_LIMIT) blend_armed = true;
		trace_once_per_second();
		return true;
	}

	void fx_blend_set_two_ticks_ago(const FxCall* list, size_t count) { set_list_two_ticks_ago(list, count); }

	uint32_t fx_blend_arg(int slot, size_t call_index)
	{
		if (slot < 0 || slot >= FX_BLEND_SLOTS) return 0;
		const Slot& state = slots[slot];
		if (!state.accepted || state.accepted_frame != ff8_interp::frame_counter() || call_index >= state.use_scratch.size() || state.use_scratch[call_index] == 0) return 0;
		return (uint32_t)(uintptr_t)(state.scratch.words + call_index * FX_PRIMITIVE_WORDS);
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
			slots[i].accepted = false;
			slots[i].accepted_frame = 0xFFFFFFFF;
			slots[i].use_scratch.clear();
			// The scratch blocks themselves deliberately survive a reset. animations_reset() runs from
			// the once-per-frame driver hook on a battle mode change, and freeing a block there would be
			// the only moment in this file where memory an ordering table may still name goes away - the
			// exact shape of the v25 use-after-free. They are bounded (one block per growth step, at most
			// FX_BLEND_MAX_PRIMITIVES packets) and the next battle reuses them without reallocating.
		}
		stat_blended_lists = 0;
		stat_fallbacks = 0;
		stat_list_prims = 0;
		stat_window_start = 0;
		stat_no_copy = 0;
		stat_passed_through = 0;
		blend_streak = 0;
		blend_misses = 0;
		blend_armed = false;
		memset(stat_reasons, 0, sizeof(stat_reasons));
		last_reason = FALLBACK_OFF;
		last_runs_current = last_runs_previous = last_runs_paired = 0;
		last_entries = last_entries_blended = last_address_delta = 0;
		stat_moved = 0;
		stat_entries_blended = 0;
		stat_snapped = 0;
		stat_incoherent = 0;
		stat_proximity_paired = 0;
		stat_max_move = 0;
		stat_identity_paired = stat_identity_ambiguous = 0;
		alpha_min = 2.0f;
		alpha_max = -1.0f;
		alpha_seen = 0;
		dump_vertex_count = 0;
		dump_packet_words = 0;
		histogram_used = 0;
		site_used = 0;
		entries_current.clear();
		entries_previous.clear();
		runs_current.clear();
		runs_previous.clear();
		run_pair.clear();
	}
}
