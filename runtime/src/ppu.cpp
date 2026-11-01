// PPU thread management, indirect calls, traps, logging.
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL3/SDL_messagebox.h>

#include "debug/profiler.h"
#include "runtime/kernel.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

// --- logging ----------------------------------------------------------------
namespace {
std::mutex g_log_lock;
LogLevel g_log_min = LogLevel::info;
}

void log_set_level(LogLevel lvl) { g_log_min = lvl; }

static void log_v(LogLevel lvl, const char* fmt, va_list ap) {
	static const char* tags[] = {"T", "I", "W", "E"};
	std::lock_guard lock(g_log_lock);
	std::fprintf(stderr, "[%s] ", tags[int(lvl)]);
	std::vfprintf(stderr, fmt, ap);
	std::fputc('\n', stderr);
}

void log_msg(LogLevel lvl, const char* fmt, ...) {
	if (lvl < g_log_min) return;
	va_list ap;
	va_start(ap, fmt);
	log_v(lvl, fmt, ap);
	va_end(ap);
}

void fatal(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	va_list ap2;
	va_copy(ap2, ap);
	log_v(LogLevel::error, fmt, ap);
	va_end(ap);
	std::fflush(stderr);
	// A console opened by double-clicking the exe closes with the process, so also show
	// the error in a box when stderr is a console (not when it goes to a log file).
	if (isatty(fileno(stderr))) {
		char msg[1024];
		std::vsnprintf(msg, sizeof(msg), fmt, ap2);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Bomberman ULTRA", msg, nullptr);
	}
	va_end(ap2);
	std::_Exit(1);
}

// --- function lookup --------------------------------------------------------
namespace {
// Dense table indexed by (addr - lo) / 4 for indirect calls.
u32 g_fn_lo, g_fn_hi;
std::vector<PPUFunc> g_fn_table;

void build_fn_table() {
	g_fn_lo = g_ppu_funcs[0].addr;
	g_fn_hi = g_ppu_funcs[g_ppu_func_count - 1].addr + 4;
	g_fn_table.assign((g_fn_hi - g_fn_lo) / 4, nullptr);
	for (size_t i = 0; i < g_ppu_func_count; i++)
		g_fn_table[(g_ppu_funcs[i].addr - g_fn_lo) / 4] = g_ppu_funcs[i].fn;
}
} // namespace

// --- HLE trampolines ----------------------------------------------------------
// Fake code addresses in the unmapped first page range; only their descriptors
// live in guest memory, so guest code can hold and call them like any function pointer.
namespace {
constexpr u32 TRAMP_CODE_BASE = 0x1000, TRAMP_MAX = 1024;
std::mutex g_tramp_lock;
std::vector<void (*)(PPUContext&)> g_tramps;
u32 g_tramp_opd_block;
}

u32 hle_trampoline(void (*fn)(PPUContext&), const char* name) {
	std::lock_guard lock(g_tramp_lock);
	if (!g_tramp_opd_block) g_tramp_opd_block = vm::alloc(vm::MAIN_BASE, TRAMP_MAX * 8, 0x1000);
	u32 i = u32(g_tramps.size());
	if (i >= TRAMP_MAX) fatal("too many HLE trampolines");
	g_tramps.push_back(fn);
	u32 opd = g_tramp_opd_block + i * 8;
	st32(opd, TRAMP_CODE_BASE + i * 4);
	st32(opd + 4, g_image_toc);
	LOG_INFO("hle: trampoline %s at opd 0x%08x", name, opd);
	return opd;
}

void ppu_icall(PPUContext& ctx, u32 addr) {
	if (addr >= TRAMP_CODE_BASE && addr < TRAMP_CODE_BASE + TRAMP_MAX * 4) {
		u32 i = (addr - TRAMP_CODE_BASE) / 4;
		if (i < g_tramps.size()) {
			g_tramps[i](ctx);
			return;
		}
	}
	if (addr >= g_fn_lo && addr < g_fn_hi && !(addr & 3)) {
		if (PPUFunc fn = g_fn_table[(addr - g_fn_lo) / 4]) {
			fn(ctx);
			return;
		}
	}
	fatal("indirect call to 0x%08x: no recompiled function there (lr=0x%08llx)", addr,
	      (unsigned long long)ctx.lr);
}

void call_guest_opd(PPUContext& ctx, u32 opd) {
	u64 saved_r2 = ctx.r[2];
	ctx.r[2] = ld32(opd + 4);
	ppu_icall(ctx, ld32(opd));
	ctx.r[2] = saved_r2;
}

static void dump_guest_state(const PPUContext& ctx);

void ppu_trap(PPUContext& ctx, u32 pc, const char* why) {
	log_msg(LogLevel::error, "guest trap at 0x%08x: %s", pc, why);
	dump_guest_state(ctx);
	std::fflush(stderr);
	std::_Exit(1);
}

thread_local PPUCallTrace t_call_trace;

static void dump_call_trace() {
	std::string s;
	char buf[16];
	u32 n = std::min<u32>(t_call_trace.pos, 32);
	for (u32 i = 0; i < n; i++) {
		std::snprintf(buf, sizeof buf, "%s%08x", i % 8 ? " " : "\n  ", t_call_trace.addr[(t_call_trace.pos - 1 - i) & 63]);
		s += buf;
	}
	log_msg(LogLevel::error, "  recently entered functions (newest first):%s", s.c_str());
}

static void dump_guest_state(const PPUContext& ctx) {
	dump_call_trace();
	std::string regs;
	char buf[64];
	for (int i = 0; i < 32; i++) {
		std::snprintf(buf, sizeof buf, "%sr%-2d=%016llx", i % 4 ? "  " : "\n  ", i, (unsigned long long)ctx.r[i]);
		regs += buf;
	}
	log_msg(LogLevel::error, "  thread 0x%x lr=0x%08llx ctr=0x%08llx%s", ctx.thread_id,
	        (unsigned long long)ctx.lr, (unsigned long long)ctx.ctr, regs.c_str());
}

PPUContext* current_ctx_ptr();

#ifdef _WIN32
// Debugging aid: --watch-write <addr> reports the first write to that page.
static u32 g_watch_addr = 0;
u32 g_debug_watch_addr = 0;

void watch_write(u32 addr) {
	g_watch_addr = addr;
	DWORD old;
	VirtualProtect(g_base + (addr & ~0xFFFu), 0x1000, PAGE_READONLY, &old);
	LOG_INFO("debug: watching writes to page 0x%08x", addr & ~0xFFFu);
}

// Report host access violations inside guest memory as guest addresses.
static LONG CALLBACK guest_fault_handler(EXCEPTION_POINTERS* ep) {
	auto* rec = ep->ExceptionRecord;
	static thread_local bool t_stepping = false;
	if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_stepping) {
		// the write on the watched page has executed; arm the watch again
		t_stepping = false;
		DWORD old;
		VirtualProtect(g_base + (g_watch_addr & ~0xFFFu), 0x1000, PAGE_READONLY, &old);
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	if (g_watch_addr && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->ExceptionInformation[0] == 1) {
		auto host = reinterpret_cast<u8*>(rec->ExceptionInformation[1]);
		u32 page = g_watch_addr & ~0xFFFu;
		if (host >= g_base + page && host < g_base + page + 0x1000) {
			u32 guest = u32(host - g_base);
			if (guest < g_watch_addr || guest >= g_watch_addr + 64) {
				// some other write on the same page: let it through, single-step, re-protect
				DWORD old;
				VirtualProtect(g_base + page, 0x1000, PAGE_READWRITE, &old);
				ep->ContextRecord->EFlags |= 0x100;
				t_stepping = true;
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			log_msg(LogLevel::warn, "debug: write to watched address at guest 0x%08x", guest);
			g_watch_addr = 0;   // report once, then fall through to the full dump below
		}
	}
	if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && rec->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
		return EXCEPTION_CONTINUE_SEARCH;
	auto module = reinterpret_cast<u8*>(GetModuleHandleW(nullptr));
	{
		// host call stack as module offsets, for llvm-symbolizer (image base 0x140000000)
		std::string frames;
		CONTEXT c = *ep->ContextRecord;
		for (int i = 0; i < 12 && c.Rip; i++) {
			char buf[32];
			std::snprintf(buf, sizeof buf, " 0x%llx", (unsigned long long)(c.Rip - u64(module)) + 0x140000000ull);
			frames += buf;
			DWORD64 image_base;
			auto* fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
			if (!fn) break;
			void* handler_data;
			DWORD64 establisher;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &establisher, nullptr);
		}
		log_msg(LogLevel::error, "host stack:%s", frames.c_str());
	}
	if (rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
		log_msg(LogLevel::error, "host stack overflow (runaway guest recursion?)");
	} else {
		auto host = reinterpret_cast<u8*>(rec->ExceptionInformation[1]);
		const char* kind = rec->ExceptionInformation[0] == 1 ? "write" : "read";
		if (host >= g_base && host < g_base + (1ull << 32) + 0x10000)
			log_msg(LogLevel::error, "guest memory fault: %s at guest 0x%08llx (host rip %p)", kind,
			        (unsigned long long)(host - g_base), rec->ExceptionAddress);
		else
			log_msg(LogLevel::error, "host memory fault: %s at %p (host rip %p)", kind, host, rec->ExceptionAddress);
	}
	if (PPUContext* ctx = current_ctx_ptr()) dump_guest_state(*ctx);
	std::fflush(stderr);
	std::_Exit(3);
}
#endif

void install_fault_handler() {
#ifdef _WIN32
	AddVectoredExceptionHandler(1, guest_fault_handler);
#endif
}

u64 ppu_timebase() {
	// The PS3 timebase ticks at 79.8 MHz.
	using namespace std::chrono;
	static const auto start = steady_clock::now();
	auto ns = duration_cast<nanoseconds>(steady_clock::now() - start).count();
	return u64(ns) * 798 / 10000;
}

// --- TLS ----------------------------------------------------------------------
namespace {
ImageInfo g_image;
std::mutex g_tls_lock;
}

void tls_init(const ImageInfo& info) { g_image = info; }

u32 tls_alloc() {
	// 0x30-byte system area followed by a fresh copy of the TLS image
	u32 size = 0x30 + g_image.tls_memsz;
	u32 block = vm::alloc(vm::MAIN_BASE, size, 0x100);
	if (!block) return 0;
	if (g_image.tls_filesz)
		std::memcpy(vm_ptr(block + 0x30), vm_ptr(g_image.tls_addr), g_image.tls_filesz);
	return block;
}

// --- threads ------------------------------------------------------------------
namespace {

struct ThreadExit {
	u64 status;
};

struct GuestThread {
	u32 id;
	std::string name;
	PPUCallTrace* trace = nullptr;   // that thread's t_call_trace, for hang dumps
	std::unique_ptr<PPUContext> ctx;
	u64 arg = 0;                     // start argument (r3 at entry)
	std::thread host;
	bool joinable;
	bool finished = false;
	u64 status = 0;
	std::mutex lock;
	std::condition_variable done;
};

std::mutex g_threads_lock;
std::map<u32, std::shared_ptr<GuestThread>> g_threads;
u32 g_next_thread_id = 0x01000000;
thread_local PPUContext* t_ctx;
thread_local u64 t_start_arg;

std::unique_ptr<PPUContext> new_context(u32 stacksize, u32 tls_block) {
	auto ctx = std::make_unique<PPUContext>();
	std::memset(ctx.get(), 0, sizeof(PPUContext));
	ctx->stack_size = (stacksize + 0xFFF) & ~0xFFFu;
	ctx->stack_addr = vm::alloc(vm::STACK_BASE, ctx->stack_size, 0x1000);
	if (!ctx->stack_addr) fatal("out of guest stack memory");
	ctx->r[1] = ctx->stack_addr + ctx->stack_size - 0x70;
	ctx->r[13] = tls_block ? tls_block + 0x7030 : 0;
	return ctx;
}

void thread_body(std::shared_ptr<GuestThread> t, u32 entry_opd) {
	profiler::register_current_thread(t->name.c_str());
	t_ctx = t->ctx.get();
	t_start_arg = t->arg;
	t->trace = &t_call_trace;
	PPUContext& ctx = *t->ctx;
	u64 status = 0;
	try {
		ctx.r[2] = ld32(entry_opd + 4);
		ctx.lr = 0;
		ppu_icall(ctx, ld32(entry_opd));
		status = ctx.r[3];
	} catch (const ThreadExit& e) {
		status = e.status;
	}
	LOG_INFO("thread 0x%x '%s' exited (status 0x%llx)", t->id, t->name.c_str(), (unsigned long long)status);
	std::lock_guard lock(t->lock);
	t->finished = true;
	t->status = status;
	t->done.notify_all();
}

} // namespace

PPUContext& attach_guest_context(const char* name, u32 stacksize) {
	auto t = std::make_shared<GuestThread>();
	t->name = name;
	t->ctx = new_context(stacksize, tls_alloc());
	t->ctx->r[2] = g_image_toc;
	{
		std::lock_guard lock(g_threads_lock);
		t->id = g_next_thread_id++;
		g_threads[t->id] = t;
	}
	t->ctx->thread_id = t->id;
	t_ctx = t->ctx.get();
	t->trace = &t_call_trace;
	profiler::register_current_thread(name);
	LOG_INFO("thread 0x%x '%s' attached (host thread)", t->id, name);
	return *t->ctx;
}

void dump_all_threads() {
	std::lock_guard lock(g_threads_lock);
	for (const auto& [id, t] : g_threads) {
		std::string s;
		if (t->trace) {
			char buf[16];
			u32 pos = t->trace->pos, n = std::min<u32>(pos, 12);
			for (u32 i = 0; i < n; i++) {
				std::snprintf(buf, sizeof buf, " %08x", t->trace->addr[(pos - 1 - i) & 63]);
				s += buf;
			}
		}
		log_msg(LogLevel::warn, "  thread 0x%x %-24s lr=0x%08llx recent:%s", id, t->name.c_str(),
		        (unsigned long long)t->ctx->lr, s.c_str());
	}
}

PPUContext& current_ctx() { return *t_ctx; }
PPUContext* current_ctx_ptr() { return t_ctx; }
u64 current_thread_arg() { return t_start_arg; }

u32 thread_create(u32 entry_opd, u64 arg, s32 prio, u32 stacksize, const char* name, bool joinable) {
	auto t = std::make_shared<GuestThread>();
	t->name = name ? name : "";
	t->joinable = joinable;
	u32 tls = tls_alloc();
	t->ctx = new_context(stacksize ? stacksize : 0x4000, tls);
	t->ctx->r[3] = arg;
	t->arg = arg;
	{
		std::lock_guard lock(g_threads_lock);
		t->id = g_next_thread_id++;
		g_threads[t->id] = t;
	}
	t->ctx->thread_id = t->id;
	LOG_INFO("thread 0x%x '%s' created (entry opd 0x%08x, prio %d, stack 0x%x)", t->id, t->name.c_str(),
	         entry_opd, prio, t->ctx->stack_size);
	t->host = std::thread(thread_body, t, entry_opd);
	if (!joinable) t->host.detach();
	return t->id;
}

void thread_exit(PPUContext& ctx, u64 status) {
	(void)ctx;
	throw ThreadExit{status};
}

bool thread_join(u32 id, u64* status) {
	std::shared_ptr<GuestThread> t;
	{
		std::lock_guard lock(g_threads_lock);
		auto it = g_threads.find(id);
		if (it == g_threads.end()) return false;
		t = it->second;
	}
	{
		std::unique_lock lock(t->lock);
		t->done.wait(lock, [&] { return t->finished; });
		if (status) *status = t->status;
	}
	if (t->host.joinable()) t->host.join();
	std::lock_guard lock(g_threads_lock);
	g_threads.erase(id);
	return true;
}

void run_main_thread(const ImageInfo& info) {
	install_fault_handler();
	build_fn_table();
	tls_init(info);

	auto t = std::make_shared<GuestThread>();
	t->name = "main_thread";
	t->ctx = new_context(info.primary_stacksize, 0);
	t->id = g_next_thread_id++;
	t->ctx->thread_id = t->id;
	{
		std::lock_guard lock(g_threads_lock);
		g_threads[t->id] = t;
	}
	PPUContext& ctx = *t->ctx;
	t_ctx = &ctx;
	t->trace = &t_call_trace;
	profiler::register_current_thread("main_thread");

	// argv ={ "/app_home/EBOOT.BIN", NULL }, envp = { NULL }, stored at the top of the stack
	const char* argv0 = "/app_home/EBOOT.BIN";
	ctx.r[1] -= 0x40;
	u32 args = u32(ctx.r[1]) + 0x10;
	std::strcpy(vm::str(args + 0x20), argv0);
	st64(args, args + 0x20);   // argv[0]
	st64(args + 8, 0);         // argv[1]
	st64(args + 16, 0);        // envp[0]
	ctx.r[1] -= 0x70;

	// sys_initialize_tls equivalent: give the main thread its TLS block
	u32 tls = tls_alloc();
	ctx.r[13] = tls + 0x7030;

	ctx.r[3] = 1;               // argc
	ctx.r[4] = args;            // argv
	ctx.r[5] = args + 16;       // envp
	ctx.r[6] = 0;               // envc
	ctx.r[7] = t->id;
	ctx.r[8] = info.tls_addr;
	ctx.r[9] = info.tls_filesz;
	ctx.r[10] = info.tls_memsz;
	ctx.r[11] = g_image_entry;
	ctx.r[12] = info.malloc_pagesize;
	ctx.r[2] = g_image_toc;

	LOG_INFO("starting main thread at 0x%08x (toc 0x%08x)", ld32(g_image_entry), g_image_toc);
	try {
		ppu_icall(ctx, ld32(g_image_entry));
		LOG_INFO("main thread returned 0x%llx", (unsigned long long)ctx.r[3]);
	} catch (const ThreadExit& e) {
		LOG_INFO("main thread exited (status 0x%llx)", (unsigned long long)e.status);
	}
}
