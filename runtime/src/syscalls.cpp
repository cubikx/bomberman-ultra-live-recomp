// lv2 system calls made directly by the game's statically linked SDK code.
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <thread>

#include "objects.h"

ObjectTable& kobjects() {
	static ObjectTable t;
	return t;
}

namespace {

using namespace std::chrono;

// timeout in microseconds, 0 = wait forever
template <typename Lock, typename Pred>
bool wait_for(std::condition_variable& cv, Lock& lock, u64 timeout_us, Pred pred) {
	if (!timeout_us) {
		cv.wait(lock, pred);
		return true;
	}
	return cv.wait_for(lock, microseconds(timeout_us), pred);
}

// --- semaphores ---------------------------------------------------------------
struct Semaphore : KernelObject {
	std::mutex m;
	std::condition_variable cv;
	s32 count, max;
};

void sys_semaphore_create(PPUContext& ctx) {
	auto s = std::make_shared<Semaphore>();
	s->count = s32(arg(ctx, 2));
	s->max = s32(arg(ctx, 3));
	st32(arg32(ctx, 0), kobjects().add(s));
	ret(ctx, CELL_OK);
}
void sys_semaphore_wait(PPUContext& ctx) {
	auto s = kobjects().get<Semaphore>(arg32(ctx, 0));
	if (!s) return ret(ctx, CELL_ESRCH);
	std::unique_lock lock(s->m);
	if (!wait_for(s->cv, lock, arg(ctx, 1), [&] { return s->count > 0; })) return ret(ctx, CELL_ETIMEDOUT);
	s->count--;
	ret(ctx, CELL_OK);
}
void sys_semaphore_post(PPUContext& ctx) {
	auto s = kobjects().get<Semaphore>(arg32(ctx, 0));
	if (!s) return ret(ctx, CELL_ESRCH);
	std::lock_guard lock(s->m);
	s32 n = s32(arg(ctx, 1));
	if (s->count + n > s->max) return ret(ctx, CELL_EBUSY);
	s->count += n;
	s->cv.notify_all();
	ret(ctx, CELL_OK);
}

// --- event queues / ports -----------------------------------------------------
struct Event {
	u64 source, d1, d2, d3;
};
struct EventQueue : KernelObject {
	std::mutex m;
	std::condition_variable cv;
	std::deque<Event> q;
	u32 size;
	bool destroyed = false;
};
struct EventPort : KernelObject {
	u64 name;
	std::weak_ptr<EventQueue> queue;
};

void sys_event_queue_create(PPUContext& ctx) {
	auto q = std::make_shared<EventQueue>();
	q->size = arg32(ctx, 3) ? arg32(ctx, 3) : 127;
	st32(arg32(ctx, 0), kobjects().add(q));
	ret(ctx, CELL_OK);
}
void sys_event_queue_destroy(PPUContext& ctx) {
	auto q = kobjects().get<EventQueue>(arg32(ctx, 0));
	if (!q) return ret(ctx, CELL_ESRCH);
	{
		std::lock_guard lock(q->m);
		q->destroyed = true;
		q->cv.notify_all();
	}
	kobjects().remove(arg32(ctx, 0));
	ret(ctx, CELL_OK);
}
void sys_event_queue_receive(PPUContext& ctx) {
	auto q = kobjects().get<EventQueue>(arg32(ctx, 0));
	if (!q) return ret(ctx, CELL_ESRCH);
	std::unique_lock lock(q->m);
	if (!wait_for(q->cv, lock, arg(ctx, 2), [&] { return !q->q.empty() || q->destroyed; }))
		return ret(ctx, CELL_ETIMEDOUT);
	if (q->destroyed) return ret(ctx, CELL_ECANCELED);
	Event e = q->q.front();
	q->q.pop_front();
	ctx.r[4] = e.source;
	ctx.r[5] = e.d1;
	ctx.r[6] = e.d2;
	ctx.r[7] = e.d3;
	ret(ctx, CELL_OK);
}
void sys_event_queue_tryreceive(PPUContext& ctx) {
	auto q = kobjects().get<EventQueue>(arg32(ctx, 0));
	if (!q) return ret(ctx, CELL_ESRCH);
	std::lock_guard lock(q->m);
	u32 out = arg32(ctx, 1), max = arg32(ctx, 2), n = 0;
	while (n < max && !q->q.empty()) {
		Event e = q->q.front();
		q->q.pop_front();
		st64(out + n * 32, e.source);
		st64(out + n * 32 + 8, e.d1);
		st64(out + n * 32 + 16, e.d2);
		st64(out + n * 32 + 24, e.d3);
		n++;
	}
	st32(arg32(ctx, 3), n);
	ret(ctx, CELL_OK);
}
void sys_event_port_create(PPUContext& ctx) {
	auto p = std::make_shared<EventPort>();
	u32 id = kobjects().add(p);
	p->name = arg(ctx, 2) ? arg(ctx, 2) : (0xFFFFFFFF00000000ull | id);
	st32(arg32(ctx, 0), id);
	ret(ctx, CELL_OK);
}
void sys_event_port_destroy(PPUContext& ctx) {
	ret(ctx, kobjects().remove(arg32(ctx, 0)) ? CELL_OK : CELL_ESRCH);
}
void sys_event_port_connect_local(PPUContext& ctx) {
	auto p = kobjects().get<EventPort>(arg32(ctx, 0));
	auto q = kobjects().get<EventQueue>(arg32(ctx, 1));
	if (!p || !q) return ret(ctx, CELL_ESRCH);
	p->queue = q;
	ret(ctx, CELL_OK);
}
void sys_event_port_disconnect(PPUContext& ctx) {
	auto p = kobjects().get<EventPort>(arg32(ctx, 0));
	if (!p) return ret(ctx, CELL_ESRCH);
	p->queue.reset();
	ret(ctx, CELL_OK);
}
void sys_event_port_send(PPUContext& ctx) {
	auto p = kobjects().get<EventPort>(arg32(ctx, 0));
	if (!p) return ret(ctx, CELL_ESRCH);
	auto q = p->queue.lock();
	if (!q) return ret(ctx, 0x80010013 /* ENOTCONN */);
	std::lock_guard lock(q->m);
	if (q->q.size() >= q->size) return ret(ctx, CELL_EBUSY);
	q->q.push_back({p->name, arg(ctx, 1), arg(ctx, 2), arg(ctx, 3)});
	q->cv.notify_one();
	ret(ctx, CELL_OK);
}

// --- memory -------------------------------------------------------------------
void sys_memory_allocate(PPUContext& ctx) {
	u32 size = arg32(ctx, 0);
	u32 align = (arg(ctx, 1) & 0xF00) == 0x200 ? 0x10000 : 0x100000;   // 64K or 1M pages
	u32 addr = vm::alloc(vm::USER_BASE, size, align);
	if (!addr) return ret(ctx, CELL_ENOMEM);
	st32(arg32(ctx, 2), addr);
	ret(ctx, CELL_OK);
}
void sys_memory_get_user_memory_size(PPUContext& ctx) {
	st32(arg32(ctx, 0), vm::USER_MEMORY_TOTAL);
	st32(arg32(ctx, 0) + 4, vm::USER_MEMORY_TOTAL); // TODO: track usage
	ret(ctx, CELL_OK);
}
void sys_memory_container_create(PPUContext& ctx) {
	static u32 next = 0x3F000001;
	st32(arg32(ctx, 0), next++);
	ret(ctx, CELL_OK);
}

// --- misc -----------------------------------------------------------------------
void sys_tty_write(PPUContext& ctx) {
	u32 len = arg32(ctx, 2);
	std::fwrite(vm_ptr(arg32(ctx, 1)), 1, len, stdout);
	std::fflush(stdout);
	if (arg32(ctx, 3)) st32(arg32(ctx, 3), len);
	ret(ctx, CELL_OK);
}
void sys_timer_usleep(PPUContext& ctx) {
	precise_sleep_us(arg(ctx, 0));
	ret(ctx, CELL_OK);
}
void sys_ppu_thread_join(PPUContext& ctx) {
	u64 status = 0;
	if (!thread_join(arg32(ctx, 0), &status)) return ret(ctx, CELL_ESRCH);
	if (arg32(ctx, 1)) st64(arg32(ctx, 1), status);
	ret(ctx, CELL_OK);
}
void sys_ppu_thread_get_priority(PPUContext& ctx) {
	st32(arg32(ctx, 1), 1001);
	ret(ctx, CELL_OK);
}

// Raw SPU thread group calls (SDK setup code such as spu_printf); every SPU
// program the game runs goes through SPURS (hle/cellSpurs*.cpp), so these are no-ops.
void spu_ok(PPUContext& ctx) { ret(ctx, CELL_OK); }
void not_available(PPUContext& ctx) { ret(ctx, CELL_ENOSYS); }

} // namespace

// raw sys_fs syscalls share the cellFs implementation (hle/sys_fs.cpp)
void fs_open(PPUContext& ctx);
void fs_read(PPUContext& ctx);
void fs_write(PPUContext& ctx);
void fs_close(PPUContext& ctx);
void fs_lseek(PPUContext& ctx);

void ppu_syscall(PPUContext& ctx) {
	u32 n = u32(ctx.r[11]);
	switch (n) {
	case 1: return ret(ctx, 0x01000500);   // sys_process_getpid
	case 44: return sys_ppu_thread_join(ctx);
	case 48: return sys_ppu_thread_get_priority(ctx);
	case 90: return sys_semaphore_create(ctx);
	case 91: return ret(ctx, kobjects().remove(arg32(ctx, 0)) ? CELL_OK : CELL_ESRCH);
	case 92: return sys_semaphore_wait(ctx);
	case 94: return sys_semaphore_post(ctx);
	case 128: return sys_event_queue_create(ctx);
	case 129: return sys_event_queue_destroy(ctx);
	case 130: return sys_event_queue_receive(ctx);
	case 131: return sys_event_queue_tryreceive(ctx);
	case 134: return sys_event_port_create(ctx);
	case 135: return sys_event_port_destroy(ctx);
	case 136: return sys_event_port_connect_local(ctx);
	case 137: return sys_event_port_disconnect(ctx);
	case 138: return sys_event_port_send(ctx);
	case 141: return sys_timer_usleep(ctx);
	case 169: case 171: case 177: case 181: case 182: case 190: case 191: case 192:
		LOG_TRACE("syscall %u (SPU) ignored", n);
		return spu_ok(ctx);
	case 341: return sys_memory_container_create(ctx);
	case 342: return ret(ctx, CELL_OK);
	case 348: return sys_memory_allocate(ctx);
	case 352: return sys_memory_get_user_memory_size(ctx);
	case 402: return ret(ctx, CELL_EIO);   // sys_tty_read: no console input
	case 403: return sys_tty_write(ctx);
	case 801: return fs_open(ctx);
	case 802: return fs_read(ctx);
	case 803: return fs_write(ctx);
	case 804: return fs_close(ctx);
	case 818: return fs_lseek(ctx);
	case 880: case 881: case 882: case 883: case 884:
		return not_available(ctx);         // sys_deci3_*: debugger link
	default:
		LOG_WARN("unimplemented syscall %u (lr=0x%08llx)", n, (unsigned long long)ctx.lr);
		return ret(ctx, CELL_ENOSYS);
	}
}
