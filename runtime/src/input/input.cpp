// Host input: SDL3 gamepads + keyboard -> PS3 controller ports.
//
// Gamepads take the lowest free port when they connect and keep it until they
// disconnect. The keyboard drives port 0 (merged with whatever gamepad is there)
// as settings.ini's input.keyboard says: by default only while no gamepad is connected.
#include "input/input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "config/config.h"
#include "runtime/kernel.h"

namespace input {
namespace {

SDL_Gamepad* g_gamepads[MAX_PORTS] = {};   // window thread only

std::mutex g_lock;
PadState g_snapshot[MAX_PORTS];   // what the game sees
PadState g_raw[MAX_PORTS];        // what the devices report
std::string g_keys[MAX_PORTS], g_names[MAX_PORTS];
std::atomic<uint32_t> g_captured{0};   // bit per port
Listener g_listener;                   // window thread only
bool g_menu_held[MAX_PORTS];           // window thread only

// Exit combo (settings.ini input.exit_combo / exit_hold_ms): held on any pad, asks to quit
uint16_t g_combo1 = 0, g_combo2 = 0;   // 0 / 0: off
Uint64 g_combo_hold_ms = 1000;
Uint64 g_combo_since[MAX_PORTS];       // when the whole combo went down (0: not held); window thread only
bool g_combo_fired[MAX_PORTS];         // asked already during this hold

std::atomic<uint16_t> g_rumble_request[MAX_PORTS];   // small << 8 | large
uint16_t g_rumble_applied[MAX_PORTS];
Uint64 g_rumble_refreshed[MAX_PORTS];

// Keyboard layout for port 0 (remapping comes with the settings file)
struct KeyBinding {
	SDL_Scancode key;
	uint16_t d1, d2;
	int pressure;   // index into PadState::pressure, -1 for none
};
constexpr KeyBinding KEYS[] = {
	{SDL_SCANCODE_UP, d1::UP, 0, 2},           {SDL_SCANCODE_DOWN, d1::DOWN, 0, 3},
	{SDL_SCANCODE_LEFT, d1::LEFT, 0, 1},       {SDL_SCANCODE_RIGHT, d1::RIGHT, 0, 0},
	{SDL_SCANCODE_RETURN, d1::START, 0, -1},   {SDL_SCANCODE_BACKSPACE, d1::SELECT, 0, -1},
	{SDL_SCANCODE_Z, 0, d2::CROSS, 6},         {SDL_SCANCODE_SPACE, 0, d2::CROSS, 6},
	{SDL_SCANCODE_X, 0, d2::CIRCLE, 5},        {SDL_SCANCODE_ESCAPE, 0, d2::CIRCLE, 5},
	{SDL_SCANCODE_A, 0, d2::SQUARE, 7},        {SDL_SCANCODE_S, 0, d2::TRIANGLE, 4},
	{SDL_SCANCODE_Q, 0, d2::L1, 8},            {SDL_SCANCODE_W, 0, d2::R1, 9},
	{SDL_SCANCODE_1, 0, d2::L2, 10},           {SDL_SCANCODE_2, 0, d2::R2, 11},
};

struct ScriptedPress {
	Uint64 at_ms;
	uint16_t d1, d2;
	bool menu = false;     // "ps": the PS / Guide button
	Uint64 hold_ms = 150;
	uint32_t port = 0;   // "button@2": controller 2
};
std::vector<ScriptedPress> g_script;   // set before the window loop starts
uint32_t g_script_ports = 0;           // bit per port that the script drives (connected from the start)

struct ButtonName {
	const char* name;
	uint16_t d1, d2;
};
constexpr ButtonName BUTTON_NAMES[] = {
	{"select", d1::SELECT, 0}, {"l3", d1::L3, 0},         {"r3", d1::R3, 0},           {"start", d1::START, 0},
	{"up", d1::UP, 0},         {"right", d1::RIGHT, 0},   {"down", d1::DOWN, 0},       {"left", d1::LEFT, 0},
	{"l2", 0, d2::L2},         {"r2", 0, d2::R2},         {"l1", 0, d2::L1},           {"r1", 0, d2::R1},
	{"triangle", 0, d2::TRIANGLE}, {"circle", 0, d2::CIRCLE}, {"cross", 0, d2::CROSS}, {"square", 0, d2::SQUARE},
};

// "l2+r2+select+start" -> button masks. False on an unknown name.
bool parse_buttons(const std::string& text, uint16_t& d1, uint16_t& d2) {
	d1 = d2 = 0;
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t end = text.find('+', pos);
		if (end == std::string::npos) end = text.size();
		std::string name = text.substr(pos, end - pos);
		auto b = std::find_if(std::begin(BUTTON_NAMES), std::end(BUTTON_NAMES), [&](const ButtonName& b) { return name == b.name; });
		if (b == std::end(BUTTON_NAMES)) return false;
		d1 |= b->d1;
		d2 |= b->d2;
		pos = end + 1;
	}
	return true;
}

// Returns whether the scripted PS button is held.
bool read_script(PadState& s, Uint64 now, uint32_t port) {
	bool menu = false;
	for (const ScriptedPress& p : g_script)
		if (p.port == port && now >= p.at_ms && now < p.at_ms + p.hold_ms) {
			s.digital1 |= p.d1;
			s.digital2 |= p.d2;
			menu |= p.menu;
		}
	return menu;
}

uint8_t axis_to_u8(Sint16 v) {
	// small dead zone so worn sticks don't drift
	float f = float(v) / 32767.0f;
	if (std::fabs(f) < 0.12f) f = 0.0f;
	return uint8_t(std::clamp(int(std::lround(127.5f + f * 127.5f)), 0, 255));
}

void read_gamepad(SDL_Gamepad* gp, PadState& s) {
	struct Button {
		SDL_GamepadButton b;
		uint16_t d1, d2;
		int pressure;
	};
	// face buttons are positional: south = cross, east = circle, west = square, north = triangle
	static constexpr Button BUTTONS[] = {
		{SDL_GAMEPAD_BUTTON_DPAD_UP, d1::UP, 0, 2},      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, d1::DOWN, 0, 3},
		{SDL_GAMEPAD_BUTTON_DPAD_LEFT, d1::LEFT, 0, 1},  {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, d1::RIGHT, 0, 0},
		{SDL_GAMEPAD_BUTTON_START, d1::START, 0, -1},    {SDL_GAMEPAD_BUTTON_BACK, d1::SELECT, 0, -1},
		{SDL_GAMEPAD_BUTTON_LEFT_STICK, d1::L3, 0, -1},  {SDL_GAMEPAD_BUTTON_RIGHT_STICK, d1::R3, 0, -1},
		{SDL_GAMEPAD_BUTTON_SOUTH, 0, d2::CROSS, 6},     {SDL_GAMEPAD_BUTTON_EAST, 0, d2::CIRCLE, 5},
		{SDL_GAMEPAD_BUTTON_WEST, 0, d2::SQUARE, 7},     {SDL_GAMEPAD_BUTTON_NORTH, 0, d2::TRIANGLE, 4},
		{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0, d2::L1, 8}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0, d2::R1, 9},
	};
	for (const Button& b : BUTTONS)
		if (SDL_GetGamepadButton(gp, b.b)) {
			s.digital1 |= b.d1;
			s.digital2 |= b.d2;
			if (b.pressure >= 0) s.pressure[b.pressure] = 255;
		}
	// analog triggers: pressure follows the trigger, the digital bit needs a firm press
	Sint16 lt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
	Sint16 rt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
	s.pressure[10] = std::max(s.pressure[10], uint8_t(std::max(0, int(lt)) * 255 / 32767));
	s.pressure[11] = std::max(s.pressure[11], uint8_t(std::max(0, int(rt)) * 255 / 32767));
	if (lt > 32767 / 3) s.digital2 |= d2::L2;
	if (rt > 32767 / 3) s.digital2 |= d2::R2;
	s.lx = axis_to_u8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX));
	s.ly = axis_to_u8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));
	s.rx = axis_to_u8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX));
	s.ry = axis_to_u8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
	if (Uint16 v = SDL_GetGamepadVendor(gp)) s.vendor = v;
	if (Uint16 p = SDL_GetGamepadProduct(gp)) s.product = p;
}

// INPUT_RECORD=1: log port 0's button presses as an --input-script, to replay a path through the menus
void record_presses(const PadState& s, Uint64 now) {
	static const bool enabled = env_flag("INPUT_RECORD");
	static uint16_t prev1 = 0, prev2 = 0;
	if (!enabled) return;
	for (const ButtonName& b : BUTTON_NAMES)
		if ((s.digital1 & ~prev1 & b.d1) || (s.digital2 & ~prev2 & b.d2)) LOG_INFO("input: record %llu:%s", (unsigned long long)now, b.name);
	prev1 = s.digital1;
	prev2 = s.digital2;
}

void read_keyboard(PadState& s) {
	const bool* keys = SDL_GetKeyboardState(nullptr);
	for (const KeyBinding& k : KEYS)
		if (keys[k.key]) {
			s.digital1 |= k.d1;
			s.digital2 |= k.d2;
			if (k.pressure >= 0) s.pressure[k.pressure] = 255;
		}
}

void apply_rumble(uint32_t port, Uint64 now) {
	uint16_t want = g_rumble_request[port].load(std::memory_order_relaxed);
	SDL_Gamepad* gp = g_gamepads[port];
	if (!gp) return;
	// SDL rumble times out, so refresh it while the game keeps a motor running
	bool refresh = want && now - g_rumble_refreshed[port] > 1000;
	if (want == g_rumble_applied[port] && !refresh) return;
	// small motor is on/off on the PS3; the large one has 256 levels
	Uint16 high = (want >> 8) ? 0xFFFF : 0;
	Uint16 low = Uint16((want & 0xFF) * 0x101);
	SDL_RumbleGamepad(gp, low, high, 2000);
	g_rumble_applied[port] = want;
	g_rumble_refreshed[port] = now;
}

} // namespace

void init() {
	const config::Settings& cfg = config::get();
	g_combo_hold_ms = cfg.exit_hold_ms;
	if (cfg.exit_combo != "off" && !parse_buttons(cfg.exit_combo, g_combo1, g_combo2)) {
		LOG_WARN("input: exit_combo '%s' has an unknown button; using l2+r2+select+start", cfg.exit_combo.c_str());
		parse_buttons("l2+r2+select+start", g_combo1, g_combo2);
	}
	// debugging: INPUT_NO_GAMEPADS=1 ignores real controllers (scripted runs while someone plays)
	if (env_flag("INPUT_NO_GAMEPADS")) return;
	if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) LOG_WARN("input: gamepad support unavailable: %s", SDL_GetError());
	// already-connected pads arrive as SDL_EVENT_GAMEPAD_ADDED events
}

void handle_event(const SDL_Event& e) {
	if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
		for (SDL_Gamepad* gp : g_gamepads)
			if (gp && SDL_GetGamepadID(gp) == e.gdevice.which) return;
		uint32_t port = 0;
		while (port < MAX_PORTS && g_gamepads[port]) port++;
		if (port == MAX_PORTS) return;
		SDL_Gamepad* gp = SDL_OpenGamepad(e.gdevice.which);
		if (!gp) return;
		g_gamepads[port] = gp;
		SDL_SetGamepadPlayerIndex(gp, int(port));   // lights the right LED where supported
		LOG_INFO("input: '%s' connected as player %u", SDL_GetGamepadName(gp), port + 1);
	} else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
		for (uint32_t port = 0; port < MAX_PORTS; port++)
			if (g_gamepads[port] && SDL_GetGamepadID(g_gamepads[port]) == e.gdevice.which) {
				LOG_INFO("input: player %u controller disconnected", port + 1);
				SDL_CloseGamepad(g_gamepads[port]);
				g_gamepads[port] = nullptr;
				g_rumble_applied[port] = 0;
			}
	}
}

// "serial:..." when the device reports a serial number, so a controller keeps its
// profile whichever port it lands on; otherwise the port.
std::string gamepad_key(SDL_Gamepad* gp, uint32_t port) {
	const char* serial = SDL_GetGamepadSerial(gp);
	if (serial && *serial) return std::string("serial:") + serial;
	return "port:" + std::to_string(port);
}

void update() {
	PadState next[MAX_PORTS];
	std::string keys[MAX_PORTS], names[MAX_PORTS];
	bool menu[MAX_PORTS] = {};
	Uint64 now = SDL_GetTicks();
	bool any_gamepad = std::any_of(std::begin(g_gamepads), std::end(g_gamepads), [](SDL_Gamepad* gp) { return gp; });
	config::KeyboardMode mode = config::get().keyboard;
	bool keyboard = mode == config::KeyboardMode::Always || (mode == config::KeyboardMode::Auto && !any_gamepad);
	for (uint32_t port = 0; port < MAX_PORTS; port++) {
		PadState& s = next[port];
		if (g_gamepads[port]) {
			s.connected = true;
			read_gamepad(g_gamepads[port], s);
			apply_rumble(port, now);
			keys[port] = gamepad_key(g_gamepads[port], port);
			const char* n = SDL_GetGamepadName(g_gamepads[port]);
			names[port] = n ? n : "Controller";
			menu[port] = SDL_GetGamepadButton(g_gamepads[port], SDL_GAMEPAD_BUTTON_GUIDE);
		}
		if (port == 0 && keyboard) {
			s.connected = true;
			read_keyboard(s);
			if (keys[0].empty()) {
				keys[0] = "keyboard";
				names[0] = "Keyboard";
			}
			menu[0] = menu[0] || SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F1];
		}
		if (g_script_ports & (1u << port)) {
			s.connected = true;
			menu[port] = read_script(s, now, port) || menu[port];
			if (keys[port].empty()) {
				keys[port] = port == 0 ? "keyboard" : "script:" + std::to_string(port + 1);
				names[port] = port == 0 ? "Keyboard" : "Script " + std::to_string(port + 1);
			}
		}
		if (port == 0) record_presses(s, now);
	}
	// Exit combo: the game never sees the full combo (no Start press reaching it), and
	// the listener is told once per hold, after exit_hold_ms
	bool combo_down[MAX_PORTS] = {}, combo_due[MAX_PORTS] = {};
	for (uint32_t port = 0; port < MAX_PORTS; port++) {
		const PadState& s = next[port];
		combo_down[port] = (g_combo1 || g_combo2) && s.connected && (s.digital1 & g_combo1) == g_combo1 &&
		                   (s.digital2 & g_combo2) == g_combo2;
		if (!combo_down[port]) {
			g_combo_since[port] = 0;
			g_combo_fired[port] = false;
			continue;
		}
		if (!g_combo_since[port]) g_combo_since[port] = now ? now : 1;
		if (!g_combo_fired[port] && now - g_combo_since[port] >= g_combo_hold_ms) {
			g_combo_fired[port] = true;
			combo_due[port] = true;
		}
	}
	std::string changed[MAX_PORTS];
	bool any_changed[MAX_PORTS] = {};
	{
		std::lock_guard lock(g_lock);
		uint32_t captured = g_captured.load();
		for (uint32_t port = 0; port < MAX_PORTS; port++) {
			g_raw[port] = next[port];
			if ((captured & (1u << port)) || combo_down[port]) {
				PadState idle;
				idle.connected = next[port].connected;
				idle.vendor = next[port].vendor;
				idle.product = next[port].product;
				next[port] = idle;
			}
			if (g_keys[port] != keys[port]) {
				any_changed[port] = true;
				g_keys[port] = keys[port];
			}
			g_names[port] = names[port];
		}
		std::copy(std::begin(next), std::end(next), g_snapshot);
	}
	// listeners run outside the lock: they may call back into input
	for (uint32_t port = 0; port < MAX_PORTS; port++) {
		if (any_changed[port]) {
			if (keys[port].empty()) {
				if (g_listener.disconnected) g_listener.disconnected(port);
			} else if (g_listener.connected) {
				g_listener.connected(port, keys[port]);
			}
		}
		if (menu[port] && !g_menu_held[port] && g_listener.menu_button) g_listener.menu_button(port);
		if (combo_due[port]) {
			LOG_INFO("input: exit combo held on player %u", port + 1);
			if (g_listener.exit_combo) g_listener.exit_combo(port);
		}
		g_menu_held[port] = menu[port];
	}
}

PadState raw_pad(uint32_t port) {
	std::lock_guard lock(g_lock);
	return port < MAX_PORTS ? g_raw[port] : PadState{};
}

std::string controller_key(uint32_t port) {
	std::lock_guard lock(g_lock);
	return port < MAX_PORTS ? g_keys[port] : std::string();
}

std::string controller_name(uint32_t port) {
	std::lock_guard lock(g_lock);
	return port < MAX_PORTS ? g_names[port] : std::string();
}

void set_captured(uint32_t port, bool captured) {
	if (port >= MAX_PORTS) return;
	if (captured) g_captured |= 1u << port;
	else g_captured &= ~(1u << port);
}

void set_listener(const Listener& l) { g_listener = l; }

PadState pad(uint32_t port) {
	std::lock_guard lock(g_lock);
	return port < MAX_PORTS ? g_snapshot[port] : PadState{};
}

uint32_t connected_count() {
	std::lock_guard lock(g_lock);
	return uint32_t(std::count_if(std::begin(g_snapshot), std::end(g_snapshot), [](const PadState& s) {
		return s.connected;
	}));
}

void set_rumble(uint32_t port, uint8_t small_motor, uint8_t large_motor) {
	if (port < MAX_PORTS) g_rumble_request[port] = uint16_t((small_motor ? 1 : 0) << 8 | large_motor);
}

bool set_script(const char* script) {
	g_script.clear();
	g_script_ports = 0;
	std::string s = script;
	size_t pos = 0;
	while (pos < s.size()) {
		size_t end = s.find(',', pos);
		if (end == std::string::npos) end = s.size();
		std::string item = s.substr(pos, end - pos);
		size_t colon = item.find(':');
		if (colon == std::string::npos) return false;
		ScriptedPress p{std::strtoull(item.c_str(), nullptr, 10), 0, 0};
		std::string name = item.substr(colon + 1);
		// "...@N": on controller N (default 1)
		if (size_t at = name.find('@'); at != std::string::npos) {
			p.port = uint32_t(std::strtoul(name.c_str() + at + 1, nullptr, 10)) - 1;
			if (p.port >= MAX_PORTS) return false;
			name.resize(at);
		}
		// "button+button/hold_ms": several buttons at once, held longer than the default
		if (size_t slash = name.find('/'); slash != std::string::npos) {
			p.hold_ms = std::strtoull(name.c_str() + slash + 1, nullptr, 10);
			name.resize(slash);
		}
		if (name == "ps") p.menu = true;
		else if (!parse_buttons(name, p.d1, p.d2)) return false;
		g_script_ports |= 1u << p.port;
		g_script.push_back(p);
		pos = end + 1;
	}
	return true;
}

} // namespace input
