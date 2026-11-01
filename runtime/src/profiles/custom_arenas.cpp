// Custom Arenas: the Xbox 360 version's saved match settings, which the PS3 port cut.
// On the 360, Power-Up Settings -> Save Settings (Y) stored the match rules and
// power-up choices in one of 3 slots of the player's profile, and Arena Select listed
// them after Random as "Custom Arena 1-3"; picking one loaded them. The PS3 code still
// has the button, the load (Arena Select, arena ids 17-19) and the slot getter, but no
// way to save, no list entries and nothing in the slots. These call hooks
// (recomp/hooks.py) put them back, with the slots kept for the whole game in
// user/settings/custom_arenas.ini instead of per player.
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

#include "ppu/recomp.h"
#include "profiles/custom_arenas.h"
#include "runtime/kernel.h"

using custom_arenas::SLOTS;
using custom_arenas::Slot;

namespace {

constexpr int32_t FIRST_ID = 17;   // arena ids 17, 18, 19 = Custom Arena 1, 2, 3
constexpr int32_t RANDOM_ID = 16;

// A slot is the 360's 7 words: {match+0x24, match+0x1C, match+0x14, arena (match+0),
// rules packed by sub_000E4228, power-ups packed by sub_000E4108, match+0x10}. The
// match config is at 0x28AB60; Arena Select unpacks a slot into it (0x109F78).
constexpr Slot OPEN_SLOT = {8, 2, 1, u32(-1), u32(-1), u32(-1), 0};
constexpr int SLOT_ARENA = 3;
constexpr u32 MATCH = 0x28ab60;
constexpr u32 PACK_RULES = 0xe4228, PACK_POWERUPS = 0xe4108;

// Arena list manager (sub_00108610): the Arena Select list is a vector of 668-byte
// entries at +4/+8 with the count of the game's own entries at +48; all arenas are in a second vector at +20/+24.
constexpr u32 ARENAS_PTR = 0x28af74;
constexpr u32 ENTRY_SIZE = 668;
constexpr u32 ENTRY_THUMB = 4, ENTRY_THUMB_CHARS = 128;   // UTF-16BE path
constexpr u32 ENTRY_OPTIONS = 260;                        // 1: arena with option lists
constexpr u32 ENTRY_NAME = 0x10c, ENTRY_DESC = 0x18c, ENTRY_TEXT_CHARS = 64;   // string ids
constexpr u32 ENTRY_SLOT = 664;                           // custom arena: its slot
constexpr u32 VECTOR_PUSH = 0x107930;                     // push_back(vector, entry*)

std::mutex g_lock;
std::array<std::optional<Slot>, SLOTS> g_slots;
Slot g_pending{};   // what the Save Custom Settings screen saves
bool g_loaded = false;
u32 g_entry_buf = 0;   // guest scratch for building a list entry

std::filesystem::path file() { return g_paths.settings / "custom_arenas.ini"; }

void load_locked() {
	if (g_loaded) return;
	g_loaded = true;
	std::ifstream in(file());
	std::string line;
	while (std::getline(in, line)) {
		size_t eq = line.find('=');
		if (line.rfind("slot", 0) != 0 || eq == std::string::npos) continue;
		int slot = std::atoi(line.c_str() + 4);
		if (slot < 1 || slot > SLOTS) continue;
		Slot words{};
		std::istringstream values(line.substr(eq + 1));
		std::string value;
		size_t n = 0;
		while (n < words.size() && std::getline(values, value, ','))
			words[n++] = u32(std::stoul(value, nullptr, 16));
		if (n == words.size()) g_slots[slot - 1] = words;
	}
}

void store_locked() {
	std::ostringstream out;
	out << "# Custom Arenas (Power-Up Settings -> Save Settings), as the Xbox 360 version kept\n"
	       "# them: 7 words per slot (see runtime/src/profiles/custom_arenas.cpp). Delete a line\n"
	       "# to empty its slot.\n";
	for (int i = 0; i < SLOTS; i++) {
		if (!g_slots[i]) continue;
		out << "slot" << i + 1 << "=";
		for (size_t w = 0; w < g_slots[i]->size(); w++) out << (w ? "," : "") << std::format("{:08x}", (*g_slots[i])[w]);
		out << "\n";
	}
	std::error_code ec;
	std::filesystem::create_directories(file().parent_path(), ec);
	std::filesystem::path tmp = file();
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		f << out.str();
	}
	std::filesystem::rename(tmp, file(), ec);
	if (ec) LOG_WARN("custom arenas: could not write %s: %s", file().string().c_str(), ec.message().c_str());
}

void put_utf16(u32 ea, const std::string& s, size_t chars) {
	size_t n = std::min(s.size(), chars - 1);
	for (size_t i = 0; i < n; i++) st16(u32(ea + 2 * i), u16(u8(s[i])));
	for (size_t i = n; i < chars; i++) st16(u32(ea + 2 * i), 0);
}

// The entry for arena `id` among all arenas (vector at +20/+24), or 0.
u32 find_arena(u32 arenas, int32_t id) {
	u32 begin = ld32(arenas + 20), end = ld32(arenas + 24);
	for (u32 e = begin; e && e + ENTRY_SIZE <= end; e += ENTRY_SIZE)
		if (int32_t(ld32(e)) == id) return e;
	return 0;
}

u32 call(PPUContext& ctx, u32 fn, u32 arg) {
	ctx.r[3] = arg;
	ppu_icall(ctx, fn);
	return u32(ctx.r[3]);
}

} // namespace

namespace custom_arenas {

// What Save Settings would store: the match config as the 360's Save packed it.
// `powerups` is a match config holding newer power-up choices (the Power-Up Settings
// screen's own copy), or 0. Game thread only (calls the game's packers).
Slot snapshot(PPUContext& ctx, u32 powerups) {
	return {ld32(MATCH + 0x24), ld32(MATCH + 0x1c), ld32(MATCH + 0x14), ld32(MATCH),
	        call(ctx, PACK_RULES, MATCH), call(ctx, PACK_POWERUPS, powerups ? powerups : MATCH),
	        ld32(MATCH + 0x10)};
}

void save(int slot, const Slot& words) {
	std::lock_guard g(g_lock);
	load_locked();
	g_slots[slot] = words;
	store_locked();
	LOG_INFO("custom arenas: saved slot %d (arena %d)", slot + 1, int32_t(words[SLOT_ARENA]));
}

void erase(int slot) {
	std::lock_guard g(g_lock);
	load_locked();
	g_slots[slot].reset();
	store_locked();
	LOG_INFO("custom arenas: deleted slot %d", slot + 1);
}

void set_pending(const Slot& words) { g_pending = words; }
const Slot& pending() { return g_pending; }

// Arena id saved in slot 0-2, or -1 for an open slot.
int32_t arena(int slot) {
	std::lock_guard g(g_lock);
	load_locked();
	return g_slots[slot] ? int32_t((*g_slots[slot])[SLOT_ARENA]) : -1;
}

// The picture of arena `id` (UTF-16BE path in guest memory), or 0.
u32 thumbnail(int32_t id) {
	u32 arenas = ld32(ARENAS_PTR);
	u32 e = arenas ? find_arena(arenas, id) : 0;
	return e ? e + ENTRY_THUMB : 0;
}

} // namespace custom_arenas

// sub_0010A800(arenas, flag, mode) builds the Arena Select list (Random last); the
// saved custom arenas go after it, as on the 360. Each is a copy of the Random entry
// with its id, slot, names and the saved arena's picture.
void hook_arena_list(PPUContext& ctx, PPUFunc original) {
	u32 arenas = u32(ctx.r[3]);
	original(ctx);
	u64 r3 = ctx.r[3];
	u32 begin = ld32(arenas + 4), end = ld32(arenas + 8);
	if (!begin || end < begin + ENTRY_SIZE || int32_t(ld32(end - ENTRY_SIZE)) != RANDOM_ID) return;
	if (!g_entry_buf) g_entry_buf = vm::alloc(vm::MAIN_BASE, ENTRY_SIZE, 16);
	for (int slot = 0; slot < SLOTS; slot++) {
		int32_t saved = custom_arenas::arena(slot);
		if (saved < 0) continue;
		u32 random = ld32(arenas + 8) - ENTRY_SIZE;
		while (int32_t(ld32(random)) != RANDOM_ID) random -= ENTRY_SIZE;   // customs follow it
		for (u32 i = 0; i < ENTRY_SIZE; i += 4) st32(g_entry_buf + i, ld32(random + i));
		st32(g_entry_buf, u32(FIRST_ID + slot));
		st32(g_entry_buf + ENTRY_OPTIONS, 0);
		st32(g_entry_buf + ENTRY_SLOT, u32(slot));
		put_utf16(g_entry_buf + ENTRY_NAME, std::format("IDS_ARENA_CUSTOM_{}", slot + 1), ENTRY_TEXT_CHARS);
		put_utf16(g_entry_buf + ENTRY_DESC, "IDS_ARENA_CUSTOM_DESC", ENTRY_TEXT_CHARS);
		if (u32 shown = find_arena(arenas, saved))
			for (u32 i = 0; i < ENTRY_THUMB_CHARS * 2; i += 2)
				st16(g_entry_buf + ENTRY_THUMB + i, ld16(shown + ENTRY_THUMB + i));
		ctx.r[3] = arenas;
		ctx.r[4] = g_entry_buf;
		ppu_icall(ctx, VECTOR_PUSH);
	}
	// +48 keeps the game's count: the random arena pick (sub_00108DA8) chooses among the
	// first count - 1 entries (all but Random), which must not include the custom arenas.
	// The list itself takes its size from the vector.
	ctx.r[3] = r3;
}

// sub_000457A8(framework, user, slot, out): the player's saved slot, read by Arena
// Select when a custom arena is picked. Answered from the whole-game slots.
void hook_custom_slot(PPUContext& ctx, PPUFunc /*original*/) {
	int slot = int32_t(ctx.r[5]);
	u32 out = u32(ctx.r[6]);
	Slot words = OPEN_SLOT;
	if (slot >= 0 && slot < SLOTS) {
		std::lock_guard g(g_lock);
		load_locked();
		if (g_slots[slot]) words = *g_slots[slot];
	}
	for (size_t i = 0; i < words.size(); i++) st32(u32(out + 4 * i), words[i]);
	LOG_INFO("custom arenas: playing slot %d (arena %d)", slot + 1, int32_t(words[SLOT_ARENA]));
}

// sub_000E41A8(match, word) unpacks a slot's power-ups as `word & bit`; the 360's
// checkboxes took any non-zero value, the PS3's Power-Up Settings (which shows the
// match's switches for a custom arena) only ticks 1.
void hook_unpack_powerups(PPUContext& ctx, PPUFunc original) {
	u32 match = u32(ctx.r[3]);
	original(ctx);
	for (u32 off = 144; off < 164 + 25 * 4; off += 4)
		if (ld32(match + off)) st32(match + off, 1);
}

// The PS3 Power-Up Settings init hides (sub_0012D5C0) and disables (sub_0012DC58) the
// Save Settings button; both get `true` instead.
void hook_show_save_button(PPUContext& ctx, PPUFunc original) {
	ctx.r[4] = 1;
	original(ctx);
}

// Power-Up Settings -> Save Settings (Y): sub_0012FA50 has just written the screen's
// lists to the match config, and the call hooked here opens SaveMatchScreen.xui (the
// screen the PS3 port cut, put back by save_match_screen.cpp). What it will save is
// taken now, while the Power-Up screen's own copy of the switches is still there.
void hook_save_settings(PPUContext& ctx, PPUFunc original) {
	u32 screen = u32(ctx.r[3]);
	u32 own = ld8(screen + 552) ? screen + 276 : 0;   // power-up switches not yet in the match
	u64 r3 = ctx.r[3];
	custom_arenas::set_pending(custom_arenas::snapshot(ctx, own));
	ctx.r[3] = r3;
	original(ctx);
}
