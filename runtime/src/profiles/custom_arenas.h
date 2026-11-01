// Custom Arenas (custom_arenas.cpp): the 3 saved match setups of the Xbox 360 version,
// kept for the whole game; the Save Custom Settings screen (save_match_screen.cpp).
#pragma once
#include <array>
#include <cstdint>

#include "ppu/recomp.h"

namespace custom_arenas {

constexpr int SLOTS = 3;
using Slot = std::array<u32, 7>;   // the 360's 7 words, see custom_arenas.cpp

Slot snapshot(PPUContext& ctx, u32 powerups);   // game thread
void set_pending(const Slot& words);            // what the save screen will save
const Slot& pending();
void save(int slot, const Slot& words);
void erase(int slot);
int32_t arena(int slot);      // arena id in slot 0-2, -1 when open
u32 thumbnail(int32_t id);    // guest UTF-16BE picture path of an arena, or 0

} // namespace custom_arenas
