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
		typedef void (__cdecl *play_clock_fn)();
		play_clock_fn original_play_clock = (play_clock_fn)FN_PLAY_CLOCK;

		// The one side effect 0x4701B0 has on EVERY call, paused or not (0x470242:
		// mov word [0x1CD2EFA], 1). A skipped call still makes it, so nothing that looks at the word
		// can tell a paced call from a real one.
		uint16_t* const CLOCK_RAN_FLAG = (uint16_t*)0x1CD2EFA;

		constexpr double CLOCK_STEP_SECONDS = 1.0 / 60.0;
		constexpr double MAX_BUDGET_SECONDS = 4.0 * CLOCK_STEP_SECONDS; // at most the native 4 steps after a stall

		double budget_seconds = 0.0;             // wall-clock seconds not yet spent on a clock step
		uint32_t budget_fed_frame = 0xFFFFFFFF; // host frame that last fed the budget
		LARGE_INTEGER counter_frequency{};
		LARGE_INTEGER last_counter{};

		// Own wall clock rather than ff8_tick::frame_delta(): the modules clock_reset() that one on
		// every mode change and, in the field, on every loading frame (FIELD_FLOW != 4), which would
		// stop play time during loads that the vanilla loop does count.
		double elapsed_seconds()
		{
			if (counter_frequency.QuadPart == 0) QueryPerformanceFrequency(&counter_frequency);
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			if (last_counter.QuadPart == 0 || counter_frequency.QuadPart == 0)
			{
				last_counter = now;
				return 0.0;
			}
			double seconds = double(now.QuadPart - last_counter.QuadPart) / double(counter_frequency.QuadPart);
			last_counter = now;
			return seconds < 0.0 ? 0.0 : seconds * ff8_interp::speed(); // the cap below bounds any long gap
		}
	}

	bool patch_site(uint32_t site, void* hook, const char* caller_name)
	{
		const uint8_t* const site_bytes = (const uint8_t*)site;
		const uint32_t call_target = site + 5 + (uint32_t)(*(const int32_t*)(site + 1));
		if (site_bytes[0] != 0xE8)
		{
			ff8_interp::log_warning("%s: play clock call site %08X is not a call (%02X), left alone.\n", caller_name, site, site_bytes[0]);
			return false;
		}
		if (call_target == FN_PLAY_CLOCK)
		{
			ff8_interp::hook_call(site, hook);
			return true;
		}
		// FFNx's field movie sync (FFNx #1014) wraps the field sites: it skips clock steps while a movie
		// faster than 15 fps plays, to keep 60 steps a second. The budget in run() already holds that rate,
		// so the hook takes over the site and steps 0x4701B0 itself. Another mod's redirect is refused.
		return ff8_interp::hook_call_chain(site, hook) != 0;
	}

	void run(bool paced)
	{
		if (!paced)
		{
			original_play_clock();
			return;
		}
		if (budget_fed_frame != ff8_interp::frame_counter())
		{
			// A gap since the previous paced call (another mode, a load) folds into the cap below.
			budget_fed_frame = ff8_interp::frame_counter();
			budget_seconds += elapsed_seconds();
			if (budget_seconds > MAX_BUDGET_SECONDS) budget_seconds = MAX_BUDGET_SECONDS;
		}
		if (budget_seconds >= CLOCK_STEP_SECONDS)
		{
			budget_seconds -= CLOCK_STEP_SECONDS;
			original_play_clock();
			return;
		}
		*CLOCK_RAN_FLAG = 1;
	}
}
