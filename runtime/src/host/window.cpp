#include "host/window.h"

#include "cafe/input.h"
#include "cafe/runtime.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace cafe::host {
namespace {

SDL_Window* g_window = nullptr;
std::atomic<uint32_t> g_width{1280}, g_height{720};
std::atomic<uint32_t> g_display_width{1280}, g_display_height{720};

// Keyboard layout: arrows for the D-pad; X/Z/S/A for A/B/X/Y (the GamePad's
// diamond); Q/W for L/R, 1/2 for ZL/ZR; Enter Plus, Backspace Minus, H Home;
// I/J/K/L the left stick.
struct Key {
    SDL_Scancode scancode;
    uint32_t button;
};
constexpr Key kKeys[] = {
    {SDL_SCANCODE_UP, os::vpad::kUp},        {SDL_SCANCODE_DOWN, os::vpad::kDown},
    {SDL_SCANCODE_LEFT, os::vpad::kLeft},    {SDL_SCANCODE_RIGHT, os::vpad::kRight},
    {SDL_SCANCODE_X, os::vpad::kA},          {SDL_SCANCODE_Z, os::vpad::kB},
    {SDL_SCANCODE_S, os::vpad::kX},          {SDL_SCANCODE_A, os::vpad::kY},
    {SDL_SCANCODE_Q, os::vpad::kL},          {SDL_SCANCODE_W, os::vpad::kR},
    {SDL_SCANCODE_1, os::vpad::kZL},         {SDL_SCANCODE_2, os::vpad::kZR},
    {SDL_SCANCODE_RETURN, os::vpad::kPlus},  {SDL_SCANCODE_BACKSPACE, os::vpad::kMinus},
    {SDL_SCANCODE_H, os::vpad::kHome},
};

// Gamepads by button position: the Wii U's A is on the right, B at the bottom.
struct PadButton {
    SDL_GamepadButton button;
    uint32_t vpad;
};
constexpr PadButton kPadButtons[] = {
    {SDL_GAMEPAD_BUTTON_EAST, os::vpad::kA},           {SDL_GAMEPAD_BUTTON_SOUTH, os::vpad::kB},
    {SDL_GAMEPAD_BUTTON_NORTH, os::vpad::kX},          {SDL_GAMEPAD_BUTTON_WEST, os::vpad::kY},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, os::vpad::kUp},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, os::vpad::kDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, os::vpad::kLeft},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, os::vpad::kRight},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, os::vpad::kL},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, os::vpad::kR},
    {SDL_GAMEPAD_BUTTON_START, os::vpad::kPlus},       {SDL_GAMEPAD_BUTTON_BACK, os::vpad::kMinus},
    {SDL_GAMEPAD_BUTTON_GUIDE, os::vpad::kHome},       {SDL_GAMEPAD_BUTTON_LEFT_STICK, os::vpad::kStickL},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, os::vpad::kStickR},
};

float axis(SDL_Gamepad* pad, SDL_GamepadAxis a) {
    const float v = SDL_GetGamepadAxis(pad, a) / 32767.0f;
    return std::fabs(v) < 0.15f ? 0.0f : std::clamp(v, -1.0f, 1.0f);
}

void update_input() {
    os::GamepadState state{};
    const bool* keys = SDL_GetKeyboardState(nullptr);
    for (const Key& k : kKeys) {
        if (keys[k.scancode]) state.buttons |= k.button;
    }
    if (keys[SDL_SCANCODE_J]) state.left_x -= 1.0f;
    if (keys[SDL_SCANCODE_L]) state.left_x += 1.0f;
    if (keys[SDL_SCANCODE_I]) state.left_y += 1.0f;
    if (keys[SDL_SCANCODE_K]) state.left_y -= 1.0f;

    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == nullptr) pad = SDL_OpenGamepad(ids[i]);
        if (pad == nullptr) continue;
        for (const PadButton& b : kPadButtons) {
            if (SDL_GetGamepadButton(pad, b.button)) state.buttons |= b.vpad;
        }
        if (axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.5f) state.buttons |= os::vpad::kZL;
        if (axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.5f) state.buttons |= os::vpad::kZR;
        state.left_x = std::clamp(state.left_x + axis(pad, SDL_GAMEPAD_AXIS_LEFTX), -1.0f, 1.0f);
        state.left_y = std::clamp(state.left_y - axis(pad, SDL_GAMEPAD_AXIS_LEFTY), -1.0f, 1.0f);
        state.right_x = std::clamp(state.right_x + axis(pad, SDL_GAMEPAD_AXIS_RIGHTX), -1.0f, 1.0f);
        state.right_y = std::clamp(state.right_y - axis(pad, SDL_GAMEPAD_AXIS_RIGHTY), -1.0f, 1.0f);
    }
    SDL_free(ids);
    os::set_gamepad_state(state);
}

void update_size() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    g_width = static_cast<uint32_t>(std::max(w, 1));
    g_height = static_cast<uint32_t>(std::max(h, 1));
}

} // namespace

bool open_window(const char* title) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "ttt2: no window: %s\n", SDL_GetError());
        return false;
    }
    // The window starts at the largest multiple of 640x360 that leaves room
    // on the desktop (1280x720 on 1080p, 1920x1080 on 1440p and up).
    int window_w = 1280, window_h = 720;
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        g_display_width = static_cast<uint32_t>(mode->w * mode->pixel_density);
        g_display_height = static_cast<uint32_t>(mode->h * mode->pixel_density);
        const int fit = std::max(1, std::min((mode->w - 64) / 640, (mode->h - 128) / 360));
        window_w = 640 * fit;
        window_h = 360 * fit;
    }
    g_window = SDL_CreateWindow(title, window_w, window_h,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (g_window == nullptr) {
        std::fprintf(stderr, "ttt2: no window: %s\n", SDL_GetError());
        return false;
    }
    update_size();
    return true;
}

bool window_open() { return g_window != nullptr; }

bool run_event_loop(const std::function<bool()>& finished) {
    using namespace std::chrono_literals;
    while (!finished()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return false;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED: update_size(); break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.scancode == SDL_SCANCODE_F11) {
                    const bool full = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
                    SDL_SetWindowFullscreen(g_window, !full);
                }
                break;
            default: break;
            }
        }
        update_input();
        std::this_thread::sleep_for(4ms);
    }
    return true;
}

std::vector<const char*> vulkan_instance_extensions() {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (names == nullptr) fatal("SDL cannot present with Vulkan: %s", SDL_GetError());
    return std::vector<const char*>(names, names + count);
}

VkSurfaceKHR create_vulkan_surface(VkInstance instance) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(g_window, instance, nullptr, &surface)) {
        fatal("cannot create a Vulkan surface for the window: %s", SDL_GetError());
    }
    return surface;
}

void display_size(uint32_t& width, uint32_t& height) {
    width = g_display_width;
    height = g_display_height;
}

void drawable_size(uint32_t& width, uint32_t& height) {
    width = g_width;
    height = g_height;
}

} // namespace cafe::host
