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

// Character model pose interpolation shared by the world map and field modules.
// Both engines decode one packed pose per model instance per frame (frame = counter >> 4)
// into bone records and build the skeletons; the instance layout and the packed format are
// identical (world routine 0x653DA0 over table 0x25030D8, field routine 0x533CD0 over table
// 0x1DCB340).

#include <stdint.h>
#include <vector>
#include <unordered_map>

namespace ff8_chara
{
	typedef void (__cdecl *animate_fn)(int, int);

	constexpr int MAX_FRAMES_AHEAD = 6; // frames decoded ahead per tick (multi-frame advance, repeated poses)
	struct CharaPose
	{
		bool valid = false;
		uint8_t* animation = nullptr;            // animation the poses belong to
		uint16_t frame = 0;                      // frame f of poses[0]
		uint16_t counter = 0;                    // counter (frame << 4 | sub) at the last tick
		int counter_delta = 16;                  // counter advance per tick measured on the last tick (16 = one frame)
		int pose_count = 0;                      // poses[0..pose_count-1] valid, consecutive frames from f
		int hold_ticks = 1;                      // ticks the decoded content has been unchanged (repeated poses)
		int next_distinct_pose = 0;              // first j with poses[j] != poses[0] (0 = none within MAX_FRAMES_AHEAD)
		std::vector<uint8_t> poses[MAX_FRAMES_AHEAD + 1]; // 8 root bytes + 8 bytes per bone (dword pairs as the decoder stores them)
		std::vector<uint8_t> previous_tick_pose; // poses[0] of the previous tick
		std::vector<uint8_t> shown;              // pose displayed on the last host frame (cross-fade source)
		std::vector<uint8_t> scratch;
		std::vector<uint8_t> feed;               // field v13: scratch in the engine's raw read layout (skipped bones 4 bytes)
		int fed_skipped_bones = -1;              // trace: skipped bones in the last feed
	};

	struct PoseInterp
	{
		uint8_t** instance_table = nullptr; // 32 instance pointers
		static constexpr int SLOTS = 32;
		CharaPose characters[SLOTS];
		// Clips that were observed to wrap (counter decreased with the same clip). Until a clip has
		// wrapped once we never blend past its last frame: non-looping clips hold their last frame.
		std::unordered_map<uint8_t*, bool> clip_loops;

		void reset();
		// Wraps the engine's animate call: on a logic tick decodes the current and lookahead frames
		// of every active instance; every host frame installs the blended raw pose and calls `original`
		// exactly once (the routines submit the model / flip buffers), then restores the instance.
		void animate(animate_fn original, int arg1, int arg2, bool is_logic_tick, float alpha, uint32_t tick_number);
	};
}
