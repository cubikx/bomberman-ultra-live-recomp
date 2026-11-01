// SPU interpreter. Straightforward and exact first; the instruction semantics live
// in spu_ops.h, shared with the static SPU recompiler's output.
#include "spu.h"

#include <array>
#include <mutex>

#include "spu/spu_ops.h"

namespace spu {

Reg SPU::read_qw(u32 a) const {
	a &= LS_MASK & ~15u;
	Reg r;
	for (int i = 0; i < 4; i++) {
		u32 v;
		std::memcpy(&v, &ls[a + 4 * i], 4);
		r.w[i] = std::byteswap(v);
	}
	return r;
}

void SPU::write_qw(u32 a, const Reg& v) {
	a &= LS_MASK & ~15u;
	for (int i = 0; i < 4; i++) {
		u32 x = std::byteswap(v.w[i]);
		std::memcpy(&ls[a + 4 * i], &x, 4);
	}
}

namespace {

// 11-bit major opcode -> Op. Shorter opcodes occupy every slot they prefix.
std::array<Op, 2048> build_table() {
	std::array<Op, 2048> t{};
	auto put = [&](u32 opcode, u32 bits, Op op) {
		u32 shift = 11 - bits;
		for (u32 i = 0; i < (1u << shift); i++) t[(opcode << shift) | i] = op;
	};
	// order matters only where widths overlap; longer opcodes are installed last
	put(0x8, 4, SELB); put(0xB, 4, SHUFB); put(0xC, 4, MPYA); put(0xD, 4, FNMS); put(0xE, 4, FMA); put(0xF, 4, FMS);
	put(0x21, 7, ILA); put(0x08, 7, HBRA); put(0x09, 7, HBRR);
	struct { u32 op; Op e; } ri10[] = {
		{0x1C, AI}, {0x1D, AHI}, {0x0C, SFI}, {0x0D, SFHI}, {0x14, ANDI}, {0x15, ANDHI}, {0x16, ANDBI}, {0x04, ORI},
		{0x05, ORHI}, {0x06, ORBI}, {0x44, XORI}, {0x45, XORHI}, {0x46, XORBI}, {0x74, MPYI}, {0x75, MPYUI},
		{0x7C, CEQI}, {0x7D, CEQHI}, {0x7E, CEQBI}, {0x4C, CGTI}, {0x4D, CGTHI}, {0x4E, CGTBI}, {0x5C, CLGTI},
		{0x5D, CLGTHI}, {0x5E, CLGTBI}, {0x34, LQD}, {0x24, STQD}, {0x7F, HEQI}, {0x4F, HGTI}, {0x5F, HLGTI}};
	for (auto& e : ri10) put(e.op, 8, e.e);
	struct { u32 op; Op e; } ri16[] = {
		{0x064, BR}, {0x060, BRA}, {0x066, BRSL}, {0x062, BRASL}, {0x042, BRNZ}, {0x040, BRZ}, {0x046, BRHNZ},
		{0x044, BRHZ}, {0x081, IL}, {0x083, ILH}, {0x082, ILHU}, {0x0C1, IOHL}, {0x065, FSMBI}, {0x061, LQA},
		{0x067, LQR}, {0x041, STQA}, {0x047, STQR}};
	for (auto& e : ri16) put(e.op, 9, e.e);
	put(0x1D8, 10, CFLTS); put(0x1D9, 10, CFLTU); put(0x1DA, 10, CSFLT); put(0x1DB, 10, CUFLT);
	struct { u32 op; Op e; } rr[] = {
		{0x1C4, LQX}, {0x144, STQX}, {0x1A8, BI}, {0x1A9, BISL}, {0x1AA, IRET}, {0x1AB, BISLED}, {0x128, BIZ},
		{0x129, BINZ}, {0x12A, BIHZ}, {0x12B, BIHNZ}, {0x1AC, HBR}, {0x0C0, OP_ADD}, {0x0C8, AH}, {0x040, SF},
		{0x048, SFH}, {0x340, ADDX}, {0x0C2, CG}, {0x342, CGX}, {0x341, SFX}, {0x042, BG}, {0x343, BGX},
		{0x3C4, MPY}, {0x3CC, MPYU}, {0x3C5, MPYH}, {0x3C7, MPYS}, {0x3C6, MPYHH}, {0x346, MPYHHA},
		{0x3CE, MPYHHU}, {0x34E, MPYHHAU}, {0x2A5, CLZ}, {0x2B4, CNTB}, {0x1B6, FSMB}, {0x1B5, FSMH},
		{0x1B4, FSM}, {0x1B2, GBB}, {0x1B1, GBH}, {0x1B0, GB}, {0x0D3, AVGB}, {0x053, ABSDB}, {0x253, SUMB},
		{0x2B6, XSBH}, {0x2AE, XSHW}, {0x2A6, XSWD}, {0x0C1, AND}, {0x2C1, ANDC}, {0x041, OR}, {0x2C9, ORC},
		{0x1F0, ORX}, {0x241, XOR}, {0x0C9, NAND}, {0x049, NOR}, {0x249, EQV}, {0x05F, SHLH}, {0x07F, SHLHI},
		{0x05B, SHL}, {0x07B, SHLI}, {0x1DB, SHLQBI}, {0x1FB, SHLQBII}, {0x1DF, SHLQBY}, {0x1FF, SHLQBYI},
		{0x1CF, SHLQBYBI}, {0x05C, ROTH}, {0x07C, ROTHI}, {0x058, ROT}, {0x078, ROTI}, {0x1DC, ROTQBY},
		{0x1FC, ROTQBYI}, {0x1CC, ROTQBYBI}, {0x1D8, ROTQBI}, {0x1F8, ROTQBII}, {0x05D, ROTHM}, {0x07D, ROTHMI},
		{0x059, ROTM}, {0x079, ROTMI}, {0x1DD, ROTQMBY}, {0x1FD, ROTQMBYI}, {0x1CD, ROTQMBYBI}, {0x1D9, ROTQMBI},
		{0x1F9, ROTQMBII}, {0x05E, ROTMAH}, {0x07E, ROTMAHI}, {0x05A, ROTMA}, {0x07A, ROTMAI}, {0x3C0, CEQ},
		{0x3C8, CEQH}, {0x3D0, CEQB}, {0x240, CGT}, {0x248, CGTH}, {0x250, CGTB}, {0x2C0, CLGT}, {0x2C8, CLGTH},
		{0x2D0, CLGTB}, {0x3D8, HEQ}, {0x258, HGT}, {0x2D8, HLGT}, {0x2C4, FA}, {0x2C5, FS}, {0x2C6, FM},
		{0x1B8, FREST}, {0x1B9, FRSQEST}, {0x3D4, FI}, {0x3C2, FCEQ}, {0x3CA, FCMEQ}, {0x2C2, FCGT},
		{0x2CA, FCMGT}, {0x3BA, FSCRWR}, {0x398, FSCRRD}, {0x2CC, DFA}, {0x2CD, DFS}, {0x2CE, DFM},
		{0x3B9, FRDS}, {0x3B8, FESD}, {0x00D, RDCH}, {0x00F, RCHCNT}, {0x10D, WRCH}, {0x000, STOP},
		{0x140, STOPD}, {0x001, LNOP}, {0x201, NOP}, {0x002, SYNC}, {0x003, DSYNC}, {0x00C, MFSPR},
		{0x10C, MTSPR}, {0x1F4, CBD}, {0x1F5, CHD}, {0x1F6, CWD}, {0x1F7, CDD}, {0x1D4, CBX}, {0x1D5, CHX},
		{0x1D6, CWX}, {0x1D7, CDX}};
	for (auto& e : rr) put(e.op, 11, e.e);
	return t;
}

std::mutex g_atomic_lock;

constexpr u64 TIMEBASE_HZ = 79'800'000;   // the decrementer counts down at the timebase rate

u64 host_ns() {
	return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
	               std::chrono::steady_clock::now().time_since_epoch()).count());
}

// --- MFC ------------------------------------------------------------------------
constexpr u32 MFC_PUT = 0x20, MFC_GET = 0x40, MFC_GETLLAR = 0xD0, MFC_PUTLLC = 0xB4, MFC_PUTLLUC = 0xB0,
              MFC_PUTQLLUC = 0xB8;

void dma(SPU& s, u32 cmd, u32 lsa, u32 ea, u32 size) {
	lsa &= LS_MASK;
	if (lsa + size > LS_SIZE) size = LS_SIZE - lsa;
	if (cmd & MFC_GET) std::memcpy(&s.ls[lsa], vm_ptr(ea), size);
	else std::memcpy(vm_ptr(ea), &s.ls[lsa], size);
}

bool valid_ea(SPU& s, u32 ea, u32 size) {
	// the first 64 KB of the guest address space is never mapped
	if (ea >= 0x10000 && u64(ea) + size <= 0x100000000ull) return true;
	LOG_ERROR("spu: DMA to unmapped guest address 0x%08x (+0x%x) at pc 0x%05x; stopping", ea, size, s.pc);
	s.dma_fault = true;
	return false;
}

void mfc_command(SPU& s, u32 cmd) {
	u32 base = cmd & ~0x07u;   // strip barrier/fence modifiers
	if (cmd == MFC_GETLLAR || cmd == MFC_PUTLLC || cmd == MFC_PUTLLUC || cmd == MFC_PUTQLLUC) {
		if (!valid_ea(s, s.mfc_eal & ~127u, 128)) return;
	} else if (!(base & 0x04) && !valid_ea(s, s.mfc_eal, s.mfc_size)) {
		return;
	}
	switch (cmd) {
	case MFC_GETLLAR: {
		std::lock_guard lock(g_atomic_lock);
		u32 ea = s.mfc_eal & ~127u;
		std::memcpy(&s.ls[s.mfc_lsa & LS_MASK & ~127u], vm_ptr(ea), 128);
		std::memcpy(s.reservation_data, vm_ptr(ea), 128);
		s.reservation_ea = ea;
		s.has_reservation = true;
		s.atomic_status = 4;   // GETLLAR succeeded
		return;
	}
	case MFC_PUTLLC: {
		std::lock_guard lock(g_atomic_lock);
		u32 ea = s.mfc_eal & ~127u;
		bool ok = s.has_reservation && s.reservation_ea == ea &&
		          std::memcmp(s.reservation_data, vm_ptr(ea), 128) == 0;
		if (ok) std::memcpy(vm_ptr(ea), &s.ls[s.mfc_lsa & LS_MASK & ~127u], 128);
		s.has_reservation = false;
		s.atomic_status = ok ? 0 : 1;   // 0 = stored, 1 = reservation lost
		return;
	}
	case MFC_PUTLLUC:
	case MFC_PUTQLLUC: {
		std::lock_guard lock(g_atomic_lock);
		std::memcpy(vm_ptr(s.mfc_eal & ~127u), &s.ls[s.mfc_lsa & LS_MASK & ~127u], 128);
		s.atomic_status = 2;
		return;
	}
	default: break;
	}
	bool list = (base & 0x04) != 0;   // GETL / PUTL
	u32 dir = (base & 0x40) ? MFC_GET : MFC_PUT;
	if ((base & ~0x07u) != MFC_GET && (base & ~0x07u) != MFC_PUT && (base & 0xF0) != 0x40 && (base & 0xF0) != 0x20) {
		LOG_WARN("spu: unsupported MFC command 0x%x", cmd);
		return;
	}
	if (!list) {
		dma(s, dir, s.mfc_lsa, s.mfc_eal, s.mfc_size);
		return;
	}
	// list: EAL is the LS address of 8-byte elements { u16 notify, u16 size, u32 eal }
	u32 lsa = s.mfc_lsa, list_addr = s.mfc_eal & LS_MASK, count = s.mfc_size / 8;
	for (u32 i = 0; i < count; i++) {
		u32 e0 = s.ls32(list_addr + i * 8), e1 = s.ls32(list_addr + i * 8 + 4);
		u32 size = e0 & 0x7FFF;
		if (!valid_ea(s, e1, size)) return;
		dma(s, dir, lsa, e1, size);
		lsa += (size + 15) & ~15u;
		if (e0 & 0x80000000u) break;   // stall-and-notify: treat as the end
	}
}

} // namespace

const std::array<Op, 2048>& op_table() {
	static const std::array<Op, 2048> t = build_table();
	return t;
}

u32 read_channel(SPU& s, u32 ch) {
	switch (ch) {
	case 24: return s.tag_mask;          // MFC_RdTagStat: everything is complete
	case 27: return s.atomic_status;     // MFC_RdAtomicStat
	case 8: return s.decrementer - u32((host_ns() - s.decrementer_written_ns) * TIMEBASE_HZ / 1'000'000'000ull);   // SPU_RdDec
	case 0: return 0;                    // SPU_RdEventStat
	case 25: return 0;                   // MFC_RdListStallStat
	default:
		if (s.host) return s.host->read(s, ch);
		LOG_WARN("spu: read of channel %u not supported (pc 0x%05x)", ch, s.pc);
		return 0;
	}
}

void write_channel(SPU& s, u32 ch, u32 v) {
	switch (ch) {
	case 16: s.mfc_lsa = v; break;
	case 17: s.mfc_eah = v; break;
	case 18: s.mfc_eal = v; break;
	case 19: s.mfc_size = v; break;
	case 20: s.mfc_tag = v; break;
	case 21: mfc_command(s, v); break;
	case 22: s.tag_mask = v; break;
	case 23: break;                      // MFC_WrTagUpdate
	case 26: break;                      // MFC_WrListStallAck
	case 1: s.event_mask = v; break;
	case 2: break;                       // SPU_WrEventAck
	case 7: s.decrementer = v, s.decrementer_written_ns = host_ns(); break;   // SPU_WrDec
	default:
		if (s.host) return s.host->write(s, ch, v);
		LOG_WARN("spu: write of channel %u not supported (pc 0x%05x)", ch, s.pc); break;
	}
}

u32 channel_count(SPU& s, u32 ch) {
	switch (ch) {
	case 21: case 16: case 17: case 18: case 19: case 20: return 16;   // MFC queue space
	case 3: case 4: case 28: case 29: case 30: return s.host ? s.host->count(s, ch) : 1;
	default: return 1;
	}
}

template <bool RANGE>
StopReason run_impl(SPU& s, u32* code, u64 max_instructions, u32 lo, u32 hi) {
	Reg* r = s.r;
	const std::array<Op, 2048>& table = op_table();
	for (u64 n = 0; n < max_instructions; n++) {
		u32 pc = s.pc & LS_MASK & ~3u;
		if constexpr (RANGE)
			if (n && pc >= lo && pc < hi) return StopReason::entered;
		u32 w = s.ls32(pc);
		u32 next = (pc + 4) & LS_MASK;
		if ((++s.executed & 4095) == 0) s.pc_samples[(s.executed >> 12) & 63] = pc;
		u32 status = 0;
		switch (table[w >> 21]) {
#define X(op) 	case op: status = exec<op>(s, r, w, pc, next, code); break;
			SPU_OP_LIST(X)
#undef X
		default: status = exec<OP_INVALID>(s, r, w, pc, next, code); break;
		}
		if (status) return StopReason(status - 1);
		s.pc = next;
	}
	LOG_ERROR("spu: instruction limit reached at 0x%05x", s.pc);
	return StopReason::error;
}

StopReason run(SPU& s, u32* code, u64 max_instructions) { return run_impl<false>(s, code, max_instructions, 0, 0); }

StopReason run_until_in(SPU& s, u32* code, u64 max_instructions, u32 lo, u32 hi) {
	return run_impl<true>(s, code, max_instructions, lo, hi);
}

std::mutex& atomic_lock() { return g_atomic_lock; }

} // namespace spu
