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

// Host interface of the FF8 interpolation module (src/ff8/interp/).
//
// Nothing under src/ff8/interp/ talks to the host program directly: every hook, log line, mode
// query, frame number and option goes through the InterpHost below. The host fills one InterpHost
// and hands it to ff8_interp::install() (core.h). adapter_ffnx.cpp is the FFNx host; a standalone DLL
// provides its own adapter with the same fields. See README.md for the full contract.

#include <stdint.h>

namespace ff8_interp
{
	// Game mode as far as the module cares. The adapter maps the host's own mode detection onto it.
	enum class GameMode : uint8_t
	{
		Other = 0,
		Field,
		World,
		Battle,
	};

	// Every user option of the module, filled by the adapter before install().
	struct InterpConfig
	{
		bool battle_enabled = false; // ff8_battle_anim_interp
		long battle_fps = 0;         // ff8_battle_fps (0 = derive from host_limiter_fps, else 30..1000)
		long battle_debug = 0;       // ff8_battle_interp_debug (kill bits, battle/internal.h DBG_*)

		bool world_enabled = false;  // ff8_world_interp
		long world_fps = 0;          // ff8_world_fps (30..1000)
		long world_debug = 0;        // ff8_world_interp_debug

		bool field_enabled = false;  // ff8_field_interp
		long field_fps = 0;          // ff8_field_fps (0 = 60, clamped 30..1000)
		long field_debug = 0;        // ff8_field_interp_debug

		// Host frame cap in effect outside our overrides: 30 or 60, anything else 0. Only used as the
		// battle rate when battle_fps is 0 (FFNx: ff8_fps_limiter 2 -> 30, 3 -> 60).
		long host_limiter_fps = 0;

		bool trace = false;          // verbose trace lines (FFNx: trace_battle_animation)
	};

	struct InterpHost
	{
		// Patch the rel32 operand of the call (E8), jump (E9) or indirect call (FF 15) at `site` so it
		// reaches `fn`. The instruction itself is kept. (FFNx: replace_call.)
		void (*hook_call)(uint32_t site, void* fn);
		// Like hook_call, but for a site another module (FFNx itself) may already have redirected:
		// the call is patched either way and the function it reached BEFORE our patch is returned, so
		// the hook can chain to it instead of to the game's original. 0 = not patched.
		uint32_t (*hook_call_chain)(uint32_t site, void* fn);
		// Overwrite the first 5 bytes of the function at `addr` with `jmp fn` (E9 rel32). The module
		// copies the prologue into its own trampoline BEFORE calling this (make_trampoline), so the
		// host must not relocate or keep a trampoline of its own. Returns an opaque host handle the
		// module never uses. (FFNx: replace_function.)
		uint32_t (*hook_function)(uint32_t addr, void* fn);

		// printf-style log sinks, the format strings end in '\n'.
		void (*log_info)(const char* fmt, ...);
		void (*log_warning)(const char* fmt, ...);
		void (*log_trace)(const char* fmt, ...);

		// Current game mode, stable within one presented frame.
		GameMode (*game_mode)();
		// Host-specific mode number (FFNx: driver_mode), only compared for change detection and
		// printed in traces: a change between two Other modes still resets the modules.
		uint32_t (*mode_id)();

		// Counter the host increments once per presented frame (FFNx: frame_counter). Read through
		// ff8_interp::frame_counter() on every hook call, so it is a pointer and not a function.
		const uint32_t* frame_counter;

		// True on the English (US) 1.2 executable, the only one the addresses are verified on.
		bool (*is_us_exe)();

		InterpConfig config;
	};

	// The installed host (core.cpp). Filled by install(), read-only afterwards.
	extern InterpHost g_host;

	inline const InterpHost& host() { return g_host; }
	inline const InterpConfig& cfg() { return g_host.config; }
	inline uint32_t frame_counter() { return *g_host.frame_counter; }
	inline GameMode game_mode() { return g_host.game_mode(); }
	inline uint32_t mode_id() { return g_host.mode_id(); }
	inline bool is_us_exe() { return g_host.is_us_exe(); }
	inline void hook_call(uint32_t site, void* fn) { g_host.hook_call(site, fn); }
	inline uint32_t hook_call_chain(uint32_t site, void* fn) { return g_host.hook_call_chain(site, fn); }
	inline uint32_t hook_function(uint32_t addr, void* fn) { return g_host.hook_function(addr, fn); }

	template<typename... A> inline void log_info(const char* fmt, A... a) { g_host.log_info(fmt, a...); }
	template<typename... A> inline void log_warning(const char* fmt, A... a) { g_host.log_warning(fmt, a...); }
	template<typename... A> inline void log_trace(const char* fmt, A... a) { g_host.log_trace(fmt, a...); }
}
