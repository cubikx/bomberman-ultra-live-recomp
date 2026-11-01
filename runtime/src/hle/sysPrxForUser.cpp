// sysPrxForUser: lightweight mutexes, PPU threads, process control, time.
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

#include "../objects.h"

namespace {

// sys_lwmutex_t lives in guest memory; we keep the real lock on the host side
// and store its id in the guest struct's sleep_queue field (offset 16).
struct LwMutex : KernelObject {
	std::mutex m;
	std::condition_variable cv;
	u32 owner = 0;
	u32 count = 0;
	bool recursive = false;
};

std::shared_ptr<LwMutex> lwmutex(u32 guest) {
	return kobjects().get<LwMutex>(ld32(guest + 16));
}

constexpr u32 SYS_SYNC_RECURSIVE = 0x10;

} // namespace

HLE_FUNC(sysPrxForUser, sys_initialize_tls) {
	// done by the loader before the entry point runs
	ret(ctx, CELL_OK);
}

HLE_FUNC(sysPrxForUser, sys_lwmutex_create) {
	u32 mtx = arg32(ctx, 0), attr = arg32(ctx, 1);
	auto m = std::make_shared<LwMutex>();
	m->recursive = ld32(attr + 4) == SYS_SYNC_RECURSIVE;
	std::memset(vm_ptr(mtx), 0, 24);
	st32(mtx + 8, ld32(attr) | ld32(attr + 4));
	st32(mtx + 16, kobjects().add(m));
	ret(ctx, CELL_OK);
}

HLE_FUNC(sysPrxForUser, sys_lwmutex_destroy) {
	u32 id = ld32(arg32(ctx, 0) + 16);
	ret(ctx, kobjects().remove(id) ? CELL_OK : CELL_ESRCH);
}

HLE_FUNC(sysPrxForUser, sys_lwmutex_lock) {
	auto m = lwmutex(arg32(ctx, 0));
	if (!m) return ret(ctx, CELL_ESRCH);
	u64 timeout = arg(ctx, 1);
	std::unique_lock lock(m->m);
	if (m->owner == ctx.thread_id) {
		if (!m->recursive) return ret(ctx, CELL_EDEADLK);
		m->count++;
		return ret(ctx, CELL_OK);
	}
	auto free = [&] { return m->owner == 0; };
	if (timeout) {
		if (!m->cv.wait_for(lock, std::chrono::microseconds(timeout), free)) return ret(ctx, CELL_ETIMEDOUT);
	} else {
		m->cv.wait(lock, free);
	}
	m->owner = ctx.thread_id;
	m->count = 1;
	ret(ctx, CELL_OK);
}

HLE_FUNC(sysPrxForUser, sys_lwmutex_trylock) {
	auto m = lwmutex(arg32(ctx, 0));
	if (!m) return ret(ctx, CELL_ESRCH);
	std::lock_guard lock(m->m);
	if (m->owner == ctx.thread_id && m->recursive) {
		m->count++;
		return ret(ctx, CELL_OK);
	}
	if (m->owner) return ret(ctx, CELL_EBUSY);
	m->owner = ctx.thread_id;
	m->count = 1;
	ret(ctx, CELL_OK);
}

HLE_FUNC(sysPrxForUser, sys_lwmutex_unlock) {
	auto m = lwmutex(arg32(ctx, 0));
	if (!m) return ret(ctx, CELL_ESRCH);
	std::lock_guard lock(m->m);
	if (m->owner != ctx.thread_id) return ret(ctx, CELL_EPERM);
	if (--m->count == 0) {
		m->owner = 0;
		m->cv.notify_one();
	}
	ret(ctx, CELL_OK);
}

HLE_FUNC(sysPrxForUser, sys_ppu_thread_create) {
	u32 id_out = arg32(ctx, 0), entry = arg32(ctx, 1);
	u64 thread_arg = arg(ctx, 2);
	s32 prio = s32(arg(ctx, 3));
	u32 stacksize = arg32(ctx, 4);
	u64 flags = arg(ctx, 5);
	const char* name = arg32(ctx, 6) ? vm::str(arg32(ctx, 6)) : "";
	u32 id = thread_create(entry, thread_arg, prio, stacksize, name, flags & 1);
	st64(id_out, id);
	ret(ctx, CELL_OK);
}

HLE_FUNC(sysPrxForUser, sys_ppu_thread_exit) {
	thread_exit(ctx, arg(ctx, 0));
}

HLE_FUNC(sysPrxForUser, sys_ppu_thread_get_id) {
	st64(arg32(ctx, 0), ctx.thread_id);
	ret(ctx, CELL_OK);
}

HLE_FUNC(sysPrxForUser, sys_process_exit) {
	LOG_INFO("sys_process_exit(%d)", s32(arg(ctx, 0)));
	std::fflush(stdout);
	std::exit(int(arg(ctx, 0)));
}

HLE_FUNC(sysPrxForUser, _sys_process_atexitspawn) { ret(ctx, CELL_OK); }
HLE_FUNC(sysPrxForUser, _sys_process_at_Exitspawn) { ret(ctx, CELL_OK); }
HLE_FUNC(sysPrxForUser, sys_prx_exitspawn_with_level) { ret(ctx, CELL_OK); }
HLE_FUNC(sysPrxForUser, sys_prx_register_library) { ret(ctx, CELL_OK); }
HLE_FUNC(sysPrxForUser, sys_spu_image_close) { ret(ctx, CELL_OK); }

HLE_FUNC(sysPrxForUser, sys_time_get_system_time) {
	using namespace std::chrono;
	static const auto start = steady_clock::now();
	ret(ctx, u64(duration_cast<microseconds>(steady_clock::now() - start).count()));
}
