#include <map>
#include <mutex>
#include <vector>

#include "runtime/kernel.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

u8* g_base = nullptr;

namespace vm {
namespace {

// Reserve 4 GiB plus slack so 16-byte accesses at the top of the space stay inside.
constexpr u64 RESERVE_SIZE = (1ull << 32) + 0x10000;
constexpr u32 PAGE = 0x1000;

struct Region {
	u32 base, size;
	std::map<u32, u32> used; // addr -> size
};

std::mutex g_lock;
bool g_write_watch = false;

// Pages written by means write watch may not see (file reads); reported once by take_written_pages().
std::mutex g_noted_lock;
std::vector<u64> g_noted;   // bit per page
bool g_any_noted = false;

Region g_regions[] = {
	{MAIN_BASE, MAIN_SIZE, {}},
	{USER_BASE, USER_SIZE, {}},
	{RSX_BASE, RSX_SIZE, {}},
	{STACK_BASE, STACK_SIZE, {}},
};

Region& region(u32 base) {
	for (auto& r : g_regions)
		if (r.base == base) return r;
	fatal("vm: no region at 0x%08x", base);
}

} // namespace

void init() {
#ifdef _WIN32
	// write watch lets caches of guest data (textures) ask which pages changed
	g_base = static_cast<u8*>(VirtualAlloc(nullptr, RESERVE_SIZE, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS));
	g_write_watch = g_base != nullptr;
	if (!g_base) g_base = static_cast<u8*>(VirtualAlloc(nullptr, RESERVE_SIZE, MEM_RESERVE, PAGE_NOACCESS));
#else
	void* p = mmap(nullptr, RESERVE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	g_base = p == MAP_FAILED ? nullptr : static_cast<u8*>(p);
#endif
	if (!g_base) fatal("vm: failed to reserve the 4 GiB guest address space");
	g_noted.assign((1ull << 32) / PAGE / 64, 0);
	LOG_INFO("vm: guest memory at host %p%s", g_base, g_write_watch ? " (write tracking on)" : "");
}

bool write_tracking() { return g_write_watch; }

void note_written(u32 addr, u32 size) {
	if (!size) return;
	std::lock_guard lock(g_noted_lock);
	for (u64 p = addr / PAGE; p <= (u64(addr) + size - 1) / PAGE; p++) g_noted[p >> 6] |= 1ull << (p & 63);
	g_any_noted = true;
}

bool take_written_pages(u32 addr, u32 size, std::vector<u32>& pages) {
	pages.clear();
	if (!g_write_watch || !size) return g_write_watch;
	u64 lo = addr & ~u64(PAGE - 1), hi = (u64(addr) + size + PAGE - 1) & ~u64(PAGE - 1);
#ifdef _WIN32
	static thread_local std::vector<void*> hits;
	hits.resize((hi - lo) / PAGE);
	ULONG_PTR count = hits.size();
	DWORD granularity = 0;
	if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_base + lo, SIZE_T(hi - lo), hits.data(), &count, &granularity))
		return false;
	for (ULONG_PTR i = 0; i < count; i++) pages.push_back(u32((static_cast<u8*>(hits[i]) - g_base) / PAGE));
#endif
	std::lock_guard lock(g_noted_lock);
	if (g_any_noted)
		for (u64 p = lo / PAGE; p < hi / PAGE; p++)
			if (g_noted[p >> 6] >> (p & 63) & 1) {
				g_noted[p >> 6] &= ~(1ull << (p & 63));
				pages.push_back(u32(p));
			}
	return true;
}

void commit(u32 addr, u32 size) {
	u64 lo = addr & ~u64(PAGE - 1);
	u64 hi = (u64(addr) + size + PAGE - 1) & ~u64(PAGE - 1);
#ifdef _WIN32
	if (!VirtualAlloc(g_base + lo, hi - lo, MEM_COMMIT, PAGE_READWRITE))
		fatal("vm: commit 0x%08x+0x%x failed", addr, size);
#else
	if (mprotect(g_base + lo, hi - lo, PROT_READ | PROT_WRITE))
		fatal("vm: commit 0x%08x+0x%x failed", addr, size);
#endif
}

void reserve(u32 addr, u32 size) {
	std::lock_guard lock(g_lock);
	for (auto& r : g_regions)
		if (addr >= r.base && addr - r.base < r.size) {
			u32 lo = addr & ~(PAGE - 1);
			r.used[lo] = ((addr + size + PAGE - 1) & ~(PAGE - 1)) - lo;
			return;
		}
}

u32 alloc(u32 region_base, u32 size, u32 align) {
	std::lock_guard lock(g_lock);
	Region& r = region(region_base);
	size = (size + PAGE - 1) & ~(PAGE - 1);
	u64 cand = r.base;
	for (auto [a, s] : r.used) {
		cand = (cand + align - 1) & ~u64(align - 1);
		if (cand + size <= a) break;
		cand = u64(a) + s;
	}
	cand = (cand + align - 1) & ~u64(align - 1);
	if (cand + size > u64(r.base) + r.size) return 0;
	r.used[u32(cand)] = size;
	commit(u32(cand), size);
	std::memset(g_base + cand, 0, size);
	return u32(cand);
}

void dealloc(u32 region_base, u32 addr) {
	std::lock_guard lock(g_lock);
	region(region_base).used.erase(addr);
}

} // namespace vm
