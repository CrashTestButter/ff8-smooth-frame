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
		CRITICAL_SECTION log_lock;
		bool log_ready = false;
		FILE* log_file = nullptr;
		LARGE_INTEGER log_counter_frequency{}, log_start_time{};
	}

	std::string module_dir(HMODULE module_handle)
	{
		char path[MAX_PATH * 2] = {};
		DWORD length = GetModuleFileNameA(module_handle, path, (DWORD)sizeof(path));
		if (length == 0 || length >= sizeof(path)) return std::string();
		std::string full_path(path, length);
		size_t last_slash = full_path.find_last_of("\\/");
		return last_slash == std::string::npos ? std::string() : full_path.substr(0, last_slash + 1);
	}

	void log_open()
	{
		if (log_ready) return;
		InitializeCriticalSection(&log_lock);
		QueryPerformanceFrequency(&log_counter_frequency);
		QueryPerformanceCounter(&log_start_time);
		std::string path = module_dir(nullptr) + "ff8interp.log";
		// Keep the previous run's log: a quick restart used to overwrite the trace of the run before it.
		const std::string previous_path = module_dir(nullptr) + "ff8interp.prev.log";
		MoveFileExA(path.c_str(), previous_path.c_str(), MOVEFILE_REPLACE_EXISTING);
		log_file = _fsopen(path.c_str(), "w", _SH_DENYWR);
		log_ready = true;
	}

	void log_line(const char* level, const char* fmt, va_list args)
	{
		if (!log_ready) return;
		char message[4096];
		vsnprintf(message, sizeof(message), fmt, args);
		size_t length = strlen(message);
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		double seconds = log_counter_frequency.QuadPart ? double(now.QuadPart - log_start_time.QuadPart) / double(log_counter_frequency.QuadPart) : 0.0;

		EnterCriticalSection(&log_lock);
		if (log_file)
		{
			const bool ends_with_newline = length && message[length - 1] == '\n';
			fprintf(log_file, "[%9.3f] %s: %s%s", seconds, level, message, ends_with_newline ? "" : "\n");
			fflush(log_file);
		}
		LeaveCriticalSection(&log_lock);
	}

	void log_info(const char* fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		log_line("INFO", fmt, args);
		va_end(args);
	}

	void log_warning(const char* fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		log_line("WARNING", fmt, args);
		va_end(args);
	}

	void log_error(const char* fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		log_line("ERROR", fmt, args);
		va_end(args);
	}
}
