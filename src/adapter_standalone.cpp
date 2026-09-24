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

// Standalone host of the FF8 interpolation module (FFNx/src/ff8/interp/), for ff8interp.dll running
// next to an UNMODIFIED stock FFNx (AF3DN.P). This file replaces adapter_ffnx.cpp; the module sources
// are compiled unchanged. See ../README.md for the full picture, in short:
//
//  1. DllMain starts watcher_thread() and returns. No hook is installed under the loader lock, and
//     FFNx has not run ff8_init_hooks yet at that point (J8 loads us in its WinMain detour, an ASI
//     loader even earlier).
//  2. watcher_thread() polls the game's frame limiter entry FN_LIMITER (0x4020F0). FFNx's
//     ff8_init_hooks writes `E9 rel32` there (replace_function(ff8_externals.fps_limiter,
//     ff8_limit_fps)) when ff8_fps_limiter >= 1 (the default). Once that jump is present, stable and
//     points into AF3DN.P, the watcher remembers FFNx's ff8_limit_fps and rewrites the rel32 so the
//     jump reaches pacer() instead. That single 4-byte write is the only thing done off the main
//     thread.
//  3. The first pacer() call happens on the game thread at the end of the first game frame, after
//     FFNx finished all of its init. It verifies the engine constants, hooks FFNx's driver flip slot
//     (the frame counter), builds the InterpHost and calls ff8_interp::install().
//  4. Every later pacer() call is FFNx's limiter call site: on_frame(), then either FFNx's own
//     ff8_limit_fps (modes we do not interpolate) or our own busy-wait at ff8_*_fps.
//
// FRAGILITY: step 2 depends on FFNx implementing its limiter as a 5-byte E9 at the game's limiter
// entry and on ff8_limit_fps having no side effect besides the busy-wait and the music cross-fade
// timer (checked against FFNx 1.22.0 .. 1.24.3 and master 2026-09: identical function). Any FFNx
// change there silently changes what pass-through means; the log states what was found.

#include "standalone.h"

#include "interp.h" // FFNx/src/ff8/interp/interp.h (include dir set in CMakeLists.txt)

#include <intrin.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <vector>

#pragma comment(lib, "version.lib")

namespace ff8interp
{
	namespace
	{
		// ------------------------------------------------------------------------------------
		// Engine addresses (English 1.2 / Steam 2013 FF8_EN.exe). How FFNx derives each one:
		// ------------------------------------------------------------------------------------
		// main_loop (module dispatcher)       0x4706B0  abs(sub_470630 + 0x24), ff8_data.cpp:108
		// _mode WORD                          0x1CD8FC6 abs(main_loop + 0x115)
		// field / world / battle / swirl loop                abs(main_loop + 0x144 / 0x2D0 / 0x340 / 0x4A3)
		// credits loop                        0x52DA20  abs(pubintro_main_loop 0x4703B0 + 0x6D)
		// main menu loop                      0x4A24B0  abs(go_to_main_menu_main_loop 0x470520 + 0x2B)
		// is_card_game DWORD                  0x1DCD798 ff8_data.cpp:246 (operand of 0x534640)
		// get_game_object()                   0x40A04A  `mov eax, [0x1A79D88]; ret`
		// game_obj->game_loop_obj.main_loop   +0xB40    (game_loop_obj at +0xB30, main_loop 5th ptr).
		//   NOT the offset FFNx's ff8.h field names suggest (+0xA1C): main_entry 0x401ED0 fills the
		//   object at +0xB30..+0xB48 (0x402022..0x40205A: main_loop = pubintro 0x4703B0 at +0xB40),
		//   WinMain's loop calls [obj+0xB40] (0x569C37). v0.1 used +0xA1C: battle (no _mode of its own)
		//   was never detected, field / world only matched by _mode.
		// game_obj->gfx driver                +0xA74    (getter 0x4098EE), driver->flip at +0x10,
		//                                               called from 0x41DF0C (`call [eax+0x10]` 0x41DF32)
		// fps_limiter                         0x4020F0  rel(field_main_loop + 0x261), ff8_data.cpp:1033
		// time_volume_change_related_1A78BE0  0x1A78BE0 abs(fps_limiter + 0x3F)
		constexpr uint32_t FN_LIMITER = 0x4020F0;
		constexpr uint32_t MAIN_LOOP = 0x4706B0;
		constexpr uint32_t PUBINTRO_LOOP = 0x4703B0;
		constexpr uint32_t GO_MAIN_MENU = 0x470520;
		constexpr uint32_t FIELD_LOOP = 0x46FEE0;
		constexpr uint32_t WORLD_LOOP = 0x53F0F0;
		constexpr uint32_t BATTLE_LOOP = 0x47CF60;
		constexpr uint32_t SWIRL_LOOP = 0x559890;
		constexpr uint32_t CREDITS_LOOP = 0x52DA20;
		constexpr uint32_t MAIN_MENU_LOOP = 0x4A24B0;
		constexpr uint32_t MODE_WORD = 0x1CD8FC6;
		constexpr uint32_t IS_CARD_GAME = 0x1DCD798;
		constexpr uint32_t GAME_OBJ_PTR = 0x1A79D88;
		constexpr uint32_t GAME_OBJ_MAIN_LOOP = 0xB40;
		constexpr uint32_t GAME_OBJ_GFX_DRIVER = 0xA74;
		constexpr uint32_t DRIVER_FLIP = 0x10;
		constexpr uint32_t VOLUME_DT = 0x1A78BE0;  // double, ms of the last frame (music cross-fade)

		// Vanilla first bytes of the limiter (sub esp,0x10 / lea eax,[esp+0]).
		const uint8_t LIMITER_PROLOGUE[5] = { 0x83, 0xEC, 0x10, 0x8D, 0x44 };

		// ------------------------------------------------------------------------------------
		// State
		// ------------------------------------------------------------------------------------
		enum State : LONG { ST_WAITING = 0, ST_ACTIVE = 1, ST_PASSIVE = 2 };
		volatile LONG g_state = ST_WAITING;

		typedef int(__cdecl* limit_fn)();
		typedef void(__cdecl* flip_fn)(void* game_obj);

		limit_fn g_ffnx_limit = nullptr;     // FFNx ff8_limit_fps, read from the E9 at FN_LIMITER
		flip_fn g_ffnx_flip = nullptr;       // FFNx common_flip, read from the driver table
		flip_fn* g_flip_slot = nullptr;
		uint32_t g_frame_counter = 0;        // our FFNx frame_counter: +1 per driver flip
		bool g_counter_from_flip = false;    // false: fallback, pacer counts frames

		HMODULE g_ffnx = nullptr;
		uint32_t g_ffnx_lo = 0, g_ffnx_hi = 0;
		Options g_opt;

		LARGE_INTEGER g_qpf{};
		int64_t g_last_exit = 0;             // QPC at the end of the previous pacer call
		bool g_paced_last = false;           // previous frame used our own wait
		uint32_t g_paced_frames = 0, g_ffnx_frames = 0;
		// ff8interp_diag: one line per second from the pacer
		int64_t g_diag_t0 = 0;
		uint32_t g_diag_paced = 0, g_diag_ffnx = 0, g_diag_flip0 = 0, g_diag_calls = 0;

		int64_t qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

		template<typename T> T rd(uint32_t addr) { T v; memcpy(&v, (const void*)addr, sizeof(T)); return v; }

		bool in_ffnx(uint32_t a) { return a >= g_ffnx_lo && a < g_ffnx_hi; }

		std::string owner_of(uint32_t addr)
		{
			HMODULE m = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) || !m)
				return "no module (heap / VirtualAlloc)";
			char path[MAX_PATH] = {};
			GetModuleFileNameA(m, path, MAX_PATH);
			const char* base = strrchr(path, '\\');
			return base ? base + 1 : path;
		}

		std::string hex_bytes(const uint8_t* p, size_t n)
		{
			std::string s;
			char b[4];
			for (size_t i = 0; i < n; i++) { snprintf(b, sizeof(b), i ? " %02X" : "%02X", p[i]); s += b; }
			return s;
		}

		// ------------------------------------------------------------------------------------
		// FF8_EN.exe as shipped (on disk): the reference for "expected original bytes"
		// ------------------------------------------------------------------------------------
		struct DiskImage
		{
			std::vector<uint8_t> file;
			uint32_t image_base = 0;
			struct Sec { uint32_t va, vsize, raw, rsize; };
			std::vector<Sec> secs;

			bool load()
			{
				char path[MAX_PATH * 2] = {};
				GetModuleFileNameA(nullptr, path, (DWORD)sizeof(path));
				FILE* f = fopen(path, "rb");
				if (!f) { log_error("cannot open %s for the byte checks\n", path); return false; }
				fseek(f, 0, SEEK_END);
				long n = ftell(f);
				fseek(f, 0, SEEK_SET);
				file.resize(n > 0 ? (size_t)n : 0);
				size_t got = file.empty() ? 0 : fread(file.data(), 1, file.size(), f);
				fclose(f);
				if (got != file.size() || file.size() < 0x400) { file.clear(); return false; }
				const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)file.data();
				if (dos->e_magic != IMAGE_DOS_SIGNATURE || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > file.size()) { file.clear(); return false; }
				const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(file.data() + dos->e_lfanew);
				image_base = nt->OptionalHeader.ImageBase;
				const IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
				for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
					secs.push_back({ sh[i].VirtualAddress, sh[i].Misc.VirtualSize, sh[i].PointerToRawData, sh[i].SizeOfRawData });
				return true;
			}

			const uint8_t* at(uint32_t va, size_t len) const
			{
				if (file.empty() || va < image_base) return nullptr;
				uint32_t rva = va - image_base;
				for (const Sec& s : secs)
					if (rva >= s.va && rva + len <= s.va + s.rsize)
					{
						size_t off = (size_t)s.raw + (rva - s.va);
						return off + len <= file.size() ? file.data() + off : nullptr;
					}
				return nullptr;
			}

			void release() { std::vector<uint8_t>().swap(file); secs.clear(); }
		};
		DiskImage g_disk;

		// ------------------------------------------------------------------------------------
		// Patching: hook_call / hook_function with byte verification and rollback
		// ------------------------------------------------------------------------------------
		struct Patch { uint32_t addr; uint8_t len; uint8_t orig[6]; uint8_t mine[6]; };
		std::vector<Patch> g_patches;
		uint32_t g_n_call = 0, g_n_func = 0, g_n_skipped = 0;

		void write_code(uint32_t addr, const uint8_t* bytes, size_t len)
		{
			DWORD old;
			VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old);
			memcpy((void*)addr, bytes, len);
			VirtualProtect((void*)addr, len, old, &old);
			FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
		}

		Patch* find_patch(uint32_t addr)
		{
			for (Patch& p : g_patches) if (p.addr == addr) return &p;
			return nullptr;
		}

		// True if the `len` bytes at addr may be patched: they equal FF8_EN.exe on disk, or they are
		// exactly what we wrote there before. Otherwise log what is there and who owns the target.
		bool verify_site(const char* kind, uint32_t addr, size_t len)
		{
			const uint8_t* mem = (const uint8_t*)addr;
			const uint8_t* disk = g_disk.at(addr, len);
			if (disk == nullptr)
			{
				log_warning("%s %08X: outside FF8_EN.exe sections, skipped\n", kind, addr);
				return false;
			}
			if (memcmp(mem, disk, len) == 0) return true;
			if (Patch* p = find_patch(addr)) if (p->len == len && memcmp(mem, p->mine, len) == 0) return true;

			std::string who;
			if (mem[0] == 0xE8 || mem[0] == 0xE9)
			{
				uint32_t target = addr + 5 + rd<int32_t>(addr + 1);
				char b[96];
				snprintf(b, sizeof(b), ", now reaches %08X in %s", target, owner_of(target).c_str());
				who = b;
			}
			log_warning("%s %08X: bytes differ from FF8_EN.exe (memory %s, disk %s%s) - patched by FFNx or another mod, skipped\n",
				kind, addr, hex_bytes(mem, len).c_str(), hex_bytes(disk, len).c_str(), who.c_str());
			g_n_skipped++;
			return false;
		}

		void record_and_write(uint32_t addr, const uint8_t* bytes, uint8_t len)
		{
			Patch* p = find_patch(addr);
			if (p == nullptr)
			{
				Patch np{};
				np.addr = addr;
				np.len = len;
				memcpy(np.orig, (const void*)addr, len);
				g_patches.push_back(np);
				p = &g_patches.back();
			}
			memcpy(p->mine, bytes, len);
			write_code(addr, bytes, len);
		}

		// InterpHost::hook_call - FFNx replace_call semantics (E8 / E9 rel32, FF 15 operand = fn - site - 6).
		void sa_hook_call(uint32_t site, void* fn)
		{
			const uint8_t* p = (const uint8_t*)site;
			uint8_t size;
			if (p[0] == 0xE8 || p[0] == 0xE9) size = 1;
			else if (p[0] == 0xFF && p[1] == 0x15) size = 2;
			else
			{
				log_warning("hook_call %08X: not a call/jmp (%s), skipped\n", site, hex_bytes(p, 6).c_str());
				g_n_skipped++;
				return;
			}
			const uint8_t len = (uint8_t)(size + 4);
			if (!verify_site("hook_call", site, len)) return;
			uint8_t bytes[6];
			memcpy(bytes, p, len);
			uint32_t rel = (uint32_t)fn - site - len;
			memcpy(bytes + size, &rel, 4);
			record_and_write(site, bytes, len);
			g_n_call++;
		}

		// InterpHost::hook_call_chain - for a call site FFNx itself redirects. Allowed when the bytes are
		// still the game's (then the chain target is the game's own callee) or when the site is an E8/E9
		// that now reaches into AF3DN.P (FFNx); anything else (another mod) is still refused. Returns the
		// function the site reached before our patch, 0 when refused.
		uint32_t sa_hook_call_chain(uint32_t site, void* fn)
		{
			const uint8_t* p = (const uint8_t*)site;
			if (p[0] != 0xE8 && p[0] != 0xE9)
			{
				log_warning("hook_call_chain %08X: not a call/jmp (%s), skipped\n", site, hex_bytes(p, 5).c_str());
				g_n_skipped++;
				return 0;
			}
			const uint32_t target = site + 5 + rd<int32_t>(site + 1);
			const uint8_t* disk = g_disk.at(site, 5);
			const bool pristine = disk != nullptr && memcmp(p, disk, 5) == 0;
			if (!pristine)
			{
				if (find_patch(site) == nullptr && _stricmp(owner_of(target).c_str(), "AF3DN.P") != 0)
				{
					log_warning("hook_call_chain %08X: redirected by %s (%08X), not FFNx - skipped\n", site, owner_of(target).c_str(), target);
					g_n_skipped++;
					return 0;
				}
				log_info("hook_call_chain %08X: FFNx redirects this call to %08X, chaining to it\n", site, target);
			}
			uint8_t bytes[5];
			bytes[0] = p[0];
			uint32_t rel = (uint32_t)fn - site - 5;
			memcpy(bytes + 1, &rel, 4);
			record_and_write(site, bytes, 5);
			g_n_call++;
			return target;
		}

		// InterpHost::hook_function - FFNx replace_function semantics (E9 rel32 over 5 bytes, no trampoline).
		uint32_t sa_hook_function(uint32_t addr, void* fn)
		{
			if (!verify_site("hook_function", addr, 5)) return 0;
			uint8_t bytes[5] = { 0xE9 };
			uint32_t rel = (uint32_t)fn - addr - 5;
			memcpy(bytes + 1, &rel, 4);
			record_and_write(addr, bytes, 5);
			g_n_func++;
			return (uint32_t)g_patches.size();
		}

		void rollback_patches()
		{
			for (size_t i = g_patches.size(); i-- > 0;)
				write_code(g_patches[i].addr, g_patches[i].orig, g_patches[i].len);
			log_warning("rolled back %u patches\n", (unsigned)g_patches.size());
			g_patches.clear();
		}

		// ------------------------------------------------------------------------------------
		// Logging sinks for the module
		// ------------------------------------------------------------------------------------
		void sa_log_info(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log_line("INFO", fmt, ap); va_end(ap); }
		void sa_log_warning(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log_line("WARNING", fmt, ap); va_end(ap); }
		void sa_log_trace(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log_line("TRACE", fmt, ap); va_end(ap); }

		// ------------------------------------------------------------------------------------
		// Game mode: a copy of FFNx getmode() (common.cpp) over the FF8 mode table (ff8_data.h)
		// ------------------------------------------------------------------------------------
		// FFNx driver modes (common.h enum game_modes), used as our mode_id.
		enum : uint32_t
		{
			DM_FIELD = 0, DM_BATTLE = 1, DM_WORLDMAP = 2, DM_MENU = 3, DM_SWIRL = 12, DM_CREDITS = 15,
			DM_INTRO = 16, DM_CARDGAME = 17, DM_UNKNOWN = 18, DM_AFTER_BATTLE = 19, DM_MAIN_MENU = 20,
		};
		struct ModeEntry { uint32_t mode; uint32_t driver_mode; uint32_t main_loop; };
		// Same order as ff8_modes[]; main_loop as set by ff8_set_main_loop() (ff8_data.cpp:91/92/204-207).
		const ModeEntry MODES[] = {
			{ 0, DM_CREDITS, CREDITS_LOOP },
			{ 1, DM_FIELD, FIELD_LOOP },
			{ 2, DM_WORLDMAP, WORLD_LOOP },
			{ 3, DM_SWIRL, SWIRL_LOOP },
			{ 4, DM_AFTER_BATTLE, 0 },
			{ 5, DM_UNKNOWN, 0 },
			{ 6, DM_MENU, 0 },
			{ 7, DM_UNKNOWN, 0 },
			{ 8, DM_CARDGAME, 0 },
			{ 9, DM_UNKNOWN, 0 },
			{ 10, DM_UNKNOWN, 0 },
			{ 11, DM_UNKNOWN, 0 },
			{ 12, DM_INTRO, 0 },
			{ 100, DM_UNKNOWN, 0 },
			{ 200, DM_MAIN_MENU, MAIN_MENU_LOOP },
			{ 999, DM_BATTLE, BATTLE_LOOP },
		};
		constexpr size_t NUM_MODES = sizeof(MODES) / sizeof(MODES[0]);

		const ModeEntry* getmode()
		{
			const uint32_t obj = rd<uint32_t>(GAME_OBJ_PTR);
			const uint32_t loop = obj ? rd<uint32_t>(obj + GAME_OBJ_MAIN_LOOP) : 0;
			const uint32_t mode = rd<uint16_t>(MODE_WORD);

			for (const ModeEntry& m : MODES) if (m.main_loop == loop && m.mode == mode) return &m;
			// FF8 has battle and card game in the same module
			if (rd<uint32_t>(IS_CARD_GAME) == 1)
				for (const ModeEntry& m : MODES) if (m.mode == mode) return &m;
			for (const ModeEntry& m : MODES) if (m.main_loop && m.main_loop == loop) return &m;
			for (const ModeEntry& m : MODES) if (m.mode == mode) return &m;
			return &MODES[11];
		}

		// getmode_cached(): re-evaluated once per frame_counter.
		const ModeEntry* getmode_cached()
		{
			static uint32_t last_frame = 0xFFFFFFFF;
			static const ModeEntry* last = &MODES[11];
			if (g_frame_counter != last_frame)
			{
				last = getmode();
				last_frame = g_frame_counter;
			}
			return last;
		}

		ff8_interp::GameMode sa_game_mode()
		{
			switch (getmode_cached()->driver_mode)
			{
			case DM_FIELD: return ff8_interp::GameMode::Field;
			case DM_WORLDMAP: return ff8_interp::GameMode::World;
			case DM_BATTLE: return ff8_interp::GameMode::Battle;
			default: return ff8_interp::GameMode::Other;
			}
		}

		uint32_t sa_mode_id() { return getmode_cached()->driver_mode; }

		// FFNx get_version(): the four "FF8 1.2 US English" variants (plain, Nvidia, Eidos, Eidos Nvidia).
		bool us_signature()
		{
			const uint32_t v1 = rd<uint32_t>(0x401004), v2 = rd<uint32_t>(0x401404);
			return (v1 == 0x3885048D && (v2 == 0x159618 || v2 == 0x1597C8)) ||
				(v1 == 0x2885048D && (v2 == 0x159598 || v2 == 0x159748));
		}
		bool g_us_ok = false;
		bool sa_is_us_exe() { return g_us_ok; }

		// Every engine constant this file relies on, checked against the running image.
		bool verify_engine_constants()
		{
			struct C { uint32_t addr; uint32_t value; const char* what; };
			const C checks[] = {
				{ MAIN_LOOP + 0x115, MODE_WORD, "_mode operand in main_loop" },
				{ MAIN_LOOP + 0x144, FIELD_LOOP, "field main loop" },
				{ MAIN_LOOP + 0x2D0, WORLD_LOOP, "world map main loop" },
				{ MAIN_LOOP + 0x340, BATTLE_LOOP, "battle main loop" },
				{ MAIN_LOOP + 0x4A3, SWIRL_LOOP, "swirl main loop" },
				{ PUBINTRO_LOOP + 0x6D, CREDITS_LOOP, "credits main loop" },
				{ 0x40204C, GAME_OBJ_MAIN_LOOP, "game_loop_obj.main_loop offset (main_entry store)" },
				{ 0x402050, PUBINTRO_LOOP, "pubintro loop stored as first game loop" },
				{ GO_MAIN_MENU + 0x2B, MAIN_MENU_LOOP, "main menu main loop" },
				{ 0x534641, IS_CARD_GAME, "is_card_game operand" },
				{ 0x40A04E, GAME_OBJ_PTR, "get_game_object operand" },
				{ 0x4098F6, GAME_OBJ_GFX_DRIVER, "gfx driver offset in game_obj" },
				{ FN_LIMITER + 0x3F, VOLUME_DT, "limiter cross-fade timer operand" },
			};
			bool ok = true;
			for (const C& c : checks)
			{
				const uint32_t v = rd<uint32_t>(c.addr);
				if (v != c.value)
				{
					log_error("engine check failed: %s at %08X is %08X, expected %08X\n", c.what, c.addr, v, c.value);
					ok = false;
				}
			}
			const uint8_t flip_call[3] = { 0xFF, 0x50, 0x10 };
			if (memcmp((const void*)0x41DF32, flip_call, 3) != 0)
			{
				log_error("engine check failed: flip call at 0041DF32 is %s, expected FF 50 10\n", hex_bytes((const uint8_t*)0x41DF32, 3).c_str());
				ok = false;
			}
			return ok;
		}

		// ------------------------------------------------------------------------------------
		// Frame counter: FFNx increments frame_counter in common_flip(); we wrap the driver's flip
		// slot (a data pointer in the struct FFNx's ff8_load_driver allocated) the same way.
		// ------------------------------------------------------------------------------------
		void __cdecl flip_hook(void* game_obj)
		{
			g_ffnx_flip(game_obj);
			g_frame_counter++;
		}

		flip_fn* current_flip_slot()
		{
			const uint32_t obj = rd<uint32_t>(GAME_OBJ_PTR);
			if (!obj) return nullptr;
			const uint32_t drv = rd<uint32_t>(obj + GAME_OBJ_GFX_DRIVER);
			return drv ? (flip_fn*)(drv + DRIVER_FLIP) : nullptr;
		}

		bool hook_flip()
		{
			flip_fn* slot = current_flip_slot();
			if (slot == nullptr) { log_warning("flip: no graphics driver yet\n"); return false; }
			flip_fn cur = *slot;
			if (cur == flip_hook) return true;
			if (!in_ffnx((uint32_t)cur))
			{
				log_warning("flip: driver flip %08X is not in AF3DN.P (%s)\n", (uint32_t)cur, owner_of((uint32_t)cur).c_str());
				return false;
			}
			g_ffnx_flip = cur;
			g_flip_slot = slot;
			*slot = flip_hook;
			log_info("flip: driver flip slot %08X hooked (FFNx common_flip %08X), frame counter = flips\n", (uint32_t)slot, (uint32_t)cur);
			return true;
		}

		// ------------------------------------------------------------------------------------
		// FFNx detection
		// ------------------------------------------------------------------------------------
		std::string ffnx_version()
		{
			wchar_t path[MAX_PATH * 2] = {};
			GetModuleFileNameW(g_ffnx, path, MAX_PATH * 2);
			DWORD dummy = 0, size = GetFileVersionInfoSizeW(path, &dummy);
			if (size == 0) return "(no version resource)";
			std::vector<uint8_t> buf(size);
			if (!GetFileVersionInfoW(path, 0, size, buf.data())) return "(version resource unreadable)";
			std::string out;
			VS_FIXEDFILEINFO* fi = nullptr;
			UINT len = 0;
			char b[160];
			if (VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) && fi)
			{
				snprintf(b, sizeof(b), "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
				out = b;
			}
			struct Lang { WORD lang, cp; }* tr = nullptr;
			if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && tr && len >= sizeof(Lang))
			{
				wchar_t key[128];
				swprintf(key, 128, L"\\StringFileInfo\\%04x%04x\\ProductVersion", tr->lang, tr->cp);
				wchar_t* s = nullptr;
				if (VerQueryValueW(buf.data(), key, (void**)&s, &len) && s && len)
				{
					snprintf(b, sizeof(b), " (ProductVersion \"%ls\")", s);
					out += b;
				}
			}
			return out.empty() ? "(unknown)" : out;
		}

		bool image_contains(HMODULE m, const char* needle)
		{
			const uint8_t* base = (const uint8_t*)m;
			const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
			const IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
			const size_t n = strlen(needle);
			for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
			{
				if (!(sh[i].Characteristics & IMAGE_SCN_MEM_READ) || (sh[i].Characteristics & IMAGE_SCN_MEM_DISCARDABLE)) continue;
				const uint8_t* p = base + sh[i].VirtualAddress;
				const size_t len = sh[i].Misc.VirtualSize;
				for (size_t j = 0; j + n <= len; j++)
					if (p[j] == (uint8_t)needle[0] && memcmp(p + j, needle, n) == 0) return true;
			}
			return false;
		}

		bool detect_ffnx()
		{
			g_ffnx = GetModuleHandleA("AF3DN.P");
			if (g_ffnx == nullptr)
			{
				log_error("FFNx (AF3DN.P) is not loaded in this process. ff8interp needs FFNx Steam edition. Mod disabled.\n");
				return false;
			}
			const uint8_t* base = (const uint8_t*)g_ffnx;
			const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
			g_ffnx_lo = (uint32_t)base;
			g_ffnx_hi = g_ffnx_lo + nt->OptionalHeader.SizeOfImage;
			log_info("FFNx: AF3DN.P at %08X-%08X, version %s\n", g_ffnx_lo, g_ffnx_hi, ffnx_version().c_str());
			// The FFNx fork (branch `interp`) compiles the same module in; running both would hook every
			// site twice.
			if (image_contains(g_ffnx, "ff8_battle_anim_interp"))
			{
				log_error("FFNx: this AF3DN.P already contains the interpolation module (FFNx interp fork). Use either the fork or ff8interp.dll with a stock FFNx, not both. Mod disabled.\n");
				return false;
			}
			return true;
		}

		// ------------------------------------------------------------------------------------
		// Deferred install (game thread, first pacer call)
		// ------------------------------------------------------------------------------------
		void deferred_install()
		{
			log_info("install: first limiter call on thread %lu, starting deferred install\n", GetCurrentThreadId());
			QueryPerformanceFrequency(&g_qpf);

			if (!verify_engine_constants())
			{
				log_error("install: this FF8_EN.exe does not match the English 1.2 layout ff8interp was written for. Mod disabled (FFNx limiter left in charge).\n");
				InterlockedExchange(&g_state, ST_PASSIVE);
				return;
			}
			g_us_ok = true;

			if (!g_disk.load())
			{
				log_error("install: could not read FF8_EN.exe from disk for the byte checks. Mod disabled.\n");
				InterlockedExchange(&g_state, ST_PASSIVE);
				return;
			}

			g_counter_from_flip = hook_flip();
			if (!g_counter_from_flip)
				log_warning("install: frame counter falls back to one per limiter call (not exactly FFNx's frame_counter)\n");

			ff8_interp::InterpHost h{};
			h.hook_call = sa_hook_call;
			h.hook_call_chain = sa_hook_call_chain;
			h.hook_function = sa_hook_function;
			h.log_info = sa_log_info;
			h.log_warning = sa_log_warning;
			h.log_trace = sa_log_trace;
			h.game_mode = sa_game_mode;
			h.mode_id = sa_mode_id;
			h.frame_counter = &g_frame_counter;
			h.is_us_exe = sa_is_us_exe;

			ff8_interp::InterpConfig& c = h.config;
			c.battle_enabled = g_opt.battle_enabled;
			c.battle_fps = g_opt.battle_fps;
			c.battle_debug = g_opt.battle_debug;
			c.world_enabled = g_opt.world_enabled;
			c.world_fps = g_opt.world_fps;
			c.world_debug = g_opt.world_debug;
			c.field_enabled = g_opt.field_enabled;
			c.field_fps = g_opt.field_fps;
			c.field_debug = g_opt.field_debug;
			c.host_limiter_fps = g_opt.ffnx_fps_limiter == 2 ? 30 : g_opt.ffnx_fps_limiter == 3 ? 60 : 0;
			c.trace = g_opt.trace;

			ff8_interp::install(h);
			g_disk.release();

			log_info("install: %u call sites and %u function entries patched, %u sites skipped\n", g_n_call, g_n_func, g_n_skipped);
			if (g_n_skipped > 0 && g_opt.strict)
			{
				log_error("install: %u hook sites were already patched by someone else (see the warnings above). "
					"ff8interp_strict = true, so every ff8interp patch is rolled back and the game runs as with FFNx alone. "
					"Set ff8interp_strict = false in ff8interp.toml to keep the partial install (the modes whose gates are missing may then run too fast).\n", g_n_skipped);
				rollback_patches();
				if (g_flip_slot && *g_flip_slot == flip_hook) *g_flip_slot = g_ffnx_flip;
				InterlockedExchange(&g_state, ST_PASSIVE);
				return;
			}
			if (g_n_call + g_n_func == 0)
				log_warning("install: no sub-module installed anything (all disabled in ff8interp.toml?). Pacing stays with FFNx.\n");
			InterlockedExchange(&g_state, ST_ACTIVE);
		}
	}

	// ------------------------------------------------------------------------------------
	// The pacer: the E9 at FN_LIMITER reaches this instead of FFNx ff8_limit_fps.
	// ------------------------------------------------------------------------------------
	extern "C" int __cdecl ff8interp_pacer()
	{
		if (g_state == ST_WAITING) deferred_install();
		if (g_state != ST_ACTIVE) return g_ffnx_limit();

		// The driver table belongs to FFNx; if it was rebuilt, hook the new one.
		if (g_counter_from_flip)
		{
			flip_fn* slot = current_flip_slot();
			if (slot != g_flip_slot || (slot && *slot != flip_hook))
			{
				log_warning("flip: driver flip slot changed (%08X -> %08X), re-hooking\n", (uint32_t)g_flip_slot, (uint32_t)slot);
				g_counter_from_flip = hook_flip();
			}
		}

		ff8_interp::on_frame();
		const double rate = ff8_interp::frame_rate(-1.0); // < 0: no override for this mode

		const int64_t entry = qpc();
		if (g_opt.diag || g_opt.trace)
		{
			g_diag_calls++;
			if (rate > 0.0) g_diag_paced++; else g_diag_ffnx++;
			if (g_diag_t0 == 0) { g_diag_t0 = entry; g_diag_flip0 = g_frame_counter; }
			const double dt = double(entry - g_diag_t0) / double(g_qpf.QuadPart);
			if (dt >= 1.0)
			{
				static const char* const GM[] = { "Other", "Field", "World", "Battle" };
				const uint32_t obj = rd<uint32_t>(GAME_OBJ_PTR);
				const ModeEntry* m = getmode_cached();
				log_info("diag: mode %s (FFNx driver_mode %u, _mode %u, loop %08X, card %u) rate %s%.0f | %.1f limiter calls/s: %u paced, %u to FFNx | %.1f flips/s | frame %u\n",
					GM[(int)sa_game_mode()], m->driver_mode, (unsigned)rd<uint16_t>(MODE_WORD),
					obj ? rd<uint32_t>(obj + GAME_OBJ_MAIN_LOOP) : 0, rd<uint32_t>(IS_CARD_GAME),
					rate > 0.0 ? "" : "FFNx/", rate > 0.0 ? rate : 0.0,
					g_diag_calls / dt, g_diag_paced, g_diag_ffnx, (g_frame_counter - g_diag_flip0) / dt, g_frame_counter);
				g_diag_t0 = entry; g_diag_flip0 = g_frame_counter; g_diag_calls = g_diag_paced = g_diag_ffnx = 0;
			}
		}
		double* const volume_dt = (double*)VOLUME_DT;

		if (rate <= 0.0)
		{
			// Mode we do not interpolate: FFNx's own limiter (its rate, speedhack and cross-fade timer).
			const int rc = g_ffnx_limit();
			// FFNx measured its cross-fade step against its own last call, which is stale after frames
			// we paced; give the game the real last-frame time instead.
			if (g_paced_last && g_last_exit) *volume_dt = 1000.0 * double(entry - g_last_exit) / double(g_qpf.QuadPart);
			g_paced_last = false;
			g_ffnx_frames++;
			g_last_exit = qpc();
			if (!g_counter_from_flip) g_frame_counter++;
			return rc;
		}

		// Our mode: the same busy-wait as FFNx ff8_limit_fps at the ff8_*_fps rate, plus its one side
		// effect, the cross-fade timer (ms since the previous limiter exit).
		*volume_dt = g_last_exit ? 1000.0 * double(entry - g_last_exit) / double(g_qpf.QuadPart) : 1000.0 / rate;
		const double frame_ticks = double(g_qpf.QuadPart) / rate;
		int64_t now;
		for (;;)
		{
			now = qpc();
			if (!(now > g_last_exit && double(now - g_last_exit) < frame_ticks)) break;
			_mm_pause();
		}
		g_last_exit = now;
		g_paced_last = true;
		if ((++g_paced_frames % 36000) == 0)
			log_info("pacer: %u frames paced at ff8_*_fps, %u passed to FFNx, frame %u\n", g_paced_frames, g_ffnx_frames, g_frame_counter);
		if (!g_counter_from_flip) g_frame_counter++;
		return 0;
	}

	// ------------------------------------------------------------------------------------
	// Watcher thread: find FFNx's limiter jump and redirect it to the pacer.
	// ------------------------------------------------------------------------------------
	DWORD WINAPI watcher_thread(LPVOID)
	{
		log_open();
		// Our code ends up in the game's call paths: never let a FreeLibrary unload it.
		HMODULE pinned = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)&watcher_thread, &pinned);
		const std::string game_dir = module_dir(nullptr);
		const std::string dll_dir = module_dir(g_self);
		log_info("ff8interp " FF8INTERP_VERSION " (FF8 Smooth Frames) loaded from %s, game folder %s\n", dll_dir.c_str(), game_dir.c_str());

		if (!us_signature())
		{
			log_error("this is not the English FF8 1.2 executable (FF8_EN.exe, Steam 2013 / 2000 v1.2). Mod disabled.\n");
			return 0;
		}
		if (!detect_ffnx()) return 0;

		g_opt = load_options(dll_dir, game_dir);
		log_info("config: %s\n", g_opt.source.c_str());
		log_info("config: battle %s %ld fps (debug %ld), world %s %ld fps (debug %ld), field %s %ld fps (debug %ld), trace %d, strict %d, diag %d; FFNx ff8_fps_limiter = %ld\n",
			g_opt.battle_enabled ? "on" : "off", g_opt.battle_fps, g_opt.battle_debug,
			g_opt.world_enabled ? "on" : "off", g_opt.world_fps, g_opt.world_debug,
			g_opt.field_enabled ? "on" : "off", g_opt.field_fps, g_opt.field_debug,
			(int)g_opt.trace, (int)g_opt.strict, (int)g_opt.diag, g_opt.ffnx_fps_limiter);

		// Wait for FFNx's ff8_init_hooks: E9 at the limiter entry. If the graphics driver exists but the
		// limiter stays vanilla, FFNx runs with ff8_fps_limiter = 0 and there is nothing to chain to.
		const DWORD start = GetTickCount();
		DWORD driver_seen = 0;
		uint32_t target = 0;
		for (;;)
		{
			const uint8_t* p = (const uint8_t*)FN_LIMITER;
			if (p[0] == 0xE9)
			{
				const uint32_t t1 = FN_LIMITER + 5 + rd<int32_t>(FN_LIMITER + 1);
				Sleep(20); // replace_function writes opcode then operand; let it settle
				const uint32_t t2 = FN_LIMITER + 5 + rd<int32_t>(FN_LIMITER + 1);
				if (t1 == t2)
				{
					target = t1;
					break;
				}
				continue;
			}
			if (memcmp(p, LIMITER_PROLOGUE, sizeof(LIMITER_PROLOGUE)) != 0)
			{
				log_error("limiter entry %08X holds unexpected bytes %s (neither vanilla nor an FFNx jump). Mod disabled.\n", FN_LIMITER, hex_bytes(p, 5).c_str());
				return 0;
			}
			if (!driver_seen && current_flip_slot() && *current_flip_slot()) driver_seen = GetTickCount();
			if (driver_seen && GetTickCount() - driver_seen > 10000)
			{
				log_error("FFNx created its graphics driver but did not hook the frame limiter at %08X within 10 s. "
					"FFNx only does that with ff8_fps_limiter >= 1 in FFNx.toml (read: %ld). Mod disabled.\n", FN_LIMITER, g_opt.ffnx_fps_limiter);
				return 0;
			}
			if (GetTickCount() - start > 600000)
			{
				log_error("FFNx did not hook the frame limiter within 10 minutes. Mod disabled.\n");
				return 0;
			}
			Sleep(1);
		}

		if (!in_ffnx(target))
		{
			log_error("the jump at the limiter entry %08X reaches %08X in %s, not FFNx's ff8_limit_fps in AF3DN.P. "
				"Another mod owns the frame limiter. Mod disabled.\n", FN_LIMITER, target, owner_of(target).c_str());
			return 0;
		}

		g_ffnx_limit = (limit_fn)target;
		MemoryBarrier();
		// Redirect: rewrite only the rel32 of FFNx's jump. 0x4020F1..F4 lies inside one cache line, so
		// the aligned-or-not 32-bit store is seen whole by the game thread (old or new target, both valid).
		const uint32_t rel = (uint32_t)&ff8interp_pacer - FN_LIMITER - 5;
		DWORD old;
		VirtualProtect((void*)FN_LIMITER, 5, PAGE_EXECUTE_READWRITE, &old);
		InterlockedExchange((volatile LONG*)(FN_LIMITER + 1), (LONG)rel);
		VirtualProtect((void*)FN_LIMITER, 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), (void*)FN_LIMITER, 5);
		log_info("limiter: FFNx ff8_limit_fps at %08X (AF3DN.P+%X); jump at %08X now reaches the ff8interp pacer %08X after %lu ms\n",
			target, target - g_ffnx_lo, FN_LIMITER, (uint32_t)&ff8interp_pacer, GetTickCount() - start);
		return 0;
	}
}
