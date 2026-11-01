// Built-in sampling profiler (debugging aid, Windows only for now).
//
// --profile <start_s>:<seconds> samples every registered thread ~1000x/s for
// <seconds>, starting <start_s> after launch, and writes profile.txt: per-thread
// sample counts and call stacks as module+offset. tools/dbg/profile.py
// symbolizes and summarizes it.
#pragma once

namespace profiler {

// Call on each thread worth profiling (guest threads, RSX, window).
void register_current_thread(const char* name);
void start(double delay_s, double duration_s);

} // namespace profiler
