// CPU players: see bot.cpp.
#pragma once

#include "input/input.h"
#include "runtime/kernel.h"

namespace bot {

void game_thread_tick();   // game main thread, from cellPadGetData

// What a bot-driven pad port reports; false for ports no bot drives.
bool pad(u32 port, input::PadState& out);

} // namespace bot
