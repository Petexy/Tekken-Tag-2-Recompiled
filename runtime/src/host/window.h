#pragma once

// The game window: SDL3 on the process's main thread, which owns the event
// loop; keyboard and gamepads feed the Wii U GamePad state. The renderer
// presents into it from the GPU thread through a Vulkan surface.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace cafe::host {

// Opens the window; false if there is no display (the port then runs
// headless). Main thread only.
bool open_window(const char* title);
bool window_open();

// Runs the event loop until `finished` returns true or the window closes.
// Returns false if the user closed the window.
bool run_event_loop(const std::function<bool()>& finished);

// For the renderer (any thread).
std::vector<const char*> vulkan_instance_extensions();
VkSurfaceKHR create_vulkan_surface(VkInstance instance);
void drawable_size(uint32_t& width, uint32_t& height);
// The desktop resolution of the primary display, in pixels.
void display_size(uint32_t& width, uint32_t& height);
// The primary display's refresh rate, in Hz.
float display_refresh_rate();
// The refresh rate of the display the window is on, in Hz (it follows the
// window to other displays; the primary's before the window opens).
float window_refresh_rate();

} // namespace cafe::host
