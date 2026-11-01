// Local player profiles: named people whose game data follows them to whichever
// controller they pick up.
//
// The game keeps one 272-byte record per controller slot (costume, stats, ...) and
// saves all slots together in its single save file, so on its own a record belongs
// to "whoever holds controller 2". Here each profile owns a record in
// user/profiles/<name>/: when the game loads its save, the records of the profiles
// on each controller are put into it; when it saves, they are taken back out; when a
// controller switches profile mid-game, the records are swapped in memory. A
// controller without a profile plays as that slot's guest, as the game always did.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace profiles {

constexpr size_t MAX_NAME = 16;

// After paths and config are set up: loads the controller -> profile memory and
// hooks the game's save file.
void init();

std::vector<std::string> list();             // all profiles, sorted by name
// Create a profile (empty data). False if the name is invalid or taken; `error` says why.
bool create(const std::string& name, std::string* error = nullptr);
bool valid_name(const std::string& name);

// Put a profile ("" = guest) on a controller slot. The controller remembers it for
// next time; a profile in use on another slot moves here (that slot becomes guest).
// During a match the change waits until the match is over.
void assign(uint32_t port, const std::string& name);
// True while a match is on, from the arena's loading screen to the results
// (match_lock.cpp); profiles do not change then.
bool in_match();
std::string assigned(uint32_t port);
// The port, other than `except`, whose connected controller has this profile (-1: none).
// A disconnected controller keeps its profile (for reconnecting mid-match) but does
// not hold it against others: anyone may take it, and that port becomes the guest.
int holder(const std::string& name, uint32_t except);

// Input listener entry points (window thread): restore what this controller used last.
void controller_connected(uint32_t port, const std::string& key);
void controller_disconnected(uint32_t port);

// Game thread, once per pad read: applies pending profile switches to game memory.
void game_thread_tick();

// Game thread, once per pad read: tells the overlay when a controller joins or leaves
// the game's player slots (Player Select), so each joining player picks a profile.
void watch_slots();

} // namespace profiles
