#pragma once

// Host-side GamePad state, set by the window/input layer and read by the
// game through VPADRead.

#include <cstdint>

namespace cafe::os {

// VPAD button bits, as the game sees them.
namespace vpad {
constexpr uint32_t kA = 0x8000, kB = 0x4000, kX = 0x2000, kY = 0x1000;
constexpr uint32_t kLeft = 0x0800, kRight = 0x0400, kUp = 0x0200, kDown = 0x0100;
constexpr uint32_t kZL = 0x0080, kZR = 0x0040, kL = 0x0020, kR = 0x0010;
constexpr uint32_t kPlus = 0x0008, kMinus = 0x0004, kHome = 0x0002;
constexpr uint32_t kStickR = 0x00020000, kStickL = 0x00040000;
} // namespace vpad

struct GamepadState {
    uint32_t buttons;
    float left_x, left_y;   // -1..1, up positive
    float right_x, right_y;
};

void set_gamepad_state(const GamepadState& state);

} // namespace cafe::os
