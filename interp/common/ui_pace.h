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

// Message-window pacing shared by the world map and field modules.
//
// 0x49A970(n) = 0x49A990(n, 0) + 0x4A2C30(n) steps the whole window system n times: it reads the pad
// once (held 0x49ED30 / pressed 0x49EDE0 at 0x49AA66 / repeat 0x49FDA0), then per step the cursor
// glow 0x49FA70, the update callback of each of the 8 windows (0x4B7110 -> [0x1D2B550 + i*0x40]:
// open/close animation, text reveal, choice cursor, auto-close timers), the countdown-timer pulse
// 0x4A6D00, 0x4B7E00 and 0x4B79D0; 0x4A2C30 then steps the pad-vibration envelopes once per 4 units.
// The field updater calls it with 2 (4 while a movie plays at 15 fps) and the world director with
// 0x20403A0 (2, or 3 in world state 0x32): 60 steps a second at the native rate (90 in state 0x32).
// Drawing is separate (0x49AB80, every frame). At 144 Hz the windows opened, typed and closed 4.8x
// too fast, the cursor flashed 4.8x too fast and rumble effects were cut short.
//
// window_step() replaces such a call: it hands the original the number of steps a wall-clock budget
// grants this host frame (usually 0 or 1). A step-less frame skips the call entirely (0x49A990 with
// n = 0 only reads the pad and returns, 0x4A2C30(0) only adds 0 to its divider). The press edge that
// 0x49A990 reads from the pad ring is the edge of the CURRENT host frame only, so a press on a
// step-less frame would be lost: the edges of every host frame since the previous step are OR-ed and
// handed to the next step through a hook on 0x49AA66 (inert outside window_step()).
//
// The field script reads the same press edges, OR-ed and published on its 30 Hz logic tick. Natively the
// script sees a press first and the window update (later in the same updater frame) sees it after. Fed
// at 60 Hz, a window step between the press and the next tick closed an ASK window first: the script then
// resumed on that tick with the press still published and its talk check opened the window again (draw
// point "Don't draw"). A caller with its own tick hands each press to the windows only from the frame its
// logic received it on (`presses_released`).

#include <stdint.h>

namespace ff8_ui_pace
{
	constexpr uint32_t FN_WINDOW_STEP = 0x49A970;  // void __cdecl (int n)
	constexpr uint32_t CALL_SITE_WINDOW_EDGE = 0x49AA66;  // inside 0x49A990: call 0x49EDE0(0, 0) (pressed, port 0)

	// Hook the pressed-edge read inside 0x49A990 (once; later calls are no-ops). `caller_name` names the caller
	// in the log. Returns false if the site is not the expected call (edges then stay live-only).
	bool install(const char* caller_name);

	// Body of a module's replace_call on a per-frame 0x49A970(native_steps) site. `steps_per_second` is the
	// native rate of that site (field 60, world 30 * native_steps). With `paced` false the original runs
	// unchanged (the kill bit). Presses collected so far reach the windows only on a frame with
	// `presses_released` (the field: its logic tick; true = on the next window step).
	void window_step(int native_steps, double steps_per_second, bool paced, bool presses_released);
}
