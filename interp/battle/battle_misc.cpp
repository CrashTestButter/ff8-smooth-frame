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

// Small battle gates that fit no other translation unit (v78). Shares the module's tick state
// through internal.h.
//
// ACTIVE-CHARACTER MARKER (the spinning inverted pyramid over the head of the character whose
// command menu is open). Frame-driver site 0x500960 calls 0x4BB090 once per RENDERED frame; that
// fetches a prim buffer (0x49AB40), draws through 0x4BB0B0 and returns the buffer (0x49AB60).
// 0x4BB0B0 (only when BattleMenu_GetActiveChar 0x4BB8F0 >= 0, the party is not fleeing and
// 0x4BA1C0(char) says the model is shown):
//   * u16 0x1D76710 += 1           - pulse counter; brightness = f(counter & 7) (read BEFORE the ++)
//   * every 2nd count u16 0x1D7670A (index into the colour script 0xB87E9C) steps and dword
//     0x1D7670C = palette pointer 0xB87EA0[script[idx]]
//   * u16 0x1D76708 += 0x70         - Y rotation angle (12-bit circle), applied BEFORE drawing
//   * draws the 5-vertex pyramid (vertex table 0xB87E38, faces 0xB87EAC) at the head bone 0xF0
//     (0x4BA200 -> 0x502170) into the OT 0x1D8E04C, fresh prims every call.
// With no active character it resets 0x1D76710's low half, 0x1D7670A and 0x1D7670C = 0xB87E60.
// All of it at host rate: the pyramid spun 0x70 per host frame (~2.5 turns/s natively at 15 fps,
// ~24 turns/s at 144 Hz) and its colour cycled ~10x fast. The earlier "icon above the head looks
// strange" reports were this. (The frame-driver inventory had listed 0x500960 as the paced
// window tick; it is not - 0x4B9C80 has its own single call site 0x4A87D1.)
//
// Gate (call-site hook, the call itself always runs because it draws fresh prims every frame):
//   * rotation - kept in our own logical angle L that advances 0x70 per logic tick in which the
//     engine really stepped; every host frame (tick frame included, rule 2.1) the engine variable
//     is set so that its own `+= 0x70` lands on L + 0x70 * alpha, then put back to L after the
//     call: a smooth spin at the native speed, one tick behind like every other blend.
//   * pulse counter / colour script - held on held frames (restored after the call when the call
//     stepped them), so they advance once per logic tick exactly as on the 15 fps engine.
// Inert outside battle and with DBG_MARKER_PACE_OFF.

#include "internal.h"

#include "../host.h"

#include <stdint.h>

namespace ff8_battle_anim
{
	namespace
	{
		constexpr uint32_t CALL_SITE_ACTIVE_MARKER = 0x500960; // call 0x4BB090, void __cdecl(void)
		constexpr uint32_t ACTIVE_MARKER_FUNCTION = 0x4BB090;
		uint16_t* const MARKER_ANGLE = (uint16_t*)0x1D76708;
		uint16_t* const MARKER_PULSE = (uint16_t*)0x1D76710;
		uint16_t* const MARKER_COLOUR_IDX = (uint16_t*)0x1D7670A;
		uint32_t* const MARKER_COLOUR_PTR = (uint32_t*)0x1D7670C;
		constexpr int MARKER_STEP = 0x70;

		typedef void (__cdecl *active_marker_fn)();
		active_marker_fn original_active_marker = (active_marker_fn)ACTIVE_MARKER_FUNCTION;

		bool logical_angle_valid = false;
		uint16_t logical_angle = 0; // logical angle L (the value the engine had after its last real tick step)
		bool trace_logged = false;

		void __cdecl active_marker_hook()
		{
			if (!in_battle() || (ff8_interp::cfg().battle_debug & DBG_MARKER_PACE_OFF))
			{
				original_active_marker();
				return;
			}
			tick_update();
			const bool is_logic_tick = is_logic_tick_now();
			const uint16_t saved_pulse = *MARKER_PULSE;
			const uint16_t saved_colour_index = *MARKER_COLOUR_IDX;
			const uint32_t saved_colour_pointer = *MARKER_COLOUR_PTR;
			if (!logical_angle_valid)
			{
				logical_angle = *MARKER_ANGLE;
				logical_angle_valid = true;
			}

			// On a tick frame the logic advances first; the draw then shows L + step * alpha.
			const uint16_t base_angle = is_logic_tick ? (uint16_t)(logical_angle + MARKER_STEP) : logical_angle;
			float alpha = tick_alpha_now();
			if (alpha < 0.0f) alpha = 0.0f;
			if (alpha > 1.0f) alpha = 1.0f;
			const int angle_lead = (int)(alpha * MARKER_STEP + 0.5f);
			*MARKER_ANGLE = (uint16_t)(base_angle + angle_lead - MARKER_STEP); // the engine adds MARKER_STEP before it draws
			original_active_marker();
			const bool stepped = *MARKER_PULSE == (uint16_t)(saved_pulse + 1);
			if (is_logic_tick && stepped) logical_angle = base_angle;
			*MARKER_ANGLE = logical_angle;
			if (!is_logic_tick && stepped)
			{
				*MARKER_PULSE = saved_pulse;
				*MARKER_COLOUR_IDX = saved_colour_index;
				*MARKER_COLOUR_PTR = saved_colour_pointer;
			}
			if (ff8_interp::cfg().trace && stepped && !trace_logged)
			{
				trace_logged = true;
				ff8_interp::log_trace("ff8marker f=%u active-character marker paced (angle %u, alpha %.2f)\n", ff8_interp::frame_counter(), (unsigned)logical_angle, alpha);
			}
		}
	}

	void battle_misc_hook_init()
	{
		logical_angle_valid = false;
		ff8_interp::hook_call(CALL_SITE_ACTIVE_MARKER, (void*)active_marker_hook);
		ff8_interp::log_info("%s: active-character marker pacing installed (site 0x%X).\n", __func__, CALL_SITE_ACTIVE_MARKER);
	}

	void battle_misc_reset()
	{
		logical_angle_valid = false;
		trace_logged = false;
	}
}
