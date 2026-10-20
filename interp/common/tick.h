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

// Real-time frame delta shared by the battle, world map and field logic gates.
//
// The engines run their logic at a fixed rate (15 fps in battle, 30 elsewhere) and we render in
// between. The gate therefore has to follow the WALL CLOCK, not the configured frame rate: the
// ff8_*_fps options are only a cap, and the host routinely presents fewer frames than that (a 60 Hz
// display, a stream, a heavy scene). Counting host frames instead made the logic run at
// fps_actual / fps_configured of its proper speed, i.e. slow motion on a 60 Hz screen configured
// for 144.

#include "../host.h"

#include <windows.h>
#include <stdint.h>

namespace ff8_tick
{
	struct ClockState
	{
		LARGE_INTEGER counter_frequency{};
		LARGE_INTEGER last_counter{};
		uint32_t last_frame = 0xFFFFFFFF;
		double cached_delta = 0.0;
	};
	inline ClockState& clock_state()
	{
		static ClockState state;
		return state;
	}

	// Seconds since the previous presented frame, computed once per frame_counter and shared by
	// every gate, so all modules see the same interval.
	inline double frame_delta()
	{
		ClockState& state = clock_state();
		if (state.last_frame == ff8_interp::frame_counter()) return state.cached_delta;
		state.last_frame = ff8_interp::frame_counter();
		if (state.counter_frequency.QuadPart == 0) QueryPerformanceFrequency(&state.counter_frequency);
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if (state.last_counter.QuadPart == 0 || state.counter_frequency.QuadPart == 0)
		{
			state.last_counter = now;
			state.cached_delta = 0.0;
			return state.cached_delta;
		}
		double dt = double(now.QuadPart - state.last_counter.QuadPart) / double(state.counter_frequency.QuadPart);
		state.last_counter = now;
		if (dt < 0.0) dt = 0.0;
		if (dt > 0.25) dt = 0.25; // a load or a pause must not release a burst of logic ticks
		state.cached_delta = dt * ff8_interp::speed(); // game time: the speedhack runs the logic faster
		return state.cached_delta;
	}

	inline void clock_reset()
	{
		ClockState& state = clock_state();
		state.last_counter.QuadPart = 0;
		state.last_frame = 0xFFFFFFFF;
		state.cached_delta = 0.0;
	}
}
