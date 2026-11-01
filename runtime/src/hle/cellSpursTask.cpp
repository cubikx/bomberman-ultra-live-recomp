// cellSpurs task sets, tasks, signals and event flags.
//
// The game's one SPURS task is the MultiStream audio mixer. Each task runs on its
// own host thread with its own SPU. The real SPURS kernel lives in local store
// below the task (LS 0x0000-0x2FFF); here that area only holds what the task
// library reads: a pointer table whose syscall entry is a `stop` stub, the
// 128-byte atomic buffer at 0x80 and the SPURS address at 0x1C0. The host answers
// the stub's syscalls (exit, wait for signal) and the task's mailbox channels
// (event ports: event flag wakeups, spu_printf).
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "debug/profiler.h"
#include "runtime/kernel.h"
#include "spu/compiled.h"
#include "spu/spu.h"

namespace {

// local store layout of the stand-in kernel
constexpr u32 LS_RETURN_TRAP = 0x0000;   // `$0` at entry; main returning lands here
constexpr u32 LS_SPURS_EA = 0x01C0;      // u64 SPURS address (+12: DMA tag the library uses)
constexpr u32 LS_KERNEL_TABLE = 0x2E00;  // reached through the word at LS 0x2FB8
constexpr u32 LS_SYSCALL_STUB = 0x2F00;  // `stop SYSCALL_CODE; bi $0`
constexpr u32 LS_TASK_INFO = 0x2FB0;
constexpr u32 TABLE_TASKSET = 152;       // u64, must be non-zero for cellSpursWaitSignal
constexpr u32 TABLE_SYSCALL = 196;       // LS address of the syscall entry
constexpr u32 SYSCALL_CODE = 0x2F0;
constexpr u32 RETURN_CODE = 0x2F1;
constexpr u32 SPU_THREAD_EXIT = 0x102;   // sys_spu_thread_exit (status in the outbound mailbox)
constexpr u32 LS_STACK_TOP = 0x3FFF0;

enum Syscall : u32 { SYS_EXIT = 0, SYS_YIELD = 1, SYS_WAIT_SIGNAL = 2, SYS_POLL = 3, SYS_RECV_WKL_FLAG = 4 };

// CellSpursEventFlag (128 bytes, big-endian)
constexpr u32 EF_EVENTS = 0x00, EF_PPU_WAIT_MASK = 0x04, EF_PPU_SLOT_MODE = 0x06, EF_PPU_PENDING = 0x07,
              EF_SPU_USED_SLOTS = 0x08, EF_SPU_PORT = 0x0C, EF_IS_IWL = 0x0D, EF_DIRECTION = 0x0E,
              EF_CLEAR_MODE = 0x0F, EF_PENDING_EVENTS = 0x30, EF_ADDR = 0x70;
constexpr u32 EVENT_FLAG_OR = 0, EVENT_FLAG_AND = 1;
constexpr u32 CLEAR_AUTO = 0;
constexpr u32 CELL_SPURS_TASK_ERROR_INVAL = 0x80410902, CELL_SPURS_TASK_ERROR_SRCH = 0x80410905;

// Event-port traffic from SPU tasks; event flag waiters re-check on every change.
std::mutex g_wake_lock;
std::condition_variable g_wake_cv;
u64 g_wake_gen = 0;

void wake_all() {
	{
		std::lock_guard lock(g_wake_lock);
		g_wake_gen++;
	}
	g_wake_cv.notify_all();
}

struct Taskset {
	u32 spurs = 0;
	u64 args = 0;
	u32 next_id = 0;
};

struct Task final : spu::ChannelHost {
	u32 taskset = 0, id = 0, spurs = 0;
	u32 elf = 0;
	u8 argument[16] = {};
	u64 taskset_args = 0;
	std::thread thread;

	std::mutex m;
	std::condition_variable cv;
	bool signal = false;
	bool shutdown = false;

	u32 out_mbox = 0;
	std::deque<u32> in_mbox;

	u32 read(spu::SPU& s, u32 ch) override;
	void write(spu::SPU& s, u32 ch, u32 v) override;
	u32 count(spu::SPU& s, u32 ch) override;
	void run();
	void port_event(spu::SPU& s, u32 port, u32 data0, u32 data1, bool reply);
};

std::mutex g_lock;
std::map<u32, Taskset> g_tasksets;
std::vector<std::shared_ptr<Task>> g_tasks;
u8 g_next_port = 16;

std::shared_ptr<Task> find_task(u32 taskset, u32 id) {
	std::lock_guard lock(g_lock);
	for (auto& t : g_tasks)
		if (t->taskset == taskset && t->id == id) return t;
	return nullptr;
}

u32 Task::read(spu::SPU& s, u32 ch) {
	if (ch == 29) {   // SPU_RdInMbox
		std::lock_guard lock(m);
		if (in_mbox.empty()) {
			LOG_WARN("spurs task %u: inbound mailbox read with nothing queued (pc 0x%05x)", id, s.pc);
			return 0;
		}
		u32 v = in_mbox.front();
		in_mbox.pop_front();
		return v;
	}
	LOG_WARN("spurs task %u: read of channel %u not supported (pc 0x%05x)", id, ch, s.pc);
	return 0;
}

u32 Task::count(spu::SPU&, u32 ch) {
	if (ch == 29) {
		std::lock_guard lock(m);
		return u32(in_mbox.size());
	}
	if (ch == 28 || ch == 30) return 1;   // outbound mailboxes always have room
	return 0;                             // signal notification: nothing pending
}

void Task::write(spu::SPU& s, u32 ch, u32 v) {
	switch (ch) {
	case 28: out_mbox = v; return;   // SPU_WrOutMbox
	case 30: {                       // SPU_WrOutIntrMbox: lv2 SPU thread event ports
		u32 code = v >> 24;
		if (code < 64) return port_event(s, code, v & 0xFFFFFF, out_mbox, true);     // send_event
		if (code < 128) return port_event(s, code - 64, v & 0xFFFFFF, out_mbox, false);   // throw_event
		LOG_WARN("spurs task %u: interrupt mailbox value 0x%08x not supported", id, v);
		return;
	}
	default: LOG_WARN("spurs task %u: write of channel %u not supported (pc 0x%05x)", id, ch, s.pc); return;
	}
}

// Event-port sends: event flag wakeups and spu_printf. Event flags are re-checked by
// their waiters, so any event only needs to wake them.
void Task::port_event(spu::SPU& s, u32 port, u32 data0, u32 data1, bool reply) {
	if (reply && port == 1) {
		// spu_printf: data1 = LS save area of the variadic call; its first quadword holds the format
		u32 area = data1 & spu::LS_MASK;
		u32 fmt = s.ls32(area) & spu::LS_MASK;
		std::string text;
		for (u32 a = fmt; a < spu::LS_SIZE && s.ls[a] && text.size() < 200; a++) text += char(s.ls[a]);
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
		LOG_INFO("spu_printf (task %u): %s [args %08x %08x %08x]", id, text.c_str(), s.ls32(area + 16),
		         s.ls32(area + 32), s.ls32(area + 48));
	} else {
		static int logged = 0;
		if (logged++ < 8)
			LOG_INFO("spurs task %u: event port %u (0x%06x, 0x%08x)%s", id, port, data0, data1,
			         reply ? " send" : " throw");
		wake_all();
	}
	if (reply) {
		std::lock_guard lock(m);
		in_mbox.push_back(CELL_OK);
	}
}

// The task's executable segment, as compiled by spu_recompile.py
struct Image {
	u32 text_ea = 0, text_size = 0, text_ls = 0, entry = 0;
};

bool load_elf(spu::SPU& s, u32 elf, Image& img) {
	if (ld32(elf) != 0x7F454C46 || vm_ptr(elf)[4] != 1) {
		LOG_ERROR("spurs: task image at 0x%08x is not an SPU ELF", elf);
		return false;
	}
	img.entry = ld32(elf + 24);
	u32 phoff = ld32(elf + 28);
	u32 phentsize = ld16(elf + 42), phnum = ld16(elf + 44);
	for (u32 i = 0; i < phnum; i++) {
		u32 ph = elf + phoff + i * phentsize;
		if (ld32(ph) != 1) continue;   // PT_LOAD
		u32 off = ld32(ph + 4), vaddr = ld32(ph + 8), filesz = ld32(ph + 16), memsz = ld32(ph + 20),
		    flags = ld32(ph + 24);
		if (u64(vaddr) + memsz > spu::LS_SIZE) {
			LOG_ERROR("spurs: task segment 0x%05x+0x%x does not fit in local store", vaddr, memsz);
			return false;
		}
		std::memcpy(&s.ls[vaddr], vm_ptr(elf + off), filesz);
		std::memset(&s.ls[vaddr + filesz], 0, memsz - filesz);
		if (flags & 1) img.text_ea = elf + off, img.text_size = filesz, img.text_ls = vaddr;
	}
	return img.text_size != 0;
}

void Task::run() {
	char name[32];
	std::snprintf(name, sizeof name, "spurs_task_%u", id);
	profiler::register_current_thread(name);

	auto sp = std::make_unique<spu::SPU>();
	spu::SPU& s = *sp;
	s.host = this;
	Image img;
	if (!load_elf(s, elf, img)) return;

	// stand-in kernel
	s.set_ls32(LS_RETURN_TRAP, spu::stop_instruction(RETURN_CODE));
	s.set_ls32(LS_SPURS_EA, 0);
	s.set_ls32(LS_SPURS_EA + 4, spurs);
	s.set_ls32(LS_SPURS_EA + 12, 31);
	s.set_ls32(LS_TASK_INFO + 8, LS_KERNEL_TABLE);
	s.set_ls32(LS_KERNEL_TABLE + TABLE_TASKSET, 0);
	s.set_ls32(LS_KERNEL_TABLE + TABLE_TASKSET + 4, taskset);
	s.set_ls32(LS_KERNEL_TABLE + TABLE_SYSCALL, LS_SYSCALL_STUB);
	s.set_ls32(LS_SYSCALL_STUB, spu::stop_instruction(SYSCALL_CODE));
	s.set_ls32(LS_SYSCALL_STUB + 4, 0x35000000);   // bi $0

	for (u32 i = 0; i < 4; i++) s.r[3].w[i] = u32(argument[i * 4] << 24 | argument[i * 4 + 1] << 16 |
	                                              argument[i * 4 + 2] << 8 | argument[i * 4 + 3]);
	s.r[4].set_d(0, taskset_args);
	s.r[0] = spu::Reg::splat(LS_RETURN_TRAP);
	s.r[1] = spu::Reg{{LS_STACK_TOP, 0, 0, 0}};
	s.pc = img.entry;
	LOG_INFO("spurs: task %u started (image 0x%08x, code LS 0x%05x+0x%x, entry 0x%05x)", id, elf, img.text_ls,
	         img.text_size, img.entry);

	for (;;) {
		u32 code = 0;
		spu::StopReason why =
		    spu::run_program(s, img.text_ea, img.text_size, img.text_ls, &code, ~0ull);
		if (why == spu::StopReason::stop && code == SYSCALL_CODE) {
			u32 n = s.r[3].w[0];
			u32 result = CELL_OK;
			if (n == SYS_EXIT) break;
			if (n == SYS_WAIT_SIGNAL) {
				std::unique_lock lock(m);
				cv.wait(lock, [&] { return signal || shutdown; });
				if (shutdown) break;
				signal = false;
			} else if (n != SYS_YIELD) {
				LOG_WARN("spurs task %u: syscall %u not supported", id, n);
				result = CELL_SPURS_TASK_ERROR_INVAL;
			}
			s.r[3] = spu::Reg{{result, 0, 0, 0}};
			continue;   // s.pc is the stub's `bi $0`
		}
		if (why == spu::StopReason::stop && (code == SPU_THREAD_EXIT || code == RETURN_CODE)) break;
		LOG_ERROR("spurs task %u: stopped with %s, code 0x%x at pc 0x%05x", id,
		          why == spu::StopReason::stop ? "stop" : why == spu::StopReason::halt ? "halt" : "error", code,
		          s.pc);
		break;
	}
	LOG_INFO("spurs: task %u exited", id);
}

} // namespace

// cellSpursCreateTaskset(spurs, taskset, args, priority[8], maxContention)
HLE_FUNC(cellSpurs, cellSpursCreateTaskset) {
	std::lock_guard lock(g_lock);
	Taskset& ts = g_tasksets[arg32(ctx, 1)];
	ts.spurs = arg32(ctx, 0);
	ts.args = arg(ctx, 2);
	ret(ctx, CELL_OK);
}

// cellSpursCreateTask(taskset, *taskId, elf, context, size, lsPattern, argument)
HLE_FUNC(cellSpurs, cellSpursCreateTask) {
	auto t = std::make_shared<Task>();
	{
		std::lock_guard lock(g_lock);
		Taskset& ts = g_tasksets[arg32(ctx, 0)];
		t->taskset = arg32(ctx, 0);
		t->id = ts.next_id++;
		t->spurs = ts.spurs;
		t->taskset_args = ts.args;
		g_tasks.push_back(t);
	}
	t->elf = arg32(ctx, 2);
	if (arg32(ctx, 6)) std::memcpy(t->argument, vm_ptr(arg32(ctx, 6)), 16);
	if (arg32(ctx, 1)) st32(arg32(ctx, 1), t->id);
	t->thread = std::thread([t] { t->run(); });
	t->thread.detach();
	ret(ctx, CELL_OK);
}

// _cellSpursSendSignal(taskset, taskId)
HLE_FUNC(cellSpurs, _cellSpursSendSignal) {
	auto t = find_task(arg32(ctx, 0), arg32(ctx, 1));
	if (!t) return ret(ctx, CELL_SPURS_TASK_ERROR_SRCH);
	{
		std::lock_guard lock(t->m);
		t->signal = true;
	}
	t->cv.notify_one();
	ret(ctx, CELL_OK);
}

// cellSpursShutdownTaskset(taskset): tasks stop at their next wait
HLE_FUNC(cellSpurs, cellSpursShutdownTaskset) {
	std::vector<std::shared_ptr<Task>> tasks;
	{
		std::lock_guard lock(g_lock);
		for (auto& t : g_tasks)
			if (t->taskset == arg32(ctx, 0)) tasks.push_back(t);
	}
	for (auto& t : tasks) {
		{
			std::lock_guard lock(t->m);
			t->shutdown = true;
		}
		t->cv.notify_one();
	}
	ret(ctx, CELL_OK);
}

// _cellSpursEventFlagInitialize(spurs, taskset, eventFlag, clearMode, direction)
HLE_FUNC(cellSpurs, _cellSpursEventFlagInitialize) {
	u32 ef = arg32(ctx, 2);
	if (!ef || (ef & 127)) return ret(ctx, CELL_SPURS_TASK_ERROR_INVAL);
	std::lock_guard lock(spu::atomic_lock());
	std::memset(vm_ptr(ef), 0, 128);
	vm_ptr(ef)[EF_CLEAR_MODE] = u8(arg32(ctx, 3));
	vm_ptr(ef)[EF_DIRECTION] = u8(arg32(ctx, 4));
	vm_ptr(ef)[EF_IS_IWL] = arg32(ctx, 1) ? 0 : 1;
	st64(ef + EF_ADDR, arg32(ctx, 1) ? arg32(ctx, 1) : arg32(ctx, 0));
	ret(ctx, CELL_OK);
}

// cellSpursEventFlagAttachLv2EventQueue(eventFlag): give the flag an SPU event port to wake PPU waiters
HLE_FUNC(cellSpurs, cellSpursEventFlagAttachLv2EventQueue) {
	u32 ef = arg32(ctx, 0);
	std::lock_guard lock(spu::atomic_lock());
	std::lock_guard lock2(g_lock);
	vm_ptr(ef)[EF_SPU_PORT] = g_next_port++;
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellSpurs, cellSpursEventFlagDetachLv2EventQueue) { ret(ctx, CELL_OK); }

// cellSpursEventFlagWait(eventFlag, *mask, mode): PPU side of an SPU-to-PPU flag. A waiter
// that cannot be satisfied right away registers its mask in a wait slot; the setting
// task then stores the matched bits in pendingRecvTaskEvents[slot], sets ppuPendingRecv
// and sends an event on the flag's port.
HLE_FUNC(cellSpurs, cellSpursEventFlagWait) {
	u32 ef = arg32(ctx, 0), mask_addr = arg32(ctx, 1), mode = arg32(ctx, 2);
	if (!ef || !mask_addr || mode > EVENT_FLAG_AND) return ret(ctx, CELL_SPURS_TASK_ERROR_INVAL);
	u16 mask = ld16(mask_addr);
	bool waiting = false;
	u32 slot = 15;
	for (;;) {
		u64 gen;
		{
			std::lock_guard lock(g_wake_lock);
			gen = g_wake_gen;
		}
		{
			std::lock_guard lock(spu::atomic_lock());
			u8* p = vm_ptr(ef);
			if (waiting) {
				if (p[EF_PPU_PENDING]) {
					st16(mask_addr, ld16(ef + EF_PENDING_EVENTS + slot * 2));
					p[EF_PPU_PENDING] = 0;
					st16(ef + EF_PPU_WAIT_MASK, 0);
					return ret(ctx, CELL_OK);
				}
			} else {
				u16 events = ld16(ef + EF_EVENTS);
				u16 matched = events & mask;
				bool ok = mode == EVENT_FLAG_AND ? matched == mask : matched != 0;
				if (ok) {
					if (p[EF_CLEAR_MODE] == CLEAR_AUTO) st16(ef + EF_EVENTS, events & ~matched);
					st16(mask_addr, matched);
					return ret(ctx, CELL_OK);
				}
				u16 used = ld16(ef + EF_SPU_USED_SLOTS);
				while (slot && (used >> slot) & 1) slot--;
				st16(ef + EF_PPU_WAIT_MASK, mask);
				p[EF_PPU_SLOT_MODE] = u8(slot << 4 | mode);
				waiting = true;
			}
		}
		std::unique_lock lock(g_wake_lock);
		g_wake_cv.wait_for(lock, std::chrono::milliseconds(5), [&] { return g_wake_gen != gen; });
	}
}
