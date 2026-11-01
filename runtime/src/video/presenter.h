// Host window and presentation (SDL3 + Vulkan).
#pragma once

#include <volk.h>

#include <cstdint>
#include <functional>
#include <mutex>

namespace video {

// The Vulkan device, shared with the RSX renderer. Both submit to the same
// queue from different threads, so every submit/present holds queue_lock.
struct GpuContext {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	uint32_t queue_family;
	VkQueue queue;
	std::mutex* queue_lock;
};
const GpuContext& gpu();

// Height in pixels of the tallest connected display (720 if unknown). After init().
uint32_t display_height();

// Create the window and Vulkan swapchain. Must be called on the main thread.
bool init(const char* title, int width, int height);

// Hand over a finished frame (BGRA8, tightly packed). Callable from any thread;
// the latest frame wins.
void submit_frame(const uint32_t* bgra, uint32_t width, uint32_t height);

// GPU path (no CPU copy): the renderer records a copy of its finished frame into
// frame_image(w, h) (B8G8R8A8_UNORM) on the shared queue and calls frame_image_ready()
// once that work is submitted. Between uses the image rests in
// VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; writers transition it from UNDEFINED.
VkImage frame_image(uint32_t width, uint32_t height);
void frame_image_ready();

// Window/event loop for the main thread. Returns after quit(). `on_close` runs
// when the user closes the window (the game decides when to actually exit).
void run(const std::function<void()>& on_close);
void quit();

} // namespace video
