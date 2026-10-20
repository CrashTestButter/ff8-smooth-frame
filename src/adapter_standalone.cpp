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
// next to an UNMODIFIED stock FFNx (AF3DN.P; eax.dll on the 2000 / Remastered path). This file
// replaces adapter_ffnx.cpp; the module sources are compiled unchanged. See ../README.md for the full
// picture, in short:
//
//  1. DllMain starts watcher_thread() and returns. No hook is installed under the loader lock, and
//     FFNx has not run ff8_init_hooks yet at that point (J8 loads us in its WinMain detour, an ASI
//     loader even earlier).
//  2. watcher_thread() polls the game's frame limiter entry FPS_LIMITER_ENTRY (0x4020F0). FFNx's
//     ff8_init_hooks writes `E9 rel32` there (replace_function(ff8_externals.fps_limiter,
//     ff8_limit_fps)) when ff8_fps_limiter >= 1 (the default). Once that jump is present, stable and
//     points into FFNx, the watcher remembers FFNx's ff8_limit_fps and rewrites the rel32 so the
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
		constexpr uint32_t FPS_LIMITER_ENTRY = 0x4020F0;
		constexpr uint32_t MAIN_LOOP = 0x4706B0;
		constexpr uint32_t PUBINTRO_LOOP = 0x4703B0;
		constexpr uint32_t GO_TO_MAIN_MENU = 0x470520;
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
		constexpr uint32_t CROSSFADE_FRAME_MS = 0x1A78BE0;  // double, ms of the last frame (music cross-fade)

		// Vanilla first bytes of the limiter (sub esp,0x10 / lea eax,[esp+0]).
		const uint8_t LIMITER_PROLOGUE[5] = { 0x83, 0xEC, 0x10, 0x8D, 0x44 };

		// ------------------------------------------------------------------------------------
		// State
		// ------------------------------------------------------------------------------------
		enum State : LONG { PACER_WAITING = 0, PACER_ACTIVE = 1, PACER_PASSIVE = 2 };
		volatile LONG g_state = PACER_WAITING;

		typedef int(__cdecl* limiter_fn)();
		typedef void(__cdecl* flip_fn)(void* game_obj);

		limiter_fn g_ffnx_limit_fps = nullptr;     // FFNx ff8_limit_fps, read from the E9 at FPS_LIMITER_ENTRY
		flip_fn g_ffnx_common_flip = nullptr;      // FFNx common_flip, read from the driver table
		flip_fn* g_flip_slot = nullptr;
		uint32_t g_frame_counter = 0;              // our FFNx frame_counter: +1 per driver flip
		bool g_frame_counter_from_flip = false;    // false: fallback, pacer counts frames

		HMODULE g_ffnx_module = nullptr;
		const char* g_ffnx_module_name = "AF3DN.P";  // AF3DN.P (Steam 2013) or eax.dll (2000 / Remastered)
		uint32_t g_ffnx_image_start = 0, g_ffnx_image_end = 0;
		Options g_options;

		LARGE_INTEGER g_counter_frequency{};
		int64_t g_last_exit_time = 0;              // QPC at the end of the previous pacer call
		bool g_previous_frame_paced = false;       // previous frame used our own wait
		uint32_t g_frames_paced = 0, g_frames_passed_to_ffnx = 0;
		// FFNx gamehacks.getCurrentSpeedhack() (thiscall, no stack arguments, result in st0), called
		// through __fastcall so `self` lands in ecx. Found in ff8_limit_fps by find_speedhack().
		typedef double(__fastcall* speedhack_getter_fn)(void* self, void* unused_edx);
		speedhack_getter_fn g_speedhack_getter = nullptr;
		void* g_gamehacks_object = nullptr;
		// ff8interp_diag: one line per second from the pacer
		int64_t g_diag_window_start = 0;
		uint32_t g_diag_paced_calls = 0, g_diag_ffnx_calls = 0;
		uint32_t g_diag_window_start_frame = 0;
		uint32_t g_diag_limiter_calls = 0;

		int64_t performance_counter_now()
		{
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}

		template<typename T> T read_memory(uint32_t address)
		{
			T value;
			memcpy(&value, (const void*)address, sizeof(T));
			return value;
		}

		bool is_in_ffnx(uint32_t address) { return address >= g_ffnx_image_start && address < g_ffnx_image_end; }

		std::string owner_of(uint32_t address)
		{
			HMODULE module_handle = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module_handle) || !module_handle)
				return "no module (heap / VirtualAlloc)";
			char path[MAX_PATH] = {};
			GetModuleFileNameA(module_handle, path, MAX_PATH);
			const char* file_name = strrchr(path, '\\');
			return file_name ? file_name + 1 : path;
		}

		std::string hex_bytes(const uint8_t* bytes, size_t count)
		{
			std::string text;
			char hex_byte[4];
			for (size_t i = 0; i < count; i++)
			{
				snprintf(hex_byte, sizeof(hex_byte), i ? " %02X" : "%02X", bytes[i]);
				text += hex_byte;
			}
			return text;
		}

		// ------------------------------------------------------------------------------------
		// FF8_EN.exe as shipped (on disk): the reference for "expected original bytes"
		// ------------------------------------------------------------------------------------
		struct DiskImage
		{
			std::vector<uint8_t> file;
			uint32_t image_base = 0;
			struct Section { uint32_t virtual_address, virtual_size, raw_offset, raw_size; };
			std::vector<Section> sections;

			bool load()
			{
				char path[MAX_PATH * 2] = {};
				GetModuleFileNameA(nullptr, path, (DWORD)sizeof(path));
				FILE* exe_file = fopen(path, "rb");
				if (!exe_file)
				{
					log_error("cannot open %s for the byte checks\n", path);
					return false;
				}
				fseek(exe_file, 0, SEEK_END);
				long file_size = ftell(exe_file);
				fseek(exe_file, 0, SEEK_SET);
				file.resize(file_size > 0 ? (size_t)file_size : 0);
				size_t bytes_read = file.empty() ? 0 : fread(file.data(), 1, file.size(), exe_file);
				fclose(exe_file);
				if (bytes_read != file.size() || file.size() < 0x400)
				{
					file.clear();
					return false;
				}
				const IMAGE_DOS_HEADER* dos_header = (const IMAGE_DOS_HEADER*)file.data();
				if (dos_header->e_magic != IMAGE_DOS_SIGNATURE || (size_t)dos_header->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > file.size())
				{
					file.clear();
					return false;
				}
				const IMAGE_NT_HEADERS32* nt_headers = (const IMAGE_NT_HEADERS32*)(file.data() + dos_header->e_lfanew);
				image_base = nt_headers->OptionalHeader.ImageBase;
				const IMAGE_SECTION_HEADER* section_headers = IMAGE_FIRST_SECTION(nt_headers);
				for (int i = 0; i < nt_headers->FileHeader.NumberOfSections; i++)
					sections.push_back({ section_headers[i].VirtualAddress, section_headers[i].Misc.VirtualSize, section_headers[i].PointerToRawData, section_headers[i].SizeOfRawData });
				return true;
			}

			const uint8_t* at(uint32_t address, size_t length) const
			{
				if (file.empty() || address < image_base) return nullptr;
				uint32_t rva = address - image_base;
				for (const Section& section : sections)
					if (rva >= section.virtual_address && rva + length <= section.virtual_address + section.raw_size)
					{
						size_t file_offset = (size_t)section.raw_offset + (rva - section.virtual_address);
						return file_offset + length <= file.size() ? file.data() + file_offset : nullptr;
					}
				return nullptr;
			}

			void release()
			{
				std::vector<uint8_t>().swap(file);
				sections.clear();
			}
		};
		DiskImage g_disk_image;

		// ------------------------------------------------------------------------------------
		// Patching: hook_call / hook_function with byte verification and rollback
		// ------------------------------------------------------------------------------------
		struct Patch { uint32_t address; uint8_t length; uint8_t original[6]; uint8_t written[6]; };
		std::vector<Patch> g_patches;
		uint32_t g_call_sites_patched = 0, g_function_entries_patched = 0, g_sites_skipped = 0;

		void write_code(uint32_t address, const uint8_t* bytes, size_t length)
		{
			DWORD old_protection;
			VirtualProtect((void*)address, length, PAGE_EXECUTE_READWRITE, &old_protection);
			memcpy((void*)address, bytes, length);
			VirtualProtect((void*)address, length, old_protection, &old_protection);
			FlushInstructionCache(GetCurrentProcess(), (void*)address, length);
		}

		Patch* find_patch(uint32_t address)
		{
			for (Patch& patch : g_patches)
			{
				if (patch.address == address) return &patch;
			}
			return nullptr;
		}

		// True if the `length` bytes at address may be patched: they equal FF8_EN.exe on disk, or they are
		// exactly what we wrote there before. Otherwise log what is there and who owns the target.
		bool verify_site(const char* kind, uint32_t address, size_t length)
		{
			const uint8_t* memory_bytes = (const uint8_t*)address;
			const uint8_t* disk_bytes = g_disk_image.at(address, length);
			if (disk_bytes == nullptr)
			{
				log_warning("%s %08X: outside FF8_EN.exe sections, skipped\n", kind, address);
				return false;
			}
			if (memcmp(memory_bytes, disk_bytes, length) == 0) return true;
			if (Patch* patch = find_patch(address))
			{
				if (patch->length == length && memcmp(memory_bytes, patch->written, length) == 0) return true;
			}

			std::string redirect_note;
			if (memory_bytes[0] == 0xE8 || memory_bytes[0] == 0xE9)
			{
				uint32_t target = address + 5 + read_memory<int32_t>(address + 1);
				char note[96];
				snprintf(note, sizeof(note), ", now reaches %08X in %s", target, owner_of(target).c_str());
				redirect_note = note;
			}
			log_warning("%s %08X: bytes differ from FF8_EN.exe (memory %s, disk %s%s) - patched by FFNx or another mod, skipped\n",
				kind, address, hex_bytes(memory_bytes, length).c_str(), hex_bytes(disk_bytes, length).c_str(), redirect_note.c_str());
			g_sites_skipped++;
			return false;
		}

		void record_and_write(uint32_t address, const uint8_t* bytes, uint8_t length)
		{
			Patch* patch = find_patch(address);
			if (patch == nullptr)
			{
				Patch new_patch{};
				new_patch.address = address;
				new_patch.length = length;
				memcpy(new_patch.original, (const void*)address, length);
				g_patches.push_back(new_patch);
				patch = &g_patches.back();
			}
			memcpy(patch->written, bytes, length);
			write_code(address, bytes, length);
		}

		// InterpHost::hook_call - FFNx replace_call semantics (E8 / E9 rel32, FF 15 operand = hook - site - 6).
		void host_hook_call(uint32_t site, void* hook)
		{
			const uint8_t* code = (const uint8_t*)site;
			uint8_t opcode_size;
			if (code[0] == 0xE8 || code[0] == 0xE9) opcode_size = 1;
			else if (code[0] == 0xFF && code[1] == 0x15) opcode_size = 2;
			else
			{
				log_warning("hook_call %08X: not a call/jmp (%s), skipped\n", site, hex_bytes(code, 6).c_str());
				g_sites_skipped++;
				return;
			}
			const uint8_t length = (uint8_t)(opcode_size + 4);
			if (!verify_site("hook_call", site, length)) return;
			uint8_t bytes[6];
			memcpy(bytes, code, length);
			uint32_t relative = (uint32_t)hook - site - length;
			memcpy(bytes + opcode_size, &relative, 4);
			record_and_write(site, bytes, length);
			g_call_sites_patched++;
		}

		// InterpHost::hook_call_chain - for a call site FFNx itself redirects. Allowed when the bytes are
		// still the game's (then the chain target is the game's own callee) or when the site is an E8/E9
		// that now reaches into the FFNx module; anything else (another mod) is still refused. Returns the
		// function the site reached before our patch, 0 when refused.
		uint32_t host_hook_call_chain(uint32_t site, void* hook)
		{
			const uint8_t* code = (const uint8_t*)site;
			if (code[0] != 0xE8 && code[0] != 0xE9)
			{
				log_warning("hook_call_chain %08X: not a call/jmp (%s), skipped\n", site, hex_bytes(code, 5).c_str());
				g_sites_skipped++;
				return 0;
			}
			const uint32_t target = site + 5 + read_memory<int32_t>(site + 1);
			const uint8_t* disk_bytes = g_disk_image.at(site, 5);
			const bool pristine = disk_bytes != nullptr && memcmp(code, disk_bytes, 5) == 0;
			if (!pristine)
			{
				if (find_patch(site) == nullptr && !is_in_ffnx(target))
				{
					log_warning("hook_call_chain %08X: redirected by %s (%08X), not FFNx - skipped\n", site, owner_of(target).c_str(), target);
					g_sites_skipped++;
					return 0;
				}
				log_info("hook_call_chain %08X: FFNx redirects this call to %08X, chaining to it\n", site, target);
			}
			uint8_t bytes[5];
			bytes[0] = code[0];
			uint32_t relative = (uint32_t)hook - site - 5;
			memcpy(bytes + 1, &relative, 4);
			record_and_write(site, bytes, 5);
			g_call_sites_patched++;
			return target;
		}

		// InterpHost::hook_function - FFNx replace_function semantics (E9 rel32 over 5 bytes, no trampoline).
		uint32_t host_hook_function(uint32_t address, void* hook)
		{
			if (!verify_site("hook_function", address, 5)) return 0;
			uint8_t bytes[5] = { 0xE9 };
			uint32_t relative = (uint32_t)hook - address - 5;
			memcpy(bytes + 1, &relative, 4);
			record_and_write(address, bytes, 5);
			g_function_entries_patched++;
			return (uint32_t)g_patches.size();
		}

		void rollback_patches()
		{
			for (size_t i = g_patches.size(); i-- > 0;)
				write_code(g_patches[i].address, g_patches[i].original, g_patches[i].length);
			log_warning("rolled back %u patches\n", (unsigned)g_patches.size());
			g_patches.clear();
		}

		// ------------------------------------------------------------------------------------
		// Logging sinks for the module
		// ------------------------------------------------------------------------------------
		void host_log_info(const char* fmt, ...)
		{
			va_list args;
			va_start(args, fmt);
			log_line("INFO", fmt, args);
			va_end(args);
		}

		void host_log_warning(const char* fmt, ...)
		{
			va_list args;
			va_start(args, fmt);
			log_line("WARNING", fmt, args);
			va_end(args);
		}

		void host_log_trace(const char* fmt, ...)
		{
			va_list args;
			va_start(args, fmt);
			log_line("TRACE", fmt, args);
			va_end(args);
		}

		// ------------------------------------------------------------------------------------
		// Game mode: a copy of FFNx getmode() (common.cpp) over the FF8 mode table (ff8_data.h)
		// ------------------------------------------------------------------------------------
		// FFNx driver modes (common.h enum game_modes), used as our mode_id.
		enum : uint32_t
		{
			DRIVER_MODE_FIELD = 0, DRIVER_MODE_BATTLE = 1, DRIVER_MODE_WORLDMAP = 2, DRIVER_MODE_MENU = 3,
			DRIVER_MODE_SWIRL = 12, DRIVER_MODE_CREDITS = 15, DRIVER_MODE_INTRO = 16, DRIVER_MODE_CARDGAME = 17,
			DRIVER_MODE_UNKNOWN = 18, DRIVER_MODE_AFTER_BATTLE = 19, DRIVER_MODE_MAIN_MENU = 20,
		};
		struct ModeEntry { uint32_t mode; uint32_t driver_mode; uint32_t main_loop; };
		// Same order as ff8_modes[]; main_loop as set by ff8_set_main_loop() (ff8_data.cpp:91/92/204-207).
		const ModeEntry MODE_TABLE[] = {
			{ 0, DRIVER_MODE_CREDITS, CREDITS_LOOP },
			{ 1, DRIVER_MODE_FIELD, FIELD_LOOP },
			{ 2, DRIVER_MODE_WORLDMAP, WORLD_LOOP },
			{ 3, DRIVER_MODE_SWIRL, SWIRL_LOOP },
			{ 4, DRIVER_MODE_AFTER_BATTLE, 0 },
			{ 5, DRIVER_MODE_UNKNOWN, 0 },
			{ 6, DRIVER_MODE_MENU, 0 },
			{ 7, DRIVER_MODE_UNKNOWN, 0 },
			{ 8, DRIVER_MODE_CARDGAME, 0 },
			{ 9, DRIVER_MODE_UNKNOWN, 0 },
			{ 10, DRIVER_MODE_UNKNOWN, 0 },
			{ 11, DRIVER_MODE_UNKNOWN, 0 },
			{ 12, DRIVER_MODE_INTRO, 0 },
			{ 100, DRIVER_MODE_UNKNOWN, 0 },
			{ 200, DRIVER_MODE_MAIN_MENU, MAIN_MENU_LOOP },
			{ 999, DRIVER_MODE_BATTLE, BATTLE_LOOP },
		};
		constexpr size_t MODE_COUNT = sizeof(MODE_TABLE) / sizeof(MODE_TABLE[0]);
		constexpr size_t UNKNOWN_MODE_INDEX = 11;

		const ModeEntry* getmode()
		{
			const uint32_t game_object = read_memory<uint32_t>(GAME_OBJ_PTR);
			const uint32_t main_loop = game_object ? read_memory<uint32_t>(game_object + GAME_OBJ_MAIN_LOOP) : 0;
			const uint32_t mode = read_memory<uint16_t>(MODE_WORD);

			for (const ModeEntry& entry : MODE_TABLE)
			{
				if (entry.main_loop == main_loop && entry.mode == mode) return &entry;
			}
			// FF8 has battle and card game in the same module
			if (read_memory<uint32_t>(IS_CARD_GAME) == 1)
			{
				for (const ModeEntry& entry : MODE_TABLE)
				{
					if (entry.mode == mode) return &entry;
				}
			}
			for (const ModeEntry& entry : MODE_TABLE)
			{
				if (entry.main_loop && entry.main_loop == main_loop) return &entry;
			}
			for (const ModeEntry& entry : MODE_TABLE)
			{
				if (entry.mode == mode) return &entry;
			}
			return &MODE_TABLE[UNKNOWN_MODE_INDEX];
		}

		// getmode_cached(): re-evaluated once per frame_counter.
		const ModeEntry* getmode_cached()
		{
			static uint32_t cached_frame = 0xFFFFFFFF;
			static const ModeEntry* cached_entry = &MODE_TABLE[UNKNOWN_MODE_INDEX];
			if (g_frame_counter != cached_frame)
			{
				cached_entry = getmode();
				cached_frame = g_frame_counter;
			}
			return cached_entry;
		}

		ff8_interp::GameMode host_game_mode()
		{
			switch (getmode_cached()->driver_mode)
			{
			case DRIVER_MODE_FIELD: return ff8_interp::GameMode::Field;
			case DRIVER_MODE_WORLDMAP: return ff8_interp::GameMode::World;
			case DRIVER_MODE_BATTLE: return ff8_interp::GameMode::Battle;
			default: return ff8_interp::GameMode::Other;
			}
		}

		uint32_t host_mode_id() { return getmode_cached()->driver_mode; }

		// FFNx get_version(): the four "FF8 1.2 US English" variants (plain, Nvidia, Eidos, Eidos Nvidia).
		bool has_us_english_signature()
		{
			const uint32_t code_at_401004 = read_memory<uint32_t>(0x401004);
			const uint32_t code_at_401404 = read_memory<uint32_t>(0x401404);
			return (code_at_401004 == 0x3885048D && (code_at_401404 == 0x159618 || code_at_401404 == 0x1597C8)) ||
				(code_at_401004 == 0x2885048D && (code_at_401404 == 0x159598 || code_at_401404 == 0x159748));
		}
		bool g_us_exe_verified = false;
		bool host_is_us_exe() { return g_us_exe_verified; }

		// Every engine constant this file relies on, checked against the running image.
		bool verify_engine_constants()
		{
			struct EngineCheck { uint32_t address; uint32_t expected; const char* description; };
			const EngineCheck checks[] = {
				{ MAIN_LOOP + 0x115, MODE_WORD, "_mode operand in main_loop" },
				{ MAIN_LOOP + 0x144, FIELD_LOOP, "field main loop" },
				{ MAIN_LOOP + 0x2D0, WORLD_LOOP, "world map main loop" },
				{ MAIN_LOOP + 0x340, BATTLE_LOOP, "battle main loop" },
				{ MAIN_LOOP + 0x4A3, SWIRL_LOOP, "swirl main loop" },
				{ PUBINTRO_LOOP + 0x6D, CREDITS_LOOP, "credits main loop" },
				{ 0x40204C, GAME_OBJ_MAIN_LOOP, "game_loop_obj.main_loop offset (main_entry store)" },
				{ 0x402050, PUBINTRO_LOOP, "pubintro loop stored as first game loop" },
				{ GO_TO_MAIN_MENU + 0x2B, MAIN_MENU_LOOP, "main menu main loop" },
				{ 0x534641, IS_CARD_GAME, "is_card_game operand" },
				{ 0x40A04E, GAME_OBJ_PTR, "get_game_object operand" },
				{ 0x4098F6, GAME_OBJ_GFX_DRIVER, "gfx driver offset in game_obj" },
				{ FPS_LIMITER_ENTRY + 0x3F, CROSSFADE_FRAME_MS, "limiter cross-fade timer operand" },
			};
			bool all_match = true;
			for (const EngineCheck& check : checks)
			{
				const uint32_t actual = read_memory<uint32_t>(check.address);
				if (actual != check.expected)
				{
					log_error("engine check failed: %s at %08X is %08X, expected %08X\n", check.description, check.address, actual, check.expected);
					all_match = false;
				}
			}
			const uint8_t flip_call[3] = { 0xFF, 0x50, 0x10 };
			if (memcmp((const void*)0x41DF32, flip_call, 3) != 0)
			{
				log_error("engine check failed: flip call at 0041DF32 is %s, expected FF 50 10\n", hex_bytes((const uint8_t*)0x41DF32, 3).c_str());
				all_match = false;
			}
			return all_match;
		}

		// ------------------------------------------------------------------------------------
		// Frame counter: FFNx increments frame_counter in common_flip(); we wrap the driver's flip
		// slot (a data pointer in the struct FFNx's ff8_load_driver allocated) the same way.
		// ------------------------------------------------------------------------------------
		void __cdecl flip_hook(void* game_obj)
		{
			g_ffnx_common_flip(game_obj);
			g_frame_counter++;
		}

		flip_fn* current_flip_slot()
		{
			const uint32_t game_object = read_memory<uint32_t>(GAME_OBJ_PTR);
			if (!game_object) return nullptr;
			const uint32_t driver = read_memory<uint32_t>(game_object + GAME_OBJ_GFX_DRIVER);
			return driver ? (flip_fn*)(driver + DRIVER_FLIP) : nullptr;
		}

		bool hook_flip()
		{
			flip_fn* slot = current_flip_slot();
			if (slot == nullptr)
			{
				log_warning("flip: no graphics driver yet\n");
				return false;
			}
			flip_fn current_flip = *slot;
			if (current_flip == flip_hook) return true;
			if (!is_in_ffnx((uint32_t)current_flip))
			{
				log_warning("flip: driver flip %08X is not in %s (%s)\n", (uint32_t)current_flip, g_ffnx_module_name, owner_of((uint32_t)current_flip).c_str());
				return false;
			}
			g_ffnx_common_flip = current_flip;
			g_flip_slot = slot;
			*slot = flip_hook;
			log_info("flip: driver flip slot %08X hooked (FFNx common_flip %08X), frame counter = flips\n", (uint32_t)slot, (uint32_t)current_flip);
			return true;
		}

		// ------------------------------------------------------------------------------------
		// FFNx detection
		// ------------------------------------------------------------------------------------
		std::string ffnx_version()
		{
			wchar_t path[MAX_PATH * 2] = {};
			GetModuleFileNameW(g_ffnx_module, path, MAX_PATH * 2);
			DWORD dummy = 0;
			DWORD size = GetFileVersionInfoSizeW(path, &dummy);
			if (size == 0) return "(no version resource)";
			std::vector<uint8_t> version_info(size);
			if (!GetFileVersionInfoW(path, 0, size, version_info.data())) return "(version resource unreadable)";
			std::string out;
			VS_FIXEDFILEINFO* fixed_info = nullptr;
			UINT length = 0;
			char text[160];
			if (VerQueryValueW(version_info.data(), L"\\", (void**)&fixed_info, &length) && fixed_info)
			{
				snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(fixed_info->dwFileVersionMS), LOWORD(fixed_info->dwFileVersionMS), HIWORD(fixed_info->dwFileVersionLS), LOWORD(fixed_info->dwFileVersionLS));
				out = text;
			}
			struct Translation { WORD language, codepage; };
			Translation* translation = nullptr;
			if (VerQueryValueW(version_info.data(), L"\\VarFileInfo\\Translation", (void**)&translation, &length) && translation && length >= sizeof(Translation))
			{
				wchar_t key[128];
				swprintf(key, 128, L"\\StringFileInfo\\%04x%04x\\ProductVersion", translation->language, translation->codepage);
				wchar_t* product_version = nullptr;
				if (VerQueryValueW(version_info.data(), key, (void**)&product_version, &length) && product_version && length)
				{
					snprintf(text, sizeof(text), " (ProductVersion \"%ls\")", product_version);
					out += text;
				}
			}
			return out.empty() ? "(unknown)" : out;
		}

		bool image_contains(HMODULE module_handle, const char* needle)
		{
			const uint8_t* base = (const uint8_t*)module_handle;
			const IMAGE_NT_HEADERS32* nt_headers = (const IMAGE_NT_HEADERS32*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
			const IMAGE_SECTION_HEADER* section_headers = IMAGE_FIRST_SECTION(nt_headers);
			const size_t needle_length = strlen(needle);
			for (int i = 0; i < nt_headers->FileHeader.NumberOfSections; i++)
			{
				if (!(section_headers[i].Characteristics & IMAGE_SCN_MEM_READ) || (section_headers[i].Characteristics & IMAGE_SCN_MEM_DISCARDABLE)) continue;
				const uint8_t* section_data = base + section_headers[i].VirtualAddress;
				const size_t section_size = section_headers[i].Misc.VirtualSize;
				for (size_t j = 0; j + needle_length <= section_size; j++)
				{
					if (section_data[j] == (uint8_t)needle[0] && memcmp(section_data + j, needle, needle_length) == 0) return true;
				}
			}
			return false;
		}

		// FFNx ships as AF3DN.P for the Steam 2013 edition and as eax.dll for the 2000 release (which
		// Junction VIII also runs on the Remastered edition's data). Both load the same FF8 1.2 image. The
		// Steam 2013 folder has a real Creative eax.dll too, so eax.dll only counts when it is FFNx.
		bool detect_ffnx()
		{
			g_ffnx_module = GetModuleHandleA("AF3DN.P");
			if (g_ffnx_module == nullptr)
			{
				HMODULE eax_module = GetModuleHandleA("eax.dll");
				if (eax_module && image_contains(eax_module, "FFNx driver version"))
				{
					g_ffnx_module = eax_module;
					g_ffnx_module_name = "eax.dll";
				}
			}
			if (g_ffnx_module == nullptr)
			{
				log_error("FFNx is not loaded in this process (neither AF3DN.P, Steam 2013 edition, nor an FFNx eax.dll, 2000 / Remastered under Junction VIII). Mod disabled.\n");
				return false;
			}
			const uint8_t* base = (const uint8_t*)g_ffnx_module;
			const IMAGE_NT_HEADERS32* nt_headers = (const IMAGE_NT_HEADERS32*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
			g_ffnx_image_start = (uint32_t)base;
			g_ffnx_image_end = g_ffnx_image_start + nt_headers->OptionalHeader.SizeOfImage;
			log_info("FFNx: %s at %08X-%08X, version %s\n", g_ffnx_module_name, g_ffnx_image_start, g_ffnx_image_end, ffnx_version().c_str());
			// The FFNx fork (branch `interp`) compiles the same module in; running both would hook every
			// site twice.
			if (image_contains(g_ffnx_module, "ff8_battle_anim_interp"))
			{
				log_error("FFNx: this %s already contains the interpolation module (FFNx interp fork). Use either the fork or ff8interp.dll with a stock FFNx, not both. Mod disabled.\n", g_ffnx_module_name);
				return false;
			}
			return true;
		}

		// ------------------------------------------------------------------------------------
		// Speedhack: FFNx's limiter multiplies its frame rate by gamehacks.getCurrentSpeedhack(); our
		// pacer replaces that limiter in the smoothed modes, so we read the same value. The call is
		// compiled (1.24.3) as
		//   B9 <&gamehacks>  E8 <getCurrentSpeedhack>  DC 4D xx   (mov ecx,imm / call / fmul [ebp-x])
		// and is the only thiscall-on-a-global in the function. Same keys, popup, step and max as
		// FFNx (Ctrl+Up/Down/Left/Right, pad L3 then R1/L1/L2|R2) on keyboard and controller alike.
		// ------------------------------------------------------------------------------------
		double host_speed()
		{
			if (g_speedhack_getter == nullptr) return 1.0;
			const double speed = g_speedhack_getter(g_gamehacks_object, nullptr);
			return (speed >= 0.05 && speed <= 64.0) ? speed : 1.0; // NaN / garbage: normal speed
		}

		void find_speedhack()
		{
			const uint8_t* code = (const uint8_t*)g_ffnx_limit_fps;
			for (int i = 0; i + 13 <= 0x180; i++)
			{
				if (code[i] != 0xB9 || code[i + 5] != 0xE8 || code[i + 10] != 0xDC || code[i + 11] != 0x4D) continue;
				const uint32_t gamehacks_object = read_memory<uint32_t>((uint32_t)(code + i + 1));
				const uint32_t getter = (uint32_t)(code + i + 10) + read_memory<int32_t>((uint32_t)(code + i + 6));
				if (!is_in_ffnx(gamehacks_object) || !is_in_ffnx(getter)) continue;
				g_gamehacks_object = (void*)gamehacks_object;
				g_speedhack_getter = (speedhack_getter_fn)getter;
				const double speed = g_speedhack_getter(g_gamehacks_object, nullptr);
				if (!(speed >= 0.05 && speed <= 64.0))
				{
					log_warning("speedhack: getter at %08X returned %g, ignored (speedhack has no effect in the smoothed modes)\n", getter, speed);
					g_speedhack_getter = nullptr;
					return;
				}
				log_info("speedhack: FFNx gamehacks %08X, getCurrentSpeedhack %08X (%s+%X), now %.2fx\n", gamehacks_object, getter, g_ffnx_module_name, getter - g_ffnx_image_start, speed);
				return;
			}
			log_warning("speedhack: getCurrentSpeedhack call not found in FFNx ff8_limit_fps (%s...), speedhack has no effect in the smoothed modes\n",
				hex_bytes(code, 16).c_str());
		}

		// ------------------------------------------------------------------------------------
		// Deferred install (game thread, first pacer call)
		// ------------------------------------------------------------------------------------
		void deferred_install()
		{
			log_info("install: first limiter call on thread %lu, starting deferred install\n", GetCurrentThreadId());
			QueryPerformanceFrequency(&g_counter_frequency);

			if (!verify_engine_constants())
			{
				log_error("install: this FF8_EN.exe does not match the English 1.2 layout ff8interp was written for. Mod disabled (FFNx limiter left in charge).\n");
				InterlockedExchange(&g_state, PACER_PASSIVE);
				return;
			}
			g_us_exe_verified = true;

			if (!g_disk_image.load())
			{
				log_error("install: could not read FF8_EN.exe from disk for the byte checks. Mod disabled.\n");
				InterlockedExchange(&g_state, PACER_PASSIVE);
				return;
			}

			g_frame_counter_from_flip = hook_flip();
			if (!g_frame_counter_from_flip)
				log_warning("install: frame counter falls back to one per limiter call (not exactly FFNx's frame_counter)\n");

			ff8_interp::InterpHost host{};
			host.hook_call = host_hook_call;
			host.hook_call_chain = host_hook_call_chain;
			host.hook_function = host_hook_function;
			host.log_info = host_log_info;
			host.log_warning = host_log_warning;
			host.log_trace = host_log_trace;
			host.game_mode = host_game_mode;
			host.mode_id = host_mode_id;
			host.frame_counter = &g_frame_counter;
			host.is_us_exe = host_is_us_exe;
			find_speedhack();
			host.speed = host_speed;

			ff8_interp::InterpConfig& config = host.config;
			config.battle_enabled = g_options.battle_enabled;
			config.battle_fps = g_options.battle_fps;
			config.world_enabled = g_options.world_enabled;
			config.world_fps = g_options.world_fps;
			config.field_enabled = g_options.field_enabled;
			config.field_fps = g_options.field_fps;
			config.host_limiter_fps = g_options.ffnx_fps_limiter == 2 ? 30 : g_options.ffnx_fps_limiter == 3 ? 60 : 0;
#if FF8INTERP_DEV
			config.battle_debug = g_options.battle_debug;
			config.battle_debug2 = g_options.battle_debug2;
			config.world_debug = g_options.world_debug;
			config.field_debug = g_options.field_debug;
			config.trace = g_options.trace;
#endif

			ff8_interp::install(host);
			g_disk_image.release();

			log_info("install: %u call sites and %u function entries patched, %u sites skipped\n", g_call_sites_patched, g_function_entries_patched, g_sites_skipped);
			if (g_sites_skipped > 0 && g_options.strict)
			{
				log_error("install: %u hook sites were already patched by someone else (see the warnings above). "
					"ff8interp_strict = true, so every ff8interp patch is rolled back and the game runs as with FFNx alone. "
					"Set ff8interp_strict = false in ff8interp.toml to keep the partial install (the modes whose gates are missing may then run too fast).\n", g_sites_skipped);
				rollback_patches();
				if (g_flip_slot && *g_flip_slot == flip_hook) *g_flip_slot = g_ffnx_common_flip;
				InterlockedExchange(&g_state, PACER_PASSIVE);
				return;
			}
			if (g_call_sites_patched + g_function_entries_patched == 0)
				log_warning("install: no sub-module installed anything (all disabled in ff8interp.toml?). Pacing stays with FFNx.\n");
			InterlockedExchange(&g_state, PACER_ACTIVE);
		}
	}

	// ------------------------------------------------------------------------------------
	// The pacer: the E9 at FPS_LIMITER_ENTRY reaches this instead of FFNx ff8_limit_fps.
	// ------------------------------------------------------------------------------------
	extern "C" int __cdecl ff8interp_pacer()
	{
		if (g_state == PACER_WAITING) deferred_install();
		if (g_state != PACER_ACTIVE) return g_ffnx_limit_fps();

		// The driver table belongs to FFNx; if it was rebuilt, hook the new one.
		if (g_frame_counter_from_flip)
		{
			flip_fn* slot = current_flip_slot();
			if (slot != g_flip_slot || (slot && *slot != flip_hook))
			{
				log_warning("flip: driver flip slot changed (%08X -> %08X), re-hooking\n", (uint32_t)g_flip_slot, (uint32_t)slot);
				g_frame_counter_from_flip = hook_flip();
			}
		}

		ff8_interp::on_frame();
		const double speed = host_speed();
		// < 0: no override for this mode. The frame cap scales with the speedhack like FFNx's own:
		// the logic gates run at most one tick per presented frame.
		const double rate = ff8_interp::frame_rate(-1.0) * speed;

		const int64_t entry_time = performance_counter_now();
		if (g_options.diagnostics || g_options.trace)
		{
			g_diag_limiter_calls++;
			if (rate > 0.0)
				g_diag_paced_calls++;
			else
				g_diag_ffnx_calls++;
			if (g_diag_window_start == 0)
			{
				g_diag_window_start = entry_time;
				g_diag_window_start_frame = g_frame_counter;
			}
			const double elapsed_seconds = double(entry_time - g_diag_window_start) / double(g_counter_frequency.QuadPart);
			if (elapsed_seconds >= 1.0)
			{
				static const char* const GAME_MODE_NAMES[] = { "Other", "Field", "World", "Battle" };
				const uint32_t game_object = read_memory<uint32_t>(GAME_OBJ_PTR);
				const ModeEntry* mode = getmode_cached();
				log_info("diag: mode %s (FFNx driver_mode %u, _mode %u, loop %08X, card %u) rate %s%.0f speed %.2f | %.1f limiter calls/s: %u paced, %u to FFNx | %.1f flips/s | frame %u\n",
					GAME_MODE_NAMES[(int)host_game_mode()], mode->driver_mode, (unsigned)read_memory<uint16_t>(MODE_WORD),
					game_object ? read_memory<uint32_t>(game_object + GAME_OBJ_MAIN_LOOP) : 0, read_memory<uint32_t>(IS_CARD_GAME),
					rate > 0.0 ? "" : "FFNx/", rate > 0.0 ? rate : 0.0, speed,
					g_diag_limiter_calls / elapsed_seconds, g_diag_paced_calls, g_diag_ffnx_calls, (g_frame_counter - g_diag_window_start_frame) / elapsed_seconds, g_frame_counter);
				g_diag_window_start = entry_time;
				g_diag_window_start_frame = g_frame_counter;
				g_diag_limiter_calls = g_diag_paced_calls = g_diag_ffnx_calls = 0;
			}
		}
		double* const crossfade_frame_ms = (double*)CROSSFADE_FRAME_MS;

		if (rate <= 0.0)
		{
			// Mode we do not interpolate: FFNx's own limiter (its rate, speedhack and cross-fade timer).
			const int result = g_ffnx_limit_fps();
			// FFNx measured its cross-fade step against its own last call, which is stale after frames
			// we paced; give the game the real last-frame time instead.
			if (g_previous_frame_paced && g_last_exit_time) *crossfade_frame_ms = 1000.0 * double(entry_time - g_last_exit_time) / double(g_counter_frequency.QuadPart);
			g_previous_frame_paced = false;
			g_frames_passed_to_ffnx++;
			g_last_exit_time = performance_counter_now();
			if (!g_frame_counter_from_flip) g_frame_counter++;
			return result;
		}

		// Our mode: the same busy-wait as FFNx ff8_limit_fps at the ff8_*_fps rate, plus its one side
		// effect, the cross-fade timer (ms since the previous limiter exit).
		*crossfade_frame_ms = g_last_exit_time ? 1000.0 * double(entry_time - g_last_exit_time) / double(g_counter_frequency.QuadPart) : 1000.0 / rate;
		const double ticks_per_frame = double(g_counter_frequency.QuadPart) / rate;
		int64_t now;
		for (;;)
		{
			now = performance_counter_now();
			if (!(now > g_last_exit_time && double(now - g_last_exit_time) < ticks_per_frame)) break;
			_mm_pause();
		}
		g_last_exit_time = now;
		g_previous_frame_paced = true;
		if ((++g_frames_paced % 36000) == 0)
			log_info("pacer: %u frames paced at ff8_*_fps, %u passed to FFNx, frame %u\n", g_frames_paced, g_frames_passed_to_ffnx, g_frame_counter);
		if (!g_frame_counter_from_flip) g_frame_counter++;
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

		if (!has_us_english_signature())
		{
			log_error("this is not the English FF8 1.2 executable (FF8_EN.exe, Steam 2013 / 2000 v1.2). Mod disabled.\n");
			return 0;
		}
		if (!detect_ffnx()) return 0;

		g_options = load_options(dll_dir, game_dir);
		log_info("config: %s\n", g_options.source.c_str());
#if FF8INTERP_DEV
		log_info("config: battle %s %ld fps (debug %ld/%ld), world %s %ld fps (debug %ld), field %s %ld fps (debug %ld), trace %d, strict %d, diag %d; FFNx ff8_fps_limiter = %ld\n",
			g_options.battle_enabled ? "on" : "off", g_options.battle_fps, g_options.battle_debug, g_options.battle_debug2,
			g_options.world_enabled ? "on" : "off", g_options.world_fps, g_options.world_debug,
			g_options.field_enabled ? "on" : "off", g_options.field_fps, g_options.field_debug,
			(int)g_options.trace, (int)g_options.strict, (int)g_options.diagnostics, g_options.ffnx_fps_limiter);
#else
		log_info("config: battle %s %ld fps, world %s %ld fps, field %s %ld fps, strict %d, diag %d; FFNx ff8_fps_limiter = %ld\n",
			g_options.battle_enabled ? "on" : "off", g_options.battle_fps,
			g_options.world_enabled ? "on" : "off", g_options.world_fps,
			g_options.field_enabled ? "on" : "off", g_options.field_fps,
			(int)g_options.strict, (int)g_options.diagnostics, g_options.ffnx_fps_limiter);
#endif

		// Wait for FFNx's ff8_init_hooks: E9 at the limiter entry. If the graphics driver exists but the
		// limiter stays vanilla, FFNx runs with ff8_fps_limiter = 0 and there is nothing to chain to.
		const DWORD start_time = GetTickCount();
		DWORD driver_seen_time = 0;
		uint32_t target = 0;
		for (;;)
		{
			const uint8_t* limiter_code = (const uint8_t*)FPS_LIMITER_ENTRY;
			if (limiter_code[0] == 0xE9)
			{
				const uint32_t first_target = FPS_LIMITER_ENTRY + 5 + read_memory<int32_t>(FPS_LIMITER_ENTRY + 1);
				Sleep(20); // replace_function writes opcode then operand; let it settle
				const uint32_t second_target = FPS_LIMITER_ENTRY + 5 + read_memory<int32_t>(FPS_LIMITER_ENTRY + 1);
				if (first_target == second_target)
				{
					target = first_target;
					break;
				}
				continue;
			}
			if (memcmp(limiter_code, LIMITER_PROLOGUE, sizeof(LIMITER_PROLOGUE)) != 0)
			{
				log_error("limiter entry %08X holds unexpected bytes %s (neither vanilla nor an FFNx jump). Mod disabled.\n", FPS_LIMITER_ENTRY, hex_bytes(limiter_code, 5).c_str());
				return 0;
			}
			if (!driver_seen_time && current_flip_slot() && *current_flip_slot()) driver_seen_time = GetTickCount();
			if (driver_seen_time && GetTickCount() - driver_seen_time > 10000)
			{
				log_error("FFNx created its graphics driver but did not hook the frame limiter at %08X within 10 s. "
					"FFNx only does that with ff8_fps_limiter >= 1 in FFNx.toml (read: %ld). Mod disabled.\n", FPS_LIMITER_ENTRY, g_options.ffnx_fps_limiter);
				return 0;
			}
			if (GetTickCount() - start_time > 600000)
			{
				log_error("FFNx did not hook the frame limiter within 10 minutes. Mod disabled.\n");
				return 0;
			}
			Sleep(1);
		}

		if (!is_in_ffnx(target))
		{
			log_error("the jump at the limiter entry %08X reaches %08X in %s, not FFNx's ff8_limit_fps in %s. "
				"Another mod owns the frame limiter. Mod disabled.\n", FPS_LIMITER_ENTRY, target, owner_of(target).c_str(), g_ffnx_module_name);
			return 0;
		}

		g_ffnx_limit_fps = (limiter_fn)target;
		MemoryBarrier();
		// Redirect: rewrite only the rel32 of FFNx's jump. 0x4020F1..F4 lies inside one cache line, so
		// the aligned-or-not 32-bit store is seen whole by the game thread (old or new target, both valid).
		const uint32_t relative = (uint32_t)&ff8interp_pacer - FPS_LIMITER_ENTRY - 5;
		DWORD old_protection;
		VirtualProtect((void*)FPS_LIMITER_ENTRY, 5, PAGE_EXECUTE_READWRITE, &old_protection);
		InterlockedExchange((volatile LONG*)(FPS_LIMITER_ENTRY + 1), (LONG)relative);
		VirtualProtect((void*)FPS_LIMITER_ENTRY, 5, old_protection, &old_protection);
		FlushInstructionCache(GetCurrentProcess(), (void*)FPS_LIMITER_ENTRY, 5);
		log_info("limiter: FFNx ff8_limit_fps at %08X (%s+%X); jump at %08X now reaches the ff8interp pacer %08X after %lu ms\n",
			target, g_ffnx_module_name, target - g_ffnx_image_start, FPS_LIMITER_ENTRY, (uint32_t)&ff8interp_pacer, GetTickCount() - start_time);
		return 0;
	}
}
