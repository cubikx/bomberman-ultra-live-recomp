// RSX command processor. Runs on its own host thread, consuming the command
// buffer the game writes through libgcm, much like the real GPU front end.
// Every method write lands in the register file (regs.h); methods with side
// effects (draws, clears, uploads, semaphores, flips) are acted on here.
#include "rsx.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../video/presenter.h"
#include "regs.h"
#include "renderer.h"

namespace rsx {

u32 g_regs[0x10000 / 4];
u32 g_vp_ucode[VP_MAX_INSTRUCTIONS * 4];
float g_vp_constants[VP_MAX_CONSTANTS * 4];
u64 g_vp_ucode_version = 1;
u64 g_vp_constants_version = 1;

namespace {

// Methods with side effects (byte offsets of the register being written)
enum Method : u32 {
	NV406E_SET_REFERENCE = 0x0050,
	NV406E_SEMAPHORE_OFFSET = 0x0064,
	NV406E_SEMAPHORE_ACQUIRE = 0x0068,
	NV406E_SEMAPHORE_RELEASE = 0x006C,
	NV4097_SET_SEMAPHORE_OFFSET = 0x1D6C,
	NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE = 0x1D70,
	NV4097_TEXTURE_READ_SEMAPHORE_RELEASE = 0x1D74,
	NV0039_SET_CONTEXT_DMA_BUFFER_IN = 0x2184,
	NV0039_SET_CONTEXT_DMA_BUFFER_OUT = 0x2188,
	NV0039_OFFSET_IN = 0x230C,
	NV0039_OFFSET_OUT = 0x2310,
	NV0039_PITCH_IN = 0x2314,
	NV0039_PITCH_OUT = 0x2318,
	NV0039_LINE_LENGTH_IN = 0x231C,
	NV0039_LINE_COUNT = 0x2320,
	NV0039_FORMAT = 0x2324,
	NV0039_BUFFER_NOTIFY = 0x2328,
	GCM_FLIP_COMMAND = 0xFEAC,
};

// FIFO command word encodings
constexpr u32 OLD_JUMP_MASK = 0xE0000003, OLD_JUMP = 0x20000000;
constexpr u32 NEW_JUMP_MASK = 0x00000003, NEW_JUMP = 0x00000001;
constexpr u32 CALL_MASK = 0x00000003, CALL = 0x00000002;
constexpr u32 RETURN_MASK = 0xFFFF0003, RETURN = 0x00020000;
constexpr u32 NON_INCREMENT = 0x40000000;

constexpr u32 FLIP_LABEL_OFFSET = 1 * 16;

Config g_cfg;
DisplayBuffer g_display[8];
std::atomic<u32> g_flip_handler{0};
std::atomic<u32> g_flip_mode{1};
std::atomic<u64> g_flips{0};
u64 g_draws = 0;
u32 g_vp_load_word = 0, g_vp_const_word = 0;

std::array<u64, 0x10000 / 4> g_method_counts{};

u32 ctrl_put() { return ld32(g_cfg.ctrl_addr + 0x00); }
u32 ctrl_get() { return ld32(g_cfg.ctrl_addr + 0x04); }
void set_ctrl_get(u32 v) { st32(g_cfg.ctrl_addr + 0x04, v); }
void set_ctrl_ref(u32 v) { st32(g_cfg.ctrl_addr + 0x08, v); }
u32 label_ea(u32 offset) { return g_cfg.labels_addr + offset; }

u32 dma_ea(u32 dma, u32 offset) {
	return dma == CONTEXT_DMA_MAIN ? io_to_ea(offset) : LOCAL_BASE + offset;
}

// Debugging aid: --dump-frame N writes the Nth presented frame to frame_N.bmp.
u64 g_dump_frame = 0;
u64 g_dump_count = 1;   // --dump-count K: K frames in all
u64 g_dump_step = 1;    // --dump-step S: every Sth frame from N (one per screen in a menu walk)

void dump_bmp(const std::vector<u32>& bgra, u32 w, u32 h, u64 frame) {
	std::string name = "frame_" + std::to_string(frame) + ".bmp";
	FILE* f = std::fopen(name.c_str(), "wb");
	if (!f) return;
	u32 data_size = w * h * 4;
	u8 hdr[54] = {'B', 'M'};
	auto put32 = [&](int off, u32 v) { std::memcpy(hdr + off, &v, 4); };
	put32(2, 54 + data_size);
	put32(10, 54);
	put32(14, 40);
	put32(18, w);
	put32(22, u32(-s32(h)));   // top-down rows
	hdr[26] = 1;
	hdr[28] = 32;
	put32(34, data_size);
	std::fwrite(hdr, 1, 54, f);
	std::fwrite(bgra.data(), 4, size_t(w) * h, f);
	std::fclose(f);
	LOG_INFO("rsx: wrote %s", name.c_str());
}

void present(u32 id) {
	const DisplayBuffer& b = g_display[id & 7];
	if (!b.width || !b.height || !b.pitch) return;
	u64 frame = g_flips + 1;
	bool dump = g_dump_frame && frame >= g_dump_frame && (frame - g_dump_frame) % g_dump_step == 0 &&
	            frame < g_dump_frame + g_dump_count * g_dump_step;
	// normally the GPU copies the frame to the window itself; dumps need it in memory
	if (!dump && gpu::present(LOCAL_BASE + b.offset, b.width, b.height)) return;
	// bring GPU-rendered content back into guest memory, then show it
	gpu::sync_to_memory(LOCAL_BASE + b.offset, b.pitch * b.height);
	std::vector<u32> pixels(size_t(b.width) * b.height);
	const u8* src = vm_ptr(LOCAL_BASE + b.offset);
	for (u32 y = 0; y < b.height; y++) {
		const u32* row = reinterpret_cast<const u32*>(src + size_t(y) * b.pitch);
		u32* dst = &pixels[size_t(y) * b.width];
		// guest XRGB big-endian words -> host BGRA bytes is a byte reversal
		for (u32 x = 0; x < b.width; x++) dst[x] = std::byteswap(row[x]) | 0xFF000000u;
	}
	if (dump) {
		// at the render scale, as shown in the window, when the GPU has the frame
		std::vector<u32> scaled;
		u32 sw, sh;
		if (gpu::read_scaled(LOCAL_BASE + b.offset, b.width, b.height, scaled, sw, sh)) dump_bmp(scaled, sw, sh, frame);
		else dump_bmp(pixels, b.width, b.height, frame);
		if (frame == g_dump_frame) gpu::dump_render_targets();
	}
	video::submit_frame(pixels.data(), b.width, b.height);
}

void log_method_census() {
	std::vector<std::pair<u64, u32>> used;
	for (u32 i = 0; i < g_method_counts.size(); i++)
		if (g_method_counts[i]) used.push_back({g_method_counts[i], i * 4});
	std::sort(used.rbegin(), used.rend());
	std::string s;
	char buf[40];
	for (const auto& [n, r] : used) {
		std::snprintf(buf, sizeof buf, " %04x:%llu", r, (unsigned long long)(n / 600));
		s += buf;
	}
	LOG_INFO("rsx: %zu distinct methods, per frame (offset:count):%s", used.size(), s.c_str());
}

void flip(PPUContext& ctx, u32 id) {
	using namespace std::chrono;
	static auto next_vblank = steady_clock::now();
	present(id);   // records the GPU copy to the window (or presents from memory)
	gpu::end_frame();
	u64 n = ++g_flips;
	// label 1 is libgcm's flip label: 0 means "no flip pending". The game's
	// inline cellGcmSetWaitFlip makes the GPU wait on it before the next frame.
	st32(label_ea(FLIP_LABEL_OFFSET), 0);
	static const auto start = steady_clock::now();
	static auto last_log = start;
	static u64 last_n = 0;
	if (n == 1 || n % 300 == 0) {
		auto now = steady_clock::now();
		double secs = duration<double>(now - last_log).count();
		double frames = double(n - last_n);
		gpu::Stats st = gpu::take_stats();
		LOG_INFO("rsx: flip #%llu at %.1f s, %.1f fps | per frame: %.0f draws, %.1f submits (GPU %.2f ms, %.2f ms "
		         "waiting, ring peak %.1f MB), %.1f readbacks (%.0f KB), %.1f uploads (%.0f KB), %.0f KB hashed",
		         (unsigned long long)n, duration<double>(now - start).count(), frames / secs, st.draws / frames,
		         st.submits / frames, st.gpu_us / frames / 1000.0, st.wait_us / frames / 1000.0, st.ring_peak / 1048576.0,
		         st.readbacks / frames,
		         st.readback_bytes / frames / 1024.0, st.uploads / frames, st.upload_bytes / frames / 1024.0,
		         st.hashed_bytes / frames / 1024.0);
		last_log = now;
		last_n = n;
	}
	if (n == 600) log_method_census();
	// Pace to 59.94 Hz like the real display. The game asks for "hsync" flips
	// (no vblank wait), but on the console the GPU itself kept it near 60 fps.
	// Debugging: RSX_UNTHROTTLED=1 skips the pacing to measure how fast frames can be made.
	static const bool unthrottled = env_flag("RSX_UNTHROTTLED");
	next_vblank += nanoseconds(16683350);
	auto now = steady_clock::now();
	if (next_vblank < now || unthrottled) next_vblank = now;
	precise_sleep_until(next_vblank);
	if (u32 handler = g_flip_handler.load()) {
		ctx.r[3] = 1;   // head
		call_guest_opd(ctx, handler);
	}
}

void semaphore_acquire(u32 offset, u32 value) {
	u32 ea = label_ea(offset);
	if (ld32(ea) == value) return;
	gpu::flush();   // work queued so far may be what the PPU is waiting for
	auto start = std::chrono::steady_clock::now();
	bool reported = false;
	while (ld32(ea) != value) {
		std::this_thread::yield();
		if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
			reported = true;
			LOG_WARN("rsx: semaphore acquire waiting >2 s: label offset 0x%x wants 0x%08x, has 0x%08x", offset,
			         value, ld32(ea));
		}
	}
}

// NV0039: linear memory-to-memory copy (byte lines with pitches)
void nv0039_copy() {
	u32 src = dma_ea(reg(NV0039_SET_CONTEXT_DMA_BUFFER_IN), reg(NV0039_OFFSET_IN));
	u32 dst = dma_ea(reg(NV0039_SET_CONTEXT_DMA_BUFFER_OUT), reg(NV0039_OFFSET_OUT));
	s32 pitch_in = s32(reg(NV0039_PITCH_IN)), pitch_out = s32(reg(NV0039_PITCH_OUT));
	u32 len = reg(NV0039_LINE_LENGTH_IN), lines = reg(NV0039_LINE_COUNT);
	u32 in_inc = reg(NV0039_FORMAT) & 0xFF, out_inc = (reg(NV0039_FORMAT) >> 8) & 0xFF;
	if (!lines || !len) return;
	static bool logged = false;
	if (!logged) {
		logged = true;
		LOG_INFO("rsx: NV0039 copy 0x%08x -> 0x%08x, %u lines of %u bytes, pitch %d -> %d", src, dst, lines, len,
		         pitch_in, pitch_out);
	}
	if (!pitch_in) pitch_in = s32(len);
	if (!pitch_out) pitch_out = s32(len);
	auto copy = [=] {
		for (u32 l = 0; l < lines; l++) {
			u32 s = src + u32(s32(l) * pitch_in), d = dst + u32(s32(l) * pitch_out);
			if (in_inc <= 1 && out_inc <= 1) {
				std::memmove(vm_ptr(d), vm_ptr(s), len);
			} else {
				for (u32 i = 0; i < len; i++) st8(d + i * std::max(out_inc, 1u), ld8(s + i * std::max(in_inc, 1u)));
			}
		}
		gpu::memory_written(dst, u32(std::abs(pitch_out)) * lines);
	};
	// Copying out of a render target: let the GPU catch up first instead of stalling for it.
	// The data reaches guest memory a little later than on the console, where the copy
	// also ran asynchronously (this game copies a downscaled frame right before the flip).
	if (gpu::sync_to_memory(src, u32(std::abs(pitch_in)) * lines, false)) gpu::after_gpu(copy);
	else copy();
}

void method(PPUContext& ctx, u32 r, u32 arg) {
	g_method_counts[(r & 0xFFFF) / 4]++;
	g_regs[(r & 0xFFFF) / 4] = arg;

	if (r >= m::TRANSFORM_PROGRAM && r < m::TRANSFORM_PROGRAM + 0x80) {
		if (g_vp_load_word < VP_MAX_INSTRUCTIONS * 4) g_vp_ucode[g_vp_load_word++] = arg;
		g_vp_ucode_version++;
		return;
	}
	if (r >= m::TRANSFORM_CONSTANT && r < m::TRANSFORM_CONSTANT + 0x80) {
		if (g_vp_const_word < VP_MAX_CONSTANTS * 4) g_vp_constants[g_vp_const_word++] = std::bit_cast<float>(arg);
		g_vp_constants_version++;
		return;
	}

	switch (r) {
	case NV406E_SET_REFERENCE: set_ctrl_ref(arg); break;
	case NV406E_SEMAPHORE_ACQUIRE: semaphore_acquire(reg(NV406E_SEMAPHORE_OFFSET), arg); break;
	case NV406E_SEMAPHORE_RELEASE: gpu::flush(); st32(label_ea(reg(NV406E_SEMAPHORE_OFFSET)), arg); break;
	case NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE:
		// the back end writes the value with its red/blue bytes swapped
		gpu::flush();
		st32(label_ea(reg(NV4097_SET_SEMAPHORE_OFFSET)),
		     (arg & 0xFF00FF00) | ((arg & 0xFF) << 16) | ((arg >> 16) & 0xFF));
		break;
	case NV4097_TEXTURE_READ_SEMAPHORE_RELEASE: gpu::flush(); st32(label_ea(reg(NV4097_SET_SEMAPHORE_OFFSET)), arg); break;
	case m::TRANSFORM_PROGRAM_LOAD: g_vp_load_word = arg * 4; break;
	case m::TRANSFORM_CONSTANT_LOAD: g_vp_const_word = arg * 4; break;
	case m::CLEAR_SURFACE: gpu::clear(arg); break;
	case m::BEGIN_END:
		if (arg) {
			gpu::begin(arg);
		} else {
			gpu::end();
			g_draws++;
		}
		break;
	case m::DRAW_ARRAYS: gpu::add_range(false, arg & 0xFFFFFF, (arg >> 24) + 1); break;
	case m::DRAW_INDEX_ARRAY: gpu::add_range(true, arg & 0xFFFFFF, (arg >> 24) + 1); break;
	case NV0039_BUFFER_NOTIFY: nv0039_copy(); break;
	case GCM_FLIP_COMMAND: flip(ctx, arg); break;
	default: break;
	}
}

void fifo_thread() {
	PPUContext& ctx = attach_guest_context("rsx_fifo", 0x10000);
	gpu::init();
	u32 call_return = 0;
	bool in_call = false;
	u32 idle_spins = 0;
	for (;;) {
		u32 get = ctrl_get(), put = ctrl_put();
		if (get == put) {
			gpu::poll();
			if (++idle_spins > 64) std::this_thread::sleep_for(std::chrono::microseconds(100));
			else std::this_thread::yield();
			continue;
		}
		idle_spins = 0;
		u32 cmd = ld32(io_to_ea(get));
		if ((cmd & OLD_JUMP_MASK) == OLD_JUMP) { set_ctrl_get(cmd & 0x1FFFFFFC); continue; }
		if ((cmd & NEW_JUMP_MASK) == NEW_JUMP) { set_ctrl_get(cmd & 0xFFFFFFFC); continue; }
		if ((cmd & CALL_MASK) == CALL) {
			call_return = get + 4;
			in_call = true;
			set_ctrl_get(cmd & 0xFFFFFFFC);
			continue;
		}
		if ((cmd & RETURN_MASK) == RETURN) {
			set_ctrl_get(in_call ? call_return : get + 4);
			in_call = false;
			continue;
		}
		if (cmd == 0) { set_ctrl_get(get + 4); continue; }   // nop

		u32 count = (cmd >> 18) & 0x7FF;
		u32 r = cmd & 0xFFFC;
		bool inc = !(cmd & NON_INCREMENT);
		for (u32 i = 0; i < count; i++)
			method(ctx, inc ? r + i * 4 : r, ld32(io_to_ea(get + 4 + i * 4)));
		set_ctrl_get(get + 4 + count * 4);
	}
}

} // namespace

const Config& init(u32 io_address, u32 io_size) {
	vm::commit(LOCAL_BASE, LOCAL_SIZE);
	vm::reserve(LOCAL_BASE, LOCAL_SIZE);
	g_cfg.io_address = io_address;
	g_cfg.io_size = io_size;
	g_cfg.ctrl_addr = vm::alloc(vm::MAIN_BASE, 0x1000, 0x1000);
	g_cfg.labels_addr = vm::alloc(vm::MAIN_BASE, 0x1000, 0x1000);
	// labels start out as 0x1337C0D3 on the real console
	for (u32 i = 0; i < 256; i++) st32(g_cfg.labels_addr + i * 16, 0x1337C0D3);
	st32(g_cfg.labels_addr + FLIP_LABEL_OFFSET, 0);   // no flip pending at boot
	LOG_INFO("rsx: local memory 0x%08x+0x%x, io 0x%08x+0x%x, control 0x%08x, labels 0x%08x", LOCAL_BASE,
	         LOCAL_SIZE, io_address, io_size, g_cfg.ctrl_addr, g_cfg.labels_addr);
	std::thread(fifo_thread).detach();
	// hang watchdog: once frames have started, a 5 s gap means something is stuck
	std::thread([] {
		u64 last = 0;
		int quiet = 0;
		bool dumped = false;
		for (;;) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			u64 flips = g_flips;
			quiet = (flips == last && flips > 0) ? quiet + 1 : 0;
			last = flips;
			if (quiet == 5 && !dumped) {
				dumped = true;
				u32 get = ctrl_get();
				LOG_WARN("rsx: no flip for 5 s (fifo get 0x%x put 0x%x); thread states:", get, ctrl_put());
				dump_all_threads();
				std::string words;
				char buf[24];
				for (u32 off = get >= 32 ? get - 32 : 0; off <= get + 32; off += 4) {
					std::snprintf(buf, sizeof buf, "%s%08x", off == get ? " >" : "  ", ld32(io_to_ea(off)));
					words += buf;
				}
				LOG_WARN("  fifo around get:%s", words.c_str());
			}
		}
	}).detach();
	return g_cfg;
}

const Config& config() { return g_cfg; }

void set_dump_frame(u64 n, u64 count, u64 step) {
	g_dump_frame = n;
	g_dump_count = std::max<u64>(count, 1);
	g_dump_step = std::max<u64>(step, 1);
}

u64 frames() { return g_flips; }

u32 io_to_ea(u32 offset) {
	if (offset >= g_cfg.io_size) fatal("rsx: IO offset 0x%08x outside the mapped 0x%x bytes", offset, g_cfg.io_size);
	return g_cfg.io_address + offset;
}

bool ea_to_offset(u32 ea, u32& offset) {
	if (ea >= LOCAL_BASE && ea < LOCAL_BASE + LOCAL_SIZE) {
		offset = ea - LOCAL_BASE;
		return true;
	}
	if (ea >= g_cfg.io_address && ea - g_cfg.io_address < g_cfg.io_size) {
		offset = ea - g_cfg.io_address;
		return true;
	}
	return false;
}

u32 location_ea(u32 location, u32 offset) {
	return location == 0 ? LOCAL_BASE + offset : io_to_ea(offset);
}

void set_display_buffer(u32 id, const DisplayBuffer& b) {
	g_display[id & 7] = b;
	LOG_INFO("rsx: display buffer %u = offset 0x%x, %ux%u, pitch %u", id, b.offset, b.width, b.height, b.pitch);
}

void set_flip_handler(u32 opd) { g_flip_handler = opd; }
void set_flip_mode(u32 mode) { g_flip_mode = mode; }

} // namespace rsx
