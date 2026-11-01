// CPU players with a plan (settings.ini cpu_ai = smart).
//
// The game's own CPU player (vtable 0x1B0220, sub_00058A20) wanders and drops bombs with
// little purpose. With cpu_ai = smart, match setup makes each CPU slot's player a
// human-class player (sub_000699F8) on a free pad port instead, and a bot here drives that
// pad: it moves and bombs exactly as a person would, with the same rules and power-ups.
// The slot stays a CPU slot, so names, results and trophies are as before.
//
// Each frame the bot reads the match from the game object (*0x1DFF70):
//   +16  arena grid, 19 x 13 cells of 8 bytes; the word at +4: bits 24-27 tile (0 / 4
//        open, 3 soft block, other solid), 0x00C00000 bomb, 0x003C0000 flame (one bit per
//        arm), 0x1000 item on the floor
//   +232 tick counter (one per frame while the match runs)
//   lists (first node at +8 of the head, next node at node +8):
//   +464 players alive: +20 / +28 x / z (tile = (v + 0x02000000) >> 26), +60 index;
//        stats (human class): +68 speed, +70 bomb kind (1 power, 2 pass-through,
//        3 dangerous, 4 remote, 13 land mine), +71 line bomb, +72 glove, +73 kick,
//        +74 punch, +75 bombs, +76 fire
//   +476 bombs: +20 / +28 position, +59 blast range, +60 owner << 4, +69 state (1 moving?);
//        a bomb goes off ~155 ticks after it appears; a remote bomb (kind 4) when its
//        owner presses Circle (the player's +104 holds it); the dangerous bomb (kind 3) is a
//        5 x 5 square
//   +500 items: +12 / +20 position, +24 type (pickup handler sub_00068A68): 1 power bomb,
//        2 pass-through, 3 remote, 4 dangerous, 5 kick, 6 punch, 7 glove, 8 bomb-up,
//        9 fire-up, 10 speed-up, 11 full fire, 12 line bomb, 13 land mine, 14 bomb-down,
//        15 fire-down, 16 speed-down, 17 wardrobe, 18-23 skulls, 24 a planted land mine
//
// It then plans on a danger map (when each tile will burn, chain reactions included):
// get out of any blast on a path that never crosses fire, collect power-ups, bomb blocks
// and players from spots it can escape from, otherwise close in on the nearest player.
// The slot's difficulty (Easy / Normal / Hard) sets how far it sees (the original CPU's
// 6 / 11 / 17 tiles), how quickly it reacts and how much it goes after players, and the
// mistakes a person makes (perceive()): it takes in another player's bomb only after a
// reaction time, misjudges fuses a little, sometimes misses a chain reaction, picks where
// to run loosely and now and then pauses. BOT_FLAWLESS=1 turns the mistakes off.
//
// Debugging: BOT_DUMP=<frame>[,step,count] writes bot_<frame>.txt with the grid and the
// lists; BOT_SHOT=<tick>,<count>,<step> saves frames from a match tick on; BOT_ALL_POWERUPS=1 turns every power-up and power-down on outside matches (match
// config +164, one word per item type); BOT_ARENA=n picks the arena; BOT_LOG=1 logs each bot's decisions;
// BOT_GIVE=<type>[,type...] hands every bot those power-ups at the start (item types below).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "config/config.h"
#include "hle/bot.h"
#include "ppu/recomp.h"
#include "profiles/profiles.h"
#include "rsx/rsx.h"
#include "runtime/kernel.h"

namespace bot {
namespace {

constexpr u32 GAME_PTR = 0x1DFF70;
constexpr u32 MATCH_CONFIG = 0x28AB60;
constexpr u32 SLOT_MANAGER = 0x28AA48;       // *: 8 player slots of 124 bytes
constexpr u32 HUMAN_PLAYER_CTOR = 0x699F8;   // sub_000699F8(obj, player, pad port, button layout)
constexpr u32 HUMAN_VTABLE = 0x1B0400, CPU_VTABLE = 0x1B0220;
constexpr u32 IS_CPU = 36;   // vtable slot: true for the CPU class (quick finish, match flow)
// bomb classes (vtables) in the bomb list
constexpr u32 BOMB_VTABLE = 0x1B02C8, POWER_VTABLE = 0x1B0620, PIERCE_VTABLE = 0x1B0578, REMOTE_VTABLE = 0x1B06B0,
              SQUARE_VTABLE = 0x1B0278, MINE_VTABLE = 0x1B0478;

constexpr int W = 19, H = 13, N = W * H;
constexpr int FUSE = 154;       // ticks from a bomb appearing to its blast (a normal one)
constexpr int REMOTE_DELAY = 34;   // from setting a remote bomb off (Circle) to its blast
constexpr int FLAME = 32;       // ticks a blast stays dangerous (seen ~20-25)
constexpr int NEVER = 1 << 28;

enum Tile : u8 { OPEN, SOFT, SOLID };

struct Window {
	int from, to;      // ticks from now
	bool soft = false;   // a remote bomb or land mine: may go off any time, never by itself
	int owner = -1;      // whose remote bomb (its owner decides when; -1 for a mine)
};

struct BombInfo {
	int x, z, range, owner;
	int fuse;   // ticks until it goes off by itself (NEVER for a remote bomb)
	int at;     // after chain reactions
	bool pierce, square;
	u32 obj = 0;
	int first = 0;              // tick it appeared
	bool chain_blind = false;   // the bot planning does not expect other blasts to set it off
};

struct Player {
	u32 obj;
	int index;
	float x, z;   // in tiles
	int tx, tz;
};

// One frame's view of the match.
struct World {
	u32 game = 0;
	int tick = 0;
	std::array<Tile, N> tile{};
	std::array<bool, N> bomb{}, flame{}, mine{};
	std::array<int, N> item_type{};
	std::array<u32, N> raw{};   // the cell words, for the log
	std::vector<BombInfo> bombs;
	std::vector<Player> players;
	std::array<std::vector<Window>, N> danger;
};

int idx(int x, int z) { return z * W + x; }
int tile_of(u32 v) { return int((v + 0x02000000u) >> 26); }

struct Bot {
	bool active = false;
	u32 game = 0;   // the game object it was made in; another one ends it
	u32 obj = 0;    // its player object (human class)
	u32 port = 0;
	u32 player = 0;
	u32 difficulty = 0;
	input::PadState pad;
	u32 rng = 1;

	// difficulty
	int awareness = 11;   // tiles within which it notices items, players and blocks
	int reaction = 8;     // ticks between decisions
	float aggression = 0.6f;
	float noise = 0.2f;   // randomness in its choices
	// a person's mistakes: what it sees of the bombs is late and approximate
	int margin = 7;           // ticks it wants off a tile before it burns
	int notice_min = 14, notice_max = 22;   // ticks before it takes in a new bomb
	int distracted = 25;      // up to this much later for one away from where it is heading
	float fuse_error = 12;    // spread of its guess of when another player's bomb goes
	float chain_miss = 0.2f;  // chance it does not expect a blast to set a bomb off
	float escape_jitter = 2;  // how loosely it picks where to run to
	float careless = 0;       // chance to bomb without checking for a way out
	float hesitate = 0;       // chance to pause after deciding (not while escaping)
	int pause_max = 20;       // longest such pause, ticks
	float item_greed = 1;     // how much power-ups matter next to bombing blocks and players
	float item_falloff = 0.2f;   // how quickly a power-up's appeal drops per tile away
	int aim_lag = 0;          // ticks behind where players really are when it aims bombs at them
	float naive_escape = 0;   // chance to run only from the bomb that scared it (see plan())
	float panic = 0;          // chance to freeze for a moment when a blast threatens it
	int freeze_max = 0;       // longest such freeze, ticks
	int naive_until = 0, panic_until = 0;
	int threats = 0;          // bombs whose reach it was in at its last decision
	struct {
		int escapes = 0, wrong_way = 0, froze = 0;
	} stats;   // this round, for the console
	int max_bombs = 99;       // bombs it keeps down at once, whatever it could
	bool walks_over_bad = false;   // does not steer round skulls and power-downs
	u32 dice = 1;             // for the above
	struct Noticed {
		u32 obj;
		int at;      // tick from which it knows of the bomb
		int error;   // ticks its guess of the fuse is off by
		bool chain_blind;
	};
	std::vector<Noticed> noticed;
	int idle_until = 0;

	// plan
	std::vector<int> path;   // tiles to walk, next first
	int goal = -1;
	bool bomb_at_goal = false;
	int next_think = 0;
	int bomb_hold = 0;   // frames the bomb button is still held
	int detonate_hold = 0;
	float speed = 1.0f / 16;   // tiles per tick, measured
	float last_x = -1, last_z = -1;
	int last_tick = 0;
	int last_bomb_tick = -1000;
	int overlap = -1;          // the other tile its body still covers, or -1
	bool reversed = false;     // a skull curse swapped its directions
	int reversed_votes = 0;
	u16 last_dir = 0;
	bool burnt = false;
	bool escaping = false;
	int stuck_ticks = 0;     // pressing a direction without moving
	int blocked = -1, blocked_until = 0;
	int flips = 0;           // presses opposite to the last one within 10 ticks (log)
	int remote_flips = 0;    // of those, while in another player's remote bomb reach (log)
	u16 prev_press = 0;
	int prev_press_tick = 0;
	std::string last_plan;   // for the log when it is knocked out
	std::string last_danger;
};

Bot g_bots[8];   // by player index
int g_last_tick = -1;
const bool g_log = [] {
	const char* e = std::getenv("BOT_LOG");
	return e && *e == '1';
}();

std::vector<u64> g_knockout_frames;   // BOT_DEATHCAM

// debugging: BOT_HOLD_REMOTES=<player> lays remote bombs and never sets them off, as a person
// holding them might
int g_remote_flips = 0;   // all bots, this run (log)

// each player's tile over the last ticks: bots aim at where they saw a player, a moment ago
constexpr int TRAIL = 64;
struct Trail {
	int tick = -1, tile = -1;
};
Trail g_trail[8][TRAIL];

int was_at(const Player& p, int tick) {
	const Trail& t = g_trail[p.index & 7][((tick % TRAIL) + TRAIL) % TRAIL];
	return t.tick == tick ? t.tile : idx(p.tx, p.tz);
}
const int g_hold_remotes = [] {
	const char* e = std::getenv("BOT_HOLD_REMOTES");
	return e && *e ? std::atoi(e) : -1;
}();

// bomb objects seen, for their age
struct Seen {
	u32 obj;
	int first;
	int curse;   // the owner's skull curse when it was placed (for the log)
	u32 cls;
	int fired = -1;   // a remote bomb: when its owner set it off (+69 left 2, "waiting")
};
std::vector<Seen> g_seen;
int g_flame_log = 0;   // log the flames this many reads after a bomb goes

// --- reading the match -------------------------------------------------------------

bool read_world(World& w) {
	w.game = ld32(GAME_PTR);
	if (!w.game) return false;
	u32 grid = ld32(w.game + 16);
	if (!grid) return false;
	w.tick = int(ld32(w.game + 232));
	for (int i = 0; i < N; i++) {
		u32 c = ld32(grid + u32(i) * 8 + 4);
		w.raw[i] = c;
		u32 t = (c >> 24) & 0xF;
		w.tile[i] = (t == 0 || t == 4) ? OPEN : t == 3 ? SOFT : SOLID;
		w.bomb[i] = (c >> 22) & 3;
		w.flame[i] = c & 0x3C0000;
		w.item_type[i] = 0;
		w.mine[i] = false;
		w.danger[i].clear();
	}
	for (u32 o = ld32(w.game + 508), n = 0; o && n < 200; o = ld32(o + 8), n++) {
		int x = tile_of(ld32(o + 12)), z = tile_of(ld32(o + 20)), type = ld8(o + 24);
		if (x < 0 || x >= W || z < 0 || z >= H) continue;
		if (type == 24) w.mine[idx(x, z)] = true;
		else w.item_type[idx(x, z)] = type;
	}
	w.players.clear();
	for (u32 o = ld32(w.game + 472), n = 0; o && n < 16; o = ld32(o + 8), n++) {
		Player p;
		p.obj = o;
		p.index = ld8(o + 60);
		p.x = float(int32_t(ld32(o + 20))) / float(1 << 26);
		p.z = float(int32_t(ld32(o + 28))) / float(1 << 26);
		p.tx = std::clamp(int(std::lround(p.x)), 0, W - 1);
		p.tz = std::clamp(int(std::lround(p.z)), 0, H - 1);
		w.players.push_back(p);
	}
	// bombs, aged from when each object was first seen
	std::vector<Seen> seen;
	w.bombs.clear();
	for (u32 o = ld32(w.game + 484), n = 0; o && n < 64; o = ld32(o + 8), n++) {
		BombInfo b;
		b.x = tile_of(ld32(o + 20));
		b.z = tile_of(ld32(o + 28));
		if (b.x < 0 || b.x >= W || b.z < 0 || b.z >= H) continue;
		b.range = std::max(1, int(ld8(o + 59)));
		b.owner = ld8(o + 60) >> 4;
		int first = w.tick;
		for (const Seen& s : g_seen)
			if (s.obj == o) first = s.first;   // a kicked bomb keeps its age
		int curse = 0, fired = -1;
		for (const Seen& s : g_seen)
			if (s.obj == o) curse = s.curse, fired = s.fired;
		if (first == w.tick)
			for (u32 p = ld32(w.game + 472), k = 0; p && k < 16; p = ld32(p + 8), k++)
				if (ld8(p + 60) == ld8(o + 60) >> 4) curse = ld8(p + 368);
		if (ld32(o) == REMOTE_VTABLE && ld8(o + 69) != 2 && fired < 0 && first < w.tick) fired = w.tick;
		seen.push_back({o, first, curse, ld32(o), fired});
		u8 kind = 0;
		for (const Player& p : w.players)
			if (p.index == b.owner) kind = ld8(p.obj + 70);
		u32 cls = ld32(o);
		static u32 classes[16];
		for (u32 i = 0; i < 16; i++) {
			if (classes[i] == cls) break;
			if (!classes[i]) {
				classes[i] = cls;
				LOG_INFO("bot: bomb class 0x%08x (owner's bomb kind %u, +69 %u)", cls, kind, ld8(o + 69));
				break;
			}
		}
		// a planted land mine goes off when stepped on: dangerous at any moment, like a
		// remote bomb
		bool mine = cls == MINE_VTABLE;
		if (mine) w.mine[idx(b.x, b.z)] = true;
		bool remote = cls == REMOTE_VTABLE;
		// fuses measured per class (ticks from appearing); skull curse 6 shortens them
		int fuse = cls == POWER_VTABLE ? 168 : FUSE;
		if (curse == 6) fuse = 86;
		b.fuse = remote || mine ? NEVER : std::max(0, fuse - (w.tick - first));
		if (remote && fired >= 0) b.fuse = std::max(0, REMOTE_DELAY - (w.tick - fired));
		b.at = b.fuse;
		b.pierce = cls == PIERCE_VTABLE;
		b.square = cls == SQUARE_VTABLE || mine;   // a mine was seen to reach diagonally
		if (cls == SQUARE_VTABLE) b.range = 2;     // the dangerous bomb: 5 x 5, whatever its fire
		if (mine) b.range = std::max(b.range, 1) + 1;
		b.obj = o;
		b.first = first;
		w.bombs.push_back(b);
		w.bomb[idx(b.x, b.z)] = true;
	}
	if (g_log)
		for (const Seen& old : g_seen)
			if (std::none_of(seen.begin(), seen.end(), [&](const Seen& n) { return n.obj == old.obj; }))
				LOG_INFO("bot: bomb 0x%08x (class 0x%08x, owner curse %d) lasted %d ticks", old.obj, old.cls, old.curse,
				         w.tick - old.first);
	g_flame_log = g_log && seen.size() < g_seen.size() ? 14 : g_flame_log;
	g_seen = std::move(seen);
	if (g_flame_log && (--g_flame_log == 12 || !g_flame_log)) {
		std::string fl;
		for (int i = 0; i < N; i++)
			if (w.flame[i]) fl += std::format(" ({},{})", i % W, i / W);
		LOG_INFO("bot: flames:%s", fl.c_str());
	}
	return true;
}

// Tiles a bomb's blast reaches. `gone` marks soft blocks another blast clears first.
void blast(const World& w, const BombInfo& b, std::vector<int>& out, const std::array<bool, N>* gone = nullptr) {
	out.clear();
	out.push_back(idx(b.x, b.z));
	if (b.square) {
		for (int dz = -b.range; dz <= b.range; dz++)
			for (int dx = -b.range; dx <= b.range; dx++) {
				int x = b.x + dx, z = b.z + dz;
				if ((dx || dz) && x > 0 && x < W - 1 && z > 0 && z < H - 1) out.push_back(idx(x, z));
			}
		return;
	}
	static const int DX[4] = {1, -1, 0, 0}, DZ[4] = {0, 0, 1, -1};
	for (int d = 0; d < 4; d++)
		for (int r = 1; r <= b.range; r++) {
			int x = b.x + DX[d] * r, z = b.z + DZ[d] * r;
			if (x < 0 || x >= W || z < 0 || z >= H) break;
			int i = idx(x, z);
			if (w.tile[i] == SOLID) break;
			out.push_back(i);
			if (w.tile[i] == SOFT && !b.pierce && !(gone && (*gone)[i])) break;
			// a bomb in the way goes off too (chain); the flame was seen to carry on past it
		}
}

// When every bomb goes off (chains included) and when each tile burns.
void build_danger(World& w) {
	std::vector<std::vector<int>> reach(w.bombs.size());
	// soft blocks some blast destroys no longer stop the others (for safety, whichever
	// goes first)
	std::array<bool, N> gone{};
	for (size_t i = 0; i < w.bombs.size(); i++) {
		blast(w, w.bombs[i], reach[i]);
		for (int t : reach[i])
			if (w.tile[t] == SOFT) gone[t] = true;
	}
	for (size_t i = 0; i < w.bombs.size(); i++) blast(w, w.bombs[i], reach[i], &gone);
	for (bool changed = true; changed;) {
		changed = false;
		for (size_t i = 0; i < w.bombs.size(); i++) {
			if (w.bombs[i].at >= NEVER) continue;
			for (size_t j = 0; j < w.bombs.size(); j++)
				if (j != i && w.bombs[j].at > w.bombs[i].at && !w.bombs[j].chain_blind &&
				    std::find(reach[i].begin(), reach[i].end(), idx(w.bombs[j].x, w.bombs[j].z)) != reach[i].end()) {
					w.bombs[j].at = w.bombs[i].at;
					changed = true;
				}
		}
	}
	for (size_t i = 0; i < w.bombs.size(); i++) {
		const BombInfo& b = w.bombs[i];
		// a land mine goes off when stepped on (never by a bot: its tile is out of bounds) or
		// when a blast reaches it, which the chains above give a time
		if (b.at >= NEVER && w.mine[idx(b.x, b.z)]) continue;
		// a remote bomb can go off at any moment
		Window win = b.at >= NEVER ? Window{0, NEVER, true, w.mine[idx(b.x, b.z)] ? -1 : b.owner}
		                           : Window{b.at, b.at + FLAME};
		for (int t : reach[i]) w.danger[t].push_back(win);
	}
	for (int i = 0; i < N; i++)
		if (w.flame[i]) w.danger[i].push_back({0, FLAME / 2});
}

// --- planning ----------------------------------------------------------------------

int g_blocked = -1;   // a tile the planning bot found it could not walk into

bool walkable(const World& w, int i) { return w.tile[i] == OPEN && !w.bomb[i] && !w.mine[i] && i != g_blocked; }

bool bad_item(int type) { return (type >= 14 && type <= 16) || (type >= 18 && type <= 23); }

int g_me = -1;   // the bot planning now: its own remote bombs go off when it says so

bool own_remote(const Window& win) { return win.soft && win.owner == g_me; }

// Free of fire from `from` to `to` ticks from now, with a margin before each blast.
// Passing through a remote bomb's or mine's reach is allowed; stopping there is not.
bool clear(const std::vector<Window>& d, float from, float to, int margin) {
	for (const Window& win : d)
		if (!win.soft && from < win.to + 4 && to > win.from - margin) return false;
	return true;
}

// Safe to stay on from `from` ticks on.
bool safe_from(const std::vector<Window>& d, float from) {
	for (const Window& win : d)
		if (win.to > from) return false;   // own remote bombs too: it stays out of their way
	return true;
}

// Fire before it could step off the tile (with `soft_too`, also another player's remote bomb
// that may go off).
bool imminent(const std::vector<Window>& d, float leave, bool soft_too) {
	for (const Window& win : d)
		if (!own_remote(win) && (win.soft ? soft_too : float(win.from) < leave) && win.to > 0) return true;
	return false;
}

bool no_danger(const std::vector<Window>& d) { return d.empty(); }

struct Search {
	std::array<int, N> dist, parent;
};

// Breadth-first search from `start` over tiles the bot can walk through without being
// caught by fire, reaching step k at k / speed ticks. `extra` adds a blast (a bomb the
// bot considers placing) over `extra_tiles`, `extra` ticks from now.
void search(const World& w, int start, float speed, int margin, Search& s, const std::vector<int>* extra_tiles = nullptr,
            Window extra = {}, bool avoid_bad = true) {
	s.dist.fill(-1);
	s.parent.fill(-1);
	s.dist[start] = 0;
	int queue[N], head = 0, tail = 0;
	queue[tail++] = start;
	float step = 1.0f / speed;
	static const int DX[4] = {1, -1, 0, 0}, DZ[4] = {0, 0, 1, -1};
	while (head < tail) {
		int c = queue[head++], cx = c % W, cz = c / W;
		for (int d = 0; d < 4; d++) {
			int x = cx + DX[d], z = cz + DZ[d];
			if (x < 0 || x >= W || z < 0 || z >= H) continue;
			int n = idx(x, z);
			if (s.dist[n] >= 0 || !walkable(w, n) || (avoid_bad && bad_item(w.item_type[n]))) continue;
			// on a tile from starting to enter it until fully off it
			float arrive = float(s.dist[c] + 1) * step;
			if (!clear(w.danger[n], arrive - step, arrive + step, margin)) continue;
			if (extra_tiles && std::find(extra_tiles->begin(), extra_tiles->end(), n) != extra_tiles->end() &&
			    arrive + step > float(extra.from - margin) && arrive - step < float(extra.to + 4))
				continue;
			s.dist[n] = s.dist[c] + 1;
			s.parent[n] = c;
			queue[tail++] = n;
		}
	}
}

std::vector<int> path_to(const Search& s, int goal) {
	std::vector<int> p;
	for (int t = goal; t >= 0 && s.parent[t] >= 0; t = s.parent[t]) p.push_back(t);
	std::reverse(p.begin(), p.end());
	return p;
}

// 0..1 for a tile, the same for ~3 s, so a choice does not flip with every re-plan
float jitter(const Bot& b, int tile, int tick) {
	u32 h = b.rng ^ (u32(tile) * 0x9E3779B1u) ^ (u32(tick / 180) * 0x85EBCA6Bu);
	h ^= h >> 15, h *= 0x2C1B3C6Du, h ^= h >> 12, h *= 0x297A2D39u, h ^= h >> 15;
	return float(h >> 8) / float(1 << 24);
}

// 0..1, a new number each call
float roll(Bot& b) {
	b.dice ^= b.dice << 13, b.dice ^= b.dice >> 17, b.dice ^= b.dice << 5;
	return float(b.dice >> 8) / float(1 << 24);
}

float item_value(const Bot& b, int type) {
	u32 o = b.obj;
	switch (type) {
	case 8: return ld8(o + 75) < 8 ? 30.0f : 3.0f;    // bomb-up
	case 9: return ld8(o + 76) < 8 ? 30.0f : 3.0f;    // fire-up
	case 11: return ld8(o + 76) < 8 ? 34.0f : 3.0f;   // full fire
	case 10: return ld8(o + 68) < 6 ? 26.0f : 3.0f;   // speed-up
	case 5: return ld8(o + 73) ? 3.0f : 26.0f;        // kick
	case 1: case 2: case 4: return 20.0f;             // power / pass-through / dangerous bomb
	case 3: return 16.0f;                             // remote
	case 6: case 7: case 12: return 14.0f;            // punch / glove / line bomb
	case 13: return 8.0f;                             // land mine
	case 17: return 20.0f;                            // wardrobe (a costume part)
	default: return 0;
	}
}

int own_bombs(const World& w, const Bot& b) {
	int n = 0;
	for (const BombInfo& m : w.bombs)
		if (m.owner == int(b.player)) n++;
	return n;
}

// Decide where to go and whether to bomb there.
void plan(Bot& b, const World& w, const Player& me) {
	int here = idx(me.tx, me.tz);
	// stopped by the tile it is mostly on (its own bomb, once it has stepped off it): it
	// goes on from the other tile it covers
	if (g_blocked == here && b.overlap >= 0) here = b.overlap;
	float measured = b.speed;
	b.speed *= 0.85f;   // plan a little slower than it walks
	struct Restore {
		Bot& b;
		float v;
		~Restore() { b.speed = v; }
	} restore{b, measured};
	int margin = b.margin;
	Search s;
	search(w, here, b.speed, margin, s, nullptr, {}, !b.walks_over_bad);
	int keep = b.goal;
	b.bomb_at_goal = false;
	b.goal = -1;
	b.path.clear();

	// in a blast's way (on its tile or still partly on the one it is leaving): the nearest
	// tile that stays safe
	// In another player's remote bomb's reach it only runs for it when it has nowhere better
	// to go: otherwise it carries on to its goal (always a safe tile), which takes it out.
	// Turning back each time it stepped in made it twitch at the edge of the reach.
	bool threatened = false, in_remote_reach = false;
	for (int t : {here, b.overlap})
		if (t >= 0) {
			threatened |= imminent(w.danger[t], 2.0f / b.speed + float(margin), false);
			in_remote_reach |= imminent(w.danger[t], 0, true);
		}
	threatened |= w.mine[here];   // just planted a land mine: off it before it arms
	// other players on or next to a tile: bots that share goals end up moving as one
	auto crowd = [&](int i) {
		float c = 0;
		for (const Player& p : w.players) {
			if (p.index == int(b.player)) continue;
			int d = std::abs(p.tx - i % W) + std::abs(p.tz - i / W);
			c += d == 0 ? 1.0f : d == 1 ? 0.6f : d == 2 ? 0.3f : 0.0f;
		}
		return c;
	};
	// another bot is already going there
	auto claimed = [&](int i) {
		for (const Bot& o : g_bots)
			if (o.active && &o != &b && o.goal == i) return true;
		return false;
	};
	auto escape = [&] {
		// the nearest safe tile, give or take a step, away from the others
		int best = -1;
		float best_cost = 0;
		// the bombs whose reach it is in; one more than at its last decision is a new scare,
		// and each new scare is a fresh chance of a person's mistakes
		std::vector<int> reach, scary;
		int threats = 0, nearest_bomb = -1;
		for (const BombInfo& m : w.bombs) {
			if (m.owner == int(b.player) && m.fuse >= NEVER) continue;   // its own remote bomb
			blast(w, m, reach);
			if (std::find(reach.begin(), reach.end(), here) == reach.end()) continue;
			scary.insert(scary.end(), reach.begin(), reach.end());
			threats++;
			if (m.owner == int(b.player)) continue;   // it planned its way out of its own bombs
			int d = std::abs(m.x - me.tx) + std::abs(m.z - me.tz);
			if (nearest_bomb < 0 || d < std::abs(nearest_bomb % W - me.tx) + std::abs(nearest_bomb / W - me.tz))
				nearest_bomb = idx(m.x, m.z);
		}
		bool fresh = threats > b.threats;
		b.threats = threats;
		if (fresh && nearest_bomb >= 0) b.stats.escapes++;   // from another player's bomb
		// running the wrong way: straight down the lane, away from the nearest bomb that
		// threatens it, to the first tile out of the scary bombs' reach (on to a dead end if
		// there is none), with no thought for the other bombs; not the corner an expert takes
		bool naive = false;
		if (fresh && nearest_bomb >= 0 && roll(b) < b.naive_escape) {
			static const int DX[4] = {1, -1, 0, 0}, DZ[4] = {0, 0, 1, -1};
			int bx = nearest_bomb % W, bz = nearest_bomb / W, run = -1, run_len = -1;
			for (int d = 0; d < 4; d++) {
				// away from the bomb (any way but toward it when it is on its own tile)
				if ((bx != me.tx || bz != me.tz) && DX[d] * (me.tx - bx) + DZ[d] * (me.tz - bz) <= 0) continue;
				int len = 0;
				for (int x = me.tx + DX[d], z = me.tz + DZ[d]; x >= 0 && x < W && z >= 0 && z < H && s.dist[idx(x, z)] >= 0;
				     x += DX[d], z += DZ[d])
					len++;
				if (len > run_len) run = d, run_len = len;
			}
			for (int k = 1; run >= 0 && k <= run_len; k++) {
				int t = idx(me.tx + DX[run] * k, me.tz + DZ[run] * k);
				best = t;
				if (std::find(scary.begin(), scary.end(), t) == scary.end()) break;   // out of their reach
			}
			naive = best >= 0;
		}
		for (int i = 0; i < N && !naive; i++) {
			if (s.dist[i] < 0 || w.mine[i] || !safe_from(w.danger[i], float(s.dist[i]) / b.speed)) continue;
			float cost = float(s.dist[i]) + 1.5f * crowd(i) + (claimed(i) ? 1.0f : 0.0f) + b.escape_jitter * jitter(b, i, w.tick);
			if (best < 0 || cost < best_cost) best = i, best_cost = cost;
		}
		// committed to that until it would notice its mistake
		if (naive) {
			b.naive_until = w.tick + b.notice_min + int(roll(b) * float(b.notice_max - b.notice_min));
			b.stats.wrong_way++;
		}
		// and now and then a moment frozen before it runs
		if (fresh && nearest_bomb >= 0 && b.freeze_max > 0 && roll(b) < b.panic) {
			b.panic_until = w.tick + b.freeze_max / 3 + int(roll(b) * float(b.freeze_max * 2 / 3));
			b.stats.froze++;
		}
		if (best >= 0) {
			b.path = path_to(s, best);
		} else {   // no clean way out in time: the nearest tile that never burns
			Search loose;
			search(w, here, b.speed, -1000, loose, nullptr, {}, false);
			for (int i = 0; i < N; i++)
				if (loose.dist[i] >= 0 && no_danger(w.danger[i]) && (best < 0 || loose.dist[i] < loose.dist[best])) best = i;
			if (best < 0) {   // fire everywhere it can go: the tile that burns last
				int latest = -1;
				for (int i = 0; i < N; i++) {
					if (loose.dist[i] < 0) continue;
					int first = NEVER;
					for (const Window& win : w.danger[i])
						if (!own_remote(win)) first = std::min(first, win.from);
					first -= int(float(loose.dist[i]) / b.speed);
					if (first > latest) latest = first, best = i;
				}
			}
			if (best >= 0) b.path = path_to(loose, best);
		}
		b.goal = best;
		b.escaping = true;
		b.last_plan = std::format("escape ({},{}) -> ({},{}){}{}{}", me.tx, me.tz, best % W, best / W,
		                          b.path.empty() && best != here ? " (no way)" : "", naive ? " (naive)" : "",
		                          w.tick < b.panic_until ? " (frozen)" : "");
		if (g_log) LOG_INFO("bot %u: %s", b.player, b.last_plan.c_str());
	};
	if (threatened) return escape();

	b.escaping = false;
	b.threats = 0;
	auto in_view = [&](int i) { return std::abs(i % W - me.tx) + std::abs(i / W - me.tz) <= b.awareness; };
	float best_score = 0;
	int best = -1;
	bool best_bomb = false;

	// power-ups
	for (int i = 0; i < N; i++) {
		if (s.dist[i] < 0 || !w.item_type[i] || !in_view(i) || !safe_from(w.danger[i], float(s.dist[i]) / b.speed))
			continue;
		float v = item_value(b, w.item_type[i]);
		if (v <= 0) continue;
		float score = v * b.item_greed * (1.0f + b.noise * (jitter(b, i, w.tick) - 0.5f)) /
		              (1.0f + float(s.dist[i]) * b.item_falloff);
		score /= 1.0f + crowd(i);
		if (claimed(i)) score *= 0.4f;
		for (const Player& p : w.players)   // someone else gets there first
			if (p.index != int(b.player) && std::abs(p.tx - i % W) + std::abs(p.tz - i / W) + 1 < s.dist[i]) {
				score *= 0.5f;
				break;
			}
		if (i == keep) score *= 1.3f;   // stick with the current goal unless clearly beaten
		if (score > best_score) best_score = score, best = i, best_bomb = false;
	}

	// bomb spots: blocks and players in reach, with a way out afterwards
	int capacity = std::min(b.max_bombs, std::max(1, int(ld8(b.obj + 75))));
	int range = std::max(1, int(ld8(b.obj + 76)));
	u8 kind = ld8(b.obj + 70);
	// with remote bombs, one waiting at a time: one left lying about goes off when some other
	// blast reaches it, with whatever it has placed since
	bool waiting = false;
	for (const BombInfo& m : w.bombs) waiting |= m.owner == int(b.player) && m.fuse >= NEVER;
	if (int(b.player) == g_hold_remotes) waiting = false;
	bool careless = roll(b) < b.careless;   // this time it does not look for a way out
	if (own_bombs(w, b) < capacity && w.tick - b.last_bomb_tick > 20 && ld8(b.obj + 368) != 3 && !waiting) {
		std::vector<int> reach;
		Search esc;
		for (int i = 0; i < N; i++) {
			if (s.dist[i] < 0 || s.dist[i] > 12 || !in_view(i) || !no_danger(w.danger[i]) || w.item_type[i]) continue;
			BombInfo hb{i % W, i / W, range, int(b.player), FUSE, FUSE, kind == 2, kind == 3};
			if (kind == 3) hb.range = 2;
			blast(w, hb, reach);
			int blocks = 0, goods = 0;
			float hits = 0;
			for (int t : reach) {
				if (w.tile[t] == SOFT && w.danger[t].empty()) blocks++;   // not already doomed
				if (w.item_type[t] && !bad_item(w.item_type[t])) goods++;
				for (const Player& p : w.players)
					if (p.index != int(b.player) && was_at(p, w.tick - b.aim_lag) == t && in_view(t)) hits += 1;   // where it saw them
			}
			if (kind == 13) blocks = 0;   // a land mine breaks nothing until someone steps on it
			float value = float(blocks) * 3.0f / (1.0f + crowd(i)) * (claimed(i) ? 0.4f : 1.0f) +
			              hits * 14.0f * b.aggression - float(goods) * 4.0f;
			if (value <= 0) continue;
			float arrive = float(s.dist[i]) / b.speed;
			// from the bomb's tile (which it cannot walk back onto) to a tile clear of it
			Window win = kind == 4 ? Window{60, NEVER} : Window{FUSE, FUSE + FLAME};
			search(w, i, b.speed, margin, esc, &reach, win);
			bool out = false;
			for (int t = 0; t < N && !out; t++)
				if (esc.dist[t] > 0 && std::find(reach.begin(), reach.end(), t) == reach.end() &&
				    safe_from(w.danger[t], arrive + float(esc.dist[t]) / b.speed))
					out = true;
			if (!out && !careless) continue;
			float score = value * (1.0f + b.noise * (jitter(b, i, w.tick) - 0.5f)) / (1.0f + float(s.dist[i]) * 0.35f);
			if (i == keep) score *= 1.3f;
			if (score > best_score) best_score = score, best = i, best_bomb = true;
		}
	}

	// walled in (power-downs count as walls until then): through a power-down after all
	int room = 0;
	for (int i = 0; i < N; i++) room += s.dist[i] >= 0;
	if (best < 0 && room <= 8) search(w, here, b.speed, margin, s, nullptr, {}, false);

	// nothing worth doing nearby: a few tiles from one of the nearest players (not on top
	// of them, and not all bots after the same one)
	if (best < 0 && b.aggression > 0.2f) {
		int target = -1;
		float d = 1e9f;
		for (const Player& p : w.players) {
			if (p.index == int(b.player)) continue;
			float dd = float(std::abs(p.tx - me.tx) + std::abs(p.tz - me.tz)) + 6.0f * jitter(b, 1000 + p.index, w.tick);
			if (dd < d) d = dd, target = idx(p.tx, p.tz);
		}
		if (target >= 0) {
			int tx = target % W, tz = target / W;
			float nd = 1e9f;
			for (int i = 0; i < N; i++) {
				if (s.dist[i] < 0 || w.mine[i] || w.bomb[i] || !safe_from(w.danger[i], float(s.dist[i]) / b.speed)) continue;
				float dd = float(std::abs(std::abs(i % W - tx) + std::abs(i / W - tz) - 3)) + 0.05f * float(s.dist[i]) +
				           crowd(i) + jitter(b, i, w.tick);
				if (dd < nd) nd = dd, best = i;
			}
		}
	}
	if (best < 0) {
		if (in_remote_reach) escape();   // nothing to go to: out of the reach, then
		return;
	}
	b.goal = best;
	b.bomb_at_goal = best_bomb;
	b.path = path_to(s, best);
	b.last_plan = std::format("at ({},{}) goal ({},{}){}{} score {:.1f}", me.tx, me.tz, best % W, best / W,
	                          best_bomb ? " bomb" : "", w.item_type[best] ? " item" : "", best_score);
	if (g_log) LOG_INFO("bot %u: %s", b.player, b.last_plan.c_str());
}

// --- acting ------------------------------------------------------------------------

void steer(Bot& b, const Player& me) {
	u16 d1 = 0;
	while (!b.path.empty()) {
		int n = b.path.front();
		float dx = float(n % W) - me.x, dz = float(n / W) - me.z;
		if (std::fabs(dx) < 0.12f && std::fabs(dz) < 0.12f) {
			b.path.erase(b.path.begin());
			continue;
		}
		// line up on the crossing axis first, then go
		if (std::fabs(dx) > std::fabs(dz)) {
			if (std::fabs(dz) > 0.15f) d1 = dz > 0 ? input::d1::DOWN : input::d1::UP;
			else d1 = dx > 0 ? input::d1::RIGHT : input::d1::LEFT;
		} else {
			if (std::fabs(dx) > 0.15f) d1 = dx > 0 ? input::d1::RIGHT : input::d1::LEFT;
			else d1 = dz > 0 ? input::d1::DOWN : input::d1::UP;
		}
		break;
	}
	if (b.path.empty() && b.goal >= 0) {
		// at the goal: settle on the tile's centre
		float dx = float(b.goal % W) - me.x, dz = float(b.goal / W) - me.z;
		if (std::fabs(dx) > 0.2f) d1 = dx > 0 ? input::d1::RIGHT : input::d1::LEFT;
		else if (std::fabs(dz) > 0.2f) d1 = dz > 0 ? input::d1::DOWN : input::d1::UP;
	}
	b.pad.digital1 = d1;
}

// How it fared against other players' bombs this round (always in the console, to judge
// the difficulty by)
void log_stats(const Bot& b, const char* when) {
	LOG_INFO("bot %u (menu slot %u) %s: %d escapes from others' bombs, %d ran the wrong way, %d froze", b.player,
	         b.difficulty, when, b.stats.escapes, b.stats.wrong_way, b.stats.froze);
}

void think(Bot& b, const World& w) {
	const Player* me = nullptr;
	for (const Player& p : w.players)
		if (p.obj == b.obj) me = &p;
	b.pad.digital1 = b.pad.digital2 = 0;
	std::memset(b.pad.pressure, 0, sizeof b.pad.pressure);
	if (!me) {   // out of the match
		if (b.last_x >= 0) {
			int x = int(std::lround(b.last_x)), z = int(std::lround(b.last_z));
			LOG_INFO("bot %u: out at (%d,%d) [%.2f,%.2f], tick %d%s; was: %s; danger here was %s", b.player, x, z, b.last_x,
			         b.last_z, w.tick, w.flame[idx(x, z)] ? " (in a blast)" : "", b.last_plan.c_str(),
			         b.last_danger.c_str());
			log_stats(b, "knocked out");
			for (u32 o = ld32(w.game + 484), n = 0; o && n < 64; o = ld32(o + 8), n++)
				if (std::abs(tile_of(ld32(o + 20)) - x) + std::abs(tile_of(ld32(o + 28)) - z) <= 6)
					LOG_INFO("bot %u:   bomb obj class 0x%08x at (%d,%d) range %u owner %u +69 %u", b.player, ld32(o),
					         tile_of(ld32(o + 20)), tile_of(ld32(o + 28)), ld8(o + 59), ld8(o + 60) >> 4, ld8(o + 69));
			std::string fl;
			for (int zz = std::max(0, z - 3); zz <= std::min(H - 1, z + 3); zz++)
				for (int xx = std::max(0, x - 3); xx <= std::min(W - 1, x + 3); xx++)
					if (w.flame[idx(xx, zz)]) fl += std::format(" ({},{})", xx, zz);
			LOG_INFO("bot %u:   flames near:%s (frame %llu)", b.player, fl.c_str(), (unsigned long long)rsx::frames());
			g_knockout_frames.push_back(rsx::frames());
			b.last_x = -1;
		}
		return;
	}

	// measured speed (tiles per tick) while walking
	if (b.last_x >= 0 && w.tick == b.last_tick + 1) {
		float moved = std::fabs(me->x - b.last_x) + std::fabs(me->z - b.last_z);
		if (moved > 0.01f && moved < 0.5f) b.speed = std::clamp(b.speed * 0.9f + moved * 0.1f, 1.0f / 40, 1.0f / 4);
	}
	if (b.last_x >= 0 && b.last_dir && ld8(b.obj + 368)) {
		float mx = me->x - b.last_x, mz = me->z - b.last_z;
		float along = b.last_dir == input::d1::RIGHT ? mx : b.last_dir == input::d1::LEFT ? -mx
		            : b.last_dir == input::d1::DOWN  ? mz : -mz;
		if (along < -0.01f) b.reversed_votes++;
		else if (along > 0.01f) b.reversed_votes = std::max(0, b.reversed_votes - 1);
		bool was = b.reversed;
		if (b.reversed_votes >= 10) b.reversed = !b.reversed, b.reversed_votes = 0;
		if (was != b.reversed && g_log)
			LOG_INFO("bot %u: directions %s (curse %u)", b.player, b.reversed ? "reversed" : "normal", ld8(b.obj + 368));
	}
	float moved_now = b.last_x >= 0 ? std::fabs(me->x - b.last_x) + std::fabs(me->z - b.last_z) : 1.0f;
	b.last_x = me->x, b.last_z = me->z, b.last_tick = w.tick;

	int here = idx(me->tx, me->tz);
	float ox = me->x - float(me->tx), oz = me->z - float(me->tz);
	b.overlap = std::fabs(ox) > 0.3f ? idx(me->tx + (ox > 0 ? 1 : -1), me->tz)
	          : std::fabs(oz) > 0.3f ? idx(me->tx, me->tz + (oz > 0 ? 1 : -1)) : -1;
	// skull curses (+368, until tick +364): reversed directions show as moving away from
	// the pressed direction; curse 3 stops bombs
	if ((w.flame[here] || (b.overlap >= 0 && w.flame[b.overlap])) && !b.burnt) {
		b.burnt = true;
		std::string bombs;
		for (const BombInfo& m : w.bombs)
			if (std::abs(m.x - me->tx) + std::abs(m.z - me->tz) <= 8)
				bombs += std::format(" ({},{}) r{} p{} in {}", m.x, m.z, m.range, m.owner, m.at);
		LOG_INFO("bot %u: caught by fire at [%.2f,%.2f] tick %d; was: %s; danger was %s; bombs:%s", b.player, me->x,
		         me->z, w.tick, b.last_plan.c_str(), b.last_danger.c_str(), bombs.c_str());
		g_knockout_frames.push_back(rsx::frames());
	}
	b.last_danger.clear();
	for (int t : {here, b.overlap})
		if (t >= 0)
			for (const Window& win : w.danger[t]) b.last_danger += std::format("[{},{}]", win.from, win.to);
	bool cursed = ld8(b.obj + 368) != 0;
	if (!cursed) b.reversed = false, b.reversed_votes = 0;
	g_me = int(b.player);
	g_blocked = w.tick < b.blocked_until ? b.blocked : -1;
	// walking into something it cannot pass: that tile is out of bounds for a while
	if (b.last_dir && w.tick > 240 && !b.burnt) {   // nor while it is knocked out   // not during the countdown, when nobody can move
		b.stuck_ticks = moved_now < 0.004f ? b.stuck_ticks + 1 : 0;
	} else {
		b.stuck_ticks = 0;
	}
	// as soon as it is plainly not moving (a few ticks): turn away, as a person would
	if (b.stuck_ticks == 6) {
		using namespace input::d1;
		// the tile its leading edge is pushing into (its own tile when it is behind the centre,
		// e.g. back onto a bomb it has stepped off)
		float ex = me->x + 0.51f * float((b.last_dir == RIGHT) - (b.last_dir == LEFT));
		float ez = me->z + 0.51f * float((b.last_dir == DOWN) - (b.last_dir == UP));
		int nx = int(std::lround(ex)), nz = int(std::lround(ez));
		int n = nx >= 0 && nx < W && nz >= 0 && nz < H ? idx(nx, nz) : -1;
		if (g_log)
			LOG_INFO("bot %u: stuck at [%.2f,%.2f] pressing %s toward (%d,%d) cell %08x; was: %s", b.player, me->x, me->z,
			         b.last_dir == UP ? "up" : b.last_dir == DOWN ? "down" : b.last_dir == LEFT ? "left" : "right", nx, nz,
			         n >= 0 ? w.raw[n] : 0u, b.last_plan.c_str());
		b.blocked = n, b.blocked_until = w.tick + 90;
		g_blocked = n;
		b.next_think = w.tick;   // re-plan now
		b.stuck_ticks = 0;
	}
	// (running the wrong way: it keeps on until it would notice)
	bool committed = w.tick < b.naive_until;
	bool goal_burns = b.goal >= 0 && !safe_from(w.danger[b.goal], 0) && !committed;
	// fire coming (or standing in a remote bomb's reach with nowhere it means to go)
	bool here_burns = false;
	for (int t : {here, b.overlap})
		if (t >= 0)
			here_burns |= imminent(w.danger[t], 2.0f / b.speed + 10.0f, b.path.empty());
	// a bomb now lies on the next tile of its way
	bool way_shut = !b.path.empty() && b.path.front() != here && !walkable(w, b.path.front());
	if ((w.tick >= b.next_think && !committed) || goal_burns || way_shut || (here_burns && (!b.escaping || b.path.empty()) && !committed) ||
	    (b.path.empty() && b.goal != here)) {
		plan(b, w, *me);
		b.next_think = w.tick + b.reaction;
		// now and then a moment's thought before going (never while running from a blast)
		b.idle_until = !b.escaping && roll(b) < b.hesitate ? w.tick + 6 + int(roll(b) * float(b.pause_max - 6)) : 0;
	}
	// arrived where it meant to bomb
	if (b.bomb_at_goal && b.goal == here && std::fabs(me->x - float(me->tx)) < 0.3f &&
	    std::fabs(me->z - float(me->tz)) < 0.3f && !w.bomb[here]) {
		b.bomb_hold = 3;
		b.bomb_at_goal = false;
		b.last_bomb_tick = w.tick;
		b.next_think = w.tick + 2;   // then get out of its way
		if (g_log) {
			std::string d;
			for (const Window& win : w.danger[here]) d += std::format("[{},{}{}]", win.from, win.to, win.soft ? " soft" : "");
			LOG_INFO("bot %u: bomb at (%d,%d); danger here %s", b.player, me->tx, me->tz, d.c_str());
		}

	}
	// remote bombs: set them off once out of their way (+104: the remote bomb the game
	// sets off on Circle; it then goes ~34 ticks later)
	if (ld8(b.obj + 70) == 4 && ld32(b.obj + 104) && b.detonate_hold == 0 && b.bomb_hold == 0 &&
	    w.tick - b.last_bomb_tick > 10 && int(b.player) != g_hold_remotes) {
		bool clear_of = true;
		std::vector<int> reach;
		for (const BombInfo& m : w.bombs)
			if (m.owner == int(b.player)) {
				blast(w, m, reach);
				for (int t : {here, b.overlap})
					clear_of &= std::find(reach.begin(), reach.end(), t) == reach.end();
			}
		if (clear_of) {
			b.detonate_hold = 3;
			if (g_log) LOG_INFO("bot %u: detonate at tick %d", b.player, w.tick);
		}
	}
	if (g_log && w.tick % 1800 == 0) LOG_INFO("bot %u: %d quick direction flips so far", b.player, b.flips);
	steer(b, *me);
	if ((w.tick < b.idle_until && !b.escaping) || w.tick < b.panic_until) b.pad.digital1 = 0;
	u16 intended = b.pad.digital1;
	if (b.reversed) {
		using namespace input::d1;
		u16 d = b.pad.digital1, r = 0;
		if (d & UP) r |= DOWN;
		if (d & DOWN) r |= UP;
		if (d & LEFT) r |= RIGHT;
		if (d & RIGHT) r |= LEFT;
		b.pad.digital1 = r;
	}
	b.last_dir = intended;
	if (intended) {
		using namespace input::d1;
		u16 opp = intended == UP ? DOWN : intended == DOWN ? UP : intended == LEFT ? RIGHT : intended == RIGHT ? LEFT : 0;
		if (b.prev_press == opp && w.tick - b.prev_press_tick < 10) {
			b.flips++;
			if (imminent(w.danger[here], 0, true)) b.remote_flips++, g_remote_flips++;
		}
		b.prev_press = intended, b.prev_press_tick = w.tick;
	}   // what it wants; the votes compare it with how it moved
	if (b.bomb_hold > 0) {
		if (--b.bomb_hold > 0) b.pad.digital2 |= input::d2::CROSS, b.pad.pressure[6] = 255;
	} else if (b.detonate_hold > 0) {
		if (--b.detonate_hold > 0) b.pad.digital2 |= input::d2::CIRCLE, b.pad.pressure[5] = 255;
	}
	static const u16 DIRS[4] = {input::d1::RIGHT, input::d1::LEFT, input::d1::UP, input::d1::DOWN};
	for (int i = 0; i < 4; i++)
		if (b.pad.digital1 & DIRS[i]) b.pad.pressure[i] = 255;
}

// The bombs as this bot sees them: a new one only after its reaction time (later when it
// is busy heading elsewhere), with a guess of its fuse that is a little off, and now and
// then without expecting another blast to set it off. Its own bombs it knows at once.
void perceive(Bot& b, World& w) {
	const Player* me = nullptr;
	for (const Player& p : w.players)
		if (p.obj == b.obj) me = &p;
	std::erase_if(b.noticed, [&](const Bot::Noticed& n) {
		return std::none_of(w.bombs.begin(), w.bombs.end(), [&](const BombInfo& m) { return m.obj == n.obj; });
	});
	std::vector<BombInfo> kept;
	for (BombInfo m : w.bombs) {
		auto it = std::find_if(b.noticed.begin(), b.noticed.end(), [&](const Bot::Noticed& n) { return n.obj == m.obj; });
		if (it == b.noticed.end()) {
			bool own = m.owner == int(b.player);
			Bot::Noticed n{m.obj, m.first, 0, roll(b) < b.chain_miss};
			if (!own) {
				n.at += b.notice_min + int(roll(b) * float(b.notice_max - b.notice_min));
				// not close by and not ahead of where it is going: noticed later
				if (me) {
					using namespace input::d1;
					int dx = m.x - me->tx, dz = m.z - me->tz;
					int ahead = b.last_dir == RIGHT ? dx : b.last_dir == LEFT ? -dx : b.last_dir == DOWN ? dz : b.last_dir == UP ? -dz : 0;
					int side = b.last_dir == RIGHT || b.last_dir == LEFT ? std::abs(dz) : std::abs(dx);
					if (b.last_dir && std::abs(dx) + std::abs(dz) > 3 && !(ahead > 0 && side <= 1))
						n.at += int(roll(b) * float(b.distracted));
				}
			}
			if (m.fuse < NEVER) n.error = int((roll(b) + roll(b) - 1.0f) * b.fuse_error * (own ? 0.3f : 1.0f));
			if (g_log && !own)
				LOG_INFO("bot %u: will see bomb at (%d,%d) %d ticks after it appeared, fuse off by %d%s", b.player, m.x, m.z,
				         n.at - m.first, n.error, n.chain_blind ? ", chain-blind" : "");
			b.noticed.push_back(n);
			it = b.noticed.end() - 1;
		}
		if (w.tick < it->at) continue;   // not seen yet (its tile still blocks the way)
		if (m.fuse < NEVER) m.fuse = std::max(0, m.fuse + it->error);
		m.at = m.fuse;
		m.chain_blind = it->chain_blind;
		kept.push_back(m);
	}
	w.bombs = std::move(kept);
}

// pad ports the match's human players use
u32 human_ports() {
	u32 used = 0, mgr = ld32(SLOT_MANAGER);
	if (!mgr) return used;
	for (u32 i = 0; i < 8; i++) {
		u32 slot = mgr + i * 124;
		if (ld8(slot) <= 7 && !ld8(slot + 79) && ld8(slot + 78) < input::MAX_PORTS) used |= 1u << ld8(slot + 78);
	}
	return used;
}

// --- debugging ---------------------------------------------------------------------

void dump_list(FILE* f, const char* name, u32 head, u32 bytes) {
	std::fprintf(f, "\n== %s (list at 0x%08x)\n", name, head);
	int n = 0;
	for (u32 o = ld32(head + 8); o && n < 64; o = ld32(o + 8), n++) {
		std::fprintf(f, "%08x:", o);
		for (u32 i = 0; i < bytes; i += 4) std::fprintf(f, "%s%08x", i % 32 ? " " : "\n  ", ld32(o + i));
		std::fprintf(f, "\n");
	}
}

void dump(u64 frame) {
	u32 game = ld32(GAME_PTR);
	if (!game) return;
	u32 grid = ld32(game + 16);
	if (!grid) return;
	FILE* f = std::fopen(("bot_" + std::to_string(frame) + ".txt").c_str(), "w");
	if (!f) return;
	std::fprintf(f, "frame %llu game %08x grid %08x tick %d\n\nword0:\n", (unsigned long long)frame, game, grid,
	             int(ld32(game + 232)));
	for (int z = 0; z < H; z++) {
		for (int x = 0; x < W; x++) std::fprintf(f, " %08x", ld32(grid + u32(idx(x, z)) * 8));
		std::fprintf(f, "\n");
	}
	std::fprintf(f, "\nword1:\n");
	for (int z = 0; z < H; z++) {
		for (int x = 0; x < W; x++) std::fprintf(f, " %08x", ld32(grid + u32(idx(x, z)) * 8 + 4));
		std::fprintf(f, "\n");
	}
	dump_list(f, "players +464", game + 464, 640);
	dump_list(f, "bombs +476", game + 476, 128);
	dump_list(f, "items +500", game + 500, 64);
	std::fprintf(f, "\n== items decoded\n");
	for (u32 o = ld32(game + 508); o; o = ld32(o + 8))
		std::fprintf(f, "  (%d,%d) type %u\n", tile_of(ld32(o + 12)), tile_of(ld32(o + 20)), ld8(o + 24));
	dump_list(f, "+548", game + 548, 64);
	std::fclose(f);
	LOG_INFO("bot: wrote bot_%llu.txt", (unsigned long long)frame);
}

void debug_tick() {
	static u64 first = 0, step = 1, count = 0, last = 0;
	static bool parsed = false;
	if (!parsed) {
		parsed = true;
		if (const char* e = std::getenv("BOT_DUMP")) {
			first = std::strtoull(e, nullptr, 10);
			const char* c = std::strchr(e, ',');
			if (c) step = std::max<u64>(1, std::strtoull(c + 1, nullptr, 10));
			if (c && (c = std::strchr(c + 1, ','))) count = std::strtoull(c + 1, nullptr, 10);
			if (!count) count = 1;
		}
	}
	// BOT_SHOT=<tick>,<count>,<step>: frame dumps (rsx) from that match tick on
	static int shot_tick = -1, shot_count = 1, shot_step = 1;
	static bool shot_parsed = false;
	if (!shot_parsed) {
		shot_parsed = true;
		if (const char* e = std::getenv("BOT_SHOT")) std::sscanf(e, "%d,%d,%d", &shot_tick, &shot_count, &shot_step);
	}
	if (shot_tick >= 0 && profiles::in_match()) {
		u32 game = ld32(GAME_PTR);
		if (game && int(ld32(game + 232)) >= shot_tick) {
			rsx::set_dump_frame(rsx::frames() + 2, u64(shot_count), u64(shot_step));
			LOG_INFO("bot: frame dumps from tick %d", int(ld32(game + 232)));
			shot_tick = -1;
		}
	}
	// BOT_DEATHCAM=1: every other frame is saved; frames more than 120 old are deleted
	// unless a bot was knocked out within the next 20, so the lead-up to each stays
	static const bool deathcam = std::getenv("BOT_DEATHCAM") && *std::getenv("BOT_DEATHCAM") == '1';
	if (deathcam) {
		static bool started = false;
		u64 now = rsx::frames();
		if (!started && profiles::in_match()) {
			started = true;
			rsx::set_dump_frame(now + 2, 1u << 30, 2);
		}
		static u64 cleaned = 0;
		for (; started && cleaned + 120 < now; cleaned++) {
			bool keep = false;
			for (u64 k : g_knockout_frames) keep |= cleaned + 120 >= k && cleaned <= k + 20;
			if (!keep) std::remove(("frame_" + std::to_string(cleaned) + ".bmp").c_str());
		}
	}
	static const bool all_powerups = std::getenv("BOT_ALL_POWERUPS") && *std::getenv("BOT_ALL_POWERUPS") == '1';
	if (all_powerups && !profiles::in_match())
		for (u32 i = 0; i < 25; i++) st32(MATCH_CONFIG + 164 + i * 4, 1);
	static const char* arena = std::getenv("BOT_ARENA");   // debugging: the arena (match config +0)
	if (arena && !profiles::in_match()) st32(MATCH_CONFIG, u32(std::atoi(arena)));
	if (!first) return;
	u64 frame = rsx::frames() + 1;
	if (frame == last || frame < first || (frame - first) % step || frame >= first + step * count) return;
	last = frame;
	dump(frame);
}

} // namespace

bool pad(u32 port, input::PadState& out) {
	for (const Bot& b : g_bots)
		if (b.active && b.port == port) {
			out = b.pad;
			return true;
		}
	return false;
}

// Game main thread, once per pad read (several times a frame).
void game_thread_tick() {
	debug_tick();
	u32 game = ld32(GAME_PTR);
	bool any = false;
	for (Bot& b : g_bots) {
		if (b.active && (b.game != game || !profiles::in_match())) {
			LOG_INFO("bot: player %u (port %u) ends; %d quick direction flips, %d in a remote bomb's reach", b.player, b.port, b.flips, b.remote_flips);
			if (b.last_x >= 0) log_stats(b, "round over");
			b.active = false;
		}
		any |= b.active;
	}
	if (!any) return;
	World w;
	if (!read_world(w) || w.tick == g_last_tick) return;
	g_last_tick = w.tick;
	for (const Player& p : w.players) g_trail[p.index & 7][w.tick % TRAIL] = {w.tick, idx(p.tx, p.tz)};
	// each bot plans on the bombs as it sees them
	for (Bot& b : g_bots)
		if (b.active) {
			World seen = w;
			perceive(b, seen);
			build_danger(seen);
			think(b, seen);
		}
	if (g_log) {   // how often bots are on top of or next to each other
		static int close = 0, ticks = 0;
		bool near = false;
		for (const Player& p : w.players)
			for (const Player& q : w.players)
				if (p.index < q.index && g_bots[p.index & 7].active && g_bots[q.index & 7].active &&
				    std::abs(p.x - q.x) + std::abs(p.z - q.z) < 1.2f) {
					if (!near && close % 60 == 0)
						LOG_INFO("bot: %d and %d close: %s | %s", p.index, q.index, g_bots[p.index & 7].last_plan.c_str(),
						         g_bots[q.index & 7].last_plan.c_str());
					near = true;
				}
		close += near, ticks++;
		if (w.tick % 1800 == 0) LOG_INFO("bot: bots close together %d of %d ticks; %d quick flips in remote reach", close, ticks, g_remote_flips);
	}
}

} // namespace bot

// Match setup (sub_00051xxx at 0x5236C): a CPU slot's player, sub_00058A20(obj, player,
// difficulty) on a 616-byte object. With cpu_ai = smart it is made a human-class player
// instead (424 bytes, so the object is big enough), on a free pad port that a bot drives.
void hook_cpu_player(PPUContext& ctx, PPUFunc original) {
	using namespace bot;
	if (config::get().cpu_ai != config::CpuAi::Smart) return original(ctx);
	u32 player = u32(ctx.r[4]) & 0xFF, difficulty = u32(ctx.r[5]) & 0xFF;
	u32 game = ld32(GAME_PTR);
	u32 used = human_ports();
	for (const Bot& b : g_bots)
		if (b.active && b.game == game && b.player != player) used |= 1u << b.port;
	u32 port = 0;
	while (port < input::MAX_PORTS && (used >> port & 1)) port++;
	if (player >= 8 || port >= input::MAX_PORTS) {
		LOG_WARN("bot: no free pad port for player %u; the game's own CPU plays", player);
		return original(ctx);
	}
	Bot& b = g_bots[player];
	b = Bot{};
	b.active = true;
	b.game = game;
	b.obj = u32(ctx.r[3]);
	b.port = port;
	b.player = player;
	b.difficulty = difficulty;
	b.rng = (0x9E3779B9u * (player + 1)) ^ u32(rsx::frames());
	b.pad.connected = true;
	if (const char* e = std::getenv("BOT_DIFFICULTY")) difficulty = u32(std::atoi(e));   // debugging
	// Easy / Normal / Hard (slot difficulty 0-1 / 2 / 3), far apart so each level plays
	// differently: the original CPU's sight, reaction time, appetite for attacking players and
	// randomness, then a person's mistakes (ticks are 1/60 s: Hard takes in a new bomb after
	// ~0.2-0.3 s, Normal ~0.4-0.6 s, Easy ~0.7-1.1 s), and how much it does at once
	if (difficulty <= 1) {
		b.awareness = 6, b.reaction = 34, b.aggression = 0.25f, b.noise = 0.7f;
		b.margin = 4, b.notice_min = 40, b.notice_max = 65, b.distracted = 70, b.fuse_error = 40, b.chain_miss = 0.75f;
		b.escape_jitter = 8, b.careless = 0.06f, b.hesitate = 0.3f, b.pause_max = 40;
		b.max_bombs = 1, b.walks_over_bad = true, b.item_greed = 0.3f, b.item_falloff = 0.6f, b.aim_lag = 42;
		b.naive_escape = 0.6f, b.panic = 0.4f, b.freeze_max = 40;
	} else if (difficulty == 2) {
		b.awareness = 11, b.reaction = 20, b.aggression = 0.4f, b.noise = 0.3f;
		b.margin = 5, b.notice_min = 30, b.notice_max = 48, b.distracted = 45, b.fuse_error = 30, b.chain_miss = 0.5f;
		b.escape_jitter = 4, b.careless = 0.015f, b.hesitate = 0.12f, b.pause_max = 24;
		b.max_bombs = 3, b.item_greed = 0.45f, b.item_falloff = 0.35f, b.aim_lag = 24;
		b.naive_escape = 0.4f, b.panic = 0.25f, b.freeze_max = 30;
	} else {
		b.awareness = 17, b.reaction = 10, b.aggression = 1.0f, b.noise = 0.1f;
		b.margin = 8, b.notice_min = 12, b.notice_max = 18, b.distracted = 18, b.fuse_error = 8, b.chain_miss = 0.1f;
		b.escape_jitter = 2, b.careless = 0, b.hesitate = 0.03f, b.pause_max = 14, b.aim_lag = 6;
		b.naive_escape = 0.05f;
	}
	b.dice = b.rng | 1;
	if (std::getenv("BOT_FLAWLESS") && *std::getenv("BOT_FLAWLESS") == '1') {
		// debugging: the bots before preview 4 (see every bomb at once, exactly)
		b.reaction = difficulty <= 1 ? 20 : difficulty == 2 ? 10 : 4;
		b.margin = 6 + b.reaction / 2, b.notice_min = b.notice_max = b.distracted = 0, b.fuse_error = 0;
		b.chain_miss = 0, b.escape_jitter = 1.2f, b.careless = b.hesitate = 0;
		b.max_bombs = 99, b.walks_over_bad = false, b.item_greed = 1, b.item_falloff = 0.2f, b.aim_lag = 0;
		b.naive_escape = b.panic = 0;
	}
	ctx.r[5] = port;
	ctx.r[6] = 0;   // default button layout
	ppu_icall(ctx, HUMAN_PLAYER_CTOR);
	// the human class's vtable, but answering "CPU" where the game asks (e.g. whether any
	// person is still in the match, for the quick-finish offer)
	static u32 vtable = 0;
	if (!vtable) {
		vtable = vm::alloc(vm::MAIN_BASE, 0x400, 0x100);
		for (u32 i = 0; i < 0x400; i += 4) st32(vtable + i, ld32(HUMAN_VTABLE + i));
		st32(vtable + IS_CPU, ld32(CPU_VTABLE + IS_CPU));
	}
	if (ld32(b.obj) == HUMAN_VTABLE) st32(b.obj, vtable);
	if (const char* e = std::getenv("BOT_GIVE")) {   // debugging: power-ups from the start (pickup handler)
		PPUContext saved = ctx;
		for (const char* p = e; *p; p = std::strchr(p, ',') ? std::strchr(p, ',') + 1 : "") {
			ctx.r[3] = b.obj;
			ctx.r[4] = u64(std::atoi(p));
			ppu_icall(ctx, 0x68A68);
		}
		u64 sp = ctx.r[1];
		ctx = saved;
		ctx.r[1] = sp;
	}
	LOG_INFO("bot: player %u (difficulty %u, menu slot %u) on pad port %u", player, difficulty, b.difficulty, port);
}
