// cellSysutil: video/audio output configuration, system parameters, system callbacks.
#include <deque>
#include <mutex>

#include "runtime/kernel.h"

namespace {

// Resolution IDs used by cellVideoOut
constexpr u8 RES_1080 = 1, RES_720 = 2, RES_480 = 4, RES_576 = 5;
constexpr u8 ASPECT_16_9 = 2, SCAN_PROGRESSIVE = 1, COLOR_RGB = 1;
constexpr u16 REFRESH_59_94 = 1;

// What the game is told it is running on. Rendering resolution on the host is
// independent of this (see the graphics backend); 720p is what the game was built for.
constexpr u8 g_reported_res = RES_720;

bool resolution_size(u32 id, u16& w, u16& h) {
	switch (id) {
	case RES_1080: w = 1920; h = 1080; return true;
	case RES_720: w = 1280; h = 720; return true;
	case RES_480: w = 720; h = 480; return true;
	case RES_576: w = 720; h = 576; return true;
	default: return false;
	}
}

void write_display_mode(u32 p, u8 res) {
	st8(p + 0, res);
	st8(p + 1, SCAN_PROGRESSIVE);
	st8(p + 2, 0);
	st8(p + 3, ASPECT_16_9);
	st16(p + 6, REFRESH_59_94);
}

struct Callback {
	u32 func = 0, userdata = 0;
};
std::mutex g_cb_lock;
Callback g_callbacks[4];
std::deque<std::pair<u64, u64>> g_events; // (status, param)

} // namespace

// Queue a system event (e.g. CELL_SYSUTIL_REQUEST_EXITGAME) for the game's next cellSysutilCheckCallback.
void sysutil_post_event(u64 status, u64 param) {
	std::lock_guard lock(g_cb_lock);
	g_events.push_back({status, param});
}

HLE_FUNC(cellSysutil, cellSysutilRegisterCallback) {
	u32 slot = arg32(ctx, 0);
	if (slot >= 4) return ret(ctx, CELL_EINVAL);
	std::lock_guard lock(g_cb_lock);
	g_callbacks[slot] = {arg32(ctx, 1), arg32(ctx, 2)};
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellSysutil, cellSysutilCheckCallback) {
	for (;;) {
		std::pair<u64, u64> ev;
		Callback cbs[4];
		{
			std::lock_guard lock(g_cb_lock);
			if (g_events.empty()) break;
			ev = g_events.front();
			g_events.pop_front();
			std::copy(std::begin(g_callbacks), std::end(g_callbacks), cbs);
		}
		for (const auto& cb : cbs) {
			if (!cb.func) continue;
			// void callback(u64 status, u64 param, void* userdata)
			u64 saved[3] = {ctx.r[3], ctx.r[4], ctx.r[5]};
			ctx.r[3] = ev.first;
			ctx.r[4] = ev.second;
			ctx.r[5] = cb.userdata;
			call_guest_opd(ctx, cb.func);
			ctx.r[3] = saved[0];
			ctx.r[4] = saved[1];
			ctx.r[5] = saved[2];
		}
	}
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellSysutil, cellSysutilGetSystemParamInt) {
	u32 id = arg32(ctx, 0), out = arg32(ctx, 1);
	s32 v = 0;
	switch (id) {
	case 0x0111: v = 1; break;   // language: English (US)
	case 0x0112: v = 1; break;   // enter button: cross
	case 0x0114: v = 1; break;   // date format: DDMMYYYY
	case 0x0115: v = 0; break;   // time format: 24h
	case 0x0121: v = 0; break;   // parental level: off
	case 0x0131: v = 0; break;   // current user has no PSN account
	case 0x0151: v = 1; break;   // pad rumble on
	default: LOG_INFO("cellSysutilGetSystemParamInt(0x%x) -> 0", id);
	}
	st32(out, u32(v));
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellSysutil, cellVideoOutGetNumberOfDevice) {
	ret(ctx, arg32(ctx, 0) == 0 ? 1 : 0); // one primary display
}

HLE_FUNC(cellSysutil, cellVideoOutGetState) {
	u32 out = arg32(ctx, 2);
	std::memset(vm_ptr(out), 0, 16);
	st8(out + 0, 0);           // enabled
	st8(out + 1, COLOR_RGB);
	write_display_mode(out + 8, g_reported_res);
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellSysutil, cellVideoOutGetResolution) {
	u16 w, h;
	if (!resolution_size(arg32(ctx, 0), w, h)) return ret(ctx, CELL_EINVAL);
	st16(arg32(ctx, 1), w);
	st16(arg32(ctx, 1) + 2, h);
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellSysutil, cellVideoOutGetResolutionAvailability) {
	u32 res = arg32(ctx, 1);
	ret(ctx, res == RES_720 || res == RES_1080 || res == RES_480 ? 1 : 0);
}

HLE_FUNC(cellSysutil, cellVideoOutConfigure) {
	u32 cfg = arg32(ctx, 1);
	LOG_INFO("cellVideoOutConfigure(res %u, format %u, aspect %u, pitch %u)", ld8(cfg), ld8(cfg + 1),
	         ld8(cfg + 2), ld32(cfg + 12));
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellSysutil, cellVideoOutGetDeviceInfo) {
	u32 out = arg32(ctx, 2);
	std::memset(vm_ptr(out), 0, 32 + 32 * 8 + 168);
	st8(out + 0, 1);           // HDMI
	st8(out + 1, COLOR_RGB);
	st8(out + 4, 2);           // two modes
	st8(out + 5, 2);           // available
	write_display_mode(out + 32, RES_720);
	write_display_mode(out + 40, RES_1080);
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellSysutil, cellAudioOutGetState) {
	u32 out = arg32(ctx, 2);
	std::memset(vm_ptr(out), 0, 20);
	st8(out + 12, 0);          // LPCM
	st8(out + 13, 2);          // stereo
	st8(out + 14, 4);          // 48 kHz
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellSysutil, cellAudioOutGetSoundAvailability) {
	ret(ctx, arg32(ctx, 1) == 0 ? 2 : 0); // LPCM stereo only
}

HLE_FUNC(cellSysutil, cellAudioOutConfigure) { ret(ctx, CELL_OK); }
