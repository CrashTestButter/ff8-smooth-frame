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

// Play clock pacing, see play_clock.h.

#include "play_clock.h"

#include "../host.h"

#include <windows.h>
#include <stdint.h>

namespace ff8_play_clock
{
	namespace
	{
		typedef void (__cdecl *play_clock_t)();
		play_clock_t orig_play_clock = (play_clock_t)FN_PLAY_CLOCK;

		// The one side effect 0x4701B0 has on EVERY call, paused or not (0x470242:
		// mov word [0x1CD2EFA], 1). A skipped call still makes it, so nothing that looks at the word
		// can tell a paced call from a real one.
		uint16_t* const CLOCK_RAN = (uint16_t*)0x1CD2EFA;

		constexpr double PERIOD = 1.0 / 60.0;
		constexpr double BUDGET_CAP = 4.0 * PERIOD; // at most the native 4 steps after a stall

		double budget = 0.0;               // wall-clock seconds not yet spent on a clock step
		uint32_t budget_frame = 0xFFFFFFFF; // host frame that last fed the budget
		LARGE_INTEGER qpc_freq{}, qpc_last{};

		// Own wall clock rather than ff8_tick::frame_delta(): the modules clock_reset() that one on
		// every mode change and, in the field, on every loading frame (FIELD_FLOW != 4), which would
		// stop play time during loads that the vanilla loop does count.
		double elapsed()
		{
			if (qpc_freq.QuadPart == 0) QueryPerformanceFrequency(&qpc_freq);
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			if (qpc_last.QuadPart == 0 || qpc_freq.QuadPart == 0) { qpc_last = now; return 0.0; }
			double dt = double(now.QuadPart - qpc_last.QuadPart) / double(qpc_freq.QuadPart);
			qpc_last = now;
			return dt < 0.0 ? 0.0 : dt; // the cap below bounds any long gap
		}
	}

	bool patch_site(uint32_t site, void* hook, const char* who)
	{
		const uint8_t* const p = (const uint8_t*)site;
		const uint32_t target = site + 5 + (uint32_t)(*(const int32_t*)(site + 1));
		if (p[0] != 0xE8 || target != FN_PLAY_CLOCK)
		{
			ff8_interp::log_warning("%s: play clock call site %08X does not call %08X (%02X -> %08X), left alone.\n",
				who, site, FN_PLAY_CLOCK, p[0], target);
			return false;
		}
		ff8_interp::hook_call(site, hook);
		return true;
	}

	void run(bool paced)
	{
		if (!paced)
		{
			orig_play_clock();
			return;
		}
		if (budget_frame != ff8_interp::frame_counter())
		{
			// A gap since the previous paced call (another mode, a load) folds into the cap below.
			budget_frame = ff8_interp::frame_counter();
			budget += elapsed();
			if (budget > BUDGET_CAP) budget = BUDGET_CAP;
		}
		if (budget >= PERIOD)
		{
			budget -= PERIOD;
			orig_play_clock();
			return;
		}
		*CLOCK_RAN = 1;
	}
}
