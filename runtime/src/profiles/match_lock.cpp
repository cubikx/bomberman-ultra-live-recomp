// Match lock: profiles must not change while a match is on. The game copies each slot's
// record into the match and writes the results back at the end, and trophies go to
// whoever is on the slot when they unlock, so a profile switched mid-match would get the
// other player's results. "In a match" is: one of the game's match screens exists, from
// the arena's loading screen to the results. Their XUI classes are registered in
// sub_000E57D8; the call hooks here (recomp/hooks.py) wrap each class's create and
// destroy to count the live ones.
#include <array>
#include <atomic>
#include <format>
#include <string>
#include <thread>
#include <utility>

#include "ppu/recomp.h"
#include "profiles/profiles.h"
#include "runtime/kernel.h"

namespace {

constexpr int MAX_CLASSES = 8;
constexpr u32 CLASS_NAME = 4, CLASS_CREATE = 16, CLASS_DESTROY = 20;   // XUIClass fields

struct Wrapped {
	u32 create = 0, destroy = 0;   // the game's own (code addresses)
	std::string name;
};
Wrapped g_classes[MAX_CLASSES];
int g_class_count = 0;
std::atomic<int> g_live{0};   // match screens that exist now

void changed(const std::string& name, int delta) {
	int live = g_live += delta;
	if (live < 0) live = g_live = 0;
	LOG_INFO("match lock: %s %s (%d match screens)", name.c_str(), delta > 0 ? "created" : "destroyed", live);
	// debugging: MATCH_DUMP=1 logs the match config (0x28ab60) as each match starts
	static const u64 dump = env_u64("MATCH_DUMP");
	if (dump && delta > 0 && live == 1) {
		std::string line;
		for (u32 off = 0; off < 0x120; off += 4) line += std::format("{}{:x}={:x}", off ? " " : "", off, ld32(0x28ab60 + off));
		LOG_INFO("match config: %s", line.c_str());
		u32 net = ld32(0x1e0380), fw = ld32(0x1d9b04);
		LOG_INFO("match config: net state (+0x20300) %d, framework+6 %d", net ? int32_t(ld32(net + 0x20300)) : -99,
		         fw ? int(ld8(fw + 6)) : -99);
		// and the wardrobe pickup's flags in the game object (*0x1dff70): +376 queued at
		// match start, +377 armed, cleared when the spawn is tried ~61 s in (sub_00055400)
		static std::atomic<bool> watching{false};
		if (!watching.exchange(true))
			std::thread([] {
				int last = -1;
				for (;;) {
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					u32 game = ld32(0x1dff70);
					if (!game) continue;
					int v = ld8(game + 376) | ld8(game + 377) << 8;
					if (v != last) LOG_INFO("wardrobe: +376 %d +377 %d (t %d)", v & 0xff, v >> 8, int32_t(ld32(game + 236)));
					last = v;
				}
			}).detach();
	}
}

// create(hObj, out): the game's create, then count it if it succeeded.
template <int I>
void create(PPUContext& ctx) {
	ppu_icall(ctx, g_classes[I].create);
	if (u32(ctx.r[3]) == 0) changed(g_classes[I].name, +1);
}

template <int I>
void destroy(PPUContext& ctx) {
	ppu_icall(ctx, g_classes[I].destroy);
	changed(g_classes[I].name, -1);
}

using Fn = void (*)(PPUContext&);
template <int... I>
constexpr std::array<Fn, MAX_CLASSES> creates(std::integer_sequence<int, I...>) { return {create<I>...}; }
template <int... I>
constexpr std::array<Fn, MAX_CLASSES> destroys(std::integer_sequence<int, I...>) { return {destroy<I>...}; }
constexpr auto CREATES = creates(std::make_integer_sequence<int, MAX_CLASSES>());
constexpr auto DESTROYS = destroys(std::make_integer_sequence<int, MAX_CLASSES>());

std::string utf16_name(u32 ea) {
	std::string s;
	for (u16 c; ea && (c = ld16(ea)) != 0 && s.size() < 64; ea += 2) s += char(c < 0x80 ? c : '?');
	return s;
}

} // namespace

namespace profiles {
bool in_match() { return g_live > 0; }
} // namespace profiles

// XuiRegisterClass(cls, out) for one of the match screens (r3 = the XUIClass on the
// stack, copied by the registration): its create and destroy are swapped for counting
// wrappers first.
void hook_match_screen_class(PPUContext& ctx, PPUFunc original) {
	u32 cls = u32(ctx.r[3]);
	if (g_class_count < MAX_CLASSES) {
		int i = g_class_count++;
		Wrapped& w = g_classes[i];
		w.name = utf16_name(ld32(cls + CLASS_NAME));
		w.create = ld32(ld32(cls + CLASS_CREATE));
		w.destroy = ld32(ld32(cls + CLASS_DESTROY));
		static std::string labels[MAX_CLASSES * 2];
		labels[2 * i] = w.name + "::CreateInstance (match lock)";
		labels[2 * i + 1] = w.name + "::DestroyInstance (match lock)";
		st32(cls + CLASS_CREATE, hle_trampoline(CREATES[i], labels[2 * i].c_str()));
		st32(cls + CLASS_DESTROY, hle_trampoline(DESTROYS[i], labels[2 * i + 1].c_str()));
	}
	original(ctx);
}
