#pragma once

// Sound output: the TV mix as 48 kHz stereo, played through SDL3's default
// playback device. The AX frame clock (os/ax.cpp) paces itself on how much
// output is queued, so the title produces sound exactly as fast as the
// device consumes it.
//
// TTT2_AUDIO=0 disables the device; TTT2_AUDIO_DUMP=<file.wav> also writes
// everything played to a WAV file (works without a device).

#include <cstdint>

namespace cafe::host {

constexpr uint32_t kAudioRate = 48000;

// Opens the playback device. Main thread, before the title starts.
void open_audio();

// True if output goes to a device (frames are then paced by it).
bool audio_device_active();

// Queues interleaved stereo frames, samples in [-1, 1]. Any thread.
void queue_audio(const float* frames, uint32_t count);

// Frames queued for the device and not yet consumed.
uint32_t queued_audio_frames();

} // namespace cafe::host
