// settings.ini: "key = value" lines and "#" or ";" comments, nothing else. Unknown keys
// and bad values are reported and ignored, so an old or hand-edited file never stops
// the game from starting. Files from older versions had "[section]" headers; those
// lines are removed on load (the keys never needed them: every name is unique).
#include "config/config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "runtime/kernel.h"

namespace config {
namespace {

Settings g_settings;

constexpr const char* FILE_HEADER = R"(# Bomberman ULTRA runtime settings.
# Delete this file to restore the defaults; it is recreated on the next start.
)";

// Every setting with its comment and default, in file order. A new file gets all of
// them; a file from an older version gets the ones it lacks appended on load.
struct Default {
	const char* keys;   // the keys `text` sets, space separated
	const char* text;
};
constexpr Default DEFAULTS[] = {
	{"presentation", R"(# How the game presents itself:
#   ultra - the PS3 release, Bomberman ULTRA (default)
#   live  - the Xbox 360 release, Bomberman LIVE: its logo, title screen, A/B/X/Y
#           button prompts, wording and title call. Needs the live folder beside the
#           executable, made from your own 360 copy with tools/media/make_live.py.
presentation = ultra
)"},
	{"aggressive_costume_spawns", R"(# Costume pickup (the wardrobe icon that unlocks a costume piece; it appears about a
# minute into a local match of 2 minutes or more). The game tries one random spot once
# and skips the pickup for that match if the spot is still covered by a block.
#   on  - keep trying other spots until one is free (default)
#   off - as in the original game
aggressive_costume_spawns = on
)"},
	{"cpu_ai", R"(# Computer players:
#   smart    - plan their moves: escape blasts, bomb blocks and players with a way out,
#              collect power-ups; Easy / Normal / Hard set how far they see and how fast
#              and boldly they act (default)
#   original - the game's own computer players
cpu_ai = smart
)"},
	{"render_scale", R"(# Render resolution, as a multiple of the game's own 1280x720:
#   auto - enough for the largest connected display (default; 1080p and 1440p: 2, 4K: 3)
#   1    - 1280x720, as on the PS3
#   2    - 2560x1440
#   3    - 3840x2160
#   4    - 5120x2880
# Higher is sharper when the window is large, and needs a faster graphics card.
render_scale = auto
)"},
	{"hud_raise", R"(# Match status bar (player heads, wins, clock) at the top of the screen. In the
# original game it covers the top edge of the arena; this moves it up by that many
# pixels of 720 (0 to 48):
#   36 - just below the top of the screen, clear of the arena (default)
#   0  - as in the original game
hud_raise = 36
)"},
	{"keyboard", R"(# Keyboard as player 1:
#   auto   - only while no controller is connected (default)
#   always - also when controllers are connected
#   off    - never
keyboard = auto
)"},
	{"exit_combo exit_hold_ms", R"(# Quit from a controller: hold these buttons together for exit_hold_ms, then
# confirm "Quit game?" (Cancel is the default). Button names joined with "+":
# cross circle square triangle l1 r1 l2 r2 l3 r3 select start up down left right
# (Xbox pads: A B X Y LB RB LT RT, Back = select). "off" turns it off.
exit_combo = l2+r2+select+start
exit_hold_ms = 1000
)"},
};

// The defaults whose keys are all missing from `seen`, a blank line between them.
std::string defaults_text(const std::vector<std::string>& seen) {
	std::string out;
	for (const Default& d : DEFAULTS) {
		std::string keys = d.keys;
		bool any = false;
		for (size_t b = 0, e; b < keys.size(); b = e + 1) {
			e = keys.find(' ', b);
			if (e == std::string::npos) e = keys.size();
			any |= std::find(seen.begin(), seen.end(), keys.substr(b, e - b)) != seen.end();
		}
		if (any) continue;
		out += std::string(out.empty() ? "" : "\n") + d.text;
	}
	return out;
}

std::string trim(const std::string& s) {
	size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
	return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return s;
}

// False for a key this version does not know.
bool apply(const std::string& key, const std::string& value, const std::filesystem::path& file, int line) {
	if (key == "presentation") {
		std::string v = lower(value);
		if (v == "ultra") g_settings.presentation = Presentation::Ultra;
		else if (v == "live") g_settings.presentation = Presentation::Live;
		else LOG_WARN("config: %s:%d: presentation must be ultra or live (got '%s')", file.string().c_str(), line, value.c_str());
		return true;
	}
	if (key == "aggressive_costume_spawns") {
		std::string v = lower(value);
		if (v == "on" || v == "true" || v == "1") g_settings.aggressive_costume_spawns = true;
		else if (v == "off" || v == "false" || v == "0") g_settings.aggressive_costume_spawns = false;
		else LOG_WARN("config: %s:%d: aggressive_costume_spawns must be on or off (got '%s')", file.string().c_str(), line, value.c_str());
		return true;
	}
	if (key == "cpu_ai") {
		std::string v = lower(value);
		if (v == "smart") g_settings.cpu_ai = CpuAi::Smart;
		else if (v == "original") g_settings.cpu_ai = CpuAi::Original;
		else LOG_WARN("config: %s:%d: cpu_ai must be smart or original (got '%s')", file.string().c_str(), line, value.c_str());
		return true;
	}
	if (key == "render_scale") {
		std::string v = lower(value);
		if (v == "auto") g_settings.render_scale = 0;
		else if (v.size() == 1 && v[0] >= '1' && v[0] <= '4') g_settings.render_scale = uint32_t(v[0] - '0');
		else LOG_WARN("config: %s:%d: render_scale must be auto or 1 to 4 (got '%s')", file.string().c_str(), line, value.c_str());
		return true;
	}
	if (key == "hud_raise") {
		char* end = nullptr;
		long v = std::strtol(value.c_str(), &end, 10);
		if (!value.empty() && *end == '\0' && v >= 0 && v <= 48) g_settings.hud_raise = uint32_t(v);
		else LOG_WARN("config: %s:%d: hud_raise must be a number from 0 to 48 (got '%s')", file.string().c_str(), line, value.c_str());
		return true;
	}
	if (key == "keyboard") {
		std::string v = lower(value);
		if (v == "auto") g_settings.keyboard = KeyboardMode::Auto;
		else if (v == "always") g_settings.keyboard = KeyboardMode::Always;
		else if (v == "off") g_settings.keyboard = KeyboardMode::Off;
		else LOG_WARN("config: %s:%d: keyboard must be auto, always or off (got '%s')", file.string().c_str(), line, value.c_str());
		return true;
	}
	if (key == "exit_combo") {
		std::string v = lower(value);   // checked by input::init
		std::erase_if(v, [](unsigned char c) { return std::isspace(c); });
		g_settings.exit_combo = v;
		return true;
	}
	if (key == "exit_hold_ms") {
		char* end = nullptr;
		unsigned long ms = std::strtoul(value.c_str(), &end, 10);
		if (end == value.c_str() || *end || ms < 100 || ms > 10000)
			LOG_WARN("config: %s:%d: exit_hold_ms must be 100 to 10000 (got '%s')", file.string().c_str(), line, value.c_str());
		else g_settings.exit_hold_ms = uint32_t(ms);
		return true;
	}
	LOG_WARN("config: %s:%d: unknown setting '%s'", file.string().c_str(), line, key.c_str());
	return false;
}

} // namespace

void load(const std::filesystem::path& file) {
	g_settings = Settings{};
	if (!std::filesystem::exists(file)) {
		std::ofstream(file) << FILE_HEADER << "\n" << defaults_text({});
		LOG_INFO("config: wrote default settings to %s", file.string().c_str());
	}
	std::ifstream in(file);
	std::string raw, last;
	std::vector<std::string> seen;
	std::map<std::string, std::string> values;   // as written, for rewriting an old file
	std::string unknown;                         // lines with keys this version does not know
	bool had_sections = false;
	int line = 0;
	while (std::getline(in, raw)) {
		last = raw;
		line++;
		std::string s = trim(raw.substr(0, raw.find_first_of("#;")));
		if (s.empty()) continue;
		if (s.front() == '[' && s.back() == ']') {
			had_sections = true;
			continue;
		}
		size_t eq = s.find('=');
		if (eq == std::string::npos) {
			LOG_WARN("config: %s:%d: expected 'key = value'", file.string().c_str(), line);
			continue;
		}
		std::string key = lower(trim(s.substr(0, eq)));
		seen.push_back(key);
		values[key] = trim(s.substr(eq + 1));
		if (!apply(key, values[key], file, line)) unknown += raw + "\n";
	}
	in.close();

	// a file from a version with "[section]" headers: rewritten in the current layout
	// (every setting with its comment), keeping its values and any unknown lines
	if (had_sections) {
		std::string text = defaults_text({}), out;
		for (size_t b = 0, e; b < text.size(); b = e + 1) {
			e = text.find('\n', b);
			std::string l = text.substr(b, e - b);
			size_t eq = l.find(" = ");
			if (!l.empty() && l[0] != '#' && eq != std::string::npos && values.count(l.substr(0, eq)))
				l = l.substr(0, eq) + " = " + values[l.substr(0, eq)];
			out += l + "\n";
		}
		if (!unknown.empty()) out += "\n# Not used by this version:\n" + unknown;
		std::ofstream(file, std::ios::trunc) << FILE_HEADER << "\n" << out;
		LOG_INFO("config: rewrote %s without [section] headers (values kept)", file.string().c_str());
		return;
	}

	// settings this file's version did not have: add them with their defaults (already
	// in g_settings), so every option shows up in the file
	std::string missing = defaults_text(seen);
	if (!missing.empty()) {
		std::ofstream(file, std::ios::app) << (last.empty() ? "" : "\n") << "\n# Added by a newer version:\n" << missing;
		LOG_INFO("config: added new settings to %s", file.string().c_str());
	}
}

const Settings& get() { return g_settings; }

} // namespace config
