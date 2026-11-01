// sys_io: cellPad on top of the host input layer (input/input.h). Keyboard and
// mouse are reported as absent; the keyboard already drives pad port 0.
#include <algorithm>
#include <cstring>
#include <mutex>

#include "input/input.h"
#include "hle/bot.h"
#include "profiles/profiles.h"
#include "trophies/trophies.h"
#include "runtime/kernel.h"

namespace {

constexpr u32 CELL_PAD_ERROR_INVALID_PARAMETER = 0x80121102, CELL_PAD_ERROR_ALREADY_INITIALIZED = 0x80121103,
              CELL_PAD_ERROR_UNINITIALIZED = 0x80121104, CELL_PAD_ERROR_NO_DEVICE = 0x80121107;
constexpr u32 CELL_KB_ERROR_UNINITIALIZED = 0x80121004, CELL_KB_ERROR_NO_DEVICE = 0x80121007;
constexpr u32 CELL_MOUSE_ERROR_UNINITIALIZED = 0x80121204, CELL_MOUSE_ERROR_NO_DEVICE = 0x80121207;

constexpr u32 CELL_PAD_STATUS_CONNECTED = 1, CELL_PAD_STATUS_ASSIGN_CHANGES = 2;
constexpr u32 CELL_PAD_SETTING_PRESS_ON = 2, CELL_PAD_SETTING_SENSOR_ON = 4;
// PS3 conformity, pressure, sensors, high-precision sticks, actuators
constexpr u32 PAD_CAPABILITIES = 0x1F;
constexpr u32 CELL_PAD_LEN_CHANGE_DEFAULT = 8, CELL_PAD_LEN_CHANGE_PRESS_ON = 20, CELL_PAD_LEN_CHANGE_SENSOR_ON = 24;
constexpr u32 CELL_MAX_PADS = 127;

std::mutex g_lock;
bool g_pad_init = false;
u32 g_max_connect = 0;
u32 g_port_setting[input::MAX_PORTS];
bool g_reported_connected[input::MAX_PORTS];   // what the game was last told, for ASSIGN_CHANGES

u32 max_ports() { return std::min(g_max_connect, input::MAX_PORTS); }

// the host pad, or the bot's on a port a CPU player drives
input::PadState pad_state(u32 port) {
	input::PadState s;
	if (bot::pad(port, s)) return s;
	return input::pad(port);
}

// port status as the game should see it, flagging connects/disconnects once
u32 port_status(u32 port, bool connected) {
	u32 s = connected ? CELL_PAD_STATUS_CONNECTED : 0;
	if (connected != g_reported_connected[port]) {
		s |= CELL_PAD_STATUS_ASSIGN_CHANGES;
		g_reported_connected[port] = connected;
	}
	return s;
}

bool kb_init = false, mouse_init = false;

} // namespace

HLE_FUNC(sys_io, cellPadInit) {
	std::lock_guard lock(g_lock);
	if (g_pad_init) return ret(ctx, CELL_PAD_ERROR_ALREADY_INITIALIZED);
	u32 max = arg32(ctx, 0);
	if (!max || max > CELL_MAX_PADS) return ret(ctx, CELL_PAD_ERROR_INVALID_PARAMETER);
	g_pad_init = true;
	g_max_connect = max;
	for (u32 p = 0; p < input::MAX_PORTS; p++) {
		g_port_setting[p] = 0;
		g_reported_connected[p] = false;
	}
	LOG_INFO("cellPad: init, max %u controllers", max);
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_io, cellPadEnd) {
	std::lock_guard lock(g_lock);
	if (!g_pad_init) return ret(ctx, CELL_PAD_ERROR_UNINITIALIZED);
	g_pad_init = false;
	for (u32 p = 0; p < input::MAX_PORTS; p++) input::set_rumble(p, 0, 0);
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_io, cellPadGetInfo) {
	std::lock_guard lock(g_lock);
	if (!g_pad_init) return ret(ctx, CELL_PAD_ERROR_UNINITIALIZED);
	u32 info = arg32(ctx, 0);
	if (!info) return ret(ctx, CELL_PAD_ERROR_INVALID_PARAMETER);
	// CellPadInfo: max_connect, now_connect, system_info, vendor_id[127], product_id[127], status[127]
	std::memset(vm_ptr(info), 0, 12 + CELL_MAX_PADS * 5);
	u32 now = 0;
	for (u32 p = 0; p < max_ports(); p++) {
		input::PadState s = pad_state(p);
		if (s.connected) now++;
		st16(info + 12 + p * 2, s.vendor);
		st16(info + 12 + CELL_MAX_PADS * 2 + p * 2, s.product);
		st8(info + 12 + CELL_MAX_PADS * 4 + p, u8(port_status(p, s.connected) & CELL_PAD_STATUS_CONNECTED));
	}
	st32(info, g_max_connect);
	st32(info + 4, now);
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_io, cellPadGetInfo2) {
	std::lock_guard lock(g_lock);
	if (!g_pad_init) return ret(ctx, CELL_PAD_ERROR_UNINITIALIZED);
	u32 info = arg32(ctx, 0);
	if (!info) return ret(ctx, CELL_PAD_ERROR_INVALID_PARAMETER);
	// CellPadInfo2: max_connect, now_connect, system_info, then port_status, port_setting,
	// device_capability and device_type, 7 words each
	std::memset(vm_ptr(info), 0, 12 + 4 * input::MAX_PORTS * 4);
	u32 now = 0;
	for (u32 p = 0; p < max_ports(); p++) {
		bool connected = pad_state(p).connected;
		if (connected) now++;
		st32(info + 12 + p * 4, port_status(p, connected));
		st32(info + 40 + p * 4, g_port_setting[p]);
		st32(info + 68 + p * 4, connected ? PAD_CAPABILITIES : 0);
		st32(info + 96 + p * 4, 0);   // CELL_PAD_DEV_TYPE_STANDARD
	}
	st32(info, g_max_connect);
	st32(info + 4, now);
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_io, cellPadGetCapabilityInfo) {
	std::lock_guard lock(g_lock);
	if (!g_pad_init) return ret(ctx, CELL_PAD_ERROR_UNINITIALIZED);
	u32 port = arg32(ctx, 0), info = arg32(ctx, 1);
	if (port >= g_max_connect || !info) return ret(ctx, CELL_PAD_ERROR_INVALID_PARAMETER);
	if (port >= input::MAX_PORTS || !pad_state(port).connected) return ret(ctx, CELL_PAD_ERROR_NO_DEVICE);
	std::memset(vm_ptr(info), 0, 32 * 4);
	st32(info, PAD_CAPABILITIES);
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_io, cellPadSetPressMode) {
	std::lock_guard lock(g_lock);
	if (!g_pad_init) return ret(ctx, CELL_PAD_ERROR_UNINITIALIZED);
	u32 port = arg32(ctx, 0), mode = arg32(ctx, 1);
	if (port >= g_max_connect || mode > 1) return ret(ctx, CELL_PAD_ERROR_INVALID_PARAMETER);
	if (port >= input::MAX_PORTS) return ret(ctx, CELL_PAD_ERROR_NO_DEVICE);
	g_port_setting[port] = mode ? g_port_setting[port] | CELL_PAD_SETTING_PRESS_ON
	                            : g_port_setting[port] & ~CELL_PAD_SETTING_PRESS_ON;
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_io, cellPadSetSensorMode) {
	std::lock_guard lock(g_lock);
	if (!g_pad_init) return ret(ctx, CELL_PAD_ERROR_UNINITIALIZED);
	u32 port = arg32(ctx, 0), mode = arg32(ctx, 1);
	if (port >= g_max_connect || mode > 1) return ret(ctx, CELL_PAD_ERROR_INVALID_PARAMETER);
	if (port >= input::MAX_PORTS) return ret(ctx, CELL_PAD_ERROR_NO_DEVICE);
	g_port_setting[port] = mode ? g_port_setting[port] | CELL_PAD_SETTING_SENSOR_ON
	                            : g_port_setting[port] & ~CELL_PAD_SETTING_SENSOR_ON;
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_io, cellPadSetActDirect) {
	std::lock_guard lock(g_lock);
	if (!g_pad_init) return ret(ctx, CELL_PAD_ERROR_UNINITIALIZED);
	u32 port = arg32(ctx, 0), param = arg32(ctx, 1);
	if (port >= g_max_connect || !param) return ret(ctx, CELL_PAD_ERROR_INVALID_PARAMETER);
	if (port >= input::MAX_PORTS || !pad_state(port).connected) return ret(ctx, CELL_PAD_ERROR_NO_DEVICE);
	input::PadState bot_pad;
	if (bot::pad(port, bot_pad)) return ret(ctx, CELL_OK);   // a bot's pad: nothing to shake
	// CellPadActParam: motor[0] = small (on/off), motor[1] = large (0..255)
	input::set_rumble(port, ld8(param), ld8(param + 1));
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_io, cellPadGetData) {
	profiles::game_thread_tick();   // the game reads pads every frame on its main thread
	profiles::watch_slots();
	trophies::game_thread_tick();
	bot::game_thread_tick();
	std::lock_guard lock(g_lock);
	if (!g_pad_init) return ret(ctx, CELL_PAD_ERROR_UNINITIALIZED);
	u32 port = arg32(ctx, 0), data = arg32(ctx, 1);
	if (port >= g_max_connect || !data) return ret(ctx, CELL_PAD_ERROR_INVALID_PARAMETER);
	input::PadState s = port < input::MAX_PORTS ? pad_state(port) : input::PadState{};
	if (port >= input::MAX_PORTS || !s.connected) return ret(ctx, CELL_PAD_ERROR_NO_DEVICE);

	// CellPadData: s32 len, u16 button[64]. Always report fresh data rather than
	// CELL_PAD_LEN_NO_CHANGE; games treat both the same.
	u32 setting = g_port_setting[port];
	u32 len = (setting & CELL_PAD_SETTING_SENSOR_ON) ? CELL_PAD_LEN_CHANGE_SENSOR_ON
	        : (setting & CELL_PAD_SETTING_PRESS_ON) ? CELL_PAD_LEN_CHANGE_PRESS_ON : CELL_PAD_LEN_CHANGE_DEFAULT;
	u16 b[24] = {};
	b[0] = 0;
	b[1] = u16(0x70 | (len / 2));
	b[2] = s.digital1;
	b[3] = s.digital2;
	b[4] = s.rx;
	b[5] = s.ry;
	b[6] = s.lx;
	b[7] = s.ly;
	if (len >= CELL_PAD_LEN_CHANGE_PRESS_ON)
		for (u32 i = 0; i < 12; i++) b[8 + i] = s.pressure[i];
	if (len >= CELL_PAD_LEN_CHANGE_SENSOR_ON) {
		// motion sensors at rest: 512 = 0 g on x/z, y feels gravity; gyro 512
		b[20] = 512;
		b[21] = 399;
		b[22] = 512;
		b[23] = 512;
	}
	st32(data, len);
	for (u32 i = 0; i < 64; i++) st16(data + 4 + i * 2, i < len ? b[i] : 0);
	ret(ctx, CELL_OK);
}

// --- keyboard / mouse: initialised fine, but nothing is ever connected -----------

HLE_FUNC(sys_io, cellKbInit) {
	kb_init = true;
	ret(ctx, CELL_OK);
}
HLE_FUNC(sys_io, cellKbEnd) {
	kb_init = false;
	ret(ctx, CELL_OK);
}
HLE_FUNC(sys_io, cellKbGetInfo) {
	if (!kb_init) return ret(ctx, CELL_KB_ERROR_UNINITIALIZED);
	// CellKbInfo: max_connect, now_connect, info, status[127]
	u32 info = arg32(ctx, 0);
	std::memset(vm_ptr(info), 0, 12 + CELL_MAX_PADS);
	st32(info, 1);
	ret(ctx, CELL_OK);
}
HLE_FUNC(sys_io, cellKbRead) { ret(ctx, kb_init ? CELL_KB_ERROR_NO_DEVICE : CELL_KB_ERROR_UNINITIALIZED); }
HLE_FUNC(sys_io, cellKbSetCodeType) { ret(ctx, kb_init ? CELL_KB_ERROR_NO_DEVICE : CELL_KB_ERROR_UNINITIALIZED); }
HLE_FUNC(sys_io, cellKbSetReadMode) { ret(ctx, kb_init ? CELL_KB_ERROR_NO_DEVICE : CELL_KB_ERROR_UNINITIALIZED); }

HLE_FUNC(sys_io, cellMouseInit) {
	mouse_init = true;
	ret(ctx, CELL_OK);
}
HLE_FUNC(sys_io, cellMouseEnd) {
	mouse_init = false;
	ret(ctx, CELL_OK);
}
HLE_FUNC(sys_io, cellMouseGetInfo) {
	if (!mouse_init) return ret(ctx, CELL_MOUSE_ERROR_UNINITIALIZED);
	// CellMouseInfo: max_connect, now_connect, info, vendor_id[127], product_id[127], status[127]
	u32 info = arg32(ctx, 0);
	std::memset(vm_ptr(info), 0, 12 + CELL_MAX_PADS * 5);
	st32(info, 1);
	ret(ctx, CELL_OK);
}
HLE_FUNC(sys_io, cellMouseGetData) {
	ret(ctx, mouse_init ? CELL_MOUSE_ERROR_NO_DEVICE : CELL_MOUSE_ERROR_UNINITIALIZED);
}
