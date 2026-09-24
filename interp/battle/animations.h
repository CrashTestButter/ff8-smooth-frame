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

namespace ff8_battle_anim
{
	// Install the battle model animation interpolation hooks (needs battle_enabled and battle_fps, or a
	// 30/60 fps host limiter).
	void animations_hook_init();
	// True once the core hooks are in (the host frame cap may then be raised to battle_fps).
	bool animations_installed();
	// Drop all per-animation interpolation state (call when leaving battle).
	void animations_reset();
	// Per host frame diagnostic heartbeat (only logs with cfg().trace).
	void animations_on_frame();
}
