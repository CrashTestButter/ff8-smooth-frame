/****************************************************************************/
//    Copyright (C) 2026 Todi                                               //
//    This file is part of ff8interp (FF8 Smooth Frames), GPL-3.0.         //
/****************************************************************************/

// ff8interp.toml reader. Only the flat subset the file uses: top-level `key = value` lines, `#`
// comments, booleans, integers (decimal or 0x hex, `_` separators), floats (truncated) and quoted
// strings. Keys after the first [table] header are ignored. No dependency on a TOML library.
//
// Search order: ff8interp.toml next to ff8interp.dll (the J8 mod folder), then next to FF8_EN.exe.
// The first file found wins; a missing file means built-in defaults (= the shipped toml).
// ff8_fps_limiter is read from the game's FFNx.toml (J8 writes its per-mod overrides there before
// launch), because stock FFNx does not export its option values.

#include "standalone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <map>

namespace ff8interp
{
	namespace
	{
		typedef std::map<std::string, std::string> KeyValues;

		std::string trim(const std::string& text)
		{
			size_t begin = 0, end = text.size();
			while (begin < end && isspace((unsigned char)text[begin])) begin++;
			while (end > begin && isspace((unsigned char)text[end - 1])) end--;
			return text.substr(begin, end - begin);
		}

		bool read_flat_toml(const std::string& path, KeyValues& values)
		{
			FILE* file = fopen(path.c_str(), "rb");
			if (!file) return false;
			std::string line;
			char buffer[1024];
			bool in_table = false;
			while (fgets(buffer, sizeof(buffer), file))
			{
				line = buffer;
				// strip a comment that is not inside a quoted string
				bool quoted = false;
				for (size_t i = 0; i < line.size(); i++)
				{
					if (line[i] == '"') quoted = !quoted;
					else if (line[i] == '#' && !quoted)
					{
						line.resize(i);
						break;
					}
				}
				line = trim(line);
				if (line.empty()) continue;
				if (line[0] == '[')
				{
					in_table = true;
					continue;
				}
				if (in_table) continue;
				size_t equals_pos = line.find('=');
				if (equals_pos == std::string::npos) continue;
				std::string key = trim(line.substr(0, equals_pos));
				std::string value = trim(line.substr(equals_pos + 1));
				const bool is_quoted_string = value.size() >= 2 && value.front() == '"' && value.back() == '"';
				if (is_quoted_string) value = value.substr(1, value.size() - 2);
				values[key] = value;
			}
			fclose(file);
			return true;
		}

		bool get_bool(const KeyValues& values, const char* key, bool fallback)
		{
			auto entry = values.find(key);
			if (entry == values.end()) return fallback;
			if (entry->second == "true") return true;
			if (entry->second == "false") return false;
			log_warning("config: %s = %s is not true/false, using %s\n", key, entry->second.c_str(), fallback ? "true" : "false");
			return fallback;
		}

		long get_long(const KeyValues& values, const char* key, long fallback)
		{
			auto entry = values.find(key);
			if (entry == values.end()) return fallback;
			std::string digits;
			for (char c : entry->second)
			{
				if (c != '_') digits += c;
			}
			const char* text = digits.c_str();
			char* end = nullptr;
			long result;
			const bool is_hex = digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X');
			if (is_hex)
			{
				result = (long)strtoul(text + 2, &end, 16);
			}
			else
			{
				double as_double = strtod(text, &end);
				result = (long)as_double;
			}
			if (end == text || *end != '\0')
			{
				log_warning("config: %s = %s is not a number, using %ld\n", key, entry->second.c_str(), fallback);
				return fallback;
			}
			return result;
		}
	}

	Options load_options(const std::string& dll_dir, const std::string& game_dir)
	{
		Options options;
		KeyValues values;
		const std::string candidates[2] = { dll_dir + "ff8interp.toml", game_dir + "ff8interp.toml" };
		for (const std::string& path : candidates)
		{
			if (!path.empty() && read_flat_toml(path, values))
			{
				options.source = path;
				break;
			}
		}
		if (options.source.empty()) options.source = "(no ff8interp.toml found, built-in defaults)";

		options.battle_enabled = get_bool(values, "ff8_battle_anim_interp", options.battle_enabled);
		options.battle_fps = get_long(values, "ff8_battle_fps", options.battle_fps);
		options.world_enabled = get_bool(values, "ff8_world_interp", options.world_enabled);
		options.world_fps = get_long(values, "ff8_world_fps", options.world_fps);
		options.field_enabled = get_bool(values, "ff8_field_interp", options.field_enabled);
		options.field_fps = get_long(values, "ff8_field_fps", options.field_fps);
#if FF8INTERP_DEV
		options.battle_debug = get_long(values, "ff8_battle_interp_debug", options.battle_debug);
		options.battle_debug2 = get_long(values, "ff8_battle_interp_debug2", options.battle_debug2);
		options.world_debug = get_long(values, "ff8_world_interp_debug", options.world_debug);
		options.field_debug = get_long(values, "ff8_field_interp_debug", options.field_debug);
		options.trace = get_bool(values, "trace_battle_animation", options.trace);
#endif
		options.strict = get_bool(values, "ff8interp_strict", options.strict);
		options.diagnostics = get_bool(values, "ff8interp_diag", options.diagnostics);

		KeyValues ffnx_values;
		if (read_flat_toml(game_dir + "FFNx.toml", ffnx_values))
			options.ffnx_fps_limiter = get_long(ffnx_values, "ff8_fps_limiter", options.ffnx_fps_limiter);
		else
			log_warning("config: %sFFNx.toml not readable, assuming ff8_fps_limiter = %ld\n", game_dir.c_str(), options.ffnx_fps_limiter);
		return options;
	}
}
