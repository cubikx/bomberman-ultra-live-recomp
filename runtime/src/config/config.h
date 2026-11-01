// User-editable runtime settings: user/settings/settings.ini.
// Written with defaults and comments when missing; deleting it restores the defaults.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace config {

enum class KeyboardMode {
	Auto,     // keyboard plays as player 1 only while no controller is connected
	Always,   // keyboard always plays as player 1, alongside a controller
	Off,      // keyboard never controls the game
};

enum class Presentation {
	Ultra,   // the PS3 release as it is
	Live,    // the Xbox 360 release's "Bomberman LIVE" logo, title screen, button prompts and wording
};

enum class CpuAi {
	Original,   // the game's own CPU players
	Smart,      // the runtime's bots (hle/bot.cpp), playing through virtual pads
};

struct Settings {
	Presentation presentation = Presentation::Ultra;
	bool aggressive_costume_spawns = true;   // retry the costume (wardrobe) pickup until its tile is free
	uint32_t render_scale = 1;   // multiple of the game's 1280x720 to render at; 0 = auto (from the display)
	CpuAi cpu_ai = CpuAi::Smart;
	uint32_t hud_raise = 36;     // pixels (of 720) to move the match status bar up; 0 = original
	KeyboardMode keyboard = KeyboardMode::Auto;
	std::string exit_combo = "l2+r2+select+start";   // buttons held together to ask to quit; "off" disables
	uint32_t exit_hold_ms = 1000;
};

// Load (creating the file with defaults if needed). Call once at startup.
void load(const std::filesystem::path& file);
const Settings& get();

} // namespace config
