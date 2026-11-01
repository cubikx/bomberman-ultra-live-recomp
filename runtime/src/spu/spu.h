// Cell SPU: interpreter, local store and MFC (DMA) for running SPU code from the game.
#pragma once

#include <memory>
#include <mutex>

#include "runtime/kernel.h"

namespace spu {

struct SPU;

// Channels owned by whoever runs the program (mailboxes, signal notification).
// Reads may block the calling host thread.
struct ChannelHost {
	virtual ~ChannelHost() = default;
	virtual u32 read(SPU& s, u32 ch) = 0;
	virtual void write(SPU& s, u32 ch, u32 v) = 0;
	virtual u32 count(SPU& s, u32 ch) = 0;
};

constexpr u32 LS_SIZE = 0x40000;
constexpr u32 LS_MASK = LS_SIZE - 1;

// 128-bit register; w[0] is the preferred slot (the first word in memory order).
struct alignas(16) Reg {
	u32 w[4];
	u8 byte(u32 i) const { return u8(w[i >> 2] >> (24 - 8 * (i & 3))); }
	void set_byte(u32 i, u8 v) {
		u32 s = 24 - 8 * (i & 3);
		w[i >> 2] = (w[i >> 2] & ~(0xFFu << s)) | (u32(v) << s);
	}
	u16 half(u32 i) const { return u16(w[i >> 1] >> (16 - 16 * (i & 1))); }
	void set_half(u32 i, u16 v) {
		u32 s = 16 - 16 * (i & 1);
		w[i >> 1] = (w[i >> 1] & ~(0xFFFFu << s)) | (u32(v) << s);
	}
	float f(u32 i) const { return std::bit_cast<float>(w[i]); }
	void set_f(u32 i, float v) { w[i] = std::bit_cast<u32>(v); }
	u64 d(u32 i) const { return (u64(w[2 * i]) << 32) | w[2 * i + 1]; }
	void set_d(u32 i, u64 v) { w[2 * i] = u32(v >> 32); w[2 * i + 1] = u32(v); }
	static Reg splat(u32 v) { return {{v, v, v, v}}; }
};

struct SPU {
	Reg r[128];
	u32 pc = 0;
	std::unique_ptr<u8[]> ls{new u8[LS_SIZE]()};

	// MFC command parameters (channels 16..21)
	u32 mfc_lsa = 0, mfc_eah = 0, mfc_eal = 0, mfc_size = 0, mfc_tag = 0;
	u32 tag_mask = 0;
	u32 atomic_status = 0;
	bool has_reservation = false;
	u32 reservation_ea = 0;
	u8 reservation_data[128];
	u32 event_mask = 0, decrementer = 0;
	u64 decrementer_written_ns = 0;   // host time of the last SPU_WrDec
	u64 executed = 0;
	u32 pc_samples[64] = {};   // every 4096th pc, for finding where a stuck program spins
	bool dma_fault = false;    // a DMA addressed unmapped guest memory; the program is stopped
	u32 last_channel_op = 0;   // (ch << 1) | is_write of the most recent channel access
	ChannelHost* host = nullptr;   // mailbox/signal channels; null for SPURS jobs

	u32 ls32(u32 a) const { u32 v; std::memcpy(&v, &ls[a & LS_MASK & ~3u], 4); return std::byteswap(v); }
	void set_ls32(u32 a, u32 v) { v = std::byteswap(v); std::memcpy(&ls[a & LS_MASK & ~3u], &v, 4); }
	Reg read_qw(u32 a) const;
	void write_qw(u32 a, const Reg& v);
};

// `entered`: run_until_in() reached [lo, hi) (compiled code takes over from there)
enum class StopReason { stop, halt, error, entered };

// Run from spu.pc until a `stop` instruction (its signal code in *code) or an error.
StopReason run(SPU& spu, u32* code, u64 max_instructions = 2'000'000'000ull);

// Same, but also returns `entered` once pc reaches [lo, hi) after at least one instruction.
StopReason run_until_in(SPU& spu, u32* code, u64 max_instructions, u32 lo, u32 hi);

// Serializes the 128-byte atomic MFC commands; host code updating a structure the
// SPU program accesses with GETLLAR/PUTLLC takes it too.
std::mutex& atomic_lock();

// The 32-bit stop instruction for a given signal code (for planting return traps).
constexpr u32 stop_instruction(u32 code) { return code & 0x3FFF; }

} // namespace spu
