// sceNpTrophy: one context and one handle, unlocks recorded per person by trophies/.
//
// The game unlocks from a thread it starts per achievement ("UnlockAchievement",
// AchievementTracker 0xF0AB0) with argument (userIndex << 32 | trophyId); that
// argument tells us which controller earned it. RegisterContext does not call the
// status callback: the game only logs once it returns.
#include "trophies/trophies.h"

#include <algorithm>
#include <cstring>

#include "runtime/kernel.h"

namespace {

constexpr u32 SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT = 0x80022906, SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED = 0x80022915,
              SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID = 0x80022920;
constexpr u32 CONTEXT_ID = 1, HANDLE_ID = 1;

// SceNpTrophyDetails: trophyId, trophyGrade, name[128], description[1024], hidden, reserved[3]
constexpr u32 DETAILS_SIZE = 1164, D_ID = 0, D_GRADE = 4, D_NAME = 8, D_DESCR = 136, D_HIDDEN = 1160;
// SceNpTrophyData: timestamp (CellRtcTick), trophyId, unlocked, reserved[3]
constexpr u32 DATA_SIZE = 16, T_TIME = 0, T_ID = 8, T_UNLOCKED = 12;

void put_string(u32 ea, const std::string& s, size_t max) {
	size_t n = std::min(s.size(), max - 1);
	std::memcpy(vm::str(ea), s.data(), n);
	st8(ea + u32(n), 0);
}

// Controller slot of the unlocking thread, from its start argument (slot 0 otherwise).
u32 unlocking_slot(s32 id) {
	u64 a = current_thread_arg();
	if (s32(u32(a)) != id) {
		LOG_WARN("sceNpTrophy: unlock of %d outside the game's unlock thread; credited to controller 1", id);
		return 0;
	}
	return u32(a >> 32);
}

} // namespace

HLE_FUNC(sceNpTrophy, sceNpTrophyInit) { ret(ctx, CELL_OK); }
HLE_FUNC(sceNpTrophy, sceNpTrophyTerm) { ret(ctx, CELL_OK); }

HLE_FUNC(sceNpTrophy, sceNpTrophyCreateContext) {
	u32 context = arg32(ctx, 0);
	if (!context) return ret(ctx, SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
	st32(context, CONTEXT_ID);
	ret(ctx, CELL_OK);
}
HLE_FUNC(sceNpTrophy, sceNpTrophyDestroyContext) { ret(ctx, CELL_OK); }

HLE_FUNC(sceNpTrophy, sceNpTrophyCreateHandle) {
	u32 handle = arg32(ctx, 0);
	if (!handle) return ret(ctx, SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
	st32(handle, HANDLE_ID);
	ret(ctx, CELL_OK);
}
HLE_FUNC(sceNpTrophy, sceNpTrophyDestroyHandle) { ret(ctx, CELL_OK); }
HLE_FUNC(sceNpTrophy, sceNpTrophyAbortHandle) { ret(ctx, CELL_OK); }

HLE_FUNC(sceNpTrophy, sceNpTrophyRegisterContext) { ret(ctx, CELL_OK); }

// (context, handle, trophyId, platinumId*)
HLE_FUNC(sceNpTrophy, sceNpTrophyUnlockTrophy) {
	s32 id = s32(arg32(ctx, 2));
	u32 platinum_out = arg32(ctx, 3);
	s32 platinum = -1;
	trophies::Result r = trophies::unlock(unlocking_slot(id), id, &platinum);
	if (platinum_out) st32(platinum_out, u32(platinum));
	switch (r) {
	case trophies::Result::unlocked: return ret(ctx, CELL_OK);
	case trophies::Result::already_unlocked: return ret(ctx, SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED);
	default: return ret(ctx, SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID);
	}
}

// (context, handle, trophyId, details*, data*). Unlock state is controller 1's: the
// game enumerates once for everyone (and only uses the names).
HLE_FUNC(sceNpTrophy, sceNpTrophyGetTrophyInfo) {
	s32 id = s32(arg32(ctx, 2));
	u32 details = arg32(ctx, 3), data = arg32(ctx, 4);
	const trophies::Trophy* t = trophies::find(id);
	if (!t) return ret(ctx, SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID);
	if (details) {
		std::memset(vm::str(details), 0, DETAILS_SIZE);
		st32(details + D_ID, u32(id));
		st32(details + D_GRADE, u32(t->grade));
		put_string(details + D_NAME, t->name, 128);
		put_string(details + D_DESCR, t->detail, 1024);
		st8(details + D_HIDDEN, t->hidden);
	}
	if (data) {
		u64 tick = trophies::unlocked_tick(0, id);
		std::memset(vm::str(data), 0, DATA_SIZE);
		st64(data + T_TIME, tick);
		st32(data + T_ID, u32(id));
		st8(data + T_UNLOCKED, tick != 0);
	}
	ret(ctx, CELL_OK);
}
