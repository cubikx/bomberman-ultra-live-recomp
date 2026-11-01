// Sampling profiler: a host thread suspends each registered thread in turn,
// reads its context, walks a few frames with the x64 unwind tables, resumes it,
// and only then records the sample (allocating while a thread is suspended can
// deadlock on a lock it holds).
#include "debug/profiler.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/kernel.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")   // timeBeginPeriod: 1 ms sleeps
#endif

namespace profiler {
namespace {

constexpr int MAX_FRAMES = 24;

struct Thread {
	std::string name;
#ifdef _WIN32
	HANDLE handle = nullptr;
#endif
	u64 samples = 0;
	std::map<std::vector<u64>, u64> stacks;
};

std::mutex g_lock;
std::vector<Thread*> g_threads;

#ifdef _WIN32
int walk(HANDLE h, u64* out) {
	CONTEXT c{};
	c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if (!GetThreadContext(h, &c)) return 0;
	int n = 0;
	while (n < MAX_FRAMES && c.Rip) {
		out[n++] = c.Rip;
		DWORD64 base = 0;
		PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
		if (!fn) {
			// leaf function: the return address is on top of the stack
			c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
			c.Rsp += 8;
		} else {
			void* handler_data = nullptr;
			DWORD64 frame = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &handler_data, &frame, nullptr);
		}
	}
	return n;
}

std::string module_of(u64 addr, u64& offset) {
	HMODULE mod = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCSTR>(addr), &mod)) {
		offset = addr;
		return "?";
	}
	char path[MAX_PATH] = {};
	GetModuleFileNameA(mod, path, MAX_PATH);
	offset = addr - reinterpret_cast<u64>(mod);
	std::string p = path;
	size_t slash = p.find_last_of("\\/");
	return slash == std::string::npos ? p : p.substr(slash + 1);
}
#endif

void write_report() {
	FILE* f = std::fopen("profile.txt", "w");
	if (!f) return;
	std::map<u64, std::string> names;   // address -> "module+0xoffset"
	auto name = [&](u64 a) -> const std::string& {
		auto it = names.find(a);
		if (it != names.end()) return it->second;
		u64 off = 0;
		std::string m = module_of(a, off);
		char buf[32];
		std::snprintf(buf, sizeof buf, "+0x%llx", (unsigned long long)off);
		return names[a] = m + buf;
	};
	std::lock_guard lock(g_lock);
	for (Thread* t : g_threads) {
		if (!t->samples) continue;
		std::fprintf(f, "thread %s %llu\n", t->name.c_str(), (unsigned long long)t->samples);
		for (const auto& [stack, count] : t->stacks) {
			std::fprintf(f, "%llu", (unsigned long long)count);
			for (u64 a : stack) std::fprintf(f, " %s", name(a).c_str());
			std::fprintf(f, "\n");
		}
	}
	std::fclose(f);
	LOG_INFO("profiler: wrote profile.txt");
}

void sampler(double delay_s, double duration_s) {
#ifdef _WIN32
	using namespace std::chrono;
	std::this_thread::sleep_for(duration<double>(delay_s));
	LOG_INFO("profiler: sampling for %.0f s", duration_s);
	timeBeginPeriod(1);
	auto end = steady_clock::now() + duration<double>(duration_s);
	u64 frames[MAX_FRAMES];
	while (steady_clock::now() < end) {
		std::vector<Thread*> threads;
		{
			std::lock_guard lock(g_lock);
			threads = g_threads;
		}
		for (Thread* t : threads) {
			if (SuspendThread(t->handle) == DWORD(-1)) continue;
			int n = walk(t->handle, frames);
			ResumeThread(t->handle);
			if (!n) continue;
			std::lock_guard lock(g_lock);
			t->samples++;
			t->stacks[std::vector<u64>(frames, frames + n)]++;
		}
		std::this_thread::sleep_for(milliseconds(1));
	}
	timeEndPeriod(1);
	write_report();
#else
	(void)delay_s;
	(void)duration_s;
#endif
}

bool g_enabled = false;

} // namespace

void register_current_thread(const char* name) {
#ifdef _WIN32
	auto* t = new Thread;   // lives for the whole run
	t->name = name;
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->handle,
	                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
	std::lock_guard lock(g_lock);
	g_threads.push_back(t);
#else
	(void)name;
#endif
}

void start(double delay_s, double duration_s) {
	if (g_enabled) return;
	g_enabled = true;
	std::thread(sampler, delay_s, duration_s).detach();
}

} // namespace profiler
