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

namespace ff8_field
{
	// Install the field module logic gating + interpolation (needs field_enabled and field_fps).
	void interp_hook_init();
	// True once the hooks are in (the host frame cap may then be raised to field_fps).
	bool interp_installed();
	// Drop interpolation snapshots (mode change).
	void interp_reset();
	// Per host frame housekeeping, called from ff8_interp::frame() before the host frame cap.
	void interp_on_frame();
}
