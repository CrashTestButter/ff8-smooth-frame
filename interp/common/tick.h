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
		LARGE_INTEGER freq{};
		LARGE_INTEGER last{};
		uint32_t last_frame = 0xFFFFFFFF;
		double cached = 0.0;
	};
	inline ClockState& clock_state()
	{
		static ClockState s;
		return s;
	}

	// Seconds since the previous presented frame, computed once per frame_counter and shared by
	// every gate, so all modules see the same interval.
	inline double frame_delta()
	{
		ClockState& s = clock_state();
		if (s.last_frame == ff8_interp::frame_counter()) return s.cached;
		s.last_frame = ff8_interp::frame_counter();
		if (s.freq.QuadPart == 0) QueryPerformanceFrequency(&s.freq);
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if (s.last.QuadPart == 0 || s.freq.QuadPart == 0)
		{
			s.last = now;
			s.cached = 0.0;
			return s.cached;
		}
		double dt = double(now.QuadPart - s.last.QuadPart) / double(s.freq.QuadPart);
		s.last = now;
		if (dt < 0.0) dt = 0.0;
		if (dt > 0.25) dt = 0.25; // a load or a pause must not release a burst of logic ticks
		s.cached = dt;
		return s.cached;
	}

	inline void clock_reset()
	{
		ClockState& s = clock_state();
		s.last.QuadPart = 0;
		s.last_frame = 0xFFFFFFFF;
		s.cached = 0.0;
	}
}
