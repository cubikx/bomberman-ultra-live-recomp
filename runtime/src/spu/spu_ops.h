// SPU instruction semantics, shared by the interpreter (spu.cpp) and the static
// SPU recompiler's output (recomp/spu_recompile.py). exec<OP> runs one instruction;
// the recompiled code calls it with constant words, so decoding folds away.
#pragma once

#include <smmintrin.h>

#include <array>
#include <bit>
#include <cmath>

#include "spu/spu.h"

namespace spu {

#define SPU_OP_LIST(X) \
	/* RRR */ \
	X(SELB) X(SHUFB) X(MPYA) X(FNMS) X(FMA) X(FMS) \
	/* RI18 */ \
	X(ILA) X(HBRA) X(HBRR) \
	/* RI10 */ \
	X(AI) X(AHI) X(SFI) X(SFHI) X(ANDI) X(ANDHI) X(ANDBI) X(ORI) X(ORHI) X(ORBI) X(XORI) X(XORHI) X(XORBI) \
	X(MPYI) X(MPYUI) X(CEQI) X(CEQHI) X(CEQBI) X(CGTI) X(CGTHI) X(CGTBI) X(CLGTI) X(CLGTHI) X(CLGTBI) X(LQD) \
	X(STQD) X(HEQI) X(HGTI) X(HLGTI) \
	/* RI16 */ \
	X(BR) X(BRA) X(BRSL) X(BRASL) X(BRNZ) X(BRZ) X(BRHNZ) X(BRHZ) X(IL) X(ILH) X(ILHU) X(IOHL) X(FSMBI) X(LQA) \
	X(LQR) X(STQA) X(STQR) \
	/* RI8 */ \
	X(CFLTS) X(CFLTU) X(CSFLT) X(CUFLT) \
	/* RR / RI7 */ \
	X(LQX) X(STQX) X(BI) X(BISL) X(IRET) X(BISLED) X(BIZ) X(BINZ) X(BIHZ) X(BIHNZ) X(HBR) X(OP_ADD) X(AH) X(SF) \
	X(SFH) X(ADDX) X(CG) X(CGX) X(SFX) X(BG) X(BGX) X(MPY) X(MPYU) X(MPYH) X(MPYS) X(MPYHH) X(MPYHHA) X(MPYHHU) \
	X(MPYHHAU) X(CLZ) X(CNTB) X(FSMB) X(FSMH) X(FSM) X(GBB) X(GBH) X(GB) X(AVGB) X(ABSDB) X(SUMB) X(XSBH) \
	X(XSHW) X(XSWD) X(AND) X(ANDC) X(OR) X(ORC) X(ORX) X(XOR) X(NAND) X(NOR) X(EQV) X(SHLH) X(SHLHI) X(SHL) \
	X(SHLI) X(SHLQBI) X(SHLQBII) X(SHLQBY) X(SHLQBYI) X(SHLQBYBI) X(ROTH) X(ROTHI) X(ROT) X(ROTI) X(ROTQBY) \
	X(ROTQBYI) X(ROTQBYBI) X(ROTQBI) X(ROTQBII) X(ROTHM) X(ROTHMI) X(ROTM) X(ROTMI) X(ROTQMBY) X(ROTQMBYI) \
	X(ROTQMBYBI) X(ROTQMBI) X(ROTQMBII) X(ROTMAH) X(ROTMAHI) X(ROTMA) X(ROTMAI) X(CEQ) X(CEQH) X(CEQB) X(CGT) \
	X(CGTH) X(CGTB) X(CLGT) X(CLGTH) X(CLGTB) X(HEQ) X(HGT) X(HLGT) X(FA) X(FS) X(FM) X(FREST) X(FRSQEST) X(FI) \
	X(FCEQ) X(FCMEQ) X(FCGT) X(FCMGT) X(FSCRWR) X(FSCRRD) X(DFA) X(DFS) X(DFM) X(FRDS) X(FESD) X(RDCH) X(RCHCNT) \
	X(WRCH) X(STOP) X(STOPD) X(LNOP) X(NOP) X(SYNC) X(DSYNC) X(MFSPR) X(MTSPR) X(CBD) X(CHD) X(CWD) X(CDD) \
	X(CBX) X(CHX) X(CWX) X(CDX)

enum Op : u8 {
	OP_INVALID,
#define X(op) op,
	SPU_OP_LIST(X)
#undef X
	OP_COUNT
};

// 11-bit major opcode (w >> 21) -> Op
const std::array<Op, 2048>& op_table();

// channel access and MFC commands (spu.cpp)
u32 read_channel(SPU& s, u32 ch);
void write_channel(SPU& s, u32 ch, u32 v);
u32 channel_count(SPU& s, u32 ch);

inline s32 sext(u32 v, u32 bits) { return s32(v << (32 - bits)) >> (32 - bits); }

// A register as one 128-bit integer (byte 0, the most significant, is w[0]'s top byte)
using u128 = unsigned __int128;
inline u128 to_u128(const Reg& a) { return (u128(a.d(0)) << 64) | a.d(1); }
inline Reg from_u128(u128 v) {
	Reg r;
	r.set_d(0, u64(v >> 64));
	r.set_d(1, u64(v));
	return r;
}

// whole-quadword byte shifts (byte 0 is the most significant)
inline Reg qw_shl_bytes(const Reg& a, u32 n) { return n >= 16 ? Reg{} : from_u128(to_u128(a) << (8 * n)); }
inline Reg qw_shr_bytes(const Reg& a, u32 n) { return n >= 16 ? Reg{} : from_u128(to_u128(a) >> (8 * n)); }
inline Reg qw_rot_bytes(const Reg& a, u32 n) {
	n &= 15;
	u128 v = to_u128(a);
	return n ? from_u128((v << (8 * n)) | (v >> (128 - 8 * n))) : a;
}

// SHUFB with SSSE3/SSE4.1. Host words are little-endian, so SPU byte i lives at
// host byte i ^ 3; swap into SPU byte order, shuffle, swap back.
inline Reg shufb(const Reg& a, const Reg& b, const Reg& c) {
	const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
	__m128i va = _mm_shuffle_epi8(_mm_load_si128(reinterpret_cast<const __m128i*>(a.w)), swap);
	__m128i vb = _mm_shuffle_epi8(_mm_load_si128(reinterpret_cast<const __m128i*>(b.w)), swap);
	__m128i vc = _mm_shuffle_epi8(_mm_load_si128(reinterpret_cast<const __m128i*>(c.w)), swap);
	__m128i idx = _mm_and_si128(vc, _mm_set1_epi8(0x0F));
	__m128i pick_b = _mm_cmpeq_epi8(_mm_and_si128(vc, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
	__m128i v = _mm_blendv_epi8(_mm_shuffle_epi8(va, idx), _mm_shuffle_epi8(vb, idx), pick_b);
	// control bytes 10xxxxxx -> 0x00, 110xxxxx -> 0xFF, 111xxxxx -> 0x80
	__m128i is_c0 = _mm_cmpeq_epi8(_mm_and_si128(vc, _mm_set1_epi8(char(0xC0))), _mm_set1_epi8(char(0xC0)));
	__m128i is_e0 = _mm_cmpeq_epi8(_mm_and_si128(vc, _mm_set1_epi8(char(0xE0))), _mm_set1_epi8(char(0xE0)));
	__m128i special = _mm_blendv_epi8(is_c0, _mm_set1_epi8(char(0x80)), is_e0);
	v = _mm_blendv_epi8(v, special, vc);   // blendv tests the top bit of each control byte
	Reg r;
	_mm_store_si128(reinterpret_cast<__m128i*>(r.w), _mm_shuffle_epi8(v, swap));
	return r;
}
inline Reg qw_shl_bits(const Reg& a, u32 n) {   // n < 8
	if (!n) return a;
	Reg r;
	for (int i = 0; i < 4; i++) r.w[i] = (a.w[i] << n) | (i < 3 ? a.w[i + 1] >> (32 - n) : 0);
	return r;
}
inline Reg qw_shr_bits(const Reg& a, u32 n) {
	if (!n) return a;
	Reg r;
	for (int i = 0; i < 4; i++) r.w[i] = (a.w[i] >> n) | (i > 0 ? a.w[i - 1] << (32 - n) : 0);
	return r;
}
inline Reg qw_rot_bits(const Reg& a, u32 n) {
	if (!n) return a;
	Reg r;
	for (int i = 0; i < 4; i++) r.w[i] = (a.w[i] << n) | (a.w[(i + 1) & 3] >> (32 - n));
	return r;
}

// insertion control masks (cbd/chd/cwd/cdd)
inline Reg insert_control(u32 t, u32 size) {
	Reg r;
	for (u32 i = 0; i < 16; i++) r.set_byte(i, u8(0x10 + i));
	t &= 16 - size;
	for (u32 i = 0; i < size; i++) r.set_byte(t + i, u8((size == 8 ? 0 : 4 - size) + i));
	return r;
}

inline float f_from_int(s64 v, int shift) { return float(std::ldexp(double(v), -shift)); }

inline s32 sat_s32(double v) {
	if (std::isnan(v)) return 0;
	if (v >= 2147483647.0) return INT32_MAX;
	if (v <= -2147483648.0) return INT32_MIN;
	return s32(v);
}
inline u32 sat_u32(double v) {
	if (std::isnan(v) || v <= 0) return 0;
	if (v >= 4294967295.0) return UINT32_MAX;
	return u32(v);
}

#define SPU_ALWAYS_INLINE __attribute__((always_inline)) inline

// Execute instruction word w (opcode OP) at pc. `next` holds pc + 4 on entry; branches
// change it. Returns 0 to continue, or 1 + StopReason to stop (s.pc is then set).
template <Op OP>
SPU_ALWAYS_INLINE u32 exec(SPU& s, Reg* r, u32 w, u32 pc, u32& next, u32* code) {
	u32 rt = w & 0x7F, ra = (w >> 7) & 0x7F, rb = (w >> 14) & 0x7F;
	s32 i10 = sext((w >> 14) & 0x3FF, 10);
	s32 i7 = sext((w >> 14) & 0x7F, 7);
	u32 i16 = (w >> 7) & 0xFFFF;
	Reg& t = r[rt];
	const Reg& A = r[ra];
	const Reg& B = r[rb];
	Reg res;
	(void)rb; (void)i10; (void)i7; (void)i16; (void)res; (void)code; (void)next; (void)t; (void)A; (void)B;

	// ---- RRR (rt in bits 21..27, rc in 0..6)
	if constexpr (OP == SELB) {
		{
		u32 rt3 = (w >> 21) & 0x7F;
		const Reg& C = r[w & 0x7F];
		for (int i = 0; i < 4; i++) res.w[i] = (C.w[i] & B.w[i]) | (~C.w[i] & A.w[i]);
		r[rt3] = res;
		}
	}
	else if constexpr (OP == SHUFB) {
		{
		u32 rt3 = (w >> 21) & 0x7F;
		r[rt3] = shufb(A, B, r[w & 0x7F]);
		}
	}
	else if constexpr (OP == MPYA) {
		{
		u32 rt3 = (w >> 21) & 0x7F;
		const Reg& C = r[w & 0x7F];
		for (int i = 0; i < 4; i++) res.w[i] = u32(s32(s16(A.w[i])) * s32(s16(B.w[i])) + s32(C.w[i]));
		r[rt3] = res;
		}
	}
	else if constexpr (OP == FMA || OP == FMS || OP == FNMS) {
		{
		u32 rt3 = (w >> 21) & 0x7F;
		const Reg& C = r[w & 0x7F];
		Op op = OP;
		for (int i = 0; i < 4; i++) {
			float p = A.f(i) * B.f(i);
			res.set_f(i, op == FMA ? p + C.f(i) : op == FMS ? p - C.f(i) : C.f(i) - p);
		}
		r[rt3] = res;
		}
	}
	// ---- immediates
	else if constexpr (OP == ILA) {
		t = Reg::splat((w >> 7) & 0x3FFFF);
	}
	else if constexpr (OP == IL) {
		t = Reg::splat(u32(sext(i16, 16)));
	}
	else if constexpr (OP == ILH) {
		t = Reg::splat(i16 | (i16 << 16));
	}
	else if constexpr (OP == ILHU) {
		t = Reg::splat(i16 << 16);
	}
	else if constexpr (OP == IOHL) {
		for (int i = 0; i < 4; i++) t.w[i] |= i16;
	}
	else if constexpr (OP == FSMBI) {
		for (u32 i = 0; i < 16; i++) t.set_byte(i, (i16 >> (15 - i)) & 1 ? 0xFF : 0);
	}
	// ---- loads / stores
	else if constexpr (OP == LQD) {
		t = s.read_qw(A.w[0] + (u32(i10) << 4));
	}
	else if constexpr (OP == STQD) {
		s.write_qw(A.w[0] + (u32(i10) << 4), t);
	}
	else if constexpr (OP == LQX) {
		t = s.read_qw(A.w[0] + B.w[0]);
	}
	else if constexpr (OP == STQX) {
		s.write_qw(A.w[0] + B.w[0], t);
	}
	else if constexpr (OP == LQA) {
		t = s.read_qw(u32(sext(i16, 16)) << 2);
	}
	else if constexpr (OP == STQA) {
		s.write_qw(u32(sext(i16, 16)) << 2, t);
	}
	else if constexpr (OP == LQR) {
		t = s.read_qw(pc + (u32(sext(i16, 16)) << 2));
	}
	else if constexpr (OP == STQR) {
		s.write_qw(pc + (u32(sext(i16, 16)) << 2), t);
	}
	// ---- branches
	else if constexpr (OP == BR) {
		next = (pc + (u32(sext(i16, 16)) << 2)) & LS_MASK;
	}
	else if constexpr (OP == BRA) {
		next = (u32(sext(i16, 16)) << 2) & LS_MASK;
	}
	else if constexpr (OP == BRSL) {
		t = Reg{{next, 0, 0, 0}}; next = (pc + (u32(sext(i16, 16)) << 2)) & LS_MASK;
	}
	else if constexpr (OP == BRASL) {
		t = Reg{{next, 0, 0, 0}}; next = (u32(sext(i16, 16)) << 2) & LS_MASK;
	}
	else if constexpr (OP == BRZ) {
		if (t.w[0] == 0) next = (pc + (u32(sext(i16, 16)) << 2)) & LS_MASK;
	}
	else if constexpr (OP == BRNZ) {
		if (t.w[0] != 0) next = (pc + (u32(sext(i16, 16)) << 2)) & LS_MASK;
	}
	else if constexpr (OP == BRHZ) {
		if ((t.w[0] & 0xFFFF) == 0) next = (pc + (u32(sext(i16, 16)) << 2)) & LS_MASK;
	}
	else if constexpr (OP == BRHNZ) {
		if ((t.w[0] & 0xFFFF) != 0) next = (pc + (u32(sext(i16, 16)) << 2)) & LS_MASK;
	}
	else if constexpr (OP == BI) {
		next = A.w[0] & LS_MASK & ~3u;
	}
	else if constexpr (OP == BISL) {
		{ u32 tgt = A.w[0] & LS_MASK & ~3u; t = Reg{{next, 0, 0, 0}}; next = tgt; }
	}
	else if constexpr (OP == BIZ) {
		if (t.w[0] == 0) next = A.w[0] & LS_MASK & ~3u;
	}
	else if constexpr (OP == BINZ) {
		if (t.w[0] != 0) next = A.w[0] & LS_MASK & ~3u;
	}
	else if constexpr (OP == BIHZ) {
		if ((t.w[0] & 0xFFFF) == 0) next = A.w[0] & LS_MASK & ~3u;
	}
	else if constexpr (OP == BIHNZ) {
		if ((t.w[0] & 0xFFFF) != 0) next = A.w[0] & LS_MASK & ~3u;
	}
	else if constexpr (OP == HBR || OP == HBRA || OP == HBRR || OP == NOP || OP == LNOP || OP == SYNC || OP == DSYNC) {
	}
	// ---- word / halfword arithmetic
	else if constexpr (OP == OP_ADD) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] + B.w[i]; t = res;
	}
	else if constexpr (OP == AI) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] + u32(i10); t = res;
	}
	else if constexpr (OP == SF) {
		for (int i = 0; i < 4; i++) res.w[i] = B.w[i] - A.w[i]; t = res;
	}
	else if constexpr (OP == SFI) {
		for (int i = 0; i < 4; i++) res.w[i] = u32(i10) - A.w[i]; t = res;
	}
	else if constexpr (OP == AH) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, u16(A.half(i) + B.half(i))); t = res;
	}
	else if constexpr (OP == AHI) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, u16(A.half(i) + u16(i10))); t = res;
	}
	else if constexpr (OP == SFH) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, u16(B.half(i) - A.half(i))); t = res;
	}
	else if constexpr (OP == SFHI) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, u16(u16(i10) - A.half(i))); t = res;
	}
	else if constexpr (OP == ADDX) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] + B.w[i] + (t.w[i] & 1); t = res;
	}
	else if constexpr (OP == SFX) {
		for (int i = 0; i < 4; i++) res.w[i] = B.w[i] + ~A.w[i] + (t.w[i] & 1); t = res;
	}
	else if constexpr (OP == CG) {
		for (int i = 0; i < 4; i++) res.w[i] = (u64(A.w[i]) + B.w[i]) >> 32; t = res;
	}
	else if constexpr (OP == CGX) {
		for (int i = 0; i < 4; i++) res.w[i] = (u64(A.w[i]) + B.w[i] + (t.w[i] & 1)) >> 32; t = res;
	}
	else if constexpr (OP == BG) {
		for (int i = 0; i < 4; i++) res.w[i] = B.w[i] >= A.w[i] ? 1 : 0; t = res;
	}
	else if constexpr (OP == BGX) {
		for (int i = 0; i < 4; i++)
			res.w[i] = (t.w[i] & 1) ? (B.w[i] >= A.w[i] ? 1 : 0) : (B.w[i] > A.w[i] ? 1 : 0);
		t = res;
	}
	else if constexpr (OP == MPY) {
		for (int i = 0; i < 4; i++) res.w[i] = u32(s32(s16(A.w[i])) * s32(s16(B.w[i]))); t = res;
	}
	else if constexpr (OP == MPYU) {
		for (int i = 0; i < 4; i++) res.w[i] = (A.w[i] & 0xFFFF) * (B.w[i] & 0xFFFF); t = res;
	}
	else if constexpr (OP == MPYI) {
		for (int i = 0; i < 4; i++) res.w[i] = u32(s32(s16(A.w[i])) * i10); t = res;
	}
	else if constexpr (OP == MPYUI) {
		for (int i = 0; i < 4; i++) res.w[i] = (A.w[i] & 0xFFFF) * (u32(i10) & 0xFFFF); t = res;
	}
	else if constexpr (OP == MPYH) {
		for (int i = 0; i < 4; i++) res.w[i] = ((A.w[i] >> 16) * (B.w[i] & 0xFFFF)) << 16; t = res;
	}
	else if constexpr (OP == MPYS) {
		for (int i = 0; i < 4; i++) res.w[i] = u32((s32(s16(A.w[i])) * s32(s16(B.w[i]))) >> 16);
		t = res;
	}
	else if constexpr (OP == MPYHH) {
		for (int i = 0; i < 4; i++) res.w[i] = u32(s32(s16(A.w[i] >> 16)) * s32(s16(B.w[i] >> 16))); t = res;
	}
	else if constexpr (OP == MPYHHU) {
		for (int i = 0; i < 4; i++) res.w[i] = (A.w[i] >> 16) * (B.w[i] >> 16); t = res;
	}
	else if constexpr (OP == MPYHHA) {
		for (int i = 0; i < 4; i++) res.w[i] = t.w[i] + u32(s32(s16(A.w[i] >> 16)) * s32(s16(B.w[i] >> 16))); t = res;
	}
	else if constexpr (OP == MPYHHAU) {
		for (int i = 0; i < 4; i++) res.w[i] = t.w[i] + (A.w[i] >> 16) * (B.w[i] >> 16); t = res;
	}
	// ---- logical
	else if constexpr (OP == AND) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] & B.w[i]; t = res;
	}
	else if constexpr (OP == ANDC) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] & ~B.w[i]; t = res;
	}
	else if constexpr (OP == OR) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] | B.w[i]; t = res;
	}
	else if constexpr (OP == ORC) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] | ~B.w[i]; t = res;
	}
	else if constexpr (OP == XOR) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] ^ B.w[i]; t = res;
	}
	else if constexpr (OP == NAND) {
		for (int i = 0; i < 4; i++) res.w[i] = ~(A.w[i] & B.w[i]); t = res;
	}
	else if constexpr (OP == NOR) {
		for (int i = 0; i < 4; i++) res.w[i] = ~(A.w[i] | B.w[i]); t = res;
	}
	else if constexpr (OP == EQV) {
		for (int i = 0; i < 4; i++) res.w[i] = ~(A.w[i] ^ B.w[i]); t = res;
	}
	else if constexpr (OP == ANDI) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] & u32(i10); t = res;
	}
	else if constexpr (OP == ORI) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] | u32(i10); t = res;
	}
	else if constexpr (OP == XORI) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] ^ u32(i10); t = res;
	}
	else if constexpr (OP == ANDHI) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, A.half(i) & u16(i10)); t = res;
	}
	else if constexpr (OP == ORHI) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, A.half(i) | u16(i10)); t = res;
	}
	else if constexpr (OP == XORHI) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, A.half(i) ^ u16(i10)); t = res;
	}
	else if constexpr (OP == ANDBI) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, A.byte(i) & u8(i10)); t = res;
	}
	else if constexpr (OP == ORBI) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, A.byte(i) | u8(i10)); t = res;
	}
	else if constexpr (OP == XORBI) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, A.byte(i) ^ u8(i10)); t = res;
	}
	else if constexpr (OP == ORX) {
		t = Reg{{A.w[0] | A.w[1] | A.w[2] | A.w[3], 0, 0, 0}};
	}
	// ---- counting, sign extension, masks
	else if constexpr (OP == CLZ) {
		for (int i = 0; i < 4; i++) res.w[i] = std::countl_zero(A.w[i]); t = res;
	}
	else if constexpr (OP == CNTB) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, u8(std::popcount(A.byte(i)))); t = res;
	}
	else if constexpr (OP == XSBH) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, u16(s16(s8(A.half(i))))); t = res;
	}
	else if constexpr (OP == XSHW) {
		for (int i = 0; i < 4; i++) res.w[i] = u32(s32(s16(A.w[i]))); t = res;
	}
	else if constexpr (OP == XSWD) {
		for (u32 i = 0; i < 2; i++) res.set_d(i, u64(s64(s32(u32(A.d(i)))))); t = res;
	}
	else if constexpr (OP == FSM) {
		for (int i = 0; i < 4; i++) res.w[i] = (A.w[0] >> (3 - i)) & 1 ? 0xFFFFFFFF : 0; t = res;
	}
	else if constexpr (OP == FSMH) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, (A.w[0] >> (7 - i)) & 1 ? 0xFFFF : 0); t = res;
	}
	else if constexpr (OP == FSMB) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, (A.w[0] >> (15 - i)) & 1 ? 0xFF : 0); t = res;
	}
	else if constexpr (OP == GB) {
		{ u32 v = 0; for (int i = 0; i < 4; i++) v = (v << 1) | (A.w[i] & 1); t = Reg{{v, 0, 0, 0}}; }
	}
	else if constexpr (OP == GBH) {
		{ u32 v = 0; for (u32 i = 0; i < 8; i++) v = (v << 1) | (A.half(i) & 1); t = Reg{{v, 0, 0, 0}}; }
	}
	else if constexpr (OP == GBB) {
		{ u32 v = 0; for (u32 i = 0; i < 16; i++) v = (v << 1) | (A.byte(i) & 1); t = Reg{{v, 0, 0, 0}}; }
	}
	else if constexpr (OP == AVGB) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, u8((A.byte(i) + B.byte(i) + 1) >> 1)); t = res;
	}
	else if constexpr (OP == ABSDB) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, u8(std::abs(int(A.byte(i)) - int(B.byte(i))))); t = res;
	}
	else if constexpr (OP == SUMB) {
		for (int i = 0; i < 4; i++) {
			u32 sb = 0, sa = 0;
			for (u32 j = 0; j < 4; j++) { sb += B.byte(i * 4 + j); sa += A.byte(i * 4 + j); }
			res.w[i] = (sb << 16) | (sa & 0xFFFF);
		}
		t = res;
	}
	// ---- word / halfword shifts and rotates
	else if constexpr (OP == SHL) {
		for (int i = 0; i < 4; i++) { u32 n = B.w[i] & 0x3F; res.w[i] = n > 31 ? 0 : A.w[i] << n; } t = res;
	}
	else if constexpr (OP == SHLI) {
		{ u32 n = u32(i7) & 0x3F; for (int i = 0; i < 4; i++) res.w[i] = n > 31 ? 0 : A.w[i] << n; t = res; }
	}
	else if constexpr (OP == SHLH) {
		for (u32 i = 0; i < 8; i++) { u32 n = B.half(i) & 0x1F; res.set_half(i, n > 15 ? 0 : u16(A.half(i) << n)); } t = res;
	}
	else if constexpr (OP == SHLHI) {
		{ u32 n = u32(i7) & 0x1F; for (u32 i = 0; i < 8; i++) res.set_half(i, n > 15 ? 0 : u16(A.half(i) << n)); t = res; }
	}
	else if constexpr (OP == ROT) {
		for (int i = 0; i < 4; i++) res.w[i] = std::rotl(A.w[i], int(B.w[i] & 31)); t = res;
	}
	else if constexpr (OP == ROTI) {
		for (int i = 0; i < 4; i++) res.w[i] = std::rotl(A.w[i], i7 & 31); t = res;
	}
	else if constexpr (OP == ROTH) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, std::rotl(A.half(i), int(B.half(i) & 15))); t = res;
	}
	else if constexpr (OP == ROTHI) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, std::rotl(A.half(i), i7 & 15)); t = res;
	}
	else if constexpr (OP == ROTM) {
		for (int i = 0; i < 4; i++) { u32 n = (0 - B.w[i]) & 0x3F; res.w[i] = n > 31 ? 0 : A.w[i] >> n; } t = res;
	}
	else if constexpr (OP == ROTMI) {
		{ u32 n = (0 - u32(i7)) & 0x3F; for (int i = 0; i < 4; i++) res.w[i] = n > 31 ? 0 : A.w[i] >> n; t = res; }
	}
	else if constexpr (OP == ROTHM) {
		for (u32 i = 0; i < 8; i++) { u32 n = (0 - B.half(i)) & 0x1F; res.set_half(i, n > 15 ? 0 : u16(A.half(i) >> n)); } t = res;
	}
	else if constexpr (OP == ROTHMI) {
		{ u32 n = (0 - u32(i7)) & 0x1F; for (u32 i = 0; i < 8; i++) res.set_half(i, n > 15 ? 0 : u16(A.half(i) >> n)); t = res; }
	}
	else if constexpr (OP == ROTMA) {
		for (int i = 0; i < 4; i++) { u32 n = (0 - B.w[i]) & 0x3F; res.w[i] = u32(s32(A.w[i]) >> std::min(n, 31u)); } t = res;
	}
	else if constexpr (OP == ROTMAI) {
		{ u32 n = (0 - u32(i7)) & 0x3F; for (int i = 0; i < 4; i++) res.w[i] = u32(s32(A.w[i]) >> std::min(n, 31u)); t = res; }
	}
	else if constexpr (OP == ROTMAH) {
		for (u32 i = 0; i < 8; i++) { u32 n = (0 - B.half(i)) & 0x1F; res.set_half(i, u16(s16(A.half(i)) >> std::min(n, 15u))); } t = res;
	}
	else if constexpr (OP == ROTMAHI) {
		{ u32 n = (0 - u32(i7)) & 0x1F; for (u32 i = 0; i < 8; i++) res.set_half(i, u16(s16(A.half(i)) >> std::min(n, 15u))); t = res; }
	}
	// ---- quadword shifts and rotates
	else if constexpr (OP == SHLQBI) {
		t = qw_shl_bits(A, B.w[0] & 7);
	}
	else if constexpr (OP == SHLQBII) {
		t = qw_shl_bits(A, u32(i7) & 7);
	}
	else if constexpr (OP == SHLQBY) {
		t = qw_shl_bytes(A, B.w[0] & 0x1F);
	}
	else if constexpr (OP == SHLQBYI) {
		t = qw_shl_bytes(A, u32(i7) & 0x1F);
	}
	else if constexpr (OP == SHLQBYBI) {
		t = qw_shl_bytes(A, (B.w[0] >> 3) & 0x1F);
	}
	else if constexpr (OP == ROTQBY) {
		t = qw_rot_bytes(A, B.w[0] & 0xF);
	}
	else if constexpr (OP == ROTQBYI) {
		t = qw_rot_bytes(A, u32(i7) & 0xF);
	}
	else if constexpr (OP == ROTQBYBI) {
		t = qw_rot_bytes(A, (B.w[0] >> 3) & 0xF);
	}
	else if constexpr (OP == ROTQBI) {
		t = qw_rot_bits(A, B.w[0] & 7);
	}
	else if constexpr (OP == ROTQBII) {
		t = qw_rot_bits(A, u32(i7) & 7);
	}
	else if constexpr (OP == ROTQMBY) {
		t = qw_shr_bytes(A, (0 - B.w[0]) & 0x1F);
	}
	else if constexpr (OP == ROTQMBYI) {
		t = qw_shr_bytes(A, (0 - u32(i7)) & 0x1F);
	}
	else if constexpr (OP == ROTQMBYBI) {
		t = qw_shr_bytes(A, (0 - (B.w[0] >> 3)) & 0x1F);
	}
	else if constexpr (OP == ROTQMBI) {
		t = qw_shr_bits(A, (0 - B.w[0]) & 7);
	}
	else if constexpr (OP == ROTQMBII) {
		t = qw_shr_bits(A, (0 - u32(i7)) & 7);
	}
	// ---- compares
	else if constexpr (OP == CEQ) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] == B.w[i] ? ~0u : 0; t = res;
	}
	else if constexpr (OP == CEQI) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] == u32(i10) ? ~0u : 0; t = res;
	}
	else if constexpr (OP == CGT) {
		for (int i = 0; i < 4; i++) res.w[i] = s32(A.w[i]) > s32(B.w[i]) ? ~0u : 0; t = res;
	}
	else if constexpr (OP == CGTI) {
		for (int i = 0; i < 4; i++) res.w[i] = s32(A.w[i]) > i10 ? ~0u : 0; t = res;
	}
	else if constexpr (OP == CLGT) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] > B.w[i] ? ~0u : 0; t = res;
	}
	else if constexpr (OP == CLGTI) {
		for (int i = 0; i < 4; i++) res.w[i] = A.w[i] > u32(i10) ? ~0u : 0; t = res;
	}
	else if constexpr (OP == CEQH) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, A.half(i) == B.half(i) ? 0xFFFF : 0); t = res;
	}
	else if constexpr (OP == CEQHI) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, A.half(i) == u16(i10) ? 0xFFFF : 0); t = res;
	}
	else if constexpr (OP == CGTH) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, s16(A.half(i)) > s16(B.half(i)) ? 0xFFFF : 0); t = res;
	}
	else if constexpr (OP == CGTHI) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, s16(A.half(i)) > i10 ? 0xFFFF : 0); t = res;
	}
	else if constexpr (OP == CLGTH) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, A.half(i) > B.half(i) ? 0xFFFF : 0); t = res;
	}
	else if constexpr (OP == CLGTHI) {
		for (u32 i = 0; i < 8; i++) res.set_half(i, A.half(i) > u16(i10) ? 0xFFFF : 0); t = res;
	}
	else if constexpr (OP == CEQB) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, A.byte(i) == B.byte(i) ? 0xFF : 0); t = res;
	}
	else if constexpr (OP == CEQBI) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, A.byte(i) == u8(i10) ? 0xFF : 0); t = res;
	}
	else if constexpr (OP == CGTB) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, s8(A.byte(i)) > s8(B.byte(i)) ? 0xFF : 0); t = res;
	}
	else if constexpr (OP == CGTBI) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, s8(A.byte(i)) > s8(u8(i10)) ? 0xFF : 0); t = res;
	}
	else if constexpr (OP == CLGTB) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, A.byte(i) > B.byte(i) ? 0xFF : 0); t = res;
	}
	else if constexpr (OP == CLGTBI) {
		for (u32 i = 0; i < 16; i++) res.set_byte(i, A.byte(i) > u8(i10) ? 0xFF : 0); t = res;
	}
	else if constexpr (OP == HEQ) {
		if (A.w[0] == B.w[0]) { s.pc = pc; return 1 + u32(StopReason::halt); }
	}
	else if constexpr (OP == HEQI) {
		if (A.w[0] == u32(i10)) { s.pc = pc; return 1 + u32(StopReason::halt); }
	}
	else if constexpr (OP == HGT) {
		if (s32(A.w[0]) > s32(B.w[0])) { s.pc = pc; return 1 + u32(StopReason::halt); }
	}
	else if constexpr (OP == HGTI) {
		if (s32(A.w[0]) > i10) { s.pc = pc; return 1 + u32(StopReason::halt); }
	}
	else if constexpr (OP == HLGT) {
		if (A.w[0] > B.w[0]) { s.pc = pc; return 1 + u32(StopReason::halt); }
	}
	else if constexpr (OP == HLGTI) {
		if (A.w[0] > u32(i10)) { s.pc = pc; return 1 + u32(StopReason::halt); }
	}
	// ---- single precision float
	else if constexpr (OP == FA) {
		for (int i = 0; i < 4; i++) res.set_f(i, A.f(i) + B.f(i)); t = res;
	}
	else if constexpr (OP == FS) {
		for (int i = 0; i < 4; i++) res.set_f(i, A.f(i) - B.f(i)); t = res;
	}
	else if constexpr (OP == FM) {
		for (int i = 0; i < 4; i++) res.set_f(i, A.f(i) * B.f(i)); t = res;
	}
	else if constexpr (OP == FREST) {
		for (int i = 0; i < 4; i++) res.set_f(i, 1.0f / A.f(i)); t = res;
	}
	else if constexpr (OP == FRSQEST) {
		for (int i = 0; i < 4; i++) res.set_f(i, 1.0f / std::sqrt(std::fabs(A.f(i)))); t = res;
	}
	else if constexpr (OP == FI) {
		t = B;   // the estimates above are already exact
	}
	else if constexpr (OP == FCEQ) {
		for (int i = 0; i < 4; i++) res.w[i] = A.f(i) == B.f(i) ? ~0u : 0; t = res;
	}
	else if constexpr (OP == FCGT) {
		for (int i = 0; i < 4; i++) res.w[i] = A.f(i) > B.f(i) ? ~0u : 0; t = res;
	}
	else if constexpr (OP == FCMEQ) {
		for (int i = 0; i < 4; i++) res.w[i] = std::fabs(A.f(i)) == std::fabs(B.f(i)) ? ~0u : 0; t = res;
	}
	else if constexpr (OP == FCMGT) {
		for (int i = 0; i < 4; i++) res.w[i] = std::fabs(A.f(i)) > std::fabs(B.f(i)) ? ~0u : 0; t = res;
	}
	else if constexpr (OP == CFLTS) {
		{ int sc = 173 - int((w >> 14) & 0xFF); for (int i = 0; i < 4; i++) res.w[i] = u32(sat_s32(std::ldexp(double(A.f(i)), sc))); t = res; }
	}
	else if constexpr (OP == CFLTU) {
		{ int sc = 173 - int((w >> 14) & 0xFF); for (int i = 0; i < 4; i++) res.w[i] = sat_u32(std::ldexp(double(A.f(i)), sc)); t = res; }
	}
	else if constexpr (OP == CSFLT) {
		{ int sc = 155 - int((w >> 14) & 0xFF); for (int i = 0; i < 4; i++) res.set_f(i, f_from_int(s32(A.w[i]), sc)); t = res; }
	}
	else if constexpr (OP == CUFLT) {
		{ int sc = 155 - int((w >> 14) & 0xFF); for (int i = 0; i < 4; i++) res.set_f(i, f_from_int(A.w[i], sc)); t = res; }
	}
	else if constexpr (OP == FSCRWR) {
	}
	else if constexpr (OP == FSCRRD) {
		t = Reg{};
	}
	else if constexpr (OP == DFA) {
		for (u32 i = 0; i < 2; i++) res.set_d(i, std::bit_cast<u64>(std::bit_cast<double>(A.d(i)) + std::bit_cast<double>(B.d(i)))); t = res;
	}
	else if constexpr (OP == DFS) {
		for (u32 i = 0; i < 2; i++) res.set_d(i, std::bit_cast<u64>(std::bit_cast<double>(A.d(i)) - std::bit_cast<double>(B.d(i)))); t = res;
	}
	else if constexpr (OP == DFM) {
		for (u32 i = 0; i < 2; i++) res.set_d(i, std::bit_cast<u64>(std::bit_cast<double>(A.d(i)) * std::bit_cast<double>(B.d(i)))); t = res;
	}
	else if constexpr (OP == FESD) {
		for (u32 i = 0; i < 2; i++) res.set_d(i, std::bit_cast<u64>(double(A.f(i * 2)))); t = res;
	}
	else if constexpr (OP == FRDS) {
		res = Reg{}; for (u32 i = 0; i < 2; i++) res.set_f(i * 2, float(std::bit_cast<double>(A.d(i)))); t = res;
	}
	// ---- insertion controls
	else if constexpr (OP == CBD) {
		t = insert_control(A.w[0] + u32(i7), 1);
	}
	else if constexpr (OP == CHD) {
		t = insert_control(A.w[0] + u32(i7), 2);
	}
	else if constexpr (OP == CWD) {
		t = insert_control(A.w[0] + u32(i7), 4);
	}
	else if constexpr (OP == CDD) {
		t = insert_control(A.w[0] + u32(i7), 8);
	}
	else if constexpr (OP == CBX) {
		t = insert_control(A.w[0] + B.w[0], 1);
	}
	else if constexpr (OP == CHX) {
		t = insert_control(A.w[0] + B.w[0], 2);
	}
	else if constexpr (OP == CWX) {
		t = insert_control(A.w[0] + B.w[0], 4);
	}
	else if constexpr (OP == CDX) {
		t = insert_control(A.w[0] + B.w[0], 8);
	}
	// ---- channels and control
	else if constexpr (OP == RDCH) {
		s.last_channel_op = ra << 1; t = Reg{{read_channel(s, ra), 0, 0, 0}};
	}
	else if constexpr (OP == RCHCNT) {
		t = Reg{{channel_count(s, ra), 0, 0, 0}};
	}
	else if constexpr (OP == WRCH) {
		s.pc = pc;
		s.last_channel_op = (ra << 1) | 1;
		write_channel(s, ra, t.w[0]);
		if (s.dma_fault) return 1 + u32(StopReason::error);
	}
	else if constexpr (OP == MFSPR) {
		t = Reg{};
	}
	else if constexpr (OP == MTSPR) {
	}
	else if constexpr (OP == STOP || OP == STOPD) {
		s.pc = next;
		if (code) *code = w & 0x3FFF;
		return 1 + u32(StopReason::stop);
	}
	else {   // IRET, BISLED, and invalid encodings
		s.pc = pc;
		LOG_ERROR("spu: unsupported instruction 0x%08x at 0x%05x", w, pc);
		return 1 + u32(StopReason::error);
	}
	return 0;
}

} // namespace spu
