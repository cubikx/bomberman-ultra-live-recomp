// Profile names in the game's own UI: call hooks (recomp/hooks.py) that replace the
// game's "Player N" labels with the name of the profile on that controller. Also
// watches the player slots so a controller that joins is asked who is playing.
#include <cstdio>
#include <format>
#include <mutex>
#include <set>

#include "ppu/recomp.h"
#include "profiles/profiles.h"
#include "runtime/kernel.h"
#include "ui/overlay.h"

namespace {

constexpr uint32_t LOCAL_SLOTS = 4;   // controllers that can carry a profile

// The player slot manager (sub_000DC100 creates it): 8 slots of 124 bytes. A slot is
// in use when byte 0 <= 7; byte 79 is set for a CPU; a human's pad port is byte 78.
// The slot's name (UTF-16BE, 20 characters) is at +2: filled in from GetPlayerName
// when the player joins, then shown by the tooltip bar, results and winner banner.
constexpr u32 SLOTS_PTR = 0x28aa48;
constexpr u32 SLOT_SIZE = 124, SLOT_COUNT = 8;
constexpr u32 SLOT_NAME = 2, SLOT_NAME_CHARS = 20;

void log_once(const char* what, int32_t index, const std::string& name) {
	static std::mutex lock;
	static std::set<std::string> logged;
	std::lock_guard g(lock);
	if (logged.insert(std::format("{}{}{}", what, index, name)).second)
		LOG_INFO("profiles: %s %d shows as %s", what, index, name.c_str());
}

// Write an ASCII name as the game's UTF-16BE text, cut to fit `chars` (with the NUL).
void put_utf16(u32 ea, const std::string& s, size_t chars) {
	size_t n = std::min(s.size(), chars - 1);
	for (size_t i = 0; i < n; i++) st16(u32(ea + 2 * i), u16(u8(s[i])));
	st16(u32(ea + 2 * n), 0);
}

bool equals_utf16(u32 ea, const std::string& s, size_t chars) {
	size_t n = std::min(s.size(), chars - 1);
	for (size_t i = 0; i < n; i++)
		if (ld16(u32(ea + 2 * i)) != u16(u8(s[i]))) return false;
	return ld16(u32(ea + 2 * n)) == 0;
}

} // namespace

// Player Select slot label: sub_000E1238(buffer, chars, "IDS_PLAYER_NUMBER", number)
// where number is the slot's userIndex + 1 (= pad port + 1).
void hook_player_label(PPUContext& ctx, PPUFunc original) {
	u32 buffer = u32(ctx.r[3]), chars = u32(ctx.r[4]);
	int32_t number = int32_t(ctx.r[6]);
	original(ctx);
	if (number < 1 || uint32_t(number) > LOCAL_SLOTS || chars < 2) return;
	std::string name = profiles::assigned(uint32_t(number - 1));
	if (name.empty()) return;   // guest: keep "Player N"
	put_utf16(buffer, name, chars);
	log_once("Player Select label", number, name);
}

// GetPlayerName(net, slot) returns the slot's name in a 17-character buffer: the
// signed-in PSN name, or "Player N". A human slot with a profile gets its name.
void hook_slot_name(PPUContext& ctx, PPUFunc original) {
	int32_t slot = int32_t(ctx.r[4]);
	original(ctx);
	u32 slots = ld32(SLOTS_PTR), name_ea = u32(ctx.r[3]);
	if (!slots || !name_ea || slot < 0 || u32(slot) >= SLOT_COUNT) return;
	u32 s = slots + u32(slot) * SLOT_SIZE;
	if (ld8(s) > 7 || ld8(s + 79)) return;
	int32_t port = int8_t(ld8(s + 78));
	if (port < 0 || uint32_t(port) >= LOCAL_SLOTS) return;
	std::string name = profiles::assigned(uint32_t(port));
	if (name.empty()) return;
	put_utf16(name_ea, name, 17);
	log_once("slot", slot, name);
}

namespace profiles {

// Also keeps each human slot's stored name in step with the profile on its controller,
// so a profile switch after joining shows everywhere (guests get the game's "Player N").
void watch_slots() {
	static uint32_t joined = 0;   // ports with a human slot, as of the last call
	uint32_t now = 0;
	if (u32 slots = ld32(SLOTS_PTR))
		for (u32 i = 0; i < SLOT_COUNT; i++) {
			u32 s = slots + i * SLOT_SIZE;
			int32_t port = int8_t(ld8(s + 78));
			if (ld8(s) > 7 || ld8(s + 79) || port < 0 || uint32_t(port) >= LOCAL_SLOTS) continue;
			now |= 1u << port;
			std::string name = assigned(uint32_t(port));
			if (name.empty()) name = std::format("Player {}", i + 1);   // as GetPlayerName(net, slot)
			if (!equals_utf16(s + SLOT_NAME, name, SLOT_NAME_CHARS)) put_utf16(s + SLOT_NAME, name, SLOT_NAME_CHARS);
		}
	for (uint32_t port = 0; port < LOCAL_SLOTS; port++) {
		uint32_t bit = 1u << port;
		if ((now & bit) == (joined & bit)) continue;
		LOG_INFO("profiles: controller %u %s", port + 1, now & bit ? "joined" : "left");
		if (now & bit) overlay::player_joined(port);
		else overlay::player_left(port);
	}
	joined = now;
}

} // namespace profiles
