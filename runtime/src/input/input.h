// Host input: SDL3 gamepads + keyboard, presented as up to 7 PS3 controller ports.
// The window thread feeds SDL events in and publishes a snapshot; the game's
// threads read the snapshot through cellPad.
#pragma once

#include <cstdint>
#include <string>

union SDL_Event;

namespace input {

constexpr uint32_t MAX_PORTS = 7;

// Digital buttons as the PS3 reports them (cellPad DIGITAL1 / DIGITAL2 words)
namespace d1 {
constexpr uint16_t SELECT = 0x01, L3 = 0x02, R3 = 0x04, START = 0x08, UP = 0x10, RIGHT = 0x20, DOWN = 0x40,
                   LEFT = 0x80;
}
namespace d2 {
constexpr uint16_t L2 = 0x01, R2 = 0x02, L1 = 0x04, R1 = 0x08, TRIANGLE = 0x10, CIRCLE = 0x20, CROSS = 0x40,
                   SQUARE = 0x80;
}

struct PadState {
	bool connected = false;
	uint16_t vendor = 0x054C, product = 0x0268;   // reported as a DualShock 3
	uint16_t digital1 = 0, digital2 = 0;
	uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;   // 0..255, 0x80 = centred, y down = 255
	// pressure (0..255) in cellPad order: right left up down triangle circle cross square L1 R1 L2 R2
	uint8_t pressure[12] = {};
};

// Window thread
void init();                          // after SDL_Init; enables gamepad events
void handle_event(const SDL_Event& e);
void update();                        // read devices and publish a new snapshot

// Any thread
PadState pad(uint32_t port);          // what the game sees (neutral while the port is captured)
PadState raw_pad(uint32_t port);      // the device's real state, also while captured
uint32_t connected_count();
void set_rumble(uint32_t port, uint8_t small_motor, uint8_t large_motor);

// Debugging: scripted presses on port 0, "ms:button,ms:button,..." with times
// from startup and buttons named like the PS3's (start, cross, up, l1, ...).
// Each press is held for 150 ms; "ms:l2+r2+select+start/1500" presses several
// buttons together and holds them 1500 ms; "ms:cross@2" presses on controller 2
// (a scripted pad, connected from the start). Returns false on a parse error.
bool set_script(const char* script);

// Stable identity of the controller on a port, for remembering who uses it: the
// device serial number where SDL reports one ("serial:..."), else "port:N"; the
// keyboard on port 0 is "keyboard". Empty when nothing is connected.
std::string controller_key(uint32_t port);
std::string controller_name(uint32_t port);

// While a port is captured (e.g. by the profile menu) the game sees it connected
// but idle; raw_pad() still reports it.
void set_captured(uint32_t port, bool captured);

// Called on the window thread when a controller's PS / Guide button (F1 for the
// keyboard) is pressed, when a controller connects or disconnects, and when a
// controller has held the exit combo (settings.ini input.exit_combo) long enough.
struct Listener {
	void (*menu_button)(uint32_t port) = nullptr;
	void (*connected)(uint32_t port, const std::string& key) = nullptr;
	void (*disconnected)(uint32_t port) = nullptr;
	void (*exit_combo)(uint32_t port) = nullptr;
};
void set_listener(const Listener& l);

} // namespace input
