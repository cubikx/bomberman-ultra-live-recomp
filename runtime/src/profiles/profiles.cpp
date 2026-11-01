// Local player profiles (see profiles.h).
#include "profiles/profiles.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>

#include "hle/savedata.h"
#include "input/input.h"
#include "runtime/kernel.h"

namespace fs = std::filesystem;

namespace profiles {
namespace {

// --- Bomberman ULTRA specifics (from the disassembly) -------------------------
// Framework::SaveDataCallbackFile saves Framework+32, 1904 bytes, as SETTINGS.CFG in
// the auto-save directory; the block is one record per userIndex (= pad port), 272
// bytes each (SetDefaultProfileSettings, Get/SetCostume, the stats accessors).
constexpr const char* SAVE_DIR = "NPUB30051-BMANULTRA-AUTO-";
constexpr const char* SAVE_FILE = "SETTINGS.CFG";
constexpr u32 FRAMEWORK_PTR = 0x1d9b04;   // Framework* (null until the game creates it)
constexpr u32 RECORDS_OFFSET = 32;
constexpr size_t RECORD_SIZE = 272, RECORD_COUNT = 7;
// Local play uses the first four slots; the rest are left to the game.
constexpr uint32_t PROFILE_SLOTS = 4;
// A slot the game never uses in local play, so its record holds the defaults that
// a new profile starts from.
constexpr uint32_t TEMPLATE_SLOT = 6;

using Record = std::array<u8, RECORD_SIZE>;

std::mutex g_lock;
fs::path g_dir;            // user/profiles
fs::path g_memory_file;    // user/settings/controllers.ini
std::map<std::string, std::string> g_remembered;   // controller key -> profile
std::string g_slot_profile[PROFILE_SLOTS];         // what each slot uses now
std::string g_slot_key[PROFILE_SLOTS];             // controller on each slot
// What the game's memory holds for each slot (applied by the game thread)
std::string g_slot_in_memory[PROFILE_SLOTS];
bool g_memory_known = false;   // false until the game's save has been loaded
// The guests' own records, kept apart while a profile occupies their slot
Record g_guest[PROFILE_SLOTS];
bool g_have_guest[PROFILE_SLOTS] = {};
// Changes asked for during a match, applied when it is over (by the game thread).
// `connect`: from a controller connecting, which never takes a profile in use.
struct Deferred {
	std::string name;
	bool connect = false;
};
std::optional<Deferred> g_deferred[PROFILE_SLOTS];

fs::path record_path(const std::string& name) { return g_dir / name / "record.bin"; }

bool load_record(const std::string& name, Record& r) {
	std::ifstream f(record_path(name), std::ios::binary);
	return f && f.read(reinterpret_cast<char*>(r.data()), RECORD_SIZE) && f.gcount() == RECORD_SIZE;
}

void save_record(const std::string& name, const Record& r) {
	std::error_code ec;
	fs::create_directories(g_dir / name, ec);
	fs::path tmp = record_path(name);
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char*>(r.data()), RECORD_SIZE);
		if (!f) {
			LOG_ERROR("profiles: cannot write %s", tmp.string().c_str());
			return;
		}
	}
	fs::rename(tmp, record_path(name), ec);
	if (ec) LOG_ERROR("profiles: cannot save %s: %s", name.c_str(), ec.message().c_str());
}

void save_memory_file() {
	std::ofstream f(g_memory_file, std::ios::trunc);
	f << "# Which profile each controller used last (written by the game; edit while it is closed).\n"
	     "# serial:... = a controller by its serial number, port:N = whatever controller is on port N\n";
	for (const auto& [key, name] : g_remembered) f << key << " = " << name << '\n';
}

void load_memory_file() {
	std::ifstream f(g_memory_file);
	std::string line;
	while (std::getline(f, line)) {
		if (line.empty() || line[0] == '#') continue;
		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		auto trim = [](std::string s) {
			s.erase(0, s.find_first_not_of(" \t"));
			s.erase(s.find_last_not_of(" \t\r") + 1);
			return s;
		};
		std::string key = trim(line.substr(0, eq)), name = trim(line.substr(eq + 1));
		if (!key.empty() && valid_name(name)) g_remembered[key] = name;
	}
}

bool exists_locked(const std::string& name) {
	std::error_code ec;
	return fs::is_directory(g_dir / name, ec);
}

u32 records_ea() {
	u32 fw = ld32(FRAMEWORK_PTR);
	return fw ? fw + RECORDS_OFFSET : 0;
}

// Save hook: assemble the game's save from the profiles on its way in, and take
// the profiles' records back out on its way to disk.
void save_hook(const std::string& dir, const std::string& file, std::vector<u8>& data, bool write) {
	if (dir != SAVE_DIR || file != SAVE_FILE || data.size() != RECORD_SIZE * RECORD_COUNT) return;
	std::lock_guard lock(g_lock);
	for (uint32_t slot = 0; slot < PROFILE_SLOTS; slot++) {
		u8* rec = data.data() + slot * RECORD_SIZE;
		const std::string& name = write ? g_slot_in_memory[slot] : g_slot_profile[slot];
		if (write) {
			if (name.empty()) continue;
			Record r;
			std::copy_n(rec, RECORD_SIZE, r.begin());
			save_record(name, r);
			// the save file keeps the slot's guest record
			if (g_have_guest[slot]) std::copy(g_guest[slot].begin(), g_guest[slot].end(), rec);
		} else {
			std::copy_n(rec, RECORD_SIZE, g_guest[slot].begin());
			g_have_guest[slot] = true;
			Record r;
			if (!name.empty() && load_record(name, r)) std::copy(r.begin(), r.end(), rec);
		}
	}
	if (!write) {
		// the game's memory will hold exactly this once its load completes
		for (uint32_t slot = 0; slot < PROFILE_SLOTS; slot++) g_slot_in_memory[slot] = g_slot_profile[slot];
		g_memory_known = true;
		LOG_INFO("profiles: save loaded with %s / %s / %s / %s",
		         g_slot_profile[0].empty() ? "guest" : g_slot_profile[0].c_str(),
		         g_slot_profile[1].empty() ? "guest" : g_slot_profile[1].c_str(),
		         g_slot_profile[2].empty() ? "guest" : g_slot_profile[2].c_str(),
		         g_slot_profile[3].empty() ? "guest" : g_slot_profile[3].c_str());
	}
}

// After the game's first load (which finds no save on a fresh install), its memory
// holds the guests' records wherever the load did not put a profile's.
void load_done(const std::string& dir, bool save, bool ok) {
	if (dir != SAVE_DIR || save) return;
	std::lock_guard lock(g_lock);
	if (!ok)
		for (auto& s : g_slot_in_memory) s.clear();
	g_memory_known = true;
}

} // namespace

bool valid_name(const std::string& name) {
	if (name.empty() || name.size() > MAX_NAME) return false;
	if (name.front() == ' ' || name.back() == ' ' || name.front() == '.') return false;
	return std::all_of(name.begin(), name.end(), [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
		       c == '_';
	});
}

void init() {
	g_dir = g_paths.user / "profiles";
	g_memory_file = g_paths.settings / "controllers.ini";
	std::error_code ec;
	fs::create_directories(g_dir, ec);
	{
		std::lock_guard lock(g_lock);
		load_memory_file();
	}
	savedata::set_hook(save_hook);
	savedata::set_done_hook(load_done);
	LOG_INFO("profiles: %zu profiles, %zu remembered controllers", list().size(), g_remembered.size());
}

std::vector<std::string> list() {
	std::vector<std::string> names;
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(g_dir, ec)) {
		std::string n = e.path().filename().string();
		if (e.is_directory() && valid_name(n)) names.push_back(n);
	}
	std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
		return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
		                                    [](char x, char y) { return std::tolower(x) < std::tolower(y); });
	});
	return names;
}

bool create(const std::string& name, std::string* error) {
	auto fail = [&](const char* why) {
		if (error) *error = why;
		return false;
	};
	if (!valid_name(name)) return fail("Use 1-16 letters, digits, spaces, - or _");
	std::lock_guard lock(g_lock);
	for (const auto& other : list())
		if (std::equal(other.begin(), other.end(), name.begin(), name.end(),
		               [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
			return fail("That name is taken");
	std::error_code ec;
	fs::create_directories(g_dir / name, ec);
	if (ec) return fail("Cannot create the profile folder");
	LOG_INFO("profiles: created '%s'", name.c_str());
	return true;
}

namespace {
void assign_locked(uint32_t port, const std::string& name) {
	if (!name.empty())
		for (uint32_t other = 0; other < PROFILE_SLOTS; other++)
			if (other != port && g_slot_profile[other] == name) g_slot_profile[other].clear();
	g_slot_profile[port] = name;
	if (!g_slot_key[port].empty()) {
		if (name.empty()) g_remembered.erase(g_slot_key[port]);
		else g_remembered[g_slot_key[port]] = name;
		save_memory_file();
	}
	LOG_INFO("profiles: controller %u -> %s", port + 1, name.empty() ? "guest" : name.c_str());
}
} // namespace

void assign(uint32_t port, const std::string& name) {
	if (port >= PROFILE_SLOTS) return;
	std::lock_guard lock(g_lock);
	if (in_match()) {
		g_deferred[port] = Deferred{name};
		LOG_INFO("profiles: controller %u -> %s after the match", port + 1, name.empty() ? "guest" : name.c_str());
		return;
	}
	g_deferred[port].reset();
	assign_locked(port, name);
}

std::string assigned(uint32_t port) {
	std::lock_guard lock(g_lock);
	return port < PROFILE_SLOTS ? g_slot_profile[port] : std::string();
}

namespace {
int holder_locked(const std::string& name, uint32_t except) {
	if (name.empty()) return -1;
	for (uint32_t p = 0; p < PROFILE_SLOTS; p++)
		if (p != except && g_slot_profile[p] == name && !g_slot_key[p].empty()) return int(p);
	return -1;
}
} // namespace

int holder(const std::string& name, uint32_t except) {
	std::lock_guard lock(g_lock);
	return holder_locked(name, except);
}

void controller_connected(uint32_t port, const std::string& key) {
	if (port >= PROFILE_SLOTS) return;
	LOG_INFO("profiles: controller %u is %s", port + 1, key.c_str());
	std::string name;
	{
		std::lock_guard lock(g_lock);
		g_slot_key[port] = key;
		auto it = g_remembered.find(key);
		if (it != g_remembered.end() && exists_locked(it->second)) name = it->second;
		if (in_match()) {   // the slot keeps its player until the match is over
			if (name != g_slot_profile[port]) g_deferred[port] = Deferred{name, true};
			return;
		}
		// never take a profile from someone playing with it now: start as the guest,
		// but keep remembering what this controller normally uses
		if (int other = holder_locked(name, port); other >= 0) {
			LOG_INFO("profiles: %s is in use on controller %d, controller %u starts as guest", name.c_str(), other + 1,
			         port + 1);
			g_slot_profile[port].clear();
			return;
		}
	}
	assign(port, name);
}

void controller_disconnected(uint32_t port) {
	if (port >= PROFILE_SLOTS) return;
	std::lock_guard lock(g_lock);
	g_slot_key[port].clear();
	// keep the profile on the slot: a controller that drops out mid-match and
	// reconnects should not lose its player
}

void game_thread_tick() {
	std::lock_guard lock(g_lock);
	if (!in_match())
		for (uint32_t port = 0; port < PROFILE_SLOTS; port++) {
			if (!g_deferred[port]) continue;
			Deferred d = *g_deferred[port];
			g_deferred[port].reset();
			// as controller_connected: never take a profile from someone playing with it
			if (d.connect && holder_locked(d.name, port) >= 0) d.name.clear();
			assign_locked(port, d.name);
		}
	if (!g_memory_known) return;
	u32 base = records_ea();
	if (!base) return;
	// Two passes, so a profile moving between slots (taken over) is saved from its old
	// slot before its new slot loads it: all departing records out, then arrivals in.
	bool changed[PROFILE_SLOTS] = {};
	for (uint32_t slot = 0; slot < PROFILE_SLOTS; slot++) {
		const std::string& have = g_slot_in_memory[slot];
		if (g_slot_profile[slot] == have) continue;
		changed[slot] = true;
		// out: the departing player's record goes home
		Record out;
		std::copy_n(vm_ptr(base + slot * u32(RECORD_SIZE)), RECORD_SIZE, out.begin());
		if (have.empty()) {
			g_guest[slot] = out;
			g_have_guest[slot] = true;
		} else {
			save_record(have, out);
		}
	}
	for (uint32_t slot = 0; slot < PROFILE_SLOTS; slot++) {
		if (!changed[slot]) continue;
		const std::string& want = g_slot_profile[slot];
		u8* mem = vm_ptr(base + slot * u32(RECORD_SIZE));
		// in: the arriving player's record, or defaults for a new profile
		Record in;
		if (want.empty()) {
			if (g_have_guest[slot]) in = g_guest[slot];
			else std::copy_n(vm_ptr(base + TEMPLATE_SLOT * u32(RECORD_SIZE)), RECORD_SIZE, in.begin());
		} else if (!load_record(want, in)) {
			std::copy_n(vm_ptr(base + TEMPLATE_SLOT * u32(RECORD_SIZE)), RECORD_SIZE, in.begin());
			save_record(want, in);
		}
		std::copy(in.begin(), in.end(), mem);
		vm::note_written(base + slot * u32(RECORD_SIZE), u32(RECORD_SIZE));
		LOG_INFO("profiles: slot %u now holds %s", slot + 1, want.empty() ? "the guest" : want.c_str());
		g_slot_in_memory[slot] = want;
	}
}

} // namespace profiles
