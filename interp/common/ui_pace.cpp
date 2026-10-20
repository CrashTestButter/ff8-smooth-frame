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

// Message-window pacing, see ui_pace.h.

#include "ui_pace.h"
#include "pace.h"

#include "../host.h"

#include <stdint.h>

namespace ff8_ui_pace
{
	namespace
	{
		constexpr uint32_t FN_PAD_PRESSED = 0x49EDE0; // uint32 __cdecl (int port, int back): ring press edges
		constexpr int MAX_STEPS = 4;                  // the most any vanilla site passes (field movie path)

		typedef void (__cdecl *window_step_fn)(int);
		typedef uint32_t (__cdecl *pad_pressed_fn)(int, int);
		window_step_fn original_window_step = (window_step_fn)FN_WINDOW_STEP;
		pad_pressed_fn original_pad_pressed = (pad_pressed_fn)FN_PAD_PRESSED;

		bool installed = false;
		bool edge_hooked = false;
		ff8_pace::Budget budget;
		uint32_t pending_presses = 0;                  // raw ring press edges since the last window step
		uint32_t unreleased_presses = 0;               // collected edges the caller's logic has not received yet
		uint32_t presses_collected_frame = 0xFFFFFFFF; // host frame whose edge is already in pending_presses
		bool inside_paced_step = false;                // inside our own paced 0x49A970 call

		// 0x49AA66: the one pressed-edge read of 0x49A990's live branch (param_2 == 0). Outside a paced
		// step (menus, card game, battle, the kill bit) it is the plain read.
		uint32_t __cdecl pad_pressed_hook(int port, int back)
		{
			uint32_t pressed = original_pad_pressed(port, back);
			if (inside_paced_step && port == 0 && back == 0) pressed |= pending_presses;
			return pressed;
		}
	}

	bool install(const char* caller_name)
	{
		if (installed) return edge_hooked;
		installed = true;
		const uint8_t* const site_bytes = (const uint8_t*)CALL_SITE_WINDOW_EDGE;
		const uint32_t call_target = CALL_SITE_WINDOW_EDGE + 5 + (uint32_t)(*(const int32_t*)(CALL_SITE_WINDOW_EDGE + 1));
		if (site_bytes[0] != 0xE8 || call_target != FN_PAD_PRESSED)
		{
			ff8_interp::log_warning("%s: window pad-edge site %08X does not call %08X (%02X -> %08X), taps between window steps may be lost.\n",
				caller_name, CALL_SITE_WINDOW_EDGE, FN_PAD_PRESSED, site_bytes[0], call_target);
			return false;
		}
		ff8_interp::hook_call(CALL_SITE_WINDOW_EDGE, (void*)pad_pressed_hook);
		edge_hooked = true;
		return true;
	}

	void window_step(int native_steps, double steps_per_second, bool paced, bool presses_released)
	{
		if (!paced || native_steps <= 0)
		{
			original_window_step(native_steps);
			return;
		}
		// Collect this host frame's press edge (the ring was advanced earlier in the frame by the field
		// updater / world head). OR-ing is idempotent, so a second call in the same frame is harmless.
		if (presses_collected_frame != ff8_interp::frame_counter())
		{
			presses_collected_frame = ff8_interp::frame_counter();
			unreleased_presses |= original_pad_pressed(0, 0);
		}
		if (presses_released)
		{
			pending_presses |= unreleased_presses;
			unreleased_presses = 0;
		}
		budget.feed(steps_per_second, (double)MAX_STEPS);
		const int due_steps = budget.take_all(MAX_STEPS);
		if (due_steps <= 0) return; // no window step due: the draw (0x49AB80) still runs, the state holds
		inside_paced_step = true;
		original_window_step(due_steps);
		inside_paced_step = false;
		pending_presses = 0;
	}
}
