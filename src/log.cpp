/****************************************************************************/
//    Copyright (C) 2026 Todi                                               //
//    This file is part of ff8interp (FF8 Smooth Frames), GPL-3.0.         //
/****************************************************************************/

// ff8interp.log next to FF8_EN.exe. Truncated at every start, one line per call, flushed at once so
// the tail survives a crash. Thread safe (the watcher thread and the game thread both log).

#include "standalone.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <share.h>

namespace ff8interp
{
	namespace
	{
		CRITICAL_SECTION log_cs;
		bool log_ready = false;
		FILE* log_file = nullptr;
		LARGE_INTEGER log_freq{}, log_t0{};
	}

	std::string module_dir(HMODULE m)
	{
		char path[MAX_PATH * 2] = {};
		DWORD n = GetModuleFileNameA(m, path, (DWORD)sizeof(path));
		if (n == 0 || n >= sizeof(path)) return std::string();
		std::string s(path, n);
		size_t slash = s.find_last_of("\\/");
		return slash == std::string::npos ? std::string() : s.substr(0, slash + 1);
	}

	void log_open()
	{
		if (log_ready) return;
		InitializeCriticalSection(&log_cs);
		QueryPerformanceFrequency(&log_freq);
		QueryPerformanceCounter(&log_t0);
		std::string path = module_dir(nullptr) + "ff8interp.log";
		log_file = _fsopen(path.c_str(), "w", _SH_DENYWR);
		log_ready = true;
	}

	void log_line(const char* level, const char* fmt, va_list ap)
	{
		if (!log_ready) return;
		char buf[4096];
		vsnprintf(buf, sizeof(buf), fmt, ap);
		size_t len = strlen(buf);
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		double t = log_freq.QuadPart ? double(now.QuadPart - log_t0.QuadPart) / double(log_freq.QuadPart) : 0.0;

		EnterCriticalSection(&log_cs);
		if (log_file)
		{
			fprintf(log_file, "[%9.3f] %s: %s%s", t, level, buf, (len && buf[len - 1] == '\n') ? "" : "\n");
			fflush(log_file);
		}
		LeaveCriticalSection(&log_cs);
	}

	void log_info(const char* fmt, ...)
	{
		va_list ap; va_start(ap, fmt); log_line("INFO", fmt, ap); va_end(ap);
	}

	void log_warning(const char* fmt, ...)
	{
		va_list ap; va_start(ap, fmt); log_line("WARNING", fmt, ap); va_end(ap);
	}

	void log_error(const char* fmt, ...)
	{
		va_list ap; va_start(ap, fmt); log_line("ERROR", fmt, ap); va_end(ap);
	}
}
