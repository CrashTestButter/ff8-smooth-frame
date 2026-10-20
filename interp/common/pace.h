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

// Wall-clock step budget for engine counters that the vanilla loops advance a fixed number of times
// per native frame (e.g. 2 calls per 30 fps frame = 60 steps a second). With the interpolation
// modules those loops run once per HOST frame, so a counter stepped per call runs host_fps / native
// times too fast. A Budget grants `steps_per_second` steps per second of real time instead, fed once per host
// frame and capped so a stall (load, pause, alt-tab) cannot release a burst.
//
// It keeps its OWN wall clock (like play_clock.cpp): the modules clock_reset() the shared
// ff8_tick::frame_delta() on every mode change and on field loading frames, which would stop the
// budget during the very frames (field exit fades) it has to pace.

#include "../host.h"

#include <windows.h>
#include <stdint.h>

namespace ff8_pace
{
	struct Budget
	{
		double granted_steps = 0.0;        // steps granted and not yet taken
		uint32_t fed_frame = 0xFFFFFFFF;   // host frame that last fed the budget
		LARGE_INTEGER last_counter{};

		// Add `steps_per_second` steps per second of wall time since the previous feed, once per host frame,
		// capped at `max_steps` steps. A gap between two feeds (another mode, a load) folds into the cap.
		void feed(double steps_per_second, double max_steps)
		{
			if (fed_frame == ff8_interp::frame_counter()) return;
			fed_frame = ff8_interp::frame_counter();
			static LARGE_INTEGER counter_frequency{};
			if (counter_frequency.QuadPart == 0) QueryPerformanceFrequency(&counter_frequency);
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			double elapsed_seconds = 0.0;
			if (last_counter.QuadPart != 0 && counter_frequency.QuadPart != 0) elapsed_seconds = double(now.QuadPart - last_counter.QuadPart) / double(counter_frequency.QuadPart);
			last_counter = now;
			if (elapsed_seconds < 0.0) elapsed_seconds = 0.0;
			granted_steps += elapsed_seconds * steps_per_second * ff8_interp::speed();
			if (granted_steps > max_steps) granted_steps = max_steps;
		}
		// One step, if one is due.
		bool take()
		{
			if (granted_steps < 1.0) return false;
			granted_steps -= 1.0;
			return true;
		}
		// Every whole step that is due, at most `max_steps`.
		int take_all(int max_steps)
		{
			int steps = (int)granted_steps;
			if (steps > max_steps) steps = max_steps;
			if (steps < 0) steps = 0;
			granted_steps -= steps;
			return steps;
		}
	};
}
