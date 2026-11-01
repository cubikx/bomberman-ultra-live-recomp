// Match HUD position: settings.ini hud_raise. The bar across the top of a match (player
// heads, win counts, clock and time) overlaps the top of the arena; it is moved up by
// that many pixels (of the game's 1280x720).
//
// The HUD object (vtable 0x1B03D0) draws each piece as a 4-vertex quad: it writes the
// vertices (24 bytes each, x / y as floats in clip space, y at +4) and then submits
// them with sub_0009EEA8(prim, count). At the call sites hooked here (recomp/hooks.py),
// r5 still holds the first vertex, so the hook moves the quad up before it is submitted.
// The sites, all inside the bar (they also follow the bar's slide-in offset, +244):
//   0x660DC / 0x661BC  head shadow / head (sub_00065D90, the HUD's draw)
//   0x66340            the marker over a head (same function)
//   0x6434C            digits: wins and time (sub_00064178)
//   0x6656C            the clock box
// Left alone: the name bubbles that follow the players (sub_00063238 / sub_00063B18) and
// the mid-screen messages (sub_00064DD8).
#include "config/config.h"
#include "ppu/recomp.h"
#include "runtime/kernel.h"

void hook_hud_quad(PPUContext& ctx, PPUFunc original) {
	if (u32 raise = config::get().hud_raise) {
		float dy = float(raise) / 360.0f;   // pixels of 720 -> clip space (2 / 720 per pixel)
		u32 v = u32(ctx.r[5]);
		for (u32 i = 0, n = u32(ctx.r[4]); i < n; i++) stf32(v + i * 24 + 4, ldf32(v + i * 24 + 4) + dy);
	}
	original(ctx);
}
