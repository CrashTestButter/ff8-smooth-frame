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

// Entry points of the FF8 interpolation module (battle, world map, field). The only header a host
// program includes. See README.md in this folder.

#include "host.h"

namespace ff8_interp
{
	// --- implemented by the adapter (adapter_ffnx.cpp for FFNx) ---

	// Build the InterpHost for this host and call install(). Once, at hook time, after the host's
	// own engine patches.
	void init();

	// --- implemented by the module (core.cpp) ---

	// Take a copy of `host` and install every enabled sub-module (each skips itself when its option is
	// off, its fps is out of range or the executable is not the US one).
	void install(const InterpHost& host);

	// Once per presented frame, before the host waits for its frame cap: mode-change resets and trace
	// heartbeats of the sub-modules.
	void on_frame();

	// Frame cap for the current frame. Returns ff8_*_fps while the current mode's sub-module is
	// installed and its fps option is set, otherwise `host_rate` unchanged.
	double frame_rate(double host_rate);
}
