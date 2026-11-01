// cellSpurs: SPU job chains, run on the SPU interpreter by a small job manager.
//
// The real SPURS kernel and job manager are SPU programs in the system
// firmware; here the job manager is host code. A job chain runs synchronously
// when the game starts it, which preserves the ordering the game relies on.
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/kernel.h"
#include "spu/compiled.h"
#include "spu/spu.h"

namespace {

std::mutex g_lock;
std::map<u32, u32> g_attr_entry;    // job chain attribute -> command list
std::map<u32, u32> g_chain_entry;   // job chain object -> command list
u32 g_last_entry = 0;

// Local store layout for a job
constexpr u32 LS_RETURN_TRAP = 0x00000;   // `stop RETURN_CODE`; jobs return here through $0
constexpr u32 LS_CONTEXT = 0x00100;
constexpr u32 LS_DESCRIPTOR = 0x00200;
constexpr u32 LS_BINARY = 0x01000;
constexpr u32 LS_STACK_TOP = 0x3FFD0;
constexpr u32 RETURN_CODE = 0x3FFF;

u32 align(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

// CellSpursJob header (big-endian, at the start of every job descriptor)
struct JobHeader {
	u32 ea_binary, size_binary, size_dma_list, ea_high_input, use_inout, size_in, size_out, size_stack, size_heap,
	    size_scratch;
	static JobHeader read(u32 d) {
		JobHeader h;
		h.ea_binary = u32(ld64(d));
		h.size_binary = u32(ld16(d + 8)) * 16;
		h.size_dma_list = ld16(d + 10);
		h.ea_high_input = ld32(d + 12);
		h.use_inout = ld32(d + 16);
		h.size_in = ld32(d + 20);
		h.size_out = ld32(d + 24);
		h.size_stack = u32(ld16(d + 28)) * 16;
		h.size_heap = u32(ld16(d + 30)) * 16;
		h.size_scratch = u32(ld16(d + 40)) * 16;
		return h;
	}
};

thread_local std::unique_ptr<spu::SPU> t_spu;
u64 g_jobs_run = 0;

// A job is SPURS bookkeeping if its only argument is the job chain object it was
// queued on: the completion job takes no DMA input and points at a registered chain.
bool is_spurs_internal_job(const JobHeader& h, u32 desc) {
	if (h.size_dma_list != 0 || h.size_in || h.size_out || h.size_binary > 0x1000) return false;
	u32 user = ld32(desc + 52);   // low word of the first user-data doubleword
	std::lock_guard lock(g_lock);
	return g_chain_entry.count(user) != 0;
}

bool run_job(u32 desc) {
	if (!t_spu) t_spu = std::make_unique<spu::SPU>();
	spu::SPU& s = *t_spu;
	JobHeader h = JobHeader::read(desc);
	static int traced = 0;
	bool trace = traced++ < 4;
	if (trace) {
		LOG_INFO("spurs: job 0x%08x header: binary 0x%08x/%u, dma list %u bytes, in %u out %u stack %u scratch %u inout %u",
		         desc, h.ea_binary, h.size_binary, h.size_dma_list, h.size_in, h.size_out, h.size_stack,
		         h.size_scratch, h.use_inout);
		std::string words;
		char buf[24];
		for (u32 i = 0; i < 64; i++) {
			std::snprintf(buf, sizeof buf, "%s%08x", i % 8 ? " " : "\n    ", ld32(desc + i * 4));
			words += buf;
		}
		LOG_INFO("spurs:   descriptor 0x%08x:%s", desc, words.c_str());
	}
	if (!h.ea_binary || h.size_binary > 0x30000) {
		LOG_ERROR("spurs: job 0x%08x has a bad binary (0x%08x, %u bytes)", desc, h.ea_binary, h.size_binary);
		return false;
	}

	// The job chain's completion job only updates SPURS' own job chain object
	// (which the real runtime maintains and this one does not). Chains run
	// synchronously here, so completion is already implied; skip it.
	if (is_spurs_internal_job(h, desc)) {
		static bool noted = false;
		if (!noted) {
			noted = true;
			LOG_INFO("spurs: skipping SPURS-internal job binary 0x%08x (%u bytes)", h.ea_binary, h.size_binary);
		}
		return true;
	}

	std::memset(s.ls.get(), 0, spu::LS_SIZE);
	std::memset(s.r, 0, sizeof(s.r));
	s.executed = 0;
	s.dma_fault = false;
	s.set_ls32(LS_RETURN_TRAP, spu::stop_instruction(RETURN_CODE));
	std::memcpy(&s.ls[LS_DESCRIPTOR], vm_ptr(desc), 0x200);
	std::memcpy(&s.ls[LS_BINARY], vm_ptr(h.ea_binary), h.size_binary);

	// buffers after the binary
	u32 io_buffer = align(LS_BINARY + h.size_binary, 128);
	u32 n_elems = h.size_dma_list / 8;
	u32 io = io_buffer;
	struct Elem { u32 ea, size, ls; };
	std::vector<Elem> elems;
	for (u32 i = 0; i < n_elems; i++) {
		u32 hi = ld32(desc + 48 + i * 8), lo = ld32(desc + 48 + i * 8 + 4);
		u32 size = hi & 0x3FFFF;   // element: size in the high word, address in the low word
		if (trace) LOG_INFO("spurs:   dma element %u: 0x%08x 0x%08x", i, hi, lo);
		elems.push_back({lo, size, io});
		if (size) std::memcpy(&s.ls[io & spu::LS_MASK], vm_ptr(lo), std::min(size, spu::LS_SIZE - io));
		io = align(io + size, 16);
	}
	u32 o_buffer = align(std::max(io, io_buffer + h.size_in), 128);
	u32 s_buffer = align(o_buffer + h.size_out, 128);
	if (s_buffer + h.size_scratch + std::max(h.size_stack, 0x4000u) > spu::LS_SIZE) {
		LOG_ERROR("spurs: job 0x%08x does not fit in local store", desc);
		return false;
	}

	static int logged = 0;
	if (n_elems && elems[0].size && logged++ < 2) {
		u32 total = 0;
		for (auto& e : elems) total += e.size;
		LOG_INFO("spurs: job 0x%08x: %u DMA elements, %u bytes (descriptor says in %u, out %u, scratch %u)", desc,
		         n_elems, total, h.size_in, h.size_out, h.size_scratch);
	}

	// CellSpursJobContext2
	u32 c = LS_CONTEXT;
	s.set_ls32(c + 0, io_buffer);    // ioBuffer
	s.set_ls32(c + 20, n_elems);     // numIoBuffer
	s.set_ls32(c + 24, 0);           // numCacheBuffer
	s.set_ls32(c + 28, o_buffer);    // oBuffer
	s.set_ls32(c + 32, s_buffer);    // sBuffer
	s.set_ls32(c + 40, 0);           // eaJobDescriptor (high)
	s.set_ls32(c + 44, desc);        // eaJobDescriptor (low)
	s.set_ls32(c + 48, 0);           // dmaTag

	s.r[0] = spu::Reg{{LS_RETURN_TRAP, 0, 0, 0}};
	s.r[1] = spu::Reg{{LS_STACK_TOP, 0, 0, 0}};
	s.r[3] = spu::Reg{{LS_CONTEXT, 0, 0, 0}};
	s.r[4] = spu::Reg{{LS_DESCRIPTOR, 0, 0, 0}};
	s.pc = LS_BINARY + 0x10;

	u32 code = 0;
	auto t0 = std::chrono::steady_clock::now();
	spu::StopReason why = spu::run_program(s, h.ea_binary, h.size_binary, LS_BINARY, &code, 5'000'000);
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	static int timed = 0;
	if (timed++ < 6)
		LOG_INFO("spurs: job 0x%08x ran %llu instructions in %.2f ms", desc, (unsigned long long)s.executed, ms);
	if (why == spu::StopReason::error && s.executed >= 5'000'000) {
		std::map<u32, int> hot;
		for (u32 p : s.pc_samples) hot[p - LS_BINARY]++;
		std::string list;
		char buf[32];
		for (auto [p, n] : hot) {
			std::snprintf(buf, sizeof buf, " %05x(x%d)", p, n);
			list += buf;
		}
		LOG_ERROR("spurs: job 0x%08x is stuck; sampled pcs (binary-relative):%s; last channel op ch%u %s", desc,
		          list.c_str(), s.last_channel_op >> 1, (s.last_channel_op & 1) ? "write" : "read");
	}
	if (why != spu::StopReason::stop || code != RETURN_CODE) {
		LOG_ERROR("spurs: job 0x%08x (binary 0x%08x) ended with %s, code 0x%x at pc 0x%05x", desc, h.ea_binary,
		          why == spu::StopReason::stop ? "stop" : why == spu::StopReason::halt ? "halt" : "error", code, s.pc);
		return false;
	}
	if (h.use_inout)
		for (auto& e : elems)
			if (e.size) std::memcpy(vm_ptr(e.ea), &s.ls[e.ls & spu::LS_MASK], e.size);
	g_jobs_run++;
	return true;
}

// Execute a job chain command list until END.
void run_chain(u32 entry, bool verbose) {
	u32 pc = entry;
	u32 call_stack[8];
	u32 depth = 0;
	for (u32 steps = 0; steps < 100000; steps++) {
		u64 cmd = ld64(pc);
		if (verbose) LOG_INFO("spurs:   [0x%08x] command 0x%016llx", pc, (unsigned long long)cmd);
		pc += 8;
		u32 op = u32(cmd & 7);
		if (op == 0 && cmd >= 16) {
			bool ok = run_job(u32(cmd));
			if (verbose) LOG_INFO("spurs:   job 0x%08x %s", u32(cmd), ok ? "completed" : "FAILED");
			continue;
		}
		switch (op) {
		case 0: break;                                   // NOP
		case 1: pc = u32(cmd & ~7ull); break;            // RESET_PC
		case 2: break;                                   // SYNC / LWSYNC (+label): jobs run in order already
		case 3: pc = u32(cmd & ~7ull); break;            // NEXT
		case 4:                                          // CALL
			if (depth < 8) call_stack[depth++] = pc;
			pc = u32(cmd & ~7ull);
			break;
		case 5: break;                                   // FLUSH
		case 6: {                                        // JOBLIST: address and count of descriptors
			u32 list = u32(cmd >> 32) ? u32(cmd & ~7ull) : u32(cmd & ~7ull);
			LOG_WARN("spurs: JOBLIST command (0x%016llx) not supported yet", (unsigned long long)cmd);
			(void)list;
			break;
		}
		case 7: {
			u32 sub = u32(cmd >> 3) & 0xF;
			if (sub == 15) return;                       // END
			if (sub == 14) {                             // RET
				if (!depth) return;
				pc = call_stack[--depth];
				break;
			}
			if (sub == 0) return;                        // ABORT
			break;                                       // GUARD / SET_LABEL: ordering only
		}
		}
	}
	LOG_ERROR("spurs: job chain at 0x%08x did not reach END", entry);
}

} // namespace

// _cellSpursJobChainAttributeInitialize(jmRevision, sdkRevision, attr*, jobChainEntry*, ...)
HLE_FUNC(cellSpurs, _cellSpursJobChainAttributeInitialize) {
	std::lock_guard lock(g_lock);
	g_attr_entry[arg32(ctx, 2)] = arg32(ctx, 3);
	g_last_entry = arg32(ctx, 3);
	ret(ctx, CELL_OK);
}

// cellSpursCreateJobChainWithAttribute(spurs*, jobChain*, attr*)
HLE_FUNC(cellSpurs, cellSpursCreateJobChainWithAttribute) {
	std::lock_guard lock(g_lock);
	auto it = g_attr_entry.find(arg32(ctx, 2));
	g_chain_entry[arg32(ctx, 1)] = it != g_attr_entry.end() ? it->second : g_last_entry;
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellSpurs, cellSpursRunJobChain) {
	u32 entry;
	{
		std::lock_guard lock(g_lock);
		auto it = g_chain_entry.find(arg32(ctx, 0));
		entry = it != g_chain_entry.end() ? it->second : g_last_entry;
	}
	static u64 runs = 0;
	if (++runs == 1 || runs % 3000 == 0)
		LOG_INFO("spurs: job chain run #%llu (%llu jobs executed so far)", (unsigned long long)runs,
		         (unsigned long long)g_jobs_run);
	if (entry) run_chain(entry, runs <= 2);
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellSpurs, cellSpursJoinJobChain) { ret(ctx, CELL_OK); }
