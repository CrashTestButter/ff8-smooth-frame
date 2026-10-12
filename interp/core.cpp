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

// Host-independent entry points, see interp.h.

#include "interp.h"
#include "battle/animations.h"
#include "world/interp.h"
#include "field/interp.h"

namespace ff8_interp
{
	InterpHost g_host;

	namespace
	{
		bool host_installed = false;
	}

	void install(const InterpHost& host)
	{
		g_host = host;
		host_installed = true;

		ff8_battle_anim::animations_hook_init();
		ff8_world::interp_hook_init();
		ff8_field::interp_hook_init();
	}

	void on_frame()
	{
		if (!host_installed) return;
		ff8_battle_anim::animations_on_frame();
		ff8_world::interp_on_frame();
		ff8_field::interp_on_frame();
	}

	double frame_rate(double host_rate)
	{
		if (!host_installed) return host_rate;
		const InterpConfig& c = g_host.config;
		// Only while the sub-module is really in: an uncapped frame rate without the logic gates
		// would run that mode's logic at the host rate (a skipped module - non-US exe, fps out of
		// range - used to get the higher cap all the same).
		switch (game_mode())
		{
		case GameMode::Battle:
			if (c.battle_enabled && c.battle_fps > 0 && ff8_battle_anim::animations_installed()) return (double)c.battle_fps;
			break;
		case GameMode::World:
			if (c.world_enabled && c.world_fps > 0 && ff8_world::interp_installed()) return (double)c.world_fps;
			break;
		case GameMode::Field:
			if (c.field_enabled && c.field_fps > 0 && ff8_field::interp_installed()) return (double)c.field_fps;
			break;
		default:
			break;
		}
		return host_rate;
	}
}
