// Everything recompiled PPU code needs: guest register context, big-endian
// guest memory access, and helpers for the awkward instructions.
#pragma once

#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <immintrin.h>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// VMX register. Bytes are stored reversed relative to the guest (host byte j
// is guest byte 15-j), so guest element e of an N-element vector is host lane
// N-1-e and lane-wise SSE ops work unchanged.
union alignas(16) v128 {
	__m128 f;
	__m128i i;
	u8 b[16];
	u32 w[4];
	float fl[4];
	s32 sw[4];
};

struct PPUContext {
	u64 r[32];
	double f[32];
	v128 v[32];
	u8 cr[32];          // one byte per CR bit: cr[4n+0]=LT, +1=GT, +2=EQ, +3=SO
	u64 lr, ctr;
	u8 xer_so, xer_ov, xer_ca, xer_cnt;
	u32 fpscr;
	u32 vrsave;
	u32 vscr;
	// runtime bookkeeping, not visible to the guest
	u32 thread_id;
	u32 stack_addr, stack_size;
	void* host_thread;
};

using PPUFunc = void (*)(PPUContext&);

#if defined(__clang__) || defined(__GNUC__)
#define PPU_INLINE [[gnu::always_inline]] inline
#else
#define PPU_INLINE __forceinline
#endif

#define PPU_FUNC(name) void name(PPUContext& __restrict ctx)

#if defined(__clang__)
#define PPU_TAILCALL(fn) [[clang::musttail]] return fn(ctx)
#else
#define PPU_TAILCALL(fn) do { fn(ctx); return; } while (0)
#endif

// Per-thread ring buffer of recently entered guest functions, printed on crashes.
// Define PPU_NO_TRACE to compile it out.
struct PPUCallTrace {
	u32 addr[64];
	u32 pos;
};
extern thread_local PPUCallTrace t_call_trace;
#ifdef PPU_NO_TRACE
#define PPU_ENTER(addr) ((void)0)
#else
#define PPU_ENTER(a) (t_call_trace.addr[t_call_trace.pos++ & 63] = (a))
#endif

struct PPUFuncEntry {
	u32 addr;
	PPUFunc fn;
};

struct HLEImport {
	u32 stub_addr;
	const char* module;
	const char* name;
	u32 nid;
};

// generated tables
extern const PPUFuncEntry g_ppu_funcs[];
extern const size_t g_ppu_func_count;
extern const HLEImport g_hle_imports[];
extern const size_t g_hle_import_count;
extern const char g_image_sha1[];
extern const u32 g_image_entry;
extern const u32 g_image_toc;

// runtime entry points used by generated code
void ppu_icall(PPUContext& ctx, u32 addr);
void ppu_hle_call(PPUContext& ctx, u32 import_index);
void ppu_syscall(PPUContext& ctx);
[[noreturn]] void ppu_trap(PPUContext& ctx, u32 pc, const char* why);
u64 ppu_timebase();

// --- guest memory ---------------------------------------------------------
// The whole 32-bit guest address space is one host reservation starting at g_base.
extern u8* g_base;

PPU_INLINE u8* vm_ptr(u32 ea) { return g_base + ea; }

PPU_INLINE u8 ld8(u32 ea) { return g_base[ea]; }
PPU_INLINE u16 ld16(u32 ea) { u16 v; std::memcpy(&v, g_base + ea, 2); return std::byteswap(v); }
PPU_INLINE u32 ld32(u32 ea) { u32 v; std::memcpy(&v, g_base + ea, 4); return std::byteswap(v); }
PPU_INLINE u64 ld64(u32 ea) { u64 v; std::memcpy(&v, g_base + ea, 8); return std::byteswap(v); }
PPU_INLINE void st8(u32 ea, u8 v) { g_base[ea] = v; }
PPU_INLINE void st16(u32 ea, u16 v) { v = std::byteswap(v); std::memcpy(g_base + ea, &v, 2); }
PPU_INLINE void st32(u32 ea, u32 v) { v = std::byteswap(v); std::memcpy(g_base + ea, &v, 4); }
PPU_INLINE void st64(u32 ea, u64 v) { v = std::byteswap(v); std::memcpy(g_base + ea, &v, 8); }
PPU_INLINE float ldf32(u32 ea) { return std::bit_cast<float>(ld32(ea)); }
PPU_INLINE double ldf64(u32 ea) { return std::bit_cast<double>(ld64(ea)); }
PPU_INLINE void stf32(u32 ea, float v) { st32(ea, std::bit_cast<u32>(v)); }
PPU_INLINE void stf64(u32 ea, double v) { st64(ea, std::bit_cast<u64>(v)); }

PPU_INLINE __m128i bswap128_mask() {
	return _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
}
PPU_INLINE v128 ldv(u32 ea) {
	v128 r;
	r.i = _mm_shuffle_epi8(_mm_load_si128(reinterpret_cast<const __m128i*>(g_base + ea)), bswap128_mask());
	return r;
}
PPU_INLINE void stv(u32 ea, const v128& v) {
	_mm_store_si128(reinterpret_cast<__m128i*>(g_base + ea), _mm_shuffle_epi8(v.i, bswap128_mask()));
}
PPU_INLINE void ppu_dcbz(u32 ea) { std::memset(g_base + (ea & ~127u), 0, 128); }

// --- condition register ---------------------------------------------------
PPU_INLINE void ppu_cr_s64(PPUContext& ctx, int f, s64 v) {
	ctx.cr[4 * f + 0] = v < 0;
	ctx.cr[4 * f + 1] = v > 0;
	ctx.cr[4 * f + 2] = v == 0;
	ctx.cr[4 * f + 3] = ctx.xer_so;
}
template <typename T>
PPU_INLINE void ppu_cmp_s(PPUContext& ctx, int f, T a, T b) {
	ctx.cr[4 * f + 0] = a < b;
	ctx.cr[4 * f + 1] = a > b;
	ctx.cr[4 * f + 2] = a == b;
	ctx.cr[4 * f + 3] = ctx.xer_so;
}
template <typename T>
PPU_INLINE void ppu_cmp_u(PPUContext& ctx, int f, T a, T b) { ppu_cmp_s(ctx, f, a, b); }

PPU_INLINE void ppu_fcmp(PPUContext& ctx, int f, double a, double b) {
	bool un = std::isnan(a) || std::isnan(b);
	ctx.cr[4 * f + 0] = !un && a < b;
	ctx.cr[4 * f + 1] = !un && a > b;
	ctx.cr[4 * f + 2] = !un && a == b;
	ctx.cr[4 * f + 3] = un;
}
PPU_INLINE u64 ppu_mfcr(const PPUContext& ctx) {
	u32 v = 0;
	for (int i = 0; i < 32; i++) v |= u32(ctx.cr[i] & 1) << (31 - i);
	return v;
}
PPU_INLINE void ppu_mtcrf(PPUContext& ctx, u32 fxm, u64 v) {
	for (int f = 0; f < 8; f++)
		if (fxm & (0x80u >> f))
			for (int b = 0; b < 4; b++) ctx.cr[4 * f + b] = (v >> (31 - (4 * f + b))) & 1;
}
PPU_INLINE void ppu_cr6_vec(PPUContext& ctx, const v128& r) {
	int m = _mm_movemask_epi8(r.i);
	ctx.cr[24] = m == 0xFFFF;   // all true
	ctx.cr[25] = 0;
	ctx.cr[26] = m == 0;        // all false
	ctx.cr[27] = 0;
}
PPU_INLINE u64 ppu_get_xer(const PPUContext& ctx) {
	return (u64(ctx.xer_so) << 31) | (u64(ctx.xer_ov) << 30) | (u64(ctx.xer_ca) << 29) | ctx.xer_cnt;
}
PPU_INLINE void ppu_set_xer(PPUContext& ctx, u64 v) {
	ctx.xer_so = (v >> 31) & 1;
	ctx.xer_ov = (v >> 30) & 1;
	ctx.xer_ca = (v >> 29) & 1;
	ctx.xer_cnt = v & 0x7F;
}

// --- integer helpers ------------------------------------------------------
PPU_INLINE u64 ppu_dup32(u64 x) { u64 l = u32(x); return l | (l << 32); }
PPU_INLINE bool ppu_carry3(u64 a, u64 b, u64 c) {
	u64 s = a + b;
	return s < a || s + c < s;
}
PPU_INLINE u64 ppu_slw(u64 s, u64 n) { n &= 63; return n > 31 ? 0 : u32(u32(s) << n); }
PPU_INLINE u64 ppu_srw(u64 s, u64 n) { n &= 63; return n > 31 ? 0 : u32(s) >> n; }
PPU_INLINE u64 ppu_sld(u64 s, u64 n) { n &= 127; return n > 63 ? 0 : s << n; }
PPU_INLINE u64 ppu_srd(u64 s, u64 n) { n &= 127; return n > 63 ? 0 : s >> n; }
PPU_INLINE u64 ppu_sraw(PPUContext& ctx, u64 s, u64 n) {
	s32 x = s32(s);
	n &= 63;
	if (n > 31) {
		ctx.xer_ca = x < 0;
		return u64(s64(x < 0 ? -1 : 0));
	}
	ctx.xer_ca = x < 0 && (u32(x) & ((1u << n) - 1)) != 0;
	return u64(s64(x >> n));
}
PPU_INLINE u64 ppu_srad(PPUContext& ctx, u64 s, u64 n) {
	s64 x = s64(s);
	n &= 127;
	if (n > 63) {
		ctx.xer_ca = x < 0;
		return u64(x < 0 ? -1 : 0);
	}
	ctx.xer_ca = x < 0 && n && (u64(x) & ((1ull << n) - 1)) != 0;
	return u64(x >> n);
}
PPU_INLINE u64 ppu_divw(u64 a, u64 b) {
	s32 x = s32(a), y = s32(b);
	if (y == 0 || (x == INT32_MIN && y == -1)) return 0;
	return u32(x / y);
}
PPU_INLINE u64 ppu_divwu(u64 a, u64 b) { return u32(b) ? u32(a) / u32(b) : 0; }
PPU_INLINE u64 ppu_divd(u64 a, u64 b) {
	s64 x = s64(a), y = s64(b);
	if (y == 0 || (x == INT64_MIN && y == -1)) return 0;
	return u64(x / y);
}
PPU_INLINE u64 ppu_divdu(u64 a, u64 b) { return b ? a / b : 0; }
PPU_INLINE u64 ppu_mulh_u64(u64 a, u64 b) { return u64((unsigned __int128)a * b >> 64); }
PPU_INLINE s64 ppu_mulh_s64(u64 a, u64 b) { return s64((__int128)s64(a) * s64(b) >> 64); }

// --- floating point -------------------------------------------------------
PPU_INLINE u64 ppu_fctiw(double x, bool truncate) {
	if (std::isnan(x)) return 0x80000000u;
	double r = truncate ? std::trunc(x) : std::nearbyint(x);
	if (r >= 2147483647.0) return 0x7FFFFFFFu;
	if (r <= -2147483648.0) return 0x80000000u;
	return u32(s32(r));
}
PPU_INLINE u64 ppu_fctid(double x, bool truncate) {
	if (std::isnan(x)) return 0x8000000000000000ull;
	double r = truncate ? std::trunc(x) : std::nearbyint(x);
	if (r >= 9223372036854775807.0) return 0x7FFFFFFFFFFFFFFFull;
	if (r <= -9223372036854775808.0) return 0x8000000000000000ull;
	return u64(s64(r));
}

// --- vector helpers (guest element e lives in host lane N-1-e) -------------
PPU_INLINE __m128i ppu_vspltw(const v128& b, int e) {
	return _mm_set1_epi32(b.sw[3 - e]);
}
PPU_INLINE v128 ppu_vsldoi(const v128& a, const v128& b, int sh) {
	// guest bytes: result[i] = (a||b)[i+sh]; host byte j = guest byte 15-j
	u8 cat[32];
	for (int i = 0; i < 16; i++) { cat[i] = a.b[15 - i]; cat[16 + i] = b.b[15 - i]; }
	v128 r;
	for (int i = 0; i < 16; i++) r.b[15 - i] = cat[i + sh];
	return r;
}
PPU_INLINE v128 ppu_vperm(const v128& a, const v128& b, const v128& c) {
	u8 cat[32];
	for (int i = 0; i < 16; i++) { cat[i] = a.b[15 - i]; cat[16 + i] = b.b[15 - i]; }
	v128 r;
	for (int i = 0; i < 16; i++) r.b[15 - i] = cat[c.b[15 - i] & 31];
	return r;
}
PPU_INLINE v128 ppu_lvsl(u32 sh) {
	v128 r;
	for (int i = 0; i < 16; i++) r.b[15 - i] = u8(sh + i);
	return r;
}
PPU_INLINE v128 ppu_lvsr(u32 sh) {
	v128 r;
	for (int i = 0; i < 16; i++) r.b[15 - i] = u8(16 - sh + i);
	return r;
}
PPU_INLINE v128 ppu_vmrghw(const v128& a, const v128& b) {
	// guest: a0 b0 a1 b1
	v128 r;
	r.w[3] = a.w[3]; r.w[2] = b.w[3]; r.w[1] = a.w[2]; r.w[0] = b.w[2];
	return r;
}
PPU_INLINE v128 ppu_vmrglw(const v128& a, const v128& b) {
	// guest: a2 b2 a3 b3
	v128 r;
	r.w[3] = a.w[1]; r.w[2] = b.w[1]; r.w[1] = a.w[0]; r.w[0] = b.w[0];
	return r;
}
PPU_INLINE v128 ppu_vslw(const v128& a, const v128& b) {
	v128 r;
	for (int i = 0; i < 4; i++) r.w[i] = a.w[i] << (b.w[i] & 31);
	return r;
}
PPU_INLINE v128 ppu_vsrw(const v128& a, const v128& b) {
	v128 r;
	for (int i = 0; i < 4; i++) r.w[i] = a.w[i] >> (b.w[i] & 31);
	return r;
}
PPU_INLINE v128 ppu_vsraw(const v128& a, const v128& b) {
	v128 r;
	for (int i = 0; i < 4; i++) r.sw[i] = a.sw[i] >> (b.w[i] & 31);
	return r;
}
PPU_INLINE v128 ppu_vctsxs(const v128& b, int scale) {
	v128 r;
	for (int i = 0; i < 4; i++) {
		double x = double(b.fl[i]) * double(1u << scale);
		r.sw[i] = std::isnan(x) ? 0 : x >= 2147483647.0 ? INT32_MAX : x <= -2147483648.0 ? INT32_MIN : s32(x);
	}
	return r;
}
PPU_INLINE v128 ppu_vcfsx(const v128& b, int scale) {
	v128 r;
	for (int i = 0; i < 4; i++) r.fl[i] = float(double(b.sw[i]) / double(1u << scale));
	return r;
}
