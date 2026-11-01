// Trophies (see trophies.h).
#include "trophies/trophies.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <vector>

#include "profiles/profiles.h"
#include "runtime/kernel.h"
#include "ui/overlay.h"

namespace fs = std::filesystem;

namespace trophies {
namespace {

// Local play uses controller slots 0-3 (as profiles do); anything else counts as slot 0.
constexpr uint32_t SLOTS = 4;

std::mutex g_lock;
std::vector<Trophy> g_list;
std::map<int32_t, std::string> g_icons;       // id -> PNG bytes
using Unlocks = std::map<int32_t, int64_t>;   // id -> unix time (seconds, UTC)
std::map<std::string, Unlocks> g_loaded;      // by file stem

// --- TROPHY.TRP ----------------------------------------------------------------
// Header: magic 0xDCA24D00, version, u64 file size, u32 entry count, u32 entry size;
// entries from 0x40: name[32], u64 offset, u64 size. TROP.SFM is the default-language
// (English) list as XML.

u32 be32(const std::string& d, size_t at) {
	return u32(u8(d[at])) << 24 | u32(u8(d[at + 1])) << 16 | u32(u8(d[at + 2])) << 8 | u8(d[at + 3]);
}
u64 be64(const std::string& d, size_t at) { return u64(be32(d, at)) << 32 | be32(d, at + 4); }

std::string trp_file(const std::string& trp, const std::string& want) {
	if (trp.size() < 0x40 || be32(trp, 0) != 0xDCA24D00) return {};
	u32 count = be32(trp, 16), esize = be32(trp, 20);
	for (u32 i = 0; i < count; i++) {
		size_t e = 0x40 + size_t(i) * esize;
		if (e + 48 > trp.size()) break;
		std::string name(trp.c_str() + e, strnlen(trp.c_str() + e, 32));
		u64 off = be64(trp, e + 32), size = be64(trp, e + 40);
		if (name == want && off + size <= trp.size()) return trp.substr(size_t(off), size_t(size));
	}
	return {};
}

std::string xml_unescape(std::string s) {
	static const std::pair<const char*, const char*> ents[] = {
		{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
	for (auto [from, to] : ents)
		for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += std::strlen(to))
			s.replace(p, std::strlen(from), to);
	return s;
}

std::string xml_text(const std::string& block, const std::string& tag) {
	size_t a = block.find("<" + tag + ">"), b = block.find("</" + tag + ">");
	if (a == std::string::npos || b == std::string::npos || b < a) return {};
	a += tag.size() + 2;
	return xml_unescape(block.substr(a, b - a));
}

void load_list() {
	fs::path trp;
	std::error_code ec;
	for (const auto& dir : fs::directory_iterator(g_paths.game / "TROPDIR", ec))
		if (fs::exists(dir.path() / "TROPHY.TRP", ec)) trp = dir.path() / "TROPHY.TRP";
	if (trp.empty()) {
		LOG_WARN("trophies: no TROPDIR/*/TROPHY.TRP in the game files; trophies are disabled");
		return;
	}
	std::ifstream f(trp, std::ios::binary);
	std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	std::string xml = trp_file(data, "TROP.SFM");
	static const std::regex trophy_re(R"re(<trophy id="(\d+)" hidden="(\w+)" ttype="(\w)"[^>]*>([\s\S]*?)</trophy>)re");
	for (std::sregex_iterator it(xml.begin(), xml.end(), trophy_re), end; it != end; ++it) {
		const auto& m = *it;
		Grade grade = m[3] == "P" ? Grade::platinum : m[3] == "G" ? Grade::gold : m[3] == "S" ? Grade::silver : Grade::bronze;
		g_list.push_back({std::stoi(m[1]), grade, m[2] == "yes", xml_text(m[4], "name"), xml_text(m[4], "detail")});
		g_icons[g_list.back().id] = trp_file(data, std::format("TROP{:03}.PNG", g_list.back().id));
	}
	LOG_INFO("trophies: %zu in %s", g_list.size(), trp.string().c_str());
}

// --- per-person files ------------------------------------------------------------

const char* grade_name(Grade g) {
	switch (g) {
	case Grade::platinum: return "platinum";
	case Grade::gold: return "gold";
	case Grade::silver: return "silver";
	default: return "bronze";
	}
}

// File stem for whoever is on `slot`. Profile names are at most 16 characters, so
// they never collide with the guests' names.
std::string owner(uint32_t slot) {
	std::string name = profiles::assigned(slot);
	return name.empty() ? "Guest on controller " + std::to_string(slot + 1) : name;
}

fs::path file_for(const std::string& stem) { return g_paths.trophies / (stem + ".json"); }

std::string json_escape(const std::string& s) {
	std::string out;
	for (char c : s) {
		if (c == '"' || c == '\\') out += '\\';
		if (u8(c) < 0x20) continue;
		out += c;
	}
	return out;
}

std::string iso_time(int64_t t) {
	std::chrono::sys_seconds tp{std::chrono::seconds(t)};
	return std::format("{:%FT%TZ}", tp);
}

// Reads what save() writes: one trophy per line. Lines a person deletes by hand drop
// that trophy; anything unreadable is skipped.
Unlocks& unlocks(const std::string& stem) {
	auto [it, fresh] = g_loaded.try_emplace(stem);
	if (!fresh) return it->second;
	std::ifstream f(file_for(stem));
	static const std::regex line_re(
		R"re("id":\s*(\d+).*"unlocked":\s*"(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)Z")re");
	std::string line;
	std::smatch m;
	while (std::getline(f, line)) {
		if (!std::regex_search(line, m, line_re)) continue;
		using namespace std::chrono;
		auto day = sys_days{year{std::stoi(m[2])} / std::stoi(m[3]) / std::stoi(m[4])};
		auto t = day + hours{std::stoi(m[5])} + minutes{std::stoi(m[6])} + seconds{std::stoi(m[7])};
		it->second[std::stoi(m[1])] = duration_cast<seconds>(t.time_since_epoch()).count();
	}
	return it->second;
}

void save(const std::string& stem, const Unlocks& u) {
	fs::path path = file_for(stem), tmp = path;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::trunc);
		f << "{\n  \"player\": \"" << json_escape(stem) << "\",\n  \"trophies\": [";
		bool first = true;
		for (const auto& [id, when] : u) {
			const Trophy* t = find(id);
			f << (first ? "\n" : ",\n") << "    {\"id\": " << id << ", \"grade\": \""
			  << (t ? grade_name(t->grade) : "unknown") << "\", \"name\": \"" << json_escape(t ? t->name : "")
			  << "\", \"unlocked\": \"" << iso_time(when) << "\"}";
			first = false;
		}
		f << "\n  ]\n}\n";
		if (!f) {
			LOG_ERROR("trophies: cannot write %s", tmp.string().c_str());
			return;
		}
	}
	std::error_code ec;
	fs::rename(tmp, path, ec);
	if (ec) LOG_ERROR("trophies: cannot save %s: %s", path.string().c_str(), ec.message().c_str());
}

void announce(const std::string& who, const Trophy& t) {
	const char* grade = grade_name(t.grade);
	overlay::notify(std::format("{} unlocked a trophy: {} ({}{})", who, t.name, char(std::toupper(grade[0])), grade + 1));
}

// TROPHY_TEST=<ms>:<slot>:<id>: at <ms> after start, call the game's own
// AchievementTracker unlock (Instance(), then UnlockAchievement(userIndex, id + 1)),
// which spawns its unlock thread like a real achievement would.
constexpr u32 TRACKER_INSTANCE = 0x000F1068, TRACKER_UNLOCK = 0x000F0AB0;
bool g_test_pending = false;
u32 g_test_slot = 0;
s32 g_test_id = 0;
std::chrono::steady_clock::time_point g_test_at;

} // namespace

void init() {
	load_list();
	if (const char* t = std::getenv("TROPHY_TEST"); t && *t && std::string(t) != "0") {
		unsigned ms = 0, slot = 0;
		int id = 0;
		if (std::sscanf(t, "%u:%u:%d", &ms, &slot, &id) == 3) {
			g_test_pending = true;
			g_test_slot = slot;
			g_test_id = id;
			g_test_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
		} else {
			LOG_WARN("trophies: TROPHY_TEST wants <ms>:<slot>:<id>, got '%s'", t);
		}
	}
}

int32_t count() { return int32_t(g_list.size()); }

const std::vector<Trophy>& list() { return g_list; }

const std::string& icon_png(int32_t id) {
	static const std::string none;
	auto it = g_icons.find(id);
	return it == g_icons.end() ? none : it->second;
}

std::string person_on(uint32_t slot) { return owner(slot < SLOTS ? slot : 0); }

std::vector<std::string> people() {
	std::vector<std::string> out = profiles::list();
	std::error_code ec;
	std::vector<std::string> guests;
	for (const auto& e : fs::directory_iterator(g_paths.trophies, ec)) {
		std::string stem = e.path().stem().string();
		if (e.path().extension() == ".json" && stem.starts_with("Guest on controller ")) guests.push_back(stem);
	}
	std::sort(guests.begin(), guests.end());
	out.insert(out.end(), guests.begin(), guests.end());
	return out;
}

std::map<int32_t, int64_t> unlocks_of(const std::string& person) {
	std::lock_guard lock(g_lock);
	return unlocks(person);
}

const Trophy* find(int32_t id) {
	for (const Trophy& t : g_list)
		if (t.id == id) return &t;
	return nullptr;
}

Result unlock(uint32_t slot, int32_t id, int32_t* platinum) {
	if (platinum) *platinum = -1;
	const Trophy* t = find(id);
	if (!t || t->grade == Grade::platinum) return Result::invalid_id;
	if (slot >= SLOTS) slot = 0;
	std::string who = owner(slot);
	std::unique_lock lock(g_lock);
	Unlocks& u = unlocks(who);
	if (u.contains(id)) return Result::already_unlocked;
	int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	u[id] = now;
	const Trophy* plat = nullptr;
	for (const Trophy& p : g_list) {
		if (p.grade != Grade::platinum || u.contains(p.id)) continue;
		bool all = true;
		for (const Trophy& o : g_list) all &= o.grade == Grade::platinum || u.contains(o.id);
		if (all) {
			u[p.id] = now;
			plat = &p;
			if (platinum) *platinum = p.id;
		}
		break;
	}
	save(who, u);
	lock.unlock();
	LOG_INFO("trophies: %s unlocked %d '%s'", who.c_str(), id, t->name.c_str());
	announce(who, *t);
	if (plat) announce(who, *plat);
	return Result::unlocked;
}

uint64_t unlocked_tick(uint32_t slot, int32_t id) {
	if (slot >= SLOTS) slot = 0;
	std::lock_guard lock(g_lock);
	const Unlocks& u = unlocks(owner(slot));
	auto it = u.find(id);
	// CellRtcTick counts microseconds from 0001-01-01; the Unix epoch is 62135596800 s later.
	return it == u.end() ? 0 : u64(it->second + 62135596800ll) * 1000000;
}

void game_thread_tick() {
	if (!g_test_pending || std::chrono::steady_clock::now() < g_test_at) return;
	g_test_pending = false;
	LOG_INFO("trophies: TROPHY_TEST unlocking %d for controller %u through the game", g_test_id, g_test_slot + 1);
	PPUContext& ctx = current_ctx();
	PPUContext saved = ctx;
	ctx.r[1] -= 512;   // below the HLE caller's frame
	ctx.r[2] = g_image_toc;
	ppu_icall(ctx, TRACKER_INSTANCE);
	u64 tracker = ctx.r[3];
	ctx.r[3] = tracker;
	ctx.r[4] = g_test_slot;
	ctx.r[5] = u64(g_test_id + 1);   // the game numbers achievements from 1
	ctx.r[2] = g_image_toc;
	ppu_icall(ctx, TRACKER_UNLOCK);
	ctx = saved;
}

} // namespace trophies
