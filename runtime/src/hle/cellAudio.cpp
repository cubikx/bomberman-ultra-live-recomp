// cellAudio: the game writes float samples into ring buffers ("ports"); the
// system consumes one 256-sample block per period and advances each port's
// read index. Here a host thread does the consuming and plays through SDL3.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "runtime/kernel.h"

namespace {

constexpr u32 BLOCK_SAMPLES = 256;
constexpr u32 SAMPLE_RATE = 48000;
constexpr u32 MAX_PORTS = 8;
constexpr u32 CELL_AUDIO_ERROR_PORT_FULL = 0x80310705;
constexpr u32 CELL_AUDIO_ERROR_PORT_NOT_OPEN = 0x80310706;

struct Port {
	bool open = false, started = false;
	u32 channels = 2, blocks = 8;
	u32 addr = 0, size = 0;
	float level = 1.0f;
};

std::mutex g_lock;
Port g_ports[MAX_PORTS];
u32 g_read_index_addr;            // one big-endian u64 read index per port
std::atomic<bool> g_running{false};
SDL_AudioStream* g_stream;

void mix_thread() {
	using namespace std::chrono;
	auto period = nanoseconds(1'000'000'000ull * BLOCK_SAMPLES / SAMPLE_RATE);
	auto next = steady_clock::now();
	std::vector<float> out(BLOCK_SAMPLES * 2);
	static const bool log_levels = env_flag("AUDIO_LOG");
	// AUDIO_DUMP=1: also write what is played to audio_dump.f32 (raw stereo float, 48 kHz)
	static std::FILE* dump = env_flag("AUDIO_DUMP") ? std::fopen("audio_dump.f32", "wb") : nullptr;
	float peak = 0.0f, ch_peak[8] = {};
	u64 blocks = 0;
	while (g_running) {
		std::fill(out.begin(), out.end(), 0.0f);
		{
			std::lock_guard lock(g_lock);
			for (u32 p = 0; p < MAX_PORTS; p++) {
				Port& port = g_ports[p];
				if (!port.open || !port.started) continue;
				u64 idx = ld64(g_read_index_addr + p * 8);
				u32 block = port.addr + u32(idx % port.blocks) * BLOCK_SAMPLES * port.channels * 4;
				for (u32 s = 0; s < BLOCK_SAMPLES; s++) {
					u32 frame = block + s * port.channels * 4;
					if (log_levels)
						for (u32 c = 0; c < port.channels && c < 8; c++)
							ch_peak[c] = std::max(ch_peak[c], std::abs(ldf32(frame + c * 4)));
					float l = ldf32(frame), r = ldf32(frame + 4);
					if (port.channels >= 6) {
						// 5.1/7.1 order: L R C LFE ... -> fold the center into both sides
						float c = ldf32(frame + 8) * 0.7071f;
						l += c;
						r += c;
					}
					out[s * 2] += l * port.level;
					out[s * 2 + 1] += r * port.level;
				}
				st64(g_read_index_addr + p * 8, (idx + 1) % port.blocks);
			}
		}
		if (log_levels) {
			for (float v : out) peak = std::max(peak, std::abs(v));
			if (++blocks % 1000 == 0) {
				LOG_INFO("audio: %llu blocks, peak level %.3f (port channels %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f)",
				         (unsigned long long)blocks, peak, ch_peak[0], ch_peak[1], ch_peak[2], ch_peak[3], ch_peak[4],
				         ch_peak[5], ch_peak[6], ch_peak[7]);
				peak = 0.0f;
				std::fill(std::begin(ch_peak), std::end(ch_peak), 0.0f);
			}
		}
		if (dump) std::fwrite(out.data(), sizeof(float), out.size(), dump);
		next += period;
		if (g_stream) {
			SDL_PutAudioStreamData(g_stream, out.data(), int(out.size() * sizeof(float)));
			// Follow the output device's clock: when ~40 ms are queued, wait for it to drain
			// instead of dropping blocks (a dropped block is an audible click).
			constexpr int limit = int(SAMPLE_RATE / 25 * 2 * sizeof(float));
			while (g_running && SDL_GetAudioStreamQueued(g_stream) > limit) {
				precise_sleep_us(1000);
				next = std::chrono::steady_clock::now();
			}
		}
		precise_sleep_until(next);
	}
}

} // namespace

HLE_FUNC(cellAudio, cellAudioInit) {
	if (g_running) return ret(ctx, CELL_OK);
	g_read_index_addr = vm::alloc(vm::MAIN_BASE, MAX_PORTS * 8, 0x1000);
	if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(SAMPLE_RATE)};
		g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		if (g_stream) SDL_ResumeAudioStreamDevice(g_stream);
	}
	if (!g_stream) LOG_WARN("audio: no output device (%s); continuing silently", SDL_GetError());
	g_running = true;
	std::thread(mix_thread).detach();
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellAudio, cellAudioQuit) {
	g_running = false;
	ret(ctx, CELL_OK);
}

// cellAudioPortOpen(CellAudioPortParam* {u64 nChannel, u64 nBlock, u64 attr, float level}, u32* portNum)
HLE_FUNC(cellAudio, cellAudioPortOpen) {
	u32 param = arg32(ctx, 0);
	std::lock_guard lock(g_lock);
	for (u32 p = 0; p < MAX_PORTS; p++) {
		Port& port = g_ports[p];
		if (port.open) continue;
		port.channels = u32(ld64(param));
		port.blocks = u32(ld64(param + 8));
		u64 attr = ld64(param + 16);
		port.level = (attr & 0x1000) ? ldf32(param + 24) : 1.0f;   // CELL_AUDIO_PORTATTR_INITLEVEL
		port.size = port.channels * port.blocks * BLOCK_SAMPLES * 4;
		port.addr = vm::alloc(vm::MAIN_BASE, port.size, 0x10000);
		port.open = true;
		port.started = false;
		st64(g_read_index_addr + p * 8, 0);
		st32(arg32(ctx, 1), p);
		LOG_INFO("audio: port %u open (%u channels, %u blocks)", p, port.channels, port.blocks);
		return ret(ctx, CELL_OK);
	}
	ret(ctx, CELL_AUDIO_ERROR_PORT_FULL);
}

// cellAudioGetPortConfig(portNum, CellAudioPortConfig*)
HLE_FUNC(cellAudio, cellAudioGetPortConfig) {
	u32 p = arg32(ctx, 0), out = arg32(ctx, 1);
	std::lock_guard lock(g_lock);
	if (p >= MAX_PORTS || !g_ports[p].open) return ret(ctx, CELL_AUDIO_ERROR_PORT_NOT_OPEN);
	const Port& port = g_ports[p];
	st32(out + 0, g_read_index_addr + p * 8);   // readIndexAddr
	st32(out + 4, port.started ? 2 : 1);         // status: ready / run
	st64(out + 8, port.channels);
	st64(out + 16, port.blocks);
	st32(out + 24, port.size);
	st32(out + 28, port.addr);
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellAudio, cellAudioPortStart) {
	u32 p = arg32(ctx, 0);
	std::lock_guard lock(g_lock);
	if (p >= MAX_PORTS || !g_ports[p].open) return ret(ctx, CELL_AUDIO_ERROR_PORT_NOT_OPEN);
	g_ports[p].started = true;
	ret(ctx, CELL_OK);
}
