// The display: a vertical blank every 1/59.94 s, as the Wii U drives the TV
// and GamePad, or, when the window's display refreshes at a multiple of
// 60 Hz, at every 60th of a second of its refreshes (host_vsync). A frame
// whose swap command has executed on the GPU flips at the next vertical
// blank that respects the swap interval. Swap and flip counts and times are
// what GX2GetSwapStatus reports; titles pace their main loops on them.

#include "gpu/gpu.h"
#include "os/kernel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

extern "C" uint64_t cafe_ppc_timebase(void);

namespace cafe::gpu {
namespace {

constexpr auto kRefreshPeriod = std::chrono::nanoseconds(1001000000 / 60); // 59.94 Hz

// All under the kernel lock, so guest waits see consistent values.
uint32_t g_swaps = 0;
uint32_t g_frames_ready = 0;
uint32_t g_flips = 0;
uint64_t g_last_flip = 0;
uint64_t g_last_vsync = 0;
uint64_t g_vsyncs = 0;
uint64_t g_last_flip_vsync = 0;
uint32_t g_swap_interval = 1;

void flip(uint64_t now) {
    ++g_flips;
    g_last_flip = now;
    g_last_flip_vsync = g_vsyncs;
}

// Steady-clock nanoseconds of the last host_vsync.
std::atomic<int64_t> g_last_host_vsync{0};

int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void vsync() {
    os::KernelLock lock(os::kernel_mutex());
    const uint64_t now = cafe_ppc_timebase();
    ++g_vsyncs;
    g_last_vsync = now;
    if (g_frames_ready > g_flips && g_vsyncs - g_last_flip_vsync >= std::max<uint32_t>(g_swap_interval, 1)) {
        flip(now);
    }
    os::wake(os::kWaitDisplay);
}

void display_main() {
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += kRefreshPeriod;
        std::this_thread::sleep_until(next);
        // The host display's blanks stand in while they come.
        if (steady_ns() - g_last_host_vsync.load() < 3 * kRefreshPeriod.count()) {
            next = std::chrono::steady_clock::now();
            continue;
        }
        vsync();
        // Far behind (debugger, suspended process): resynchronise.
        if (std::chrono::steady_clock::now() - next > kRefreshPeriod * 10) next = std::chrono::steady_clock::now();
    }
}

} // namespace

void host_vsync() {
    g_last_host_vsync.store(steady_ns());
    vsync();
}

void start_display() {
    std::thread(display_main).detach();
}

SwapStatus swap_status() {
    os::KernelLock lock(os::kernel_mutex());
    return {g_swaps, g_flips, g_last_flip, g_last_vsync};
}

void note_swap_requested() {
    os::KernelLock lock(os::kernel_mutex());
    ++g_swaps;
}

void frame_ready() {
    os::KernelLock lock(os::kernel_mutex());
    ++g_frames_ready;
    if (g_swap_interval == 0) flip(cafe_ppc_timebase());
    os::wake(os::kWaitDisplay);
}

void wait_for_vsync() {
    os::KernelLock lock(os::kernel_mutex());
    const uint64_t target = g_vsyncs + 1;
    os::wait_until(lock, os::kWaitDisplay, [&] { return g_vsyncs >= target; });
}

void wait_for_flip() {
    os::KernelLock lock(os::kernel_mutex());
    if (g_flips == g_swaps) return; // nothing pending
    const uint32_t target = g_flips + 1;
    os::wait_until(lock, os::kWaitDisplay, [&] { return g_flips >= target; });
}

void set_swap_interval(uint32_t interval) {
    os::KernelLock lock(os::kernel_mutex());
    g_swap_interval = interval;
}

uint32_t swap_interval() {
    os::KernelLock lock(os::kernel_mutex());
    return g_swap_interval;
}

} // namespace cafe::gpu
