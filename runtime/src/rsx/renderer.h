// Vulkan backend for the RSX: render targets, shaders, vertex fetch, textures.
// All entry points run on the RSX thread.
#pragma once

#include <functional>
#include <vector>

#include "runtime/kernel.h"

namespace rsx::gpu {

void init();

void clear(u32 mask);                                 // NV4097_CLEAR_SURFACE
void begin(u32 primitive);                            // NV4097_SET_BEGIN_END(prim)
void add_range(bool indexed, u32 first, u32 count);   // DRAW_ARRAYS / DRAW_INDEX_ARRAY
void end();                                           // NV4097_SET_BEGIN_END(0): draw

// Submit everything recorded so far (labels, semaphores); does not wait for the GPU.
void flush();
// Finish GPU work that has completed (deferred readbacks); call when idle.
void poll();
// Record a copy of the color target at `ea` (w x h) into the window's frame image.
// False if there is no such GPU target; then the caller presents from guest memory.
bool present(u32 ea, u32 w, u32 h);
// Frame boundary: submits the frame's work (and a recorded present()).
void end_frame();
// Make guest memory in [ea, ea+size) current with GPU-rendered data. wait=false only
// queues the copies (they land once the GPU is done; see after_gpu); returns whether any were queued.
bool sync_to_memory(u32 ea, u32 size, bool wait = true);
// Run f once the GPU has finished everything recorded so far.
void after_gpu(std::function<void()> f);
// Guest memory in [ea, ea+size) was written outside the renderer.
void memory_written(u32 ea, u32 size);

// Performance counters since the last take_stats(); for the periodic flip log.
struct Stats {
	u64 draws = 0, submits = 0, wait_us = 0, gpu_us = 0;
	u64 readbacks = 0, readback_bytes = 0;
	u64 uploads = 0, upload_bytes = 0, hashed_bytes = 0;
	u64 ring_peak = 0;   // most streaming-buffer bytes one submission used
};
Stats take_stats();

// Debugging aids
void set_dump_shaders(bool on);   // write generated GLSL to shaders/
void dump_render_targets();        // write every color render target to rt_<address>.bmp
// The color target at `ea` (guest size w x h) as rendered, at the render scale: BGRA
// pixels, out_w x out_h. False if there is no such GPU target. Waits for the GPU.
bool read_scaled(u32 ea, u32 w, u32 h, std::vector<u32>& out, u32& out_w, u32& out_h);

} // namespace rsx::gpu
