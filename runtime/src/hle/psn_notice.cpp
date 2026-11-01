// Offline build: the game's "not connected to the PlayStation Network" warning
// (IDS_POPUP_XBOX_LIVE_NOT_CONNECTED: "Game scores will not be posted to the
// leaderboards...") never applies, since this build never connects to PSN.
#include "ppu/recomp.h"
#include "runtime/kernel.h"

// Player Select opens it at 0x129484 when not signed in (sub_000478C8()+6 == 0), its
// show byte (+60) is set and the countdown at 0x1D76B8 runs out, with
// sub_000EB670(*0x1E0378, 0, text, 0 = Ok only, 255). The result is unused and the
// game's bookkeeping after the call (countdown reset, show byte cleared) still runs,
// so only the call is skipped.
// Left alone: the same text with IDS_POPUP_NETWORK_DISCONNECT_TITLE (0x11C248, online
// flow) and the session tick's copy (0x487A0, connection lost).
// Revisit once LAN / matchmaking replaces PSN.
void hook_psn_notice(PPUContext& ctx, PPUFunc original) {
	(void)original;
	static bool logged = false;
	if (!logged) {
		logged = true;
		LOG_INFO("hle: skipped the 'not connected to PSN' popup");
	}
	ctx.r[3] = 0;
}
