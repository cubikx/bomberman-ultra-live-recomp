// Vulkan backend for the RSX.
//
// Model: the RSX thread records draws into a command buffer using dynamic
// rendering and submits it at frame end, at semaphores and when buffers fill up.
// Submissions rotate through a few slots (command buffer, fence, streaming buffer,
// descriptor pool), so the GPU runs behind the recording without being waited for.
// Everything the GPU reads from guest memory (vertices, indices, constants,
// textures) is copied at record time, so the only reason to wait is a readback:
// render targets live in GPU images keyed by their guest address and are copied
// back to guest memory only when something reads that memory (a DMA copy, a dump).
#include "renderer.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../config/config.h"
#include "../video/presenter.h"
#include "glsl.h"
#include "regs.h"
#include "rsx.h"
#include "shader.h"
#include "texture.h"

namespace rsx::gpu {
namespace {

#define VK_CHECK(expr)                                                         \
	do {                                                                       \
		VkResult r_ = (expr);                                                  \
		if (r_ != VK_SUCCESS) fatal("vulkan: %s failed (%d)", #expr, int(r_)); \
	} while (0)

struct PushConstants {
	float vp_scale[4];
	float vp_offset[4];
	float surface[4];
	float alpha_test[4];
};

// ---------------------------------------------------------------------------
// Device-level objects

video::GpuContext g_gpu;
VkCommandPool g_pool;
VkCommandBuffer g_cmd;   // the current submission's
bool g_recording = false;
u64 g_frame = 1;
Stats g_stats;
bool g_present_pending = false;   // a copy to the window's frame image is recorded

VkDescriptorSetLayout g_set_layout;
VkPipelineLayout g_pipeline_layout;
VkDescriptorPool g_desc_pool;   // the current submission's
u32 g_desc_used = 0;
constexpr u32 DESC_POOL_SETS = 4096;

VkFormat g_depth24_format;

// Render scale: render targets are g_res times their guest size in each direction
// (settings.ini [video] render_scale). Shaders work in guest pixels; the viewport,
// scissor and clears are scaled, and transfers to or from guest memory are scaled
// back to the guest size on the way.
u32 g_res = 1;

u32 find_memory(u32 type_bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer = 0) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(g_gpu.physical, &props);
	for (VkMemoryPropertyFlags w : {want | prefer, want})
		for (u32 i = 0; i < props.memoryTypeCount; i++)
			if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & w) == w) return i;
	fatal("vulkan: no suitable memory type");
}

struct Buffer {
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	u8* ptr = nullptr;
	VkDeviceSize size = 0;
};

Buffer make_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
	Buffer b;
	b.size = size;
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = size;
	bci.usage = usage;
	VK_CHECK(vkCreateBuffer(g_gpu.device, &bci, nullptr, &b.buf));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(g_gpu.device, b.buf, &req);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	// cached if possible: readbacks are read by the CPU, and uncached reads are very slow
	mai.memoryTypeIndex = find_memory(req.memoryTypeBits,
	                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
	VK_CHECK(vkAllocateMemory(g_gpu.device, &mai, nullptr, &b.mem));
	VK_CHECK(vkBindBufferMemory(g_gpu.device, b.buf, b.mem, 0));
	VK_CHECK(vkMapMemory(g_gpu.device, b.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.ptr)));
	return b;
}

// The current submission's streaming buffer for vertices, indices, uniforms and uploads.
struct Ring {
	Buffer b;
	VkDeviceSize offset = 0;
} g_ring;
u64 g_ring_generation = 1;   // bumped whenever a different or recycled ring becomes current
constexpr VkDeviceSize RING_SIZE = 32ull << 20;

struct Submission {
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	Buffer ring;
	VkDescriptorPool desc_pool = VK_NULL_HANDLE;
	bool pending = false;   // submitted, fence not yet waited for
	u32 query = 0;          // first of its two timestamp queries (start, end)
	std::vector<std::function<void()>> on_complete;   // guest memory updates once the GPU is done
};
constexpr u32 SUBMISSIONS = 6;
Submission g_subs[SUBMISSIONS];
u32 g_cur = 0;
VkQueryPool g_timestamps = VK_NULL_HANDLE;   // GPU time per submission, for the stats
double g_timestamp_ns = 0;                   // nanoseconds per tick; 0 = unsupported
VkDeviceSize g_ubo_align = 256;

struct Image {
	VkImage img = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	u32 width = 0, height = 0;
};

Image make_image(u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                 VkComponentMapping swizzle = {}) {
	Image im;
	im.width = w;
	im.height = h;
	im.format = fmt;
	im.aspect = aspect;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = usage;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK(vkCreateImage(g_gpu.device, &ici, nullptr, &im.img));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(g_gpu.device, im.img, &req);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkAllocateMemory(g_gpu.device, &mai, nullptr, &im.mem));
	VK_CHECK(vkBindImageMemory(g_gpu.device, im.img, im.mem, 0));
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = im.img;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = fmt;
	vci.components = swizzle;
	// views of depth/stencil images expose depth only (that is what gets sampled)
	vci.subresourceRange = {VkImageAspectFlags((aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? VK_IMAGE_ASPECT_COLOR_BIT
	                                                                                : VK_IMAGE_ASPECT_DEPTH_BIT),
	                        0, 1, 0, 1};
	VK_CHECK(vkCreateImageView(g_gpu.device, &vci, nullptr, &im.view));
	return im;
}

void destroy_image(Image& im) {
	if (im.view) vkDestroyImageView(g_gpu.device, im.view, nullptr);
	if (im.img) vkDestroyImage(g_gpu.device, im.img, nullptr);
	if (im.mem) vkFreeMemory(g_gpu.device, im.mem, nullptr);
	im = {};
}

// ---------------------------------------------------------------------------
// Command recording

bool g_rendering = false;
void end_rendering();

void ensure_recording() {
	if (g_recording) return;
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(vkResetCommandBuffer(g_cmd, 0));
	VK_CHECK(vkBeginCommandBuffer(g_cmd, &bi));
	g_recording = true;
	if (g_timestamp_ns) {
		u32 q = g_subs[g_cur].query;
		vkCmdResetQueryPool(g_cmd, g_timestamps, q, 2);
		vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_timestamps, q);
	}
}

void transition(Image& im, VkImageLayout to) {
	if (im.layout == to) return;
	ensure_recording();
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.oldLayout = im.layout;
	b.newLayout = to;
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = im.img;
	b.subresourceRange = {im.aspect, 0, 1, 0, 1};
	vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
	                     nullptr, 0, nullptr, 1, &b);
	im.layout = to;
}

VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align);

void complete(Submission& sub) {
	sub.pending = false;
	u64 t[2];
	if (g_timestamp_ns && vkGetQueryPoolResults(g_gpu.device, g_timestamps, sub.query, 2, sizeof t, t, 8,
	                                            VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
		g_stats.gpu_us += u64(double(t[1] - t[0]) * g_timestamp_ns / 1000.0);
	for (auto& f : sub.on_complete) f();
	sub.on_complete.clear();
}

void wait_for(Submission& sub) {
	if (!sub.pending) return;
	auto t0 = std::chrono::steady_clock::now();
	VK_CHECK(vkWaitForFences(g_gpu.device, 1, &sub.fence, VK_TRUE, UINT64_MAX));
	g_stats.wait_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
	complete(sub);
}

// Finish the submissions the GPU is done with, oldest first (the current slot comes last).
void poll_submissions() {
	for (u32 k = 1; k <= SUBMISSIONS; k++) {
		Submission& sub = g_subs[(g_cur + k) % SUBMISSIONS];
		if (!sub.pending) continue;
		if (vkGetFenceStatus(g_gpu.device, sub.fence) != VK_SUCCESS) break;
		complete(sub);
	}
}

// Make submission slot i current, waiting for its previous use to finish first.
void use_submission(u32 i) {
	g_cur = i;
	Submission& sub = g_subs[i];
	wait_for(sub);
	g_cmd = sub.cmd;
	g_ring.b = sub.ring;
	g_ring.offset = 0;
	g_ring_generation++;
	g_desc_pool = sub.desc_pool;
	VK_CHECK(vkResetDescriptorPool(g_gpu.device, g_desc_pool, 0));
	g_desc_used = 0;
}

// Submit what has been recorded and move on to the next slot. wait: also wait until
// the GPU has finished it, and with it everything submitted before (the queue runs
// in order); the slot then stays current and its streaming buffer keeps its contents
// until the next allocation.
void submit(bool wait) {
	if (g_recording) {
		end_rendering();
		Submission& sub = g_subs[g_cur];
		if (g_timestamp_ns)
			vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_timestamps, sub.query + 1);
		VK_CHECK(vkEndCommandBuffer(g_cmd));
		VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &g_cmd;
		VK_CHECK(vkResetFences(g_gpu.device, 1, &sub.fence));
		{
			std::lock_guard lock(*g_gpu.queue_lock);
			VK_CHECK(vkQueueSubmit(g_gpu.queue, 1, &si, sub.fence));
		}
		sub.pending = true;
		g_recording = false;
		g_stats.submits++;
		g_stats.ring_peak = std::max<u64>(g_stats.ring_peak, g_ring.offset);
	} else if (!wait) {
		return;
	}
	if (!wait) {
		poll_submissions();
		return use_submission((g_cur + 1) % SUBMISSIONS);
	}
	for (u32 k = 1; k <= SUBMISSIONS; k++) wait_for(g_subs[(g_cur + k) % SUBMISSIONS]);   // oldest first
	use_submission(g_cur);
}

VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align) {
	VkDeviceSize off = (g_ring.offset + align - 1) & ~(align - 1);
	if (off + size > g_ring.b.size) {
		submit(false);   // continues in the next slot's ring
		off = 0;
		if (size > g_ring.b.size) fatal("rsx: %llu-byte upload exceeds the streaming buffer", (unsigned long long)size);
	}
	g_ring.offset = off + size;
	return off;
}

// ---------------------------------------------------------------------------
// Render targets

struct RenderTarget {
	Image image;
	u32 ea = 0, pitch = 0;
	u32 surface_format = 0;   // RSX color or depth format code
	bool depth = false;
	u32 scale_x = 1, scale_y = 1;
	bool gpu_dirty = false;    // GPU holds data newer than guest memory
	bool cpu_dirty = false;    // guest memory was written; re-upload before use
	u64 last_used = 0;
};
std::map<u32, std::unique_ptr<RenderTarget>> g_targets;   // by guest address

VkFormat color_format(u32 f) {
	switch (f) {
	case 11: return VK_FORMAT_R16G16B16A16_SFLOAT;
	case 12: return VK_FORMAT_R32G32B32A32_SFLOAT;
	case 13: return VK_FORMAT_R32_SFLOAT;
	default: return VK_FORMAT_B8G8R8A8_UNORM;   // the 8-bit ARGB family
	}
}

u32 color_bpp(u32 f) {
	switch (f) {
	case 11: return 8;
	case 12: return 16;
	case 3: case 1: case 10: return 2;
	case 9: return 1;
	default: return 4;
	}
}

bool color_format_supported(u32 f) { return f == 4 || f == 5 || f == 8 || f == 11 || f == 12 || f == 13; }

// size of a render target in guest memory
u32 guest_w(const RenderTarget& rt) { return rt.image.width / g_res; }
u32 guest_h(const RenderTarget& rt) { return rt.image.height / g_res; }

// A guest-size color image of format `fmt`, for scaled transfers (one per size and format).
Image& guest_size_image(u32 w, u32 h, VkFormat fmt) {
	static std::map<std::tuple<u32, u32, VkFormat>, Image> images;
	Image& im = images[{w, h, fmt}];
	if (!im.img)
		im = make_image(w, h, fmt, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		                VK_IMAGE_ASPECT_COLOR_BIT);
	return im;
}

// Scale all of `src` into all of `dst` (outside a rendering pass). Downscales of 8-bit
// color average; everything else keeps exact pixel values.
void blit_image(Image& src, Image& dst) {
	transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkImageBlit b{};
	b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	b.srcOffsets[1] = {s32(src.width), s32(src.height), 1};
	b.dstOffsets[1] = {s32(dst.width), s32(dst.height), 1};
	bool average = dst.width < src.width && src.format == VK_FORMAT_B8G8R8A8_UNORM;
	vkCmdBlitImage(g_cmd, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
	               &b, average ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// guest bytes <-> image bytes for a row of pixels
void guest_to_image(u32 fmt, const u8* src, u8* dst, u32 pixels) {
	if (fmt == 11 || fmt == 12 || fmt == 13) {
		u32 comp = fmt == 11 ? 2 : 4, n = pixels * color_bpp(fmt) / comp;
		for (u32 i = 0; i < n; i++)
			for (u32 b = 0; b < comp; b++) dst[i * comp + b] = src[i * comp + comp - 1 - b];
		return;
	}
	for (u32 i = 0; i < pixels; i++) {   // ARGB bytes -> BGRA bytes
		dst[i * 4 + 0] = src[i * 4 + 3];
		dst[i * 4 + 1] = src[i * 4 + 2];
		dst[i * 4 + 2] = src[i * 4 + 1];
		dst[i * 4 + 3] = src[i * 4 + 0];
	}
}
void image_to_guest(u32 fmt, const u8* src, u8* dst, u32 pixels) {
	guest_to_image(fmt, src, dst, pixels);   // both conversions are byte reversals
}

// Copy guest memory into a render target (it may have been written by the CPU or a DMA).
void upload_target(RenderTarget& rt) {
	if (rt.depth || !color_format_supported(rt.surface_format)) {
		rt.cpu_dirty = false;
		return;
	}
	u32 bpp = color_bpp(rt.surface_format);
	u32 w = guest_w(rt), h = guest_h(rt);
	VkDeviceSize size = VkDeviceSize(w) * h * bpp;
	VkDeviceSize off = ring_alloc(size, 16);
	for (u32 y = 0; y < h; y++)
		guest_to_image(rt.surface_format, vm_ptr(rt.ea + y * rt.pitch), g_ring.b.ptr + off + VkDeviceSize(y) * w * bpp, w);
	end_rendering();
	Image& dst = g_res > 1 ? guest_size_image(w, h, rt.image.format) : rt.image;
	transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy copy{};
	copy.bufferOffset = off;
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {w, h, 1};
	vkCmdCopyBufferToImage(g_cmd, g_ring.b.buf, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	if (&dst != &rt.image) blit_image(dst, rt.image);
	rt.cpu_dirty = false;
}

// Copy a render target back to guest memory. wait=false: the copy lands in guest
// memory once the GPU gets there (completion work of the current submission).
void readback_target(RenderTarget& rt, bool wait = true) {
	if (!rt.gpu_dirty) return;
	rt.gpu_dirty = false;
	if (rt.depth || !color_format_supported(rt.surface_format)) return;
	u32 bpp = color_bpp(rt.surface_format);
	u32 w = guest_w(rt), h = guest_h(rt);
	if (rt.pitch < w * bpp) return;
	VkDeviceSize size = VkDeviceSize(w) * h * bpp;
	VkDeviceSize off = ring_alloc(size, 16);
	end_rendering();
	ensure_recording();
	Image* from = &rt.image;
	if (g_res > 1) {
		from = &guest_size_image(w, h, rt.image.format);
		blit_image(rt.image, *from);
	}
	transition(*from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy copy{};
	copy.bufferOffset = off;
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {w, h, 1};
	vkCmdCopyImageToBuffer(g_cmd, from->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_ring.b.buf, 1, &copy);
	g_stats.readbacks++;
	g_stats.readback_bytes += size;
	static std::set<u32> logged;
	if (logged.insert(rt.ea).second)
		LOG_INFO("rsx: first readback of target 0x%08x (%ux%u, fmt %u)", rt.ea, w, h, rt.surface_format);
	// the slot's ring is not reused before its completion work has run
	const u8* src = g_ring.b.ptr + off;
	g_subs[g_cur].on_complete.push_back([src, size, w, h, bpp, ea = rt.ea, pitch = rt.pitch, fmt = rt.surface_format] {
		// one bulk read: the mapped buffer is uncached, so byte-wise reads from it are slow
		std::vector<u8> pixels(src, src + size);
		for (u32 y = 0; y < h; y++)
			image_to_guest(fmt, pixels.data() + VkDeviceSize(y) * w * bpp, vm_ptr(ea + y * pitch), w);
	});
	if (wait) submit(true);
}

RenderTarget* get_target(u32 ea, u32 pitch, u32 fmt, bool depth, u32 w, u32 h, u32 sx, u32 sy) {
	// color and depth targets may share an address; bit 0 of the key tells them apart
	auto& slot = g_targets[ea | u32(depth)];
	VkFormat vkfmt = depth ? (fmt == 1 ? VK_FORMAT_D16_UNORM : g_depth24_format) : color_format(fmt);
	if (slot && (slot->image.width != w * sx || slot->image.height != h * sy || slot->image.format != vkfmt ||
	             slot->depth != depth)) {
		readback_target(*slot);
		submit(true);   // the old image may still be referenced by recorded or running commands
		destroy_image(slot->image);
		slot.reset();
	}
	if (!slot) {
		slot = std::make_unique<RenderTarget>();
		RenderTarget& rt = *slot;
		rt.ea = ea;
		rt.pitch = pitch;
		rt.surface_format = fmt;
		rt.depth = depth;
		rt.scale_x = sx;
		rt.scale_y = sy;
		if (depth) {
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (fmt == 1 ? 0 : VK_IMAGE_ASPECT_STENCIL_BIT);
			rt.image = make_image(w * sx, h * sy, vkfmt,
			                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			                      aspect);
		} else {
			rt.image = make_image(w * sx, h * sy, vkfmt,
			                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			                      VK_IMAGE_ASPECT_COLOR_BIT);
			rt.cpu_dirty = true;   // start from whatever is in guest memory
		}
		LOG_INFO("rsx: render target 0x%08x %s fmt %u %ux%u (x%ux%u), pitch %u", ea, depth ? "depth" : "color", fmt,
		         w, h, sx, sy, pitch);
	}
	slot->pitch = pitch;
	slot->last_used = g_frame;
	return slot.get();
}

// ---------------------------------------------------------------------------
// Current surface

struct Surface {
	RenderTarget* color = nullptr;
	RenderTarget* depth = nullptr;
	u32 clip_w = 0, clip_h = 0;
	u32 scale_x = 1, scale_y = 1;
	bool operator==(const Surface& o) const {
		return color == o.color && depth == o.depth && clip_w == o.clip_w && clip_h == o.clip_h;
	}
};
Surface g_active;   // what the open dynamic-rendering pass targets

Surface current_surface() {
	using namespace m;
	Surface s;
	u32 fmt = reg(SURFACE_FORMAT);
	u32 color_fmt = fmt & 0x1F, depth_fmt = (fmt >> 5) & 7, aa = (fmt >> 12) & 0xF;
	s.scale_x = ((aa == 3 || aa == 4 || aa == 5) ? 2 : 1) * g_res;
	s.scale_y = ((aa == 4 || aa == 5) ? 2 : 1) * g_res;
	u32 clip_h_reg = reg(SURFACE_CLIP_HORIZONTAL), clip_v_reg = reg(SURFACE_CLIP_VERTICAL);
	s.clip_w = (clip_h_reg >> 16) + (clip_h_reg & 0xFFFF);
	s.clip_h = (clip_v_reg >> 16) + (clip_v_reg & 0xFFFF);
	if (!s.clip_w || !s.clip_h) return s;
	u32 target = reg(SURFACE_COLOR_TARGET);
	if (target & 1 || target == 0x13) {
		u32 ea = location_ea(reg(CONTEXT_DMA_COLOR_A) == CONTEXT_DMA_MAIN, reg(SURFACE_COLOR_AOFFSET));
		s.color = get_target(ea, reg(SURFACE_PITCH_A), color_fmt, false, s.clip_w, s.clip_h, s.scale_x, s.scale_y);
	} else if (target == 2) {
		u32 ea = location_ea(reg(CONTEXT_DMA_COLOR_B) == CONTEXT_DMA_MAIN, reg(SURFACE_COLOR_BOFFSET));
		s.color = get_target(ea, reg(SURFACE_PITCH_B), color_fmt, false, s.clip_w, s.clip_h, s.scale_x, s.scale_y);
	}
	// libgcm sets a zeta pitch of 64 when there is no depth buffer
	if (depth_fmt && reg(SURFACE_PITCH_Z) > 64 && reg(SURFACE_PITCH_Z) != 0xFFFFFFFF) {
		u32 ea = location_ea(reg(CONTEXT_DMA_ZETA) == CONTEXT_DMA_MAIN, reg(SURFACE_ZETA_OFFSET));
		s.depth = get_target(ea, reg(SURFACE_PITCH_Z), depth_fmt, true, s.clip_w, s.clip_h, s.scale_x, s.scale_y);
	}
	return s;
}

void end_rendering() {
	if (!g_rendering) return;
	vkCmdEndRendering(g_cmd);
	g_rendering = false;
}

// Make `s` the open rendering pass (transitions and uploads happen outside passes).
bool begin_rendering(const Surface& s) {
	if (!s.color && !s.depth) return false;
	if (g_rendering && s == g_active) return true;
	end_rendering();
	ensure_recording();
	if (s.color && s.color->cpu_dirty) upload_target(*s.color);
	if (s.color) transition(s.color->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	if (s.depth) transition(s.depth->image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	RenderTarget* any = s.color ? s.color : s.depth;
	VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, {any->image.width, any->image.height}};
	if (s.color && s.depth) {
		ri.renderArea.extent.width = std::min(s.color->image.width, s.depth->image.width);
		ri.renderArea.extent.height = std::min(s.color->image.height, s.depth->image.height);
	}
	ri.layerCount = 1;
	if (s.color) {
		color.imageView = s.color->image.view;
		color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &color;
	}
	if (s.depth) {
		depth.imageView = s.depth->image.view;
		depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		ri.pDepthAttachment = &depth;
		if (s.depth->image.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &depth;
	}
	vkCmdBeginRendering(g_cmd, &ri);
	g_rendering = true;
	g_active = s;
	if (s.color) s.color->gpu_dirty = true;
	if (s.depth) s.depth->gpu_dirty = true;
	return true;
}

// ---------------------------------------------------------------------------
// Shaders and pipelines

struct VertexShader {
	VkShaderModule module = VK_NULL_HANDLE;
	u32 inputs = 0;
};
struct FragmentShader {
	VkShaderModule module = VK_NULL_HANDLE;
	std::vector<u32> constant_offsets;
	u32 textures = 0;
};
std::unordered_map<u64, VertexShader> g_vs_cache;
std::unordered_map<u64, FragmentShader> g_fs_cache;
std::set<u64> g_failed_shaders;
bool g_dump_shaders = false;

void dump_shader_source(const char* kind, u64 key, const std::string& glsl) {
	if (!g_dump_shaders) return;
	std::error_code ec;
	std::filesystem::create_directories("shaders", ec);
	char name[64];
	std::snprintf(name, sizeof name, "shaders/%s_%016llx.glsl", kind, (unsigned long long)key);
	if (FILE* f = std::fopen(name, "wb")) {
		std::fwrite(glsl.data(), 1, glsl.size(), f);
		std::fclose(f);
	}
}

VkShaderModule make_module(const std::vector<u32>& spirv) {
	VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = spirv.size() * 4;
	ci.pCode = spirv.data();
	VkShaderModule mod;
	VK_CHECK(vkCreateShaderModule(g_gpu.device, &ci, nullptr, &mod));
	return mod;
}

u64 hash_words(const u32* p, size_t n, u64 seed = 0xcbf29ce484222325ull) {
	u64 h = seed;
	for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
	return h;
}

const VertexShader* vertex_shader() {
	static u64 cached_version = 0, cached_key = 0;
	static const VertexShader* cached = nullptr;
	u32 start = reg(m::TRANSFORM_PROGRAM_START);
	if (cached && cached_version == g_vp_ucode_version && cached_key == start) return cached;
	// key: the program words from the entry point to the end bit
	u32 n = 0;
	while (start + n < VP_MAX_INSTRUCTIONS) {
		n++;
		if (g_vp_ucode[(start + n - 1) * 4 + 3] & 1) break;
	}
	u64 key = hash_words(&g_vp_ucode[start * 4], n * 4, start);
	auto it = g_vs_cache.find(key);
	if (it == g_vs_cache.end()) {
		if (g_failed_shaders.count(key)) return nullptr;
		auto vp = shader::translate_vertex(g_vp_ucode, start);
		dump_shader_source("vp", key, vp.glsl);
		std::vector<u32> spirv;
		if (vp.ok) spirv = glsl::compile(glsl::Stage::vertex, vp.glsl);
		if (spirv.empty()) {
			LOG_ERROR("rsx: vertex program at slot %u unusable: %s", start, vp.ok ? "compile failed" : vp.error.c_str());
			g_failed_shaders.insert(key);
			return nullptr;
		}
		it = g_vs_cache.emplace(key, VertexShader{make_module(spirv), vp.inputs_used}).first;
		LOG_INFO("rsx: vertex program %016llx compiled (%u instructions, %zu cached)", (unsigned long long)key, n,
		         g_vs_cache.size());
	}
	cached_version = g_vp_ucode_version;
	cached_key = start;
	cached = &it->second;
	return cached;
}

shader::TexDim texture_dim(u32 unit) {
	tex::Info t = tex::read_state(unit);
	if (t.cube) return shader::TexDim::cube;
	if (t.dimension == 3) return shader::TexDim::tex3d;
	return shader::TexDim::tex2d;
}

const FragmentShader* fragment_shader(u32& program_ea) {
	u32 prog = reg(m::SHADER_PROGRAM);
	program_ea = location_ea(((prog & 3) - 1) & 1, prog & ~3u);
	u32 control = reg(m::SHADER_CONTROL);
	shader::FragmentKey fk = shader::fragment_key(program_ea);
	shader::TexDim dims[shader::MAX_TEXTURE_UNITS] = {};
	u64 key = fk.hash ^ (u64(control) << 32);
	for (u32 t = 0; t < shader::MAX_TEXTURE_UNITS; t++) {
		if (!(fk.textures_used & (1u << t))) continue;
		dims[t] = texture_dim(t);
		key = (key ^ (u64(dims[t]) << (t * 2))) * 0x100000001b3ull;
	}
	auto it = g_fs_cache.find(key);
	if (it != g_fs_cache.end()) return &it->second;
	if (g_failed_shaders.count(key)) return nullptr;
	if (fk.size >= 4096 * 16)
		LOG_WARN("rsx: fragment program at 0x%08x has no end bit (program memory not written yet?)", program_ea);
	auto fp = shader::translate_fragment(program_ea, control, dims);
	dump_shader_source("fp", key, fp.glsl);
	std::vector<u32> spirv;
	if (fp.ok) spirv = glsl::compile(glsl::Stage::fragment, fp.glsl);
	if (spirv.empty()) {
		LOG_ERROR("rsx: fragment program at 0x%08x unusable: %s", program_ea, fp.ok ? "compile failed" : fp.error.c_str());
		g_failed_shaders.insert(key);
		return nullptr;
	}
	FragmentShader fs{make_module(spirv), fp.constant_offsets, fp.textures_used};
	LOG_INFO("rsx: fragment program %016llx compiled (%u bytes, %zu constants, %zu cached)", (unsigned long long)key,
	         fk.size, fp.constant_offsets.size(), g_fs_cache.size() + 1);
	return &g_fs_cache.emplace(key, std::move(fs)).first->second;
}

struct PipelineKey {
	VkShaderModule vs, fs;
	u32 inputs;
	VkFormat color, depth;
	u32 blend_enable, blend_src, blend_dst, blend_eq, color_mask;
	VkPrimitiveTopology topology;
	bool operator<(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof(*this)) < 0; }
};
std::map<PipelineKey, VkPipeline> g_pipelines;

VkBlendFactor blend_factor(u32 f) {
	switch (f) {
	case 0: return VK_BLEND_FACTOR_ZERO;
	case 1: return VK_BLEND_FACTOR_ONE;
	case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
	case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
	case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
	case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
	case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
	case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
	case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
	case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
	default: return VK_BLEND_FACTOR_ONE;
	}
}

VkBlendOp blend_op(u32 e) {
	switch (e) {
	case 0x8007: return VK_BLEND_OP_MIN;
	case 0x8008: return VK_BLEND_OP_MAX;
	case 0x800A: return VK_BLEND_OP_SUBTRACT;
	case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
	default: return VK_BLEND_OP_ADD;
	}
}

VkPipeline pipeline(const PipelineKey& k) {
	auto it = g_pipelines.find(k);
	if (it != g_pipelines.end()) return it->second;

	VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = k.vs;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = k.fs;
	stages[1].pName = "main";

	// every used input is a float4 in one interleaved stream
	std::vector<VkVertexInputAttributeDescription> attrs;
	u32 n = 0;
	for (u32 i = 0; i < 16; i++)
		if (k.inputs & (1u << i)) attrs.push_back({i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 * n++});
	VkVertexInputBindingDescription binding{0, std::max(16u * n, 16u), VK_VERTEX_INPUT_RATE_VERTEX};
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = n ? 1 : 0;
	vi.pVertexBindingDescriptions = &binding;
	vi.vertexAttributeDescriptionCount = u32(attrs.size());
	vi.pVertexAttributeDescriptions = attrs.data();

	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = k.topology;
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkPipelineColorBlendAttachmentState att{};
	att.blendEnable = k.blend_enable;
	att.srcColorBlendFactor = blend_factor(k.blend_src & 0xFFFF);
	att.srcAlphaBlendFactor = blend_factor(k.blend_src >> 16);
	att.dstColorBlendFactor = blend_factor(k.blend_dst & 0xFFFF);
	att.dstAlphaBlendFactor = blend_factor(k.blend_dst >> 16);
	att.colorBlendOp = blend_op(k.blend_eq & 0xFFFF);
	att.alphaBlendOp = blend_op(k.blend_eq >> 16);
	att.colorWriteMask = k.color_mask;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = k.color != VK_FORMAT_UNDEFINED ? 1 : 0;
	cb.pAttachments = &att;
	VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
	                        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
	                        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
	                        VK_DYNAMIC_STATE_BLEND_CONSTANTS};
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = u32(std::size(dyn));
	dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rinfo.colorAttachmentCount = cb.attachmentCount;
	rinfo.pColorAttachmentFormats = &k.color;
	rinfo.depthAttachmentFormat = k.depth;
	if (k.depth == g_depth24_format) rinfo.stencilAttachmentFormat = k.depth;

	VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pci.pNext = &rinfo;
	pci.stageCount = 2;
	pci.pStages = stages;
	pci.pVertexInputState = &vi;
	pci.pInputAssemblyState = &ia;
	pci.pViewportState = &vp;
	pci.pRasterizationState = &rs;
	pci.pMultisampleState = &ms;
	pci.pDepthStencilState = &ds;
	pci.pColorBlendState = &cb;
	pci.pDynamicState = &dy;
	pci.layout = g_pipeline_layout;
	VkPipeline p;
	VK_CHECK(vkCreateGraphicsPipelines(g_gpu.device, VK_NULL_HANDLE, 1, &pci, nullptr, &p));
	g_pipelines[k] = p;
	return p;
}

// ---------------------------------------------------------------------------
// Textures

struct CachedTexture {
	Image image;
	u32 guest_size = 0;
	u64 hash = 0;
	u64 checked_frame = 0;
	bool tracked = false;     // verified_epoch is valid
	u32 verified_epoch = 0;   // the guest bytes matched the image as of this write epoch
};
std::map<std::tuple<u32, u32, u32, u32, u32, u32>, std::unique_ptr<CachedTexture>> g_textures;
std::map<std::pair<u32, u32>, VkSampler> g_samplers;
Image g_dummy_texture;
VkSampler g_dummy_sampler;

// Write tracking: each guest page remembers the epoch in which it was last seen written.
// Epochs only advance when writes are found, so they stay small.
std::vector<u32> g_page_epoch;
u32 g_epoch = 1;

// Collect the writes to [ea, ea+size) that happened since it was last asked about.
// Returns the epoch that now covers them, or 0 without write tracking.
u32 take_writes(u32 ea, u32 size) {
	static std::vector<u32> pages;
	if (!vm::take_written_pages(ea, size, pages)) return 0;
	if (pages.empty()) return g_epoch;
	if (g_page_epoch.empty()) g_page_epoch.assign((1ull << 32) / 0x1000, 0);
	g_epoch++;
	for (u32 p : pages) g_page_epoch[p] = g_epoch;
	return g_epoch;
}

bool written_since(u32 ea, u32 size, u32 epoch) {
	if (g_page_epoch.empty()) return false;
	for (u64 p = ea / 0x1000; p <= (u64(ea) + size - 1) / 0x1000; p++)
		if (g_page_epoch[p] > epoch) return true;
	return false;
}

u64 hash_memory(u32 ea, u32 size) {
	u64 h = 0x9E3779B97F4A7C15ull;
	const u8* p = vm_ptr(ea);
	u32 n = size / 8;
	for (u32 i = 0; i < n; i++) {
		u64 v;
		std::memcpy(&v, p + i * 8, 8);
		h = (h ^ v) * 0xFF51AFD7ED558CCDull;
		h ^= h >> 29;
	}
	for (u32 i = n * 8; i < size; i++) h = (h ^ p[i]) * 0x100000001b3ull;
	return h;
}

VkSamplerAddressMode address_mode(u32 m) {
	switch (m) {
	case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	case 4: case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	case 6: case 8: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
	default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}
}

VkSampler sampler(u32 address, u32 filter) {
	auto key = std::make_pair(address & 0x000F0F0F, filter & 0x0FFF0000);
	auto it = g_samplers.find(key);
	if (it != g_samplers.end()) return it->second;
	u32 min = (filter >> 16) & 0xFF, mag = (filter >> 24) & 0xF;
	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	sci.minFilter = (min == 1 || min == 3 || min == 5) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = address_mode(address & 0xF);
	sci.addressModeV = address_mode((address >> 8) & 0xF);
	sci.addressModeW = address_mode((address >> 16) & 0xF);
	sci.maxLod = 0.0f;
	sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	VkSampler s;
	VK_CHECK(vkCreateSampler(g_gpu.device, &sci, nullptr, &s));
	g_samplers[key] = s;
	return s;
}

// Image view for texture unit `unit`, uploading or refreshing it as needed.
// Called outside a rendering pass.
VkImageView texture_view(u32 unit, const Surface& target) {
	tex::Info t = tex::read_state(unit);
	if (!t.width || !t.height) return g_dummy_texture.view;

	// rendered earlier this frame? sample the render target directly
	auto rt_it = g_targets.find(t.ea);
	if (rt_it != g_targets.end() && !rt_it->second->depth) {
		RenderTarget& rt = *rt_it->second;
		if (&rt == target.color) return g_dummy_texture.view;   // feedback loop: not supported
		if (rt.cpu_dirty) upload_target(rt);
		transition(rt.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		return rt.image.view;
	}

	auto key = std::make_tuple(t.ea, t.format | (t.linear ? 0x100 : 0), (t.width << 16) | t.height, t.remap, t.pitch,
	                           u32(t.cube));
	auto& slot = g_textures[key];
	if (slot && slot->checked_frame == g_frame) return slot->image.view;
	u32 size = tex::guest_size(t);
	// with write tracking, only memory written since the last check needs hashing
	u32 epoch = take_writes(t.ea, size);
	if (slot && epoch && slot->tracked && !written_since(t.ea, size, slot->verified_epoch)) {
		slot->checked_frame = g_frame;
		return slot->image.view;
	}
	u64 h = hash_memory(t.ea, size);
	g_stats.hashed_bytes += size;
	if (slot && slot->hash == h) {
		slot->checked_frame = g_frame;
		slot->tracked = epoch != 0;
		slot->verified_epoch = epoch;
		return slot->image.view;
	}
	tex::Decoded d;
	if (!tex::decode(t, d)) return g_dummy_texture.view;
	if (!slot || slot->image.format != d.format) {
		if (slot) {
			submit(true);
			destroy_image(slot->image);
		}
		slot = std::make_unique<CachedTexture>();
		slot->image = make_image(t.width, t.height, d.format,
		                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		                         VK_IMAGE_ASPECT_COLOR_BIT, d.swizzle);
	}
	VkDeviceSize off = ring_alloc(d.data.size(), 16);
	std::memcpy(g_ring.b.ptr + off, d.data.data(), d.data.size());
	g_stats.uploads++;
	g_stats.upload_bytes += d.data.size();
	transition(slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy copy{};
	copy.bufferOffset = off;
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {t.width, t.height, 1};
	vkCmdCopyBufferToImage(g_cmd, g_ring.b.buf, slot->image.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	transition(slot->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	slot->hash = h;
	slot->guest_size = size;
	slot->checked_frame = g_frame;
	slot->tracked = epoch != 0;
	slot->verified_epoch = epoch;
	return slot->image.view;
}

// ---------------------------------------------------------------------------
// Vertex fetch

float half_to_float(u16 h) {
	u32 sign = u32(h >> 15) << 31, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
	u32 bits;
	if (exp == 0) {
		if (!mant) bits = sign;
		else {
			exp = 127 - 15 + 1;
			while (!(mant & 0x400)) { mant <<= 1; exp--; }
			bits = sign | (exp << 23) | ((mant & 0x3FF) << 13);
		}
	} else if (exp == 31) {
		bits = sign | 0x7F800000 | (mant << 13);
	} else {
		bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
	}
	return std::bit_cast<float>(bits);
}

// Read one attribute element (up to 4 components) as floats.
void fetch_attribute(u32 type, u32 size, u32 ea, float* out) {
	out[0] = out[1] = out[2] = 0.0f;
	out[3] = 1.0f;
	switch (type) {
	case 1: for (u32 c = 0; c < size; c++) out[c] = std::max(-1.0f, float(s16(ld16(ea + c * 2))) / 32767.0f); break;
	case 2: for (u32 c = 0; c < size; c++) out[c] = ldf32(ea + c * 4); break;
	case 3: for (u32 c = 0; c < size; c++) out[c] = half_to_float(ld16(ea + c * 2)); break;
	case 4: for (u32 c = 0; c < size; c++) out[c] = float(ld8(ea + c)) / 255.0f; break;
	case 5: for (u32 c = 0; c < size; c++) out[c] = float(s16(ld16(ea + c * 2))); break;
	case 6: {
		// 11:11:10 signed normalized
		u32 v = ld32(ea);
		auto sext = [](u32 x, u32 bits) { return s32(x << (32 - bits)) >> (32 - bits); };
		out[0] = std::max(-1.0f, float(sext(v & 0x7FF, 11)) / 1023.0f);
		out[1] = std::max(-1.0f, float(sext((v >> 11) & 0x7FF, 11)) / 1023.0f);
		out[2] = std::max(-1.0f, float(sext(v >> 22, 10)) / 511.0f);
		break;
	}
	case 7: for (u32 c = 0; c < size; c++) out[c] = float(ld8(ea + c)); break;
	default: break;
	}
}

// ---------------------------------------------------------------------------
// Draw state

u32 g_primitive = 0;
struct Range {
	bool indexed;
	u32 first, count;
};
std::vector<Range> g_ranges;

void append_list(u32 prim, const std::vector<u32>& v, std::vector<u32>& out) {
	size_t n = v.size();
	switch (prim) {
	case 1: case 2: case 5: out.insert(out.end(), v.begin(), v.end()); break;   // points, lines, triangles
	case 3:   // line loop
		for (size_t i = 0; i + 1 < n; i++) { out.push_back(v[i]); out.push_back(v[i + 1]); }
		if (n > 1) { out.push_back(v[n - 1]); out.push_back(v[0]); }
		break;
	case 4:   // line strip
		for (size_t i = 0; i + 1 < n; i++) { out.push_back(v[i]); out.push_back(v[i + 1]); }
		break;
	case 6:   // triangle strip (keep the winding consistent)
		for (size_t i = 0; i + 2 < n; i++) {
			if (i & 1) { out.push_back(v[i + 1]); out.push_back(v[i]); out.push_back(v[i + 2]); }
			else { out.push_back(v[i]); out.push_back(v[i + 1]); out.push_back(v[i + 2]); }
		}
		break;
	case 7: case 10:   // triangle fan, polygon
		for (size_t i = 1; i + 1 < n; i++) { out.push_back(v[0]); out.push_back(v[i]); out.push_back(v[i + 1]); }
		break;
	case 8:   // quads
		for (size_t i = 0; i + 3 < n; i += 4) {
			out.insert(out.end(), {v[i], v[i + 1], v[i + 2], v[i], v[i + 2], v[i + 3]});
		}
		break;
	case 9:   // quad strip
		for (size_t i = 0; i + 3 < n; i += 2) {
			out.insert(out.end(), {v[i], v[i + 1], v[i + 3], v[i], v[i + 3], v[i + 2]});
		}
		break;
	default: break;
	}
}

VkPrimitiveTopology topology(u32 prim) {
	if (prim == 1) return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	if (prim >= 2 && prim <= 4) return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

std::set<std::string> g_warned;
void warn_once(const std::string& msg) {
	if (g_warned.insert(msg).second) LOG_WARN("rsx: %s", msg.c_str());
}

void draw() {
	using namespace m;
	if (g_ranges.empty()) return;

	// gather vertex indices
	std::vector<u32> elements;
	u32 idx_dma = reg(INDEX_ARRAY_DMA);
	u32 idx_ea = location_ea(idx_dma & 1, reg(INDEX_ARRAY_ADDRESS));
	bool idx16 = ((idx_dma >> 4) & 0xF) == 1;
	u32 base_index = reg(VERTEX_DATA_BASE_INDEX);
	// All ranges between BEGIN and END form one primitive stream (a strip may continue
	// across the 256-index ranges a long draw is split into). Primitive restart (the
	// restart value, compared before the base index is added) ends the current strip/fan;
	// this game separates the rows of a grid mesh with it.
	bool restart = reg(RESTART_INDEX_ENABLE) & 1;
	u32 restart_index = reg(RESTART_INDEX);
	std::vector<u32> v;
	for (const Range& r : g_ranges) {
		for (u32 i = 0; i < r.count; i++) {
			if (!r.indexed) {
				v.push_back(r.first + i);
				continue;
			}
			u32 index = idx16 ? ld16(idx_ea + (r.first + i) * 2) : ld32(idx_ea + (r.first + i) * 4);
			if (restart && index == restart_index) {
				append_list(g_primitive, v, elements);
				v.clear();
				continue;
			}
			v.push_back(index + base_index);
		}
	}
	append_list(g_primitive, v, elements);
	g_ranges.clear();
	if (elements.empty()) return;

	Surface surface = current_surface();
	if (!surface.color && !surface.depth) return;
	if (surface.color && !color_format_supported(surface.color->surface_format)) {
		warn_once("draw to unsupported color format " + std::to_string(surface.color->surface_format));
		return;
	}

	const VertexShader* vs = vertex_shader();
	u32 fp_ea;
	const FragmentShader* fs = fragment_shader(fp_ea);
	if (!vs || !fs) return;

	// Everything this draw allocates must survive until it is recorded: recycle
	// the streaming buffer and descriptor pool up front if they are getting full.
	if (g_desc_used >= DESC_POOL_SETS || g_ring.offset > RING_SIZE * 3 / 4) submit(false);

	// textures (uploads and layout changes must happen outside the pass)
	VkImageView views[shader::MAX_TEXTURE_UNITS];
	VkSampler samplers[shader::MAX_TEXTURE_UNITS];
	bool any_texture = fs->textures != 0;
	if (any_texture) end_rendering();
	ensure_recording();
	for (u32 t = 0; t < shader::MAX_TEXTURE_UNITS; t++) {
		views[t] = g_dummy_texture.view;
		samplers[t] = g_dummy_sampler;
		if (!(fs->textures & (1u << t))) continue;
		views[t] = texture_view(t, surface);
		samplers[t] = sampler(reg(TEXTURE_ADDRESS + t * 0x20), reg(TEXTURE_FILTER + t * 0x20));
	}

	// vertices: convert the referenced range into interleaved float4 attributes
	u32 vmin = *std::min_element(elements.begin(), elements.end());
	u32 vmax = *std::max_element(elements.begin(), elements.end());
	u32 vcount = vmax - vmin + 1;
	if (vcount > 4u << 20) {
		warn_once("draw referencing more than 4M vertices skipped");
		return;
	}
	u32 ninputs = std::popcount(vs->inputs);
	VkDeviceSize vstride = 16ull * std::max(ninputs, 1u);
	VkDeviceSize voff = ring_alloc(vstride * vcount, 16);
	float* vdst = reinterpret_cast<float*>(g_ring.b.ptr + voff);
	u32 base_offset = reg(VERTEX_DATA_BASE_OFFSET);
	u32 slot = 0;
	for (u32 i = 0; i < 16; i++) {
		if (!(vs->inputs & (1u << i))) continue;
		u32 fmt = reg(VERTEX_ARRAY_FORMAT + i * 4);
		u32 type = fmt & 0xF, size = (fmt >> 4) & 0xF, stride = (fmt >> 8) & 0xFF;
		u32 off = reg(VERTEX_ARRAY_OFFSET + i * 4);
		float* d = vdst + slot * 4;
		if (!size) {
			// no array: the attribute's constant register value
			float c[4];
			for (int k = 0; k < 4; k++) c[k] = regf(VERTEX_DATA4F + i * 16 + k * 4);
			for (u32 v = 0; v < vcount; v++) std::memcpy(d + v * (vstride / 4), c, 16);
		} else {
			u32 ea = location_ea(off >> 31, (off & 0x7FFFFFFF) + base_offset);
			for (u32 v = 0; v < vcount; v++) fetch_attribute(type, size, ea + (vmin + v) * stride, d + v * (vstride / 4));
		}
		slot++;
	}
	VkDeviceSize ioff = ring_alloc(elements.size() * 4, 4);
	u32* idst = reinterpret_cast<u32*>(g_ring.b.ptr + ioff);
	for (size_t i = 0; i < elements.size(); i++) idst[i] = elements[i] - vmin;

	// vertex constants: re-upload only when they changed or the ring was recycled
	static u64 vc_version = 0, vc_generation = 0;
	static VkDeviceSize vc_off = 0;
	if (vc_version != g_vp_constants_version || vc_generation != g_ring_generation) {
		vc_off = ring_alloc(sizeof(g_vp_constants), g_ubo_align);
		std::memcpy(g_ring.b.ptr + vc_off, g_vp_constants, sizeof(g_vp_constants));
		vc_version = g_vp_constants_version;
		vc_generation = g_ring_generation;
	}
	VkDeviceSize fc_size = 16ull * shader::MAX_FRAGMENT_CONSTANTS;
	VkDeviceSize fc_off = ring_alloc(fc_size, g_ubo_align);
	float* fc = reinterpret_cast<float*>(g_ring.b.ptr + fc_off);
	for (size_t i = 0; i < fs->constant_offsets.size(); i++)
		for (u32 c = 0; c < 4; c++)
			fc[i * 4 + c] = std::bit_cast<float>(shader::fp_word(fp_ea + fs->constant_offsets[i] + c * 4));

	// descriptor set (capacity was ensured at the top of draw())
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = g_desc_pool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &g_set_layout;
	VkDescriptorSet set;
	VK_CHECK(vkAllocateDescriptorSets(g_gpu.device, &dai, &set));
	g_desc_used++;
	VkDescriptorBufferInfo vcb{g_ring.b.buf, vc_off, sizeof(g_vp_constants)};
	VkDescriptorBufferInfo fcb{g_ring.b.buf, fc_off, fc_size};
	VkDescriptorImageInfo imgs[shader::MAX_TEXTURE_UNITS];
	for (u32 t = 0; t < shader::MAX_TEXTURE_UNITS; t++)
		imgs[t] = {samplers[t], views[t], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet writes[2 + shader::MAX_TEXTURE_UNITS] = {};
	for (auto& wr : writes) {
		wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr.dstSet = set;
		wr.descriptorCount = 1;
	}
	writes[0].dstBinding = 0;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[0].pBufferInfo = &vcb;
	writes[1].dstBinding = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[1].pBufferInfo = &fcb;
	for (u32 t = 0; t < shader::MAX_TEXTURE_UNITS; t++) {
		writes[2 + t].dstBinding = 2 + t;
		writes[2 + t].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[2 + t].pImageInfo = &imgs[t];
	}
	vkUpdateDescriptorSets(g_gpu.device, u32(std::size(writes)), writes, 0, nullptr);

	// pipeline (keys are compared bytewise, so clear the padding too)
	PipelineKey pk;
	std::memset(&pk, 0, sizeof(pk));
	pk.vs = vs->module;
	pk.fs = fs->module;
	pk.inputs = vs->inputs;
	pk.color = surface.color ? surface.color->image.format : VK_FORMAT_UNDEFINED;
	pk.depth = surface.depth ? surface.depth->image.format : VK_FORMAT_UNDEFINED;
	pk.blend_enable = reg(BLEND_ENABLE) & 1;
	if (pk.blend_enable) {
		pk.blend_src = reg(BLEND_FUNC_SFACTOR);
		pk.blend_dst = reg(BLEND_FUNC_DFACTOR);
		pk.blend_eq = reg(BLEND_EQUATION);
	}
	u32 cm = reg(COLOR_MASK);
	pk.color_mask = ((cm >> 16) & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) | ((cm >> 8) & 1 ? VK_COLOR_COMPONENT_G_BIT : 0) |
	                ((cm >> 0) & 1 ? VK_COLOR_COMPONENT_B_BIT : 0) | ((cm >> 24) & 1 ? VK_COLOR_COMPONENT_A_BIT : 0);
	pk.topology = topology(g_primitive);
	VkPipeline pipe = pipeline(pk);

	if (!begin_rendering(surface)) return;
	vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline_layout, 0, 1, &set, 0, nullptr);
	vkCmdBindVertexBuffers(g_cmd, 0, 1, &g_ring.b.buf, &voff);
	vkCmdBindIndexBuffer(g_cmd, g_ring.b.buf, ioff, VK_INDEX_TYPE_UINT32);

	// fixed-function state
	u32 window = reg(SHADER_WINDOW);
	// The game uses origin "bottom" with a negative viewport y scale (the classic libgcm
	// setup), which already yields a top-down image in memory. So a bottom origin needs
	// no extra flip here; only a top origin does. (Determined from this game's output.)
	bool flip_y = ((window >> 12) & 0xF) != 1;
	u32 window_h = window & 0xFFF;
	if (!window_h) window_h = surface.clip_h;
	RenderTarget* any = surface.color ? surface.color : surface.depth;
	float zmin = std::clamp(regf(CLIP_MIN), 0.0f, 1.0f), zmax = std::clamp(regf(CLIP_MAX), 0.0f, 1.0f);
	if (zmax <= zmin) { zmin = 0.0f; zmax = 1.0f; }
	VkViewport viewport{0, 0, float(any->image.width), float(any->image.height), zmin, zmax};
	vkCmdSetViewport(g_cmd, 0, 1, &viewport);
	u32 sh = reg(SCISSOR_HORIZONTAL), sv = reg(SCISSOR_VERTICAL);
	s32 sx = s32(sh & 0xFFFF), sw = s32(sh >> 16), sy = s32(sv & 0xFFFF), shh = s32(sv >> 16);
	if (flip_y) sy = s32(window_h) - (sy + shh);
	s32 x0 = std::max(sx, 0), y0 = std::max(sy, 0);
	s32 x1 = std::min(sx + sw, s32(surface.clip_w)), y1 = std::min(sy + shh, s32(surface.clip_h));
	VkRect2D scissor{{x0 * s32(surface.scale_x), y0 * s32(surface.scale_y)},
	                 {u32(std::max(0, x1 - x0)) * surface.scale_x, u32(std::max(0, y1 - y0)) * surface.scale_y}};
	vkCmdSetScissor(g_cmd, 0, 1, &scissor);
	u32 cull = reg(CULL_FACE);
	vkCmdSetCullMode(g_cmd, !(reg(CULL_FACE_ENABLE) & 1) ? VK_CULL_MODE_NONE
	                        : cull == 0x404 ? VK_CULL_MODE_FRONT_BIT
	                        : cull == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT);
	// winding is defined in window space, which is our framebuffer space unless we flip y
	// (confirmed with the 3D models: with origin bottom the game's CCW/back-cull setup
	// needs CCW here). FRONT_FACE is 0 until written; the hardware default is CCW.
	bool ccw = reg(FRONT_FACE) != 0x900;
	if (flip_y) ccw = !ccw;
	vkCmdSetFrontFace(g_cmd, ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
	bool depth_test = surface.depth && (reg(DEPTH_TEST_ENABLE) & 1);
	vkCmdSetDepthTestEnable(g_cmd, depth_test);
	vkCmdSetDepthWriteEnable(g_cmd, depth_test && (reg(DEPTH_MASK) & 1));
	u32 df = reg(DEPTH_FUNC);
	vkCmdSetDepthCompareOp(g_cmd, df >= 0x200 && df <= 0x207 ? VkCompareOp(df - 0x200) : VK_COMPARE_OP_LESS);
	u32 bc = reg(BLEND_COLOR);
	float blend_consts[4] = {float((bc >> 16) & 0xFF) / 255.0f, float((bc >> 8) & 0xFF) / 255.0f,
	                         float(bc & 0xFF) / 255.0f, float(bc >> 24) / 255.0f};
	vkCmdSetBlendConstants(g_cmd, blend_consts);

	PushConstants pc{};
	for (int k = 0; k < 4; k++) {
		pc.vp_scale[k] = regf(VIEWPORT_SCALE + k * 4);
		pc.vp_offset[k] = regf(VIEWPORT_OFFSET + k * 4);
	}
	pc.surface[0] = float(surface.clip_w);
	pc.surface[1] = float(surface.clip_h);
	pc.surface[2] = flip_y ? 1.0f : 0.0f;
	pc.surface[3] = float(window_h);
	u32 af = reg(ALPHA_FUNC);
	pc.alpha_test[0] = af >= 0x200 && af <= 0x207 ? float(af - 0x200) : 7.0f;
	pc.alpha_test[1] = float(reg(ALPHA_REF) & 0xFF) / 255.0f;
	pc.alpha_test[2] = (reg(ALPHA_TEST_ENABLE) & 1) ? 1.0f : 0.0f;
	pc.alpha_test[3] = float(g_res);
	vkCmdPushConstants(g_cmd, g_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
	                   sizeof(pc), &pc);

	vkCmdDrawIndexed(g_cmd, u32(elements.size()), 1, 0, 0, 0);

	// debugging: RSX_LOG_DRAWS=N writes a summary of every draw in frames N and N+1 to draws_<frame>.txt
	static const u64 log_from = env_u64("RSX_LOG_DRAWS");
	if (log_from && g_frame >= log_from && g_frame <= log_from + 1) {
		static FILE* f = nullptr;
		static u64 f_frame = 0;
		static u32 n = 0;
		if (f_frame != g_frame) {
			if (f) std::fclose(f);
			f = std::fopen(("draws_" + std::to_string(g_frame) + ".txt").c_str(), "w");
			f_frame = g_frame;
			n = 0;
		}
		if (f) {
			std::fprintf(f, "#%u rt %08x prim %u idx %zu vtx %u fs %p blend %u %08x %08x %08x mask %x |", n++,
			             any->ea, g_primitive, elements.size(), vcount, (void*)fs->module, pk.blend_enable,
			             reg(BLEND_FUNC_SFACTOR), reg(BLEND_FUNC_DFACTOR), reg(BLEND_EQUATION), pk.color_mask);
			std::fprintf(f, " idxdma %08x idxea %08x base %u el", idx_dma, idx_ea, base_index);
			for (size_t k = 0; k < std::min<size_t>(elements.size(), 12); k++) std::fprintf(f, " %u", elements[k]);
			std::fprintf(f, " win %08x clip %ux%u x%ux%u sc %08x %08x -> %d,%d %ux%u vp %g %g %g %g |", window,
			             surface.clip_w, surface.clip_h, surface.scale_x, surface.scale_y, sh, sv, scissor.offset.x,
			             scissor.offset.y, scissor.extent.width, scissor.extent.height, pc.vp_scale[0],
			             pc.vp_scale[1], pc.vp_offset[0], pc.vp_offset[1]);
			for (u32 t = 0; t < shader::MAX_TEXTURE_UNITS; t++)
				if (fs->textures & (1u << t)) {
					tex::Info ti = tex::read_state(t);
					std::fprintf(f, " t%u %08x %02x %ux%u", t, ti.ea, ti.format, ti.width, ti.height);
				}
			std::fprintf(f, " | z %u %x %x vpz %g %g clip %g %g cull %u %x ff %x | vc0-6", depth_test, df, reg(DEPTH_MASK),
			             pc.vp_scale[2], pc.vp_offset[2], regf(CLIP_MIN), regf(CLIP_MAX), reg(CULL_FACE_ENABLE), cull,
			             reg(FRONT_FACE));
			for (u32 k = 0; k < 28; k++) std::fprintf(f, " %.4g", g_vp_constants[k]);
			std::fprintf(f, " vc466-467");
			for (u32 k = 466 * 4; k < 468 * 4; k++) std::fprintf(f, " %.4g", g_vp_constants[k]);
			std::fprintf(f, " |");
			for (u32 i = 0; i < 16; i++)
				if (vs->inputs & (1u << i))
					std::fprintf(f, " in%u %08x@%08x", i, reg(VERTEX_ARRAY_FORMAT + i * 4), reg(VERTEX_ARRAY_OFFSET + i * 4));
			// converted values of the first referenced vertex
			const float* v = reinterpret_cast<const float*>(g_ring.b.ptr + voff);
			std::fprintf(f, " | v0");
			for (u32 k = 0; k < ninputs * 4; k++) std::fprintf(f, " %.3g", v[k]);
			if (vcount <= 8) {
				std::fprintf(f, " | pos");
				for (u32 i = 0; i < vcount; i++)
					std::fprintf(f, " (%.4g %.4g)", v[i * ninputs * 4], v[i * ninputs * 4 + 1]);
			}
			std::fprintf(f, "\n");
		}
	}
}

} // namespace

// ---------------------------------------------------------------------------

void init() {
	g_gpu = video::gpu();
	// auto: the smallest scale whose height covers the largest display (1080p: 2, 4K: 3)
	g_res = config::get().render_scale;
	if (!g_res) g_res = std::clamp((video::display_height() + 719) / 720, 1u, 4u);
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = g_gpu.queue_family;
	VK_CHECK(vkCreateCommandPool(g_gpu.device, &pci, nullptr, &g_pool));

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(g_gpu.physical, &props);
	g_ubo_align = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);

	// D24S8 is not universally supported (AMD); fall back to D32S8
	VkFormatProperties fp;
	vkGetPhysicalDeviceFormatProperties(g_gpu.physical, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
	g_depth24_format = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
	                       ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;

	// one binding per texture unit (2 + n), matching how the translated shaders declare
	// their samplers; units can differ in dimensionality, so they cannot share an array
	VkDescriptorSetLayoutBinding b[2 + shader::MAX_TEXTURE_UNITS] = {};
	b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
	b[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	for (u32 t = 0; t < shader::MAX_TEXTURE_UNITS; t++)
		b[2 + t] = {2 + t, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = u32(std::size(b));
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout(g_gpu.device, &lci, nullptr, &g_set_layout));
	VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &g_set_layout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout(g_gpu.device, &plci, nullptr, &g_pipeline_layout));
	VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * DESC_POOL_SETS},
	                                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, shader::MAX_TEXTURE_UNITS * DESC_POOL_SETS}};
	VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpci.maxSets = DESC_POOL_SETS;
	dpci.poolSizeCount = 2;
	dpci.pPoolSizes = sizes;
	if (props.limits.timestampComputeAndGraphics) {
		VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qci.queryCount = 2 * SUBMISSIONS;
		VK_CHECK(vkCreateQueryPool(g_gpu.device, &qci, nullptr, &g_timestamps));
		g_timestamp_ns = props.limits.timestampPeriod;
	}
	for (Submission& sub : g_subs) {
		sub.query = u32(&sub - g_subs) * 2;
		VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = g_pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		VK_CHECK(vkAllocateCommandBuffers(g_gpu.device, &cai, &sub.cmd));
		VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VK_CHECK(vkCreateFence(g_gpu.device, &fci, nullptr, &sub.fence));
		sub.ring = make_host_buffer(RING_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		                                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
		                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		VK_CHECK(vkCreateDescriptorPool(g_gpu.device, &dpci, nullptr, &sub.desc_pool));
	}
	use_submission(0);

	// 1x1 white texture for unbound/unsupported units
	g_dummy_texture = make_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
	                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	ensure_recording();
	VkDeviceSize off = ring_alloc(4, 4);
	std::memset(g_ring.b.ptr + off, 0xFF, 4);
	transition(g_dummy_texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy copy{};
	copy.bufferOffset = off;
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {1, 1, 1};
	vkCmdCopyBufferToImage(g_cmd, g_ring.b.buf, g_dummy_texture.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	transition(g_dummy_texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	submit(true);
	g_dummy_sampler = sampler(0x00010101, 0x02020000);
	LOG_INFO("rsx: Vulkan renderer ready (depth format %d, render scale %u)", int(g_depth24_format), g_res);
}

void clear(u32 mask) {
	Surface s = current_surface();
	if (!begin_rendering(s)) return;
	using namespace m;
	u32 window = reg(SHADER_WINDOW);
	bool flip_y = ((window >> 12) & 0xF) != 1;   // see draw()
	u32 window_h = (window & 0xFFF) ? (window & 0xFFF) : s.clip_h;
	u32 sh = reg(SCISSOR_HORIZONTAL), sv = reg(SCISSOR_VERTICAL);
	s32 sx = s32(sh & 0xFFFF), sw = s32(sh >> 16), sy = s32(sv & 0xFFFF), shh = s32(sv >> 16);
	if (flip_y) sy = s32(window_h) - (sy + shh);
	s32 x0 = std::max(sx, 0), y0 = std::max(sy, 0);
	s32 x1 = std::min(sx + sw, s32(s.clip_w)), y1 = std::min(sy + shh, s32(s.clip_h));
	if (x1 <= x0 || y1 <= y0) return;
	VkClearRect rect{{{x0 * s32(s.scale_x), y0 * s32(s.scale_y)},
	                  {u32(x1 - x0) * s.scale_x, u32(y1 - y0) * s.scale_y}}, 0, 1};
	VkClearAttachment atts[2];
	u32 n = 0;
	if ((mask & 0xF0) && s.color) {
		u32 c = reg(COLOR_CLEAR_VALUE);
		VkClearAttachment& a = atts[n++];
		a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		a.colorAttachment = 0;
		a.clearValue.color = {{float((c >> 16) & 0xFF) / 255.0f, float((c >> 8) & 0xFF) / 255.0f,
		                       float(c & 0xFF) / 255.0f, float(c >> 24) / 255.0f}};
	}
	if ((mask & 3) && s.depth) {
		u32 z = reg(ZSTENCIL_CLEAR_VALUE);
		VkClearAttachment& a = atts[n++];
		bool z16 = s.depth->surface_format == 1;
		a.aspectMask = ((mask & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
		               ((mask & 2) && !z16 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
		if (!a.aspectMask) n--;
		a.clearValue.depthStencil.depth = z16 ? float(z & 0xFFFF) / 65535.0f : float(z >> 8) / 16777215.0f;
		a.clearValue.depthStencil.stencil = z & 0xFF;
	}
	if (n) vkCmdClearAttachments(g_cmd, n, atts, 1, &rect);
}

void begin(u32 primitive) {
	g_primitive = primitive;
	g_ranges.clear();
}

void add_range(bool indexed, u32 first, u32 count) { g_ranges.push_back({indexed, first, count}); }

void end() {
	g_stats.draws++;
	draw();
}

Stats take_stats() {
	Stats s = g_stats;
	g_stats = {};
	return s;
}

void flush() { submit(false); }

void set_dump_shaders(bool on) { g_dump_shaders = on; }

// Debugging: write every color render target to rt_<address>.bmp
void dump_render_targets() {
	submit(true);
	for (auto& [key, rt] : g_targets) {
		if (rt->depth || !color_format_supported(rt->surface_format) || color_bpp(rt->surface_format) != 4) continue;
		rt->gpu_dirty = true;
		readback_target(*rt);
		u32 w = guest_w(*rt), h = guest_h(*rt);
		std::vector<u32> px(size_t(w) * h);
		for (u32 y = 0; y < h; y++)
			// ld32 yields 0xAARRGGBB, which stored little-endian is exactly BMP's B,G,R,A byte order
			for (u32 x = 0; x < w; x++) px[size_t(y) * w + x] = ld32(rt->ea + y * rt->pitch + x * 4) | 0xFF000000u;
		char name[48];
		std::snprintf(name, sizeof name, "rt_%08x.bmp", rt->ea);
		if (FILE* f = std::fopen(name, "wb")) {
			u32 data_size = w * h * 4;
			u8 hdr[54] = {'B', 'M'};
			auto put32 = [&](int off, u32 v) { std::memcpy(hdr + off, &v, 4); };
			put32(2, 54 + data_size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, u32(-s32(h)));
			hdr[26] = 1; hdr[28] = 32; put32(34, data_size);
			std::fwrite(hdr, 1, 54, f);
			std::fwrite(px.data(), 4, px.size(), f);
			std::fclose(f);
		}
		LOG_INFO("rsx: wrote %s (%ux%u)", name, w, h);
	}
}

bool read_scaled(u32 ea, u32 w, u32 h, std::vector<u32>& out, u32& out_w, u32& out_h) {
	auto it = g_targets.find(ea);
	if (it == g_targets.end() || it->second->cpu_dirty || it->second->image.format != VK_FORMAT_B8G8R8A8_UNORM)
		return false;
	RenderTarget& rt = *it->second;
	out_w = std::min(w * rt.scale_x, rt.image.width);
	out_h = std::min(h * rt.scale_y, rt.image.height);
	VkDeviceSize size = VkDeviceSize(out_w) * out_h * 4;
	if (size > RING_SIZE) return false;
	VkDeviceSize off = ring_alloc(size, 16);
	end_rendering();
	ensure_recording();
	transition(rt.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy copy{};
	copy.bufferOffset = off;
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {out_w, out_h, 1};
	vkCmdCopyImageToBuffer(g_cmd, rt.image.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_ring.b.buf, 1, &copy);
	const u8* src = g_ring.b.ptr + off;
	submit(true);   // keeps the slot current, so `src` stays valid
	out.assign(reinterpret_cast<const u32*>(src), reinterpret_cast<const u32*>(src + size));
	for (u32& px : out) px |= 0xFF000000u;
	return true;
}

bool present(u32 ea, u32 w, u32 h) {
	auto it = g_targets.find(ea);
	if (it == g_targets.end() || it->second->cpu_dirty || it->second->image.format != VK_FORMAT_B8G8R8A8_UNORM)
		return false;
	RenderTarget& rt = *it->second;
	VkImage dst = video::frame_image(w * g_res, h * g_res);
	end_rendering();
	ensure_recording();
	transition(rt.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	// the presenter may still be reading the previous frame: wait for all earlier queue work
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = 0;
	b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = dst;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
	                     nullptr, 1, &b);
	VkImageBlit blit{};
	blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.srcOffsets[1] = {s32(std::min(w * rt.scale_x, rt.image.width)), s32(std::min(h * rt.scale_y, rt.image.height)), 1};
	blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.dstOffsets[1] = {s32(w * g_res), s32(h * g_res), 1};
	vkCmdBlitImage(g_cmd, rt.image.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	               1, &blit, VK_FILTER_LINEAR);
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
	                     nullptr, 1, &b);
	g_present_pending = true;
	return true;
}

void end_frame() {
	submit(false);
	if (g_present_pending) {
		g_present_pending = false;
		video::frame_image_ready();
	}
	g_frame++;
}

bool sync_to_memory(u32 ea, u32 size, bool wait) {
	bool queued = false;
	for (auto& [key, rt] : g_targets) {
		u32 addr = rt->ea, bytes = rt->pitch * guest_h(*rt);
		if (addr < ea + size && ea < addr + bytes && rt->gpu_dirty) {
			readback_target(*rt, wait);
			queued = !wait;
		}
	}
	return queued;
}

void after_gpu(std::function<void()> f) { g_subs[g_cur].on_complete.push_back(std::move(f)); }

void poll() { poll_submissions(); }

void memory_written(u32 ea, u32 size) {
	for (auto& [key, rt] : g_targets) {
		u32 addr = rt->ea, bytes = rt->pitch * guest_h(*rt);
		if (addr < ea + size && ea < addr + bytes) rt->cpu_dirty = true;
	}
	for (auto& [key, t] : g_textures)
		if (std::get<0>(key) < ea + size && ea < std::get<0>(key) + t->guest_size) t->checked_frame = 0;
}

} // namespace rsx::gpu
