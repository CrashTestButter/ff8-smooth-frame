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
		char path[MAX_PATH * 2] = {};
		GetModuleFileNameA(nullptr, path, (DWORD)sizeof(path));
		const char* base = strrchr(path, '\\');
		base = base ? base + 1 : path;
		// FF8_EN.exe (Steam 2013) / FF8.exe (2000). Not the launcher, not Chocobo World.
		return _strnicmp(base, "FF8_", 4) == 0 ? _stricmp(base, "FF8_Launcher.exe") != 0 : _stricmp(base, "FF8.exe") == 0;
	}
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
	if (reason != DLL_PROCESS_ATTACH) return TRUE;
	DisableThreadLibraryCalls(inst);
	ff8interp::g_self = inst;

	if (!is_ff8_game_process()) return TRUE;

	// One instance per process, even if both J8 and an ASI loader load a copy.
	char name[64];
	snprintf(name, sizeof(name), "Local\\ff8interp_%lu", GetCurrentProcessId());
	HANDLE mtx = CreateMutexA(nullptr, FALSE, name);
	if (mtx == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
	{
		if (mtx) CloseHandle(mtx);
		return TRUE;
	}
	// The mutex handle is kept open for the life of the process on purpose.

	HANDLE t = CreateThread(nullptr, 0, ff8interp::watcher_thread, nullptr, 0, nullptr);
	if (t) CloseHandle(t);
	return TRUE;
}
