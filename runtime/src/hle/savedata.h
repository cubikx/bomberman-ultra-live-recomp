// Save data (cellSaveData) seen from the rest of the runtime.
#pragma once

#include <string>
#include <vector>

#include "runtime/kernel.h"

namespace savedata {

// Called with a save file's whole contents: after it is read from disk and before
// the game sees it (the hook may change `data`), and before it is written (the
// hook observes what the game saves). `dir` is the save directory name.
using Hook = void (*)(const std::string& dir, const std::string& file, std::vector<u8>& data, bool write);
void set_hook(Hook hook);

// Called when an auto load or save of `dir` has finished, successfully or not.
using DoneHook = void (*)(const std::string& dir, bool save, bool ok);
void set_done_hook(DoneHook hook);

} // namespace savedata
