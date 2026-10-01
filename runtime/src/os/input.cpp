// Controllers: the Wii U GamePad (vpad) is the one input device; its state
// comes from the host (set_gamepad_state, fed by the window layer). No Wii
// Remotes or Pro Controllers are connected (padscore: KPAD/WPAD).

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/input.h"

#include <cstring>
#include <mutex>

namespace cafe::os {
namespace {

std::mutex g_input_mutex;
GamepadState g_gamepad{};
uint32_t g_previous_hold = 0;

// ------------------------------------------------------------------- vpad
constexpr int32_t kVpadOk = 0;
constexpr int32_t kVpadNoController = -2;

struct Vec2 { be<float> x, y; };
struct Vec3 { be<float> x, y, z; };
struct TouchData { be<uint16_t> x, y, touched, validity; };
struct VPADStatus {
    be<uint32_t> hold, trigger, release;
    Vec2 left_stick, right_stick;
    Vec3 acceleration;
    be<float> acc_magnitude, acc_variation;
    Vec2 acc_xy;
    Vec3 gyro, angle;
    int8_t error;
    uint8_t pad1;
    TouchData touch, touch_processed1, touch_processed2;
    uint8_t pad2[2];
    Vec3 direction[3];
    uint8_t headphone;
    uint8_t pad3[3];
    Vec3 magnet;
    uint8_t volume, battery, mic, volume_ex;
    uint8_t pad4[8];
};
static_assert(sizeof(VPADStatus) == 0xAC);

void VPADInit() {}

int32_t VPADRead(int32_t channel, VPADStatus* status, uint32_t count, be<int32_t>* error) {
    if (channel != 0 || count == 0 || status == nullptr) {
        if (error) *error = kVpadNoController;
        return 0;
    }
    std::lock_guard lock(g_input_mutex);
    std::memset(status, 0, sizeof(VPADStatus));
    const uint32_t hold = g_gamepad.buttons;
    status->hold = hold;
    status->trigger = hold & ~g_previous_hold;
    status->release = g_previous_hold & ~hold;
    g_previous_hold = hold;
    status->left_stick = {g_gamepad.left_x, g_gamepad.left_y};
    status->right_stick = {g_gamepad.right_x, g_gamepad.right_y};
    // A GamePad lying flat and still: gravity on -Z (in g), identity frame.
    status->acceleration = {0.0f, 0.0f, -1.0f};
    status->acc_magnitude = 1.0f;
    status->direction[0] = {1.0f, 0.0f, 0.0f};
    status->direction[1] = {0.0f, 1.0f, 0.0f};
    status->direction[2] = {0.0f, 0.0f, 1.0f};
    status->battery = 0xC0;
    status->volume = 0xFF;
    status->volume_ex = 0xFF;
    if (error) *error = kVpadOk;
    return 1;
}

void VPADSetBtnRepeat(int32_t, float, float) {}
void VPADSetAccParam(int32_t, float, float) {}
int32_t VPADControlMotor(int32_t, uint32_t, uint32_t) { return 0; }
void VPADStopMotor(int32_t) {}

// Touch coordinates come in raw; calibrated output is the 1280x720 screen.
void VPADGetTPCalibratedPoint(int32_t, TouchData* out, const TouchData* in) {
    if (out && in) *out = *in;
}

// --------------------------------------------------------------- padscore
constexpr int32_t kWpadNoController = -1;

uint32_t g_wpad_alloc = 0, g_wpad_free = 0;
uint32_t g_connect_callbacks[7]{};

void WPADRegisterAllocator(uint32_t alloc, uint32_t free) {
    g_wpad_alloc = alloc;
    g_wpad_free = free;
}
void KPADInit() {}
void KPADShutdown() {}
uint32_t KPADSetConnectCallback(uint32_t channel, uint32_t callback) {
    if (channel >= 7) return 0;
    const uint32_t previous = g_connect_callbacks[channel];
    g_connect_callbacks[channel] = callback;
    return previous;
}
int32_t KPADReadEx(int32_t, uint32_t, uint32_t, be<int32_t>* error) {
    if (error) *error = kWpadNoController;
    return 0;
}
int32_t WPADProbe(int32_t, be<uint32_t>* type) {
    if (type) *type = 0xFD; // no device
    return kWpadNoController;
}
void WPADRead(int32_t, uint8_t* status) {
    if (status) std::memset(status, 0, 0x2A);
}
int32_t WPADSetDataFormat(int32_t, uint32_t) { return kWpadNoController; }
uint32_t WPADGetLatestIndexInBuf(int32_t) { return 0; }
void WPADSetAutoSamplingBuf(int32_t, uint32_t, uint32_t) {}
void WPADControlMotor(int32_t, uint32_t) {}
bool WPADIsMotorEnabled() { return true; }
int32_t WPADEnableURCC(int32_t) { return 0; }
void WPADDisconnect(int32_t) {}
int32_t WPADControlDpd(int32_t, uint32_t, uint32_t) { return kWpadNoController; }
void KPADEnableDPD(int32_t) {}
void KPADDisableDPD(int32_t) {}
void WPADClampStick(int32_t, uint32_t, uint32_t) {}
void WPADClampTrigger(int32_t, uint32_t, uint32_t) {}

} // namespace

void set_gamepad_state(const GamepadState& state) {
    std::lock_guard lock(g_input_mutex);
    g_gamepad = state;
}

CAFE_EXPORT(vpad, VPADInit, VPADInit);
CAFE_EXPORT(vpad, VPADRead, VPADRead);
CAFE_EXPORT(vpad, VPADSetBtnRepeat, VPADSetBtnRepeat);
CAFE_EXPORT(vpad, VPADSetAccParam, VPADSetAccParam);
CAFE_EXPORT(vpad, VPADControlMotor, VPADControlMotor);
CAFE_EXPORT(vpad, VPADStopMotor, VPADStopMotor);
CAFE_EXPORT(vpad, VPADGetTPCalibratedPoint, VPADGetTPCalibratedPoint);

CAFE_EXPORT(padscore, WPADRegisterAllocator, WPADRegisterAllocator);
CAFE_EXPORT(padscore, KPADInit, KPADInit);
CAFE_EXPORT(padscore, KPADShutdown, KPADShutdown);
CAFE_EXPORT(padscore, KPADSetConnectCallback, KPADSetConnectCallback);
CAFE_EXPORT(padscore, KPADReadEx, KPADReadEx);
CAFE_EXPORT(padscore, WPADProbe, WPADProbe);
CAFE_EXPORT(padscore, WPADRead, WPADRead);
CAFE_EXPORT(padscore, WPADSetDataFormat, WPADSetDataFormat);
CAFE_EXPORT(padscore, WPADGetLatestIndexInBuf, WPADGetLatestIndexInBuf);
CAFE_EXPORT(padscore, WPADSetAutoSamplingBuf, WPADSetAutoSamplingBuf);
CAFE_EXPORT(padscore, WPADControlMotor, WPADControlMotor);
CAFE_EXPORT(padscore, WPADIsMotorEnabled, WPADIsMotorEnabled);
CAFE_EXPORT(padscore, WPADEnableURCC, WPADEnableURCC);
CAFE_EXPORT(padscore, WPADDisconnect, WPADDisconnect);
CAFE_EXPORT(padscore, WPADControlDpd, WPADControlDpd);
CAFE_EXPORT(padscore, KPADEnableDPD, KPADEnableDPD);
CAFE_EXPORT(padscore, KPADDisableDPD, KPADDisableDPD);
CAFE_EXPORT(padscore, WPADClampStick, WPADClampStick);
CAFE_EXPORT(padscore, WPADClampTrigger, WPADClampTrigger);

} // namespace cafe::os
