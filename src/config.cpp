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
		typedef std::map<std::string, std::string> Flat;

		std::string trim(const std::string& s)
		{
			size_t a = 0, b = s.size();
			while (a < b && isspace((unsigned char)s[a])) a++;
			while (b > a && isspace((unsigned char)s[b - 1])) b--;
			return s.substr(a, b - a);
		}

		bool read_flat(const std::string& path, Flat& out)
		{
			FILE* f = fopen(path.c_str(), "rb");
			if (!f) return false;
			std::string line;
			char buf[1024];
			bool in_table = false;
			while (fgets(buf, sizeof(buf), f))
			{
				line = buf;
				// strip a comment that is not inside a quoted string
				bool quoted = false;
				for (size_t i = 0; i < line.size(); i++)
				{
					if (line[i] == '"') quoted = !quoted;
					else if (line[i] == '#' && !quoted) { line.resize(i); break; }
				}
				line = trim(line);
				if (line.empty()) continue;
				if (line[0] == '[') { in_table = true; continue; }
				if (in_table) continue;
				size_t eq = line.find('=');
				if (eq == std::string::npos) continue;
				std::string key = trim(line.substr(0, eq));
				std::string val = trim(line.substr(eq + 1));
				if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
				out[key] = val;
			}
			fclose(f);
			return true;
		}

		bool get_bool(const Flat& m, const char* key, bool def)
		{
			auto it = m.find(key);
			if (it == m.end()) return def;
			if (it->second == "true") return true;
			if (it->second == "false") return false;
			log_warning("config: %s = %s is not true/false, using %s\n", key, it->second.c_str(), def ? "true" : "false");
			return def;
		}

		long get_long(const Flat& m, const char* key, long def)
		{
			auto it = m.find(key);
			if (it == m.end()) return def;
			std::string v;
			for (char c : it->second) if (c != '_') v += c;
			const char* s = v.c_str();
			char* end = nullptr;
			long r;
			if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) r = (long)strtoul(s + 2, &end, 16);
			else
			{
				double d = strtod(s, &end);
				r = (long)d;
			}
			if (end == s || *end != '\0')
			{
				log_warning("config: %s = %s is not a number, using %ld\n", key, it->second.c_str(), def);
				return def;
			}
			return r;
		}
	}

	Options load_options(const std::string& dll_dir, const std::string& game_dir)
	{
		Options o;
		Flat m;
		const std::string candidates[2] = { dll_dir + "ff8interp.toml", game_dir + "ff8interp.toml" };
		for (const std::string& p : candidates)
		{
			if (!p.empty() && read_flat(p, m)) { o.source = p; break; }
		}
		if (o.source.empty()) o.source = "(no ff8interp.toml found, built-in defaults)";

		o.battle_enabled = get_bool(m, "ff8_battle_anim_interp", o.battle_enabled);
		o.battle_fps = get_long(m, "ff8_battle_fps", o.battle_fps);
		o.battle_debug = get_long(m, "ff8_battle_interp_debug", o.battle_debug);
		o.world_enabled = get_bool(m, "ff8_world_interp", o.world_enabled);
		o.world_fps = get_long(m, "ff8_world_fps", o.world_fps);
		o.world_debug = get_long(m, "ff8_world_interp_debug", o.world_debug);
		o.field_enabled = get_bool(m, "ff8_field_interp", o.field_enabled);
		o.field_fps = get_long(m, "ff8_field_fps", o.field_fps);
		o.field_debug = get_long(m, "ff8_field_interp_debug", o.field_debug);
		o.trace = get_bool(m, "trace_battle_animation", o.trace);
		o.strict = get_bool(m, "ff8interp_strict", o.strict);
		o.diag = get_bool(m, "ff8interp_diag", o.diag);

		Flat ffnx;
		if (read_flat(game_dir + "FFNx.toml", ffnx)) o.ffnx_fps_limiter = get_long(ffnx, "ff8_fps_limiter", o.ffnx_fps_limiter);
		else log_warning("config: %sFFNx.toml not readable, assuming ff8_fps_limiter = %ld\n", game_dir.c_str(), o.ffnx_fps_limiter);
		return o;
	}
}
