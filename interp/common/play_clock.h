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

// Play clock pacing shared by the battle, world map and field interpolation modules.
//
// 0x4701B0 (void __cdecl, no arguments) advances the save-file play time 0x1CFE928 and the scripted
// event countdown 0x1CFE92C by one 1/60 s step per call (0x88F into a 17-bit fraction). Every mode loop
// calls it so that the native frame rate yields exactly 60 calls a second:
//   field   0x46FFF9 / 0x46FFFE             2 per 30 fps frame, plus 0x47000C / 0x470011 while
//                                            0x209A798 != 0 (the 15 fps movie path): 4 per frame
//   world   0x53F20C / 0x53F211             2 per 30 fps frame
//   battle  0x47D076 / 7B / 80 / 85         4 per 15 fps frame
//   menus   0x4A2362 (0x4A22C0), results 0x4A2735 (0x4A2690): 1 per 60 fps frame, own busy-wait - NOT hooked
// With the interpolation modules those loops run once per HOST frame, so at 144 Hz the clock ran
// 4.8x (field / world) and 9.6x (battle) too fast - menu "Time", SeeD salary and every timed story
// countdown. Each module replace_call()s its own sites onto a hook that calls run() below.

#include <stdint.h>

namespace ff8_play_clock
{
	constexpr uint32_t FN_PLAY_CLOCK = 0x4701B0;

	// Verify that `site` is an E8 call to 0x4701B0 and redirect it to `hook` (a void __cdecl ()).
	// Returns false and leaves the site untouched on any mismatch. `who` names the caller in the log.
	bool patch_site(uint32_t site, void* hook, const char* who);

	// Body of a module's hook. With `paced` false the original runs on every call (the kill bit).
	// Otherwise a 60 Hz wall-clock budget, fed once per host frame and capped at a few periods so a
	// stall cannot release a burst, decides whether this call steps the clock. Whatever the number of
	// calls per host frame, the clock then advances exactly 60 times a second - the vanilla rate of
	// every loop above - as long as the loop calls at least as often as the budget grants.
	// The budget keeps its own wall clock (a gap between two paced calls counts, capped like a
	// stall), so the modules need no reset hook.
	void run(bool paced);
}
