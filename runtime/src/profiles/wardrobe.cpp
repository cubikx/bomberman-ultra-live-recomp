// Wardrobe pickup (costume unlock, unlockpu.x): settings.ini [game] aggressive_costume_spawns.
// The game arms one pickup per local match of 2 minutes or more (game object +377, set
// by sub_00051948) and tries it once, ~61 s in (sub_00055400 at 0x55A68): it clears
// +377, picks one of 9 fixed tiles at random and gives up for the match if that tile
// holds a block or item. With the setting on, the call hooks here (recomp/hooks.py) keep
// it armed until a pickup is actually made, so it is tried on another random tile every
// frame until one is free (the try stops for good in the last 40 s, as before).
#include "config/config.h"
#include "ppu/recomp.h"
#include "runtime/kernel.h"

namespace {
constexpr u32 ARMED = 377;   // game object byte: try to place the pickup this frame
u32 g_tries = 0;             // this match's tries so far (for the log)
}

// The random pick (sub_00031388) right after the game cleared +377: armed again, in
// case the picked tile turns out to be covered. r27 = the game object.
void hook_wardrobe_pick(PPUContext& ctx, PPUFunc original) {
	if (g_tries++ == 0) LOG_INFO("wardrobe: placing the pickup");
	if (config::get().aggressive_costume_spawns) st8(u32(ctx.r[27]) + ARMED, 1);
	original(ctx);
}

// The pickup's constructor (sub_0007BAB8), reached only when the tile was free: placed,
// so no more tries this match.
void hook_wardrobe_placed(PPUContext& ctx, PPUFunc original) {
	u32 game = u32(ctx.r[27]);
	original(ctx);
	st8(game + ARMED, 0);
	LOG_INFO("wardrobe: pickup placed (try %u)", g_tries);
	g_tries = 0;
}
