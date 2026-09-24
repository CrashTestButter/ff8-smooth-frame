/****************************************************************************/
//    Copyright (C) 2026 Todi                                               //
//                                                                          //
//    This file is part of ff8interp (FF8 Smooth Frames).                   //
//                                                                          //
//    ff8interp is free software: you can redistribute it and/or modify     //
//    it under the terms of the GNU General Public License as published by  //
//    the Free Software Foundation, either version 3 of the License         //
//                                                                          //
//    ff8interp is distributed in the hope that it will be useful,          //
//    but WITHOUT ANY WARRANTY; without even the implied warranty of        //
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         //
//    GNU General Public License for more details.                          //
/****************************************************************************/

#pragma once

// Internal glue of the standalone host (ff8interp.dll). Nothing under FFNx/src/ff8/interp/ includes
// this; the module only sees the InterpHost built in adapter_standalone.cpp.

#include <windows.h>
#include <stdint.h>
#include <string>

#define FF8INTERP_VERSION "0.2"

namespace ff8interp
{
	// ---- log.cpp: ff8interp.log in the game folder ----
	void log_open();
	void log_line(const char* level, const char* fmt, va_list ap);
	void log_info(const char* fmt, ...);
	void log_warning(const char* fmt, ...);
	void log_error(const char* fmt, ...);

	// ---- config.cpp: flat "key = value" TOML subset ----
	struct Options
	{
		// Same keys as the FFNx fork (FFNx.toml). Defaults: see README.md / mod/ff8interp.toml.
		bool battle_enabled = true;  // ff8_battle_anim_interp
		long battle_fps = 144;       // ff8_battle_fps
		long battle_debug = 0;       // ff8_battle_interp_debug
		bool world_enabled = true;   // ff8_world_interp
		long world_fps = 144;        // ff8_world_fps
		long world_debug = 0;        // ff8_world_interp_debug
		bool field_enabled = true;   // ff8_field_interp
		long field_fps = 144;        // ff8_field_fps
		long field_debug = 0;        // ff8_field_interp_debug
		bool trace = false;          // trace_battle_animation
		// Standalone only: roll every patch back and stay passive when any hook site holds bytes
		// that differ from FF8_EN.exe on disk (another mod / FFNx patched it).
		bool strict = true;          // ff8interp_strict
		bool diag = false;           // ff8interp_diag: 1/s pacer line (mode, rate, paced/FFNx frames, flips/s)

		// Read from the game's FFNx.toml (stock FFNx key), not from ff8interp.toml.
		long ffnx_fps_limiter = 1;   // ff8_fps_limiter (FFNx default FPS_LIMITER_DEFAULT = 1)
		std::string source;          // file the options came from (for the log)
	};
	Options load_options(const std::string& dll_dir, const std::string& game_dir);

	// ---- paths ----
	std::string module_dir(HMODULE m);  // folder of a module, with trailing backslash
	extern HMODULE g_self;

	// ---- adapter_standalone.cpp ----
	// Watcher thread body (started from DllMain): waits until FFNx has hooked the game's frame
	// limiter, then redirects that jump to our pacer. Everything else happens on the game's main
	// thread in the first pacer call.
	DWORD WINAPI watcher_thread(LPVOID);
}
