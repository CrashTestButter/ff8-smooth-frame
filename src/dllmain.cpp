/****************************************************************************/
//    Copyright (C) 2026 Todi                                               //
//    This file is part of ff8interp (FF8 Smooth Frames), GPL-3.0.         //
/****************************************************************************/

// DllMain installs NOTHING: it runs under the loader lock and before FFNx's ff8_init_hooks. It only
// starts the watcher thread (adapter_standalone.cpp), which waits for FFNx and redirects the frame
// limiter jump; the real install happens later on the game thread.
//
// Loaders: Junction VIII <LoadLibrary> (NativeLibrary.Load in its WinMain detour) or an ASI loader
// (ff8interp.dll renamed to ff8interp.asi). Both only need DllMain.

#include "standalone.h"

#include <stdio.h>
#include <string.h>

namespace ff8interp
{
	HMODULE g_self = nullptr;
}

namespace
{
	bool is_ff8_game_process()
	{
		char exe_path[MAX_PATH * 2] = {};
		GetModuleFileNameA(nullptr, exe_path, (DWORD)sizeof(exe_path));
		const char* exe_name = strrchr(exe_path, '\\');
		exe_name = exe_name ? exe_name + 1 : exe_path;
		// FF8_EN.exe (Steam 2013) / FF8.exe (2000). Not the launcher, not Chocobo World.
		const bool has_ff8_prefix = _strnicmp(exe_name, "FF8_", 4) == 0;
		return has_ff8_prefix ? _stricmp(exe_name, "FF8_Launcher.exe") != 0 : _stricmp(exe_name, "FF8.exe") == 0;
	}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason != DLL_PROCESS_ATTACH) return TRUE;
	DisableThreadLibraryCalls(instance);
	ff8interp::g_self = instance;

	if (!is_ff8_game_process()) return TRUE;

	// One instance per process, even if both J8 and an ASI loader load a copy.
	char mutex_name[64];
	snprintf(mutex_name, sizeof(mutex_name), "Local\\ff8interp_%lu", GetCurrentProcessId());
	HANDLE instance_mutex = CreateMutexA(nullptr, FALSE, mutex_name);
	if (instance_mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
	{
		if (instance_mutex) CloseHandle(instance_mutex);
		return TRUE;
	}
	// The mutex handle is kept open for the life of the process on purpose.

	HANDLE watcher = CreateThread(nullptr, 0, ff8interp::watcher_thread, nullptr, 0, nullptr);
	if (watcher) CloseHandle(watcher);
	return TRUE;
}
