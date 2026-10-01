#pragma once

// The GPU side of the port. A command processor thread executes the PM4
// command buffers GX2 submits, in order, against a Latte register file and
// hands draws, clears, copies and scan-outs to a rendering backend. A
// display clock flips finished frames at the TV refresh rate.
//
// Addresses in command buffers are guest (effective) addresses: the title
// never translates them itself, so the runtime has no physical address map.

#include <cstdint>

namespace cafe::gpu {

// Starts the command processor and the display clock. Idempotent.
void start();

// ------------------------------------------------------------- submission
// Queues `words` PM4 dwords at guest address `buffer` (big-endian, as GX2
// writes them). Returns the timestamp that retires once the GPU has
// executed them. Timestamps start at 1 and increase by one per submission.
uint64_t submit(uint32_t buffer, uint32_t words);
uint64_t last_submitted_timestamp();
uint64_t retired_timestamp();
// Blocks the calling guest thread until `timestamp` has retired. Returns
// false if `timeout_ns` passed first.
bool wait_timestamp(uint64_t timestamp, uint64_t timeout_ns);

// The clock the command processor samples for GPU timestamps, in CPU timer
// ticks (so GX2GPUTimeToCPUTime is the identity).
uint64_t clock();

// ---------------------------------------------------------------- display
struct SwapStatus {
    uint32_t swaps;      // swaps the CPU requested
    uint32_t flips;      // frames the display has shown
    uint64_t last_flip;  // system time of the last flip
    uint64_t last_vsync; // system time of the last vertical blank
};
SwapStatus swap_status();
void note_swap_requested();
// Called by the command processor when a swap command executes: the frame
// is complete and waits for the next vertical blank (or flips now with a
// swap interval of 0).
void frame_ready();
void wait_for_vsync();
void wait_for_flip();
void set_swap_interval(uint32_t interval);
uint32_t swap_interval();

} // namespace cafe::gpu
