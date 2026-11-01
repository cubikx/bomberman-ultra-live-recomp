// SDL3 window + Vulkan swapchain. The game's finished frame is uploaded to an
// image and blitted to the swapchain, scaled to fit with letterboxing.
#include "presenter.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <volk.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "input/input.h"
#include "runtime/kernel.h"
#include "ui/overlay.h"

namespace video {
namespace {

#define VK_CHECK(expr)                                                         \
	do {                                                                       \
		VkResult r_ = (expr);                                                  \
		if (r_ != VK_SUCCESS) fatal("vulkan: %s failed (%d)", #expr, int(r_)); \
	} while (0)

SDL_Window* g_window;
std::atomic<bool> g_running{true};
std::mutex g_queue_lock;
GpuContext g_gpu;

// pending frame from the emulated GPU
std::mutex g_frame_lock;
std::vector<uint32_t> g_frame;
uint32_t g_frame_w, g_frame_h;
bool g_frame_new;
bool g_gpu_frame_new;   // the renderer wrote vk.src directly

struct Vk {
	VkInstance instance;
	VkSurfaceKHR surface;
	VkPhysicalDevice gpu;
	VkDevice device;
	uint32_t queue_family;
	VkQueue queue;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat format;
	VkExtent2D extent;
	std::vector<VkImage> images;
	std::vector<VkImageView> views;          // for the overlay
	std::vector<VkSemaphore> render_done;   // one per swapchain image
	VkFence image_ready;   // swapchain acquire; waited for on this thread (see draw())
	VkFence fence;
	VkCommandPool pool;
	VkCommandBuffer cmd;
	// the game's frame
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	void* staging_ptr = nullptr;
	VkImage src = VK_NULL_HANDLE;
	VkDeviceMemory src_mem = VK_NULL_HANDLE;
	uint32_t src_w = 0, src_h = 0;
	bool src_valid = false;
	bool need_recreate = false;
} vk;

// The renderer submits to the same queue from another thread, so wait on the
// queue under its lock rather than using vkDeviceWaitIdle.
void queue_idle() {
	std::lock_guard lock(g_queue_lock);
	vkQueueWaitIdle(vk.queue);
}

uint32_t find_memory(uint32_t type_bits, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(vk.gpu, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++)
		if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
	fatal("vulkan: no suitable memory type");
}

void create_instance() {
	if (!SDL_Vulkan_LoadLibrary(nullptr)) fatal("SDL_Vulkan_LoadLibrary: %s", SDL_GetError());
	volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr()));
	Uint32 ext_count = 0;
	const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "bomberman-recomp";
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ci.pApplicationInfo = &app;
	ci.enabledExtensionCount = ext_count;
	ci.ppEnabledExtensionNames = exts;
	VK_CHECK(vkCreateInstance(&ci, nullptr, &vk.instance));
	volkLoadInstance(vk.instance);
	if (!SDL_Vulkan_CreateSurface(g_window, vk.instance, nullptr, &vk.surface))
		fatal("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
}

void create_device() {
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(vk.instance, &n, nullptr);
	std::vector<VkPhysicalDevice> gpus(n);
	vkEnumeratePhysicalDevices(vk.instance, &n, gpus.data());
	// prefer a discrete GPU that can present to our window
	int best = -1;
	for (int pass = 0; pass < 2 && best < 0; pass++) {
		for (uint32_t i = 0; i < n && best < 0; i++) {
			VkPhysicalDeviceProperties p;
			vkGetPhysicalDeviceProperties(gpus[i], &p);
			if (pass == 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
			if (p.apiVersion < VK_API_VERSION_1_3) continue;   // the renderer uses dynamic rendering
			uint32_t qn = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qn, nullptr);
			std::vector<VkQueueFamilyProperties> qs(qn);
			vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qn, qs.data());
			for (uint32_t q = 0; q < qn; q++) {
				VkBool32 present = VK_FALSE;
				vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], q, vk.surface, &present);
				if (present && (qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
					vk.gpu = gpus[i];
					vk.queue_family = q;
					best = int(i);
					LOG_INFO("video: using %s", p.deviceName);
					break;
				}
			}
		}
	}
	if (best < 0) fatal("vulkan: no GPU can present to the window");

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = vk.queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	f13.dynamicRendering = VK_TRUE;
	VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	features.pNext = &f13;
	features.features.independentBlend = VK_TRUE;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &features;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = exts;
	VK_CHECK(vkCreateDevice(vk.gpu, &dci, nullptr, &vk.device));
	volkLoadDevice(vk.device);
	vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);

	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = vk.queue_family;
	VK_CHECK(vkCreateCommandPool(vk.device, &pci, nullptr, &vk.pool));
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = vk.pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VK_CHECK(vkAllocateCommandBuffers(vk.device, &cai, &vk.cmd));
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VK_CHECK(vkCreateFence(vk.device, &fci, nullptr, &vk.image_ready));
	fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	VK_CHECK(vkCreateFence(vk.device, &fci, nullptr, &vk.fence));
}

void create_swapchain() {
	queue_idle();
	VkSurfaceCapabilitiesKHR caps;
	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, vk.surface, &caps));
	if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
		fatal("vulkan: swapchain images cannot be blitted to");

	uint32_t n = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &n, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(n);
	vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &n, formats.data());
	VkSurfaceFormatKHR fmt = formats[0];
	for (auto f : formats)
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt = f;
	vk.format = fmt.format;

	int w, h;
	SDL_GetWindowSizeInPixels(g_window, &w, &h);
	vk.extent = caps.currentExtent.width != 0xFFFFFFFF
	                ? caps.currentExtent
	                : VkExtent2D{std::clamp<uint32_t>(w, caps.minImageExtent.width, caps.maxImageExtent.width),
	                             std::clamp<uint32_t>(h, caps.minImageExtent.height, caps.maxImageExtent.height)};
	if (!vk.extent.width || !vk.extent.height) return;   // minimized

	VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	ci.surface = vk.surface;
	ci.minImageCount = std::max(caps.minImageCount, 2u);
	if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
	ci.imageFormat = fmt.format;
	ci.imageColorSpace = fmt.colorSpace;
	ci.imageExtent = vk.extent;
	ci.imageArrayLayers = 1;
	ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.preTransform = caps.currentTransform;
	ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	// Mailbox (vsync without blocking) when available: the renderer shares this queue,
	// and with FIFO a present waiting for vblank holds up the game's rendering behind it.
	uint32_t nmodes = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, vk.surface, &nmodes, nullptr);
	std::vector<VkPresentModeKHR> modes(nmodes);
	vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, vk.surface, &nmodes, modes.data());
	bool mailbox = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end();
	ci.presentMode = mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
	if (mailbox) ci.minImageCount = std::max(ci.minImageCount, std::min(3u, caps.maxImageCount ? caps.maxImageCount : 3u));
	ci.clipped = VK_TRUE;
	ci.oldSwapchain = vk.swapchain;
	LOG_INFO("video: swapchain %ux%u, %s, %u images", vk.extent.width, vk.extent.height, mailbox ? "mailbox" : "fifo",
	         ci.minImageCount);
	VkSwapchainKHR sc;
	VK_CHECK(vkCreateSwapchainKHR(vk.device, &ci, nullptr, &sc));
	if (vk.swapchain) vkDestroySwapchainKHR(vk.device, vk.swapchain, nullptr);
	vk.swapchain = sc;

	vkGetSwapchainImagesKHR(vk.device, sc, &n, nullptr);
	vk.images.resize(n);
	vkGetSwapchainImagesKHR(vk.device, sc, &n, vk.images.data());
	for (auto v : vk.views) vkDestroyImageView(vk.device, v, nullptr);
	vk.views.resize(n);
	for (uint32_t i = 0; i < n; i++) {
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = vk.images[i];
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = vk.format;
		vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VK_CHECK(vkCreateImageView(vk.device, &vci, nullptr, &vk.views[i]));
	}
	for (auto s : vk.render_done) vkDestroySemaphore(vk.device, s, nullptr);
	vk.render_done.resize(n);
	VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	for (auto& s : vk.render_done) VK_CHECK(vkCreateSemaphore(vk.device, &sci, nullptr, &s));
	vk.need_recreate = false;
}

void ensure_source(uint32_t w, uint32_t h) {
	if (vk.src && vk.src_w == w && vk.src_h == h) return;
	queue_idle();
	if (vk.src) {
		vkDestroyImage(vk.device, vk.src, nullptr);
		vkFreeMemory(vk.device, vk.src_mem, nullptr);
		vkDestroyBuffer(vk.device, vk.staging, nullptr);
		vkFreeMemory(vk.device, vk.staging_mem, nullptr);
	}
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = VkDeviceSize(w) * h * 4;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VK_CHECK(vkCreateBuffer(vk.device, &bci, nullptr, &vk.staging));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(vk.device, vk.staging, &req);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VK_CHECK(vkAllocateMemory(vk.device, &mai, nullptr, &vk.staging_mem));
	VK_CHECK(vkBindBufferMemory(vk.device, vk.staging, vk.staging_mem, 0));
	VK_CHECK(vkMapMemory(vk.device, vk.staging_mem, 0, VK_WHOLE_SIZE, 0, &vk.staging_ptr));

	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_B8G8R8A8_UNORM;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK(vkCreateImage(vk.device, &ici, nullptr, &vk.src));
	vkGetImageMemoryRequirements(vk.device, vk.src, &req);
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkAllocateMemory(vk.device, &mai, nullptr, &vk.src_mem));
	VK_CHECK(vkBindImageMemory(vk.device, vk.src, vk.src_mem, 0));
	vk.src_w = w;
	vk.src_h = h;
	vk.src_valid = false;
}

void barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
             VkAccessFlags dst_access) {
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.oldLayout = from;
	b.newLayout = to;
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
	                     nullptr, 0, nullptr, 1, &b);
}

void draw() {
	if (vk.need_recreate || !vk.swapchain) create_swapchain();
	if (!vk.swapchain || !vk.extent.width) return;

	VK_CHECK(vkWaitForFences(vk.device, 1, &vk.fence, VK_TRUE, UINT64_MAX));
	uint32_t index;
	// The acquire is waited for here rather than by the submission: the renderer shares
	// the queue, and a submission waiting for the display would hold its work back.
	VK_CHECK(vkResetFences(vk.device, 1, &vk.image_ready));
	VkResult r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, VK_NULL_HANDLE, vk.image_ready, &index);
	if (r == VK_ERROR_OUT_OF_DATE_KHR) {
		vk.need_recreate = true;
		return;
	}
	VK_CHECK(vkWaitForFences(vk.device, 1, &vk.image_ready, VK_TRUE, UINT64_MAX));
	VK_CHECK(vkResetFences(vk.device, 1, &vk.fence));

	// the renderer may recreate vk.src (frame_image), so hold the lock until submitted
	std::lock_guard frame_lock(g_frame_lock);
	bool upload = false;
	if (g_frame_new) {
		ensure_source(g_frame_w, g_frame_h);
		std::memcpy(vk.staging_ptr, g_frame.data(), g_frame.size() * 4);
		g_frame_new = false;
		upload = true;
	}
	g_gpu_frame_new = false;

	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(vkResetCommandBuffer(vk.cmd, 0));
	VK_CHECK(vkBeginCommandBuffer(vk.cmd, &bi));

	if (upload) {
		barrier(vk.src, vk.src_valid ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
		        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {vk.src_w, vk.src_h, 1};
		vkCmdCopyBufferToImage(vk.cmd, vk.staging, vk.src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		barrier(vk.src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		vk.src_valid = true;
	}

	VkImage dst = vk.images[index];
	barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
	VkClearColorValue black{};
	VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(vk.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	if (vk.src_valid) {
		// fit the game's frame into the window, keeping its aspect ratio
		float sx = float(vk.extent.width) / vk.src_w, sy = float(vk.extent.height) / vk.src_h;
		float s = std::min(sx, sy);
		int32_t w = int32_t(vk.src_w * s), h = int32_t(vk.src_h * s);
		int32_t x = (int32_t(vk.extent.width) - w) / 2, y = (int32_t(vk.extent.height) - h) / 2;
		VkImageBlit blit{};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.srcOffsets[1] = {int32_t(vk.src_w), int32_t(vk.src_h), 1};
		blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.dstOffsets[0] = {x, y, 0};
		blit.dstOffsets[1] = {x + w, y + h, 1};
		vkCmdBlitImage(vk.cmd, vk.src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
		               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
	}
	if (overlay::visible()) {
		barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
		overlay::render(vk.cmd, vk.views[index], vk.extent);
		barrier(dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
	} else {
		barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
	}
	VK_CHECK(vkEndCommandBuffer(vk.cmd));

	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &vk.cmd;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &vk.render_done[index];
	VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &vk.render_done[index];
	pi.swapchainCount = 1;
	pi.pSwapchains = &vk.swapchain;
	pi.pImageIndices = &index;
	{
		std::lock_guard lock(g_queue_lock);
		VK_CHECK(vkQueueSubmit(vk.queue, 1, &si, vk.fence));
		r = vkQueuePresentKHR(vk.queue, &pi);
	}
	if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk.need_recreate = true;
	else if (r != VK_SUCCESS) fatal("vulkan: present failed (%d)", int(r));
}

uint32_t g_display_h = 720;   // tallest display, measured by init()

// SDL's display queries belong on the main thread, so init() asks once.
uint32_t query_display_height() {
	uint32_t best = 720;
	int n = 0;
	if (SDL_DisplayID* ids = SDL_GetDisplays(&n)) {
		for (int i = 0; i < n; i++)
			if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(ids[i]))
				best = std::max(best, uint32_t(float(m->h) * (m->pixel_density > 0 ? m->pixel_density : 1.0f)));
		SDL_free(ids);
	}
	return best;
}

} // namespace

bool init(const char* title, int width, int height) {
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) fatal("SDL_Init: %s", SDL_GetError());
	// Testing: VIDEO_DISPLAY=n opens the window centred on display n (1 = first)
	int pos = SDL_WINDOWPOS_UNDEFINED;
	if (const char* e = std::getenv("VIDEO_DISPLAY"); e && *e) {
		int n = 0;
		if (SDL_DisplayID* ids = SDL_GetDisplays(&n)) {
			int i = std::atoi(e) - 1;
			if (i >= 0 && i < n) pos = int(SDL_WINDOWPOS_CENTERED_DISPLAY(ids[i]));
			else std::printf("[W] video: VIDEO_DISPLAY=%s, but there are %d displays\n", e, n);
			SDL_free(ids);
		}
	}
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, pos);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, pos);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	g_window = SDL_CreateWindowWithProperties(props);
	SDL_DestroyProperties(props);
	if (!g_window) fatal("SDL_CreateWindow: %s", SDL_GetError());
	input::init();
	create_instance();
	create_device();
	create_swapchain();
	// Testing: VIDEO_MINIMIZED=1 minimizes once the swapchain exists (draw() skips while minimized)
	if (env_flag("VIDEO_MINIMIZED")) SDL_MinimizeWindow(g_window);
	overlay::init(g_window, {vk.instance, vk.gpu, vk.device, vk.queue_family, vk.queue, vk.format, uint32_t(vk.images.size())});
	g_gpu = {vk.instance, vk.gpu, vk.device, vk.queue_family, vk.queue, &g_queue_lock};
	g_display_h = query_display_height();
	return true;
}

const GpuContext& gpu() { return g_gpu; }

uint32_t display_height() { return g_display_h; }

void submit_frame(const uint32_t* bgra, uint32_t width, uint32_t height) {
	std::lock_guard lock(g_frame_lock);
	g_frame.assign(bgra, bgra + size_t(width) * height);
	g_frame_w = width;
	g_frame_h = height;
	g_frame_new = true;
}

VkImage frame_image(uint32_t width, uint32_t height) {
	std::lock_guard lock(g_frame_lock);
	ensure_source(width, height);
	return vk.src;
}

void frame_image_ready() {
	std::lock_guard lock(g_frame_lock);
	vk.src_valid = true;
	g_gpu_frame_new = true;
}

void run(const std::function<void()>& on_close) {
	Uint64 last_draw = 0;
	while (g_running) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			input::handle_event(e);
			overlay::process_event(e);
			switch (e.type) {
			case SDL_EVENT_QUIT:
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				on_close();
				break;
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				vk.need_recreate = true;
				break;
			case SDL_EVENT_KEY_DOWN:
				if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
					bool fs = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN;
					SDL_SetWindowFullscreen(g_window, !fs);
				}
				break;
			default: break;
			}
		}
		input::update();
		overlay::update();
		bool fresh;
		{
			std::lock_guard lock(g_frame_lock);
			fresh = g_frame_new || g_gpu_frame_new;
		}
		// redraw on new frames, and now and then so resizes and expose events show up
		Uint64 now = SDL_GetTicks();
		// Debugging: VIDEO_NO_PRESENT=1 never shows frames, to measure speed past a display or driver cap
		static const bool no_present = env_flag("VIDEO_NO_PRESENT");
		if (no_present) fresh = false;
		// the overlay animates on its own, so it redraws at ~60 Hz while shown
		bool overlay_due = overlay::visible() && now - last_draw >= 16;
		if (fresh || overlay_due || vk.need_recreate || now - last_draw > 250) {
			draw();
			last_draw = now;
		} else {
			SDL_Delay(1);
		}
	}
	if (vk.device) queue_idle();
	overlay::shutdown();
}

void quit() { g_running = false; }

} // namespace video
