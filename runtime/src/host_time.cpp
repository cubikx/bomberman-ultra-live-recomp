// Host helpers: debug switches from the environment, and precise sleeping. On
// Windows a high-resolution waitable timer covers all but the last ~0.25 ms of a
// sleep, which is spun with yields; elsewhere the standard sleep is accurate.
#include <cstdlib>
#include <cstring>
#include <thread>

#include "runtime/kernel.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace std::chrono;

bool env_flag(const char* name) {
	const char* v = std::getenv(name);
	return v && *v && std::strcmp(v, "0") != 0;
}

u64 env_u64(const char* name, u64 fallback) {
	const char* v = std::getenv(name);
	if (!v || !*v) return fallback;
	char* end = nullptr;
	u64 n = std::strtoull(v, &end, 0);
	return end && *end == 0 ? n : fallback;
}

void precise_sleep_until(steady_clock::time_point t) {
#ifdef _WIN32
	constexpr auto SPIN = microseconds(250);
	thread_local HANDLE timer =
		CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	auto now = steady_clock::now();
	if (t - now > SPIN && timer) {
		LARGE_INTEGER due;
		due.QuadPart = -duration_cast<nanoseconds>(t - now - SPIN).count() / 100;   // relative, 100 ns units
		if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
	}
	while (steady_clock::now() < t) std::this_thread::yield();
#else
	std::this_thread::sleep_until(t);
#endif
}

void precise_sleep_us(u64 us) {
	if (!us) {
		std::this_thread::yield();
		return;
	}
	precise_sleep_until(steady_clock::now() + microseconds(us));
}
