// Overlay drawn over the game: the profile menu (PS / Guide button, F1 on the
// keyboard) and short notices. Everything here runs on the window thread
// except notify(), player_joined() and player_left().
#pragma once

#include <cstdint>
#include <string>

#include <volk.h>

struct SDL_Window;
union SDL_Event;

namespace overlay {

struct VulkanInfo {
	VkInstance instance;
	VkPhysicalDevice gpu;
	VkDevice device;
	uint32_t queue_family;
	VkQueue queue;
	VkFormat format;        // swapchain format
	uint32_t image_count;   // swapchain images
};
void init(SDL_Window* window, const VulkanInfo& vk);
void shutdown();

void process_event(const SDL_Event& e);
void update();                      // after input::update(): menu navigation
bool visible();                     // something to draw (the presenter redraws while true)
// Draw into `view` (COLOR_ATTACHMENT_OPTIMAL) with dynamic rendering.
void render(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent);

// Input listener entry points
void menu_button(uint32_t port);
void controller_connected(uint32_t port);
void exit_combo(uint32_t port);   // opens "Quit game?" for that controller
// Game thread: a controller joined / left the game's player slots (join mode menu queue).
void player_joined(uint32_t port);
void player_left(uint32_t port);
void notify(const std::string& text);   // any thread

} // namespace overlay
