// Runtime services shared by the loader, syscalls and HLE modules.
#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "ppu/recomp.h"

// --- logging ----------------------------------------------------------------
enum class LogLevel { trace, info, warn, error };
void log_msg(LogLevel lvl, const char* fmt, ...);
#define LOG_TRACE(...) log_msg(LogLevel::trace, __VA_ARGS__)
#define LOG_INFO(...) log_msg(LogLevel::info, __VA_ARGS__)
#define LOG_WARN(...) log_msg(LogLevel::warn, __VA_ARGS__)
#define LOG_ERROR(...) log_msg(LogLevel::error, __VA_ARGS__)
[[noreturn]] void fatal(const char* fmt, ...);

// --- debug switches ---------------------------------------------------------
// Environment variable as an on/off switch: unset, empty or "0" means off
// (PowerShell 7.5 keeps variables set to '' instead of removing them).
bool env_flag(const char* name);
// Numeric environment variable; `fallback` when unset, empty or not a number.
u64 env_u64(const char* name, u64 fallback = 0);

// --- host timing ------------------------------------------------------------
// Sleeps accurate to well under a millisecond (std::this_thread::sleep_* round up
// to the OS timer tick on Windows, up to ~15 ms).
void precise_sleep_us(u64 us);
void precise_sleep_until(std::chrono::steady_clock::time_point t);

// --- guest memory map -------------------------------------------------------
namespace vm {
// Fixed regions, mirroring the real console's user address space.
constexpr u32 MAIN_BASE = 0x00010000, MAIN_SIZE = 0x2FFF0000;   // ELF + small allocations
constexpr u32 USER_BASE = 0x30000000, USER_SIZE = 0x10000000;   // sys_memory_allocate
constexpr u32 RSX_BASE = 0xC0000000, RSX_SIZE = 0x10000000;     // RSX local memory
constexpr u32 STACK_BASE = 0xD0000000, STACK_SIZE = 0x10000000; // thread stacks
constexpr u32 USER_MEMORY_TOTAL = 0x0D500000;                   // what the game is told it has

void init();
// Commit [addr, addr+size) as read/write. Used for fixed mappings such as ELF segments.
void commit(u32 addr, u32 size);
// Mark a fixed mapping (e.g. an ELF segment) as used so alloc() never hands it out.
void reserve(u32 addr, u32 size);
// Allocate from one of the regions above (first fit, page-aligned).
u32 alloc(u32 region_base, u32 size, u32 align = 0x10000);
void dealloc(u32 region_base, u32 addr);

// Write tracking (Windows write watch over the whole guest space), for caches of guest data.
bool write_tracking();
// The pages (addr / 0x1000) of [addr, addr+size) written since the last call that covered
// them; resets them. False if tracking is unavailable.
bool take_written_pages(u32 addr, u32 size, std::vector<u32>& pages);
// Report a write made where tracking may not see it (file reads into guest memory).
void note_written(u32 addr, u32 size);

inline char* str(u32 ea) { return reinterpret_cast<char*>(vm_ptr(ea)); }
}

// --- loader -----------------------------------------------------------------
struct ImageInfo {
	u32 tls_addr = 0, tls_filesz = 0, tls_memsz = 0;
	u32 primary_prio = 1001, primary_stacksize = 0x10000;
	u32 malloc_pagesize = 0x100000;
	u32 data_end = 0;
};
ImageInfo load_image(const std::filesystem::path& eboot);

// --- paths ------------------------------------------------------------------
struct Paths {
	std::filesystem::path game;      // NPUB30051/ (PARAM.SFO, USRDIR, ...)
	std::filesystem::path user;      // user/
	std::filesystem::path settings;  // user/settings
	std::filesystem::path saves;     // user/saves
	std::filesystem::path trophies;  // user/trophies
	std::filesystem::path live;      // live/ (Bomberman LIVE files from tools/media/make_live.py)
};
extern Paths g_paths;
// Map a guest path ("/dev_hdd0/game/NPUB30051/USRDIR/x", "/app_home/x", ...) to the host.
std::filesystem::path host_path(const std::string& guest);

// --- threads ----------------------------------------------------------------
PPUContext& current_ctx();
// The argument the calling guest thread was started with (0 for host-attached threads).
u64 current_thread_arg();
// Start a guest thread running the function behind descriptor `entry_opd`.
u32 thread_create(u32 entry_opd, u64 arg, s32 prio, u32 stacksize, const char* name, bool joinable);
[[noreturn]] void thread_exit(PPUContext& ctx, u64 status);
bool thread_join(u32 id, u64* status);
// Call the guest function behind a descriptor from host code (callbacks).
void call_guest_opd(PPUContext& ctx, u32 opd);
// Give the calling host thread its own guest context (stack, TLS, thread id) so
// it can run guest callbacks, e.g. the RSX thread calling the flip handler.
PPUContext& attach_guest_context(const char* name, u32 stacksize);
// Guest-callable descriptor (OPD) whose code is the host function `fn`. Used for
// function pointers the system hands to the game, such as the GCM buffer callback.
u32 hle_trampoline(void (*fn)(PPUContext&), const char* name);
void run_main_thread(const ImageInfo& info);
// Log every guest thread's most recently entered functions (hang diagnosis).
void dump_all_threads();
// Debugging: report the first write to the page holding `addr` (--watch-write).
extern u32 g_debug_watch_addr;
void watch_write(u32 addr);
void tls_init(const ImageInfo& info);
u32 tls_alloc();

// Stack scratch space for HLE functions that must pass pointers to guest callbacks.
struct GuestStackAlloc {
	PPUContext& ctx;
	u32 size;
	u32 addr;
	GuestStackAlloc(PPUContext& c, u32 sz) : ctx(c), size((sz + 15) & ~15u) {
		ctx.r[1] -= size;
		addr = u32(ctx.r[1]);
	}
	~GuestStackAlloc() { ctx.r[1] += size; }
};

// --- HLE --------------------------------------------------------------------
using HLEFunc = void (*)(PPUContext&);
struct HLERegistrar {
	HLERegistrar(const char* module, const char* name, HLEFunc fn);
};
void hle_bind_imports();

// Defines `void hle_<name>(PPUContext& ctx)` and registers it under module::name.
#define HLE_FUNC(module, name)                                           \
	static void hle_##name(PPUContext& ctx);                             \
	static HLERegistrar hle_reg_##name(#module, #name, hle_##name);      \
	static void hle_##name([[maybe_unused]] PPUContext& ctx)

// Argument / return helpers for HLE functions (PPC64 ELF ABI: r3..r10, return in r3)
inline u64 arg(PPUContext& ctx, int i) { return ctx.r[3 + i]; }
inline u32 arg32(PPUContext& ctx, int i) { return u32(ctx.r[3 + i]); }
inline void ret(PPUContext& ctx, u64 v) { ctx.r[3] = v; }

// Common CELL error codes
constexpr u32 CELL_OK = 0;
constexpr u32 CELL_EAGAIN = 0x80010001, CELL_EINVAL = 0x80010002, CELL_ENOSYS = 0x80010003,
              CELL_ENOMEM = 0x80010004, CELL_ESRCH = 0x80010005, CELL_ENOENT = 0x80010006,
              CELL_EDEADLK = 0x80010008, CELL_EPERM = 0x80010009, CELL_EBUSY = 0x8001000A,
              CELL_ETIMEDOUT = 0x8001000B, CELL_EFAULT = 0x8001000D, CELL_EBADF = 0x8001002A,
              CELL_EIO = 0x8001002B, CELL_EEXIST = 0x80010014, CELL_ENOTDIR = 0x8001002F,
              CELL_EISDIR = 0x80010030, CELL_ECANCELED = 0x80010029;
