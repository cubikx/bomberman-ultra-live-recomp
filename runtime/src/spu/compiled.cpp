// Registry and runner for statically recompiled SPU programs.
#include "spu/compiled.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace spu {
namespace {

struct Entry {
	CompiledProgram p;
	int verified = 0;   // 0 = not checked yet, 1 = bytes match, -1 = mismatch
};

std::map<u32, Entry>& registry() {   // by guest address; filled by static constructors
	static std::map<u32, Entry> r;
	return r;
}
std::mutex g_lock;

u64 fnv1a(const u8* p, u32 n) {
	u64 h = 0xcbf29ce484222325ull;
	for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
	return h;
}


// everything an SPU program can observe: registers, local store, channel/MFC state
void copy_state(SPU& d, const SPU& s) {
	std::memcpy(d.r, s.r, sizeof(d.r));
	std::memcpy(d.ls.get(), s.ls.get(), LS_SIZE);
	d.pc = s.pc;
	d.mfc_lsa = s.mfc_lsa, d.mfc_eah = s.mfc_eah, d.mfc_eal = s.mfc_eal, d.mfc_size = s.mfc_size;
	d.mfc_tag = s.mfc_tag, d.tag_mask = s.tag_mask, d.atomic_status = s.atomic_status;
	d.has_reservation = s.has_reservation, d.reservation_ea = s.reservation_ea;
	std::memcpy(d.reservation_data, s.reservation_data, sizeof(d.reservation_data));
	d.event_mask = s.event_mask, d.decrementer = s.decrementer;
}

// SPU_VERIFY: compare a compiled run against the interpreter from the same start
void verify(const SPU& before, const SPU& compiled, StopReason why, u32 code, u32 max_instructions) {
	static u64 runs = 0, mismatches = 0;
	auto ref = std::make_unique<SPU>();
	copy_state(*ref, before);
	u32 ref_code = 0;
	StopReason ref_why = run(*ref, &ref_code, max_instructions);
	runs++;
	bool same = ref_why == why && ref_code == code && ref->pc == compiled.pc;
	int bad_reg = -1;
	for (int i = 0; i < 128 && bad_reg < 0; i++)
		if (std::memcmp(&ref->r[i], &compiled.r[i], sizeof(Reg))) bad_reg = i;
	u32 bad_ls = LS_SIZE;
	for (u32 a = 0; a < LS_SIZE && bad_ls == LS_SIZE; a += 16)
		if (std::memcmp(&ref->ls[a], &compiled.ls[a], 16)) bad_ls = a;
	if (same && bad_reg < 0 && bad_ls == LS_SIZE) {
		if (runs % 1000 == 0) LOG_INFO("spu verify: %llu runs, %llu mismatches", (unsigned long long)runs,
		                               (unsigned long long)mismatches);
		return;
	}
	if (mismatches++ < 10)
		LOG_ERROR("spu verify: mismatch (stop %d/%d, code %x/%x, pc %05x/%05x, first reg %d, first LS 0x%05x)",
		          int(ref_why), int(why), ref_code, code, ref->pc, compiled.pc, bad_reg, bad_ls);
}

} // namespace

CompiledRegistrar::CompiledRegistrar(const CompiledProgram& p) { registry()[p.ea] = {p}; }

const CompiledProgram* find_compiled(u32 ea, u32 size, u32 ls_base) {
	static const bool off = env_flag("SPU_INTERPRET");
	if (off) return nullptr;
	std::lock_guard lock(g_lock);   // also guards run_program's log
	auto it = registry().find(ea);
	if (it == registry().end()) return nullptr;
	Entry& e = it->second;
	if (e.p.size != size || e.p.ls_base != ls_base) return nullptr;
	if (!e.verified) {
		e.verified = fnv1a(vm_ptr(ea), size) == e.p.hash ? 1 : -1;
		if (e.verified > 0) LOG_INFO("spu: using recompiled %s (0x%08x, %u bytes)", e.p.name, ea, size);
		else LOG_WARN("spu: %s at 0x%08x does not match its recompiled copy; interpreting", e.p.name, ea);
	}
	return e.verified > 0 ? &e.p : nullptr;
}

StopReason run_program(SPU& s, u32 ea, u32 size, u32 ls_base, u32* code, u64 max_instructions) {
	const CompiledProgram* p = find_compiled(ea, size, ls_base);
	if (!p) return run(s, code, max_instructions);
	// programs with host channels have side effects a second run would repeat
	static const bool check_jobs = env_flag("SPU_VERIFY");
	bool check = check_jobs && !s.host;
	std::unique_ptr<SPU> before;
	if (check) {
		before = std::make_unique<SPU>();
		copy_state(*before, s);
	}
	u32 lo = p->ls_base, hi = p->ls_base + p->size;
	StopReason why;
	for (;;) {
		if (u32 status = p->fn(s, code)) {
			why = StopReason(status - 1);
			break;
		}
		// left the compiled code (the return trap, kernel stubs): interpret until it comes back
		u32 pc = s.pc;
		if (pc >= lo && pc < hi) {
			std::lock_guard lock(g_lock);
			static std::map<u32, bool> logged;
			if (!logged[pc]) {
				logged[pc] = true;
				LOG_WARN("spu: %s jumped to 0x%05x, which has no compiled entry; interpreting it", p->name, pc);
			}
		}
		why = run_until_in(s, code, max_instructions, lo, hi);
		if (why != StopReason::entered) break;
	}
	if (check) verify(*before, s, why, code ? *code : 0, u32(max_instructions));
	return why;
}

} // namespace spu
