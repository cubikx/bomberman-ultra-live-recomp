// Trophies, kept per person in user/trophies/.
//
// On PS3 trophies belong to the console's signed-in user. Bomberman ULTRA is an
// XBLA port whose AchievementTracker earns achievements per userIndex (= pad port),
// so here each unlock goes to whoever plays on that controller: their profile, or
// that controller's guest. One readable JSON file per person; deleting it (or the
// whole folder) resets their trophies. The list itself comes from the game's
// TROPDIR/<commId>/TROPHY.TRP.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace trophies {

enum class Grade { platinum = 1, gold, silver, bronze };   // SceNpTrophyGrade values

struct Trophy {
	int32_t id;
	Grade grade;
	bool hidden;
	std::string name, detail;
};

// After paths and profiles are set up: reads the trophy list.
void init();

int32_t count();
const Trophy* find(int32_t id);
const std::vector<Trophy>& list();
// The trophy's icon (TROPnnn.PNG from TROPHY.TRP); empty if missing.
const std::string& icon_png(int32_t id);

// Whoever plays on controller `slot`: a profile name or "Guest on controller N".
std::string person_on(uint32_t slot);
// Everyone with trophies to show: all profiles, then guests that have a file.
std::vector<std::string> people();
// id -> unlock time (Unix seconds, UTC) for one person.
std::map<int32_t, int64_t> unlocks_of(const std::string& person);

enum class Result { unlocked, already_unlocked, invalid_id };
// Unlock `id` for the player on controller `slot`. When that completes a set with a
// platinum trophy, the platinum is unlocked too and its id returned in `platinum`
// (-1 otherwise). Shows an overlay notice.
Result unlock(uint32_t slot, int32_t id, int32_t* platinum);
// Unlock time (CellRtcTick: microseconds since 0001-01-01) for the player on `slot`, or 0.
uint64_t unlocked_tick(uint32_t slot, int32_t id);

// Game thread, once per pad read: the TROPHY_TEST debug switch.
void game_thread_tick();

} // namespace trophies
