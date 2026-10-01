// snd_core (AX): voices and the audio frame clock.
//
// The title drives sound through voices whose playback position it watches,
// and through callbacks AX runs every 3 ms audio frame. This implements that
// model faithfully (positions advance at each voice's sample-rate ratio,
// loop or stop at their end, frame callbacks fire on time) so the title's
// sound engine behaves; producing actual output from the voices is a later
// milestone and plugs into advance_voices().
//
// AXVPB (guest, 0x58 bytes, Cemu's layout): +0x00 index, +0x04 state,
// +0x1C priority, +0x20 drop callback, +0x24 user data, +0x34 offsets
// (format u16, loop flag u16, loop/end/current u32, samples pointer).

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/sysmem.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace cafe::os {
namespace {

constexpr int kVoiceCount = 96;
constexpr uint32_t kVpbSize = 0x58;
constexpr uint32_t kSamplesPerFrame = 96; // 3 ms at the 32 kHz renderer
constexpr auto kFramePeriod = std::chrono::microseconds(3000);
enum Format : uint16_t { kAdpcm = 0x00, kPcm16 = 0x0A, kPcm8 = 0x19 };

struct Voice {
    uint32_t vpb = 0;
    bool acquired = false;
    bool running = false;
    uint16_t format = kPcm16;
    bool loop = false;
    // Positions in samples; the fraction is 16 bits.
    uint64_t position = 0;
    uint64_t loop_start = 0;
    uint64_t end = 0;
    uint32_t samples = 0; // guest address of the sample data
    uint32_t ratio = 0x10000;
};

std::recursive_mutex g_ax_mutex;
Voice g_voices[kVoiceCount];
bool g_initialized = false;
uint32_t g_frame_callback = 0;
uint32_t g_frame_callback2 = 0;
uint32_t g_aux_callbacks[3][2]{};
uint32_t g_final_mix_callbacks[2]{};
uint16_t g_master_volume = 0x8000;
Thread* g_ax_thread = nullptr;

// ADPCM addresses count nibbles; every 16-nibble frame starts with a 2-nibble
// header, leaving 14 samples.
uint64_t nibble_to_sample(uint32_t nibble) { return uint64_t{nibble / 16} * 14 + (nibble % 16 < 2 ? 0 : nibble % 16 - 2); }
uint32_t sample_to_nibble(uint64_t sample) { return static_cast<uint32_t>(sample / 14 * 16 + sample % 14 + 2); }

uint64_t to_sample(const Voice& v, uint32_t offset) {
    return v.format == kAdpcm ? nibble_to_sample(offset) : offset;
}
uint32_t from_sample(const Voice& v, uint64_t sample) {
    return v.format == kAdpcm ? sample_to_nibble(sample) : static_cast<uint32_t>(sample);
}

Voice* voice_of(uint32_t vpb) {
    if (vpb == 0) return nullptr;
    const uint32_t index = field<uint32_t>(vpb, 0x00);
    return index < kVoiceCount && g_voices[index].vpb == vpb ? &g_voices[index] : nullptr;
}

void publish_state(const Voice& v) {
    field<uint32_t>(v.vpb, 0x04) = v.running ? 1u : 0u;
    field<uint32_t>(v.vpb, 0x34 + 0x0C) = from_sample(v, v.position >> 16);
}

void advance_voices() {
    for (Voice& v : g_voices) {
        if (!v.acquired || !v.running) continue;
        v.position += uint64_t{v.ratio} * kSamplesPerFrame;
        const uint64_t end_fixed = (v.end + 1) << 16;
        if (v.position >= end_fixed) {
            if (v.loop && v.end >= v.loop_start) {
                const uint64_t span = (v.end + 1 - v.loop_start) << 16;
                v.position = (v.loop_start << 16) + (v.position - end_fixed) % span;
            } else {
                v.position = v.end << 16;
                v.running = false;
            }
        }
        publish_state(v);
    }
}

void* ax_main(void*) {
    Thread* t = g_ax_thread;
    bind_current_thread(t);
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += kFramePeriod;
        std::this_thread::sleep_until(next);
        uint32_t callback, callback2;
        {
            std::lock_guard lock(g_ax_mutex);
            if (!g_initialized) continue;
            advance_voices();
            callback = g_frame_callback;
            callback2 = g_frame_callback2;
        }
        // Frame callbacks run as AX's interrupt handler would.
        KernelLock lock(kernel_mutex());
        acquire_interrupt_lock(lock, t);
        lock.unlock();
        if (callback) call_guest(t->ctx, callback, {});
        if (callback2) call_guest(t->ctx, callback2, {});
        lock.lock();
        bool held = false;
        release_interrupt_lock_for_wait(t, held);
        // Running far behind (debugger, heavy load): skip, don't burst.
        if (std::chrono::steady_clock::now() - next > kFramePeriod * 10) next = std::chrono::steady_clock::now();
    }
    return nullptr;
}

void AXInit() {
    std::lock_guard lock(g_ax_mutex);
    if (g_initialized) return;
    for (int i = 0; i < kVoiceCount; ++i) {
        g_voices[i] = Voice{};
        g_voices[i].vpb = system_alloc(kVpbSize, 32);
        field<uint32_t>(g_voices[i].vpb, 0x00) = static_cast<uint32_t>(i);
    }
    g_initialized = true;
    g_ax_thread = create_internal_thread("cafe audio", 0);
    pthread_t host;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_create(&host, &attr, ax_main, nullptr);
    pthread_attr_destroy(&attr);
    pthread_detach(host);
}

void AXQuit() {
    std::lock_guard lock(g_ax_mutex);
    for (Voice& v : g_voices) v.running = false;
    g_frame_callback = g_frame_callback2 = 0;
}

bool AXIsInit() { return g_initialized; }

// Lowest priority value is dropped first; a free voice is used before any.
uint32_t AXAcquireVoice(PPCContext& ctx, uint32_t priority, uint32_t drop_callback, uint32_t user) {
    Voice* chosen = nullptr;
    uint32_t dropped_callback = 0, dropped_vpb = 0;
    {
        std::lock_guard lock(g_ax_mutex);
        for (Voice& v : g_voices) {
            if (!v.acquired) {
                chosen = &v;
                break;
            }
        }
        if (chosen == nullptr) {
            for (Voice& v : g_voices) {
                const uint32_t p = field<uint32_t>(v.vpb, 0x1C);
                if (p < priority && (chosen == nullptr || p < field<uint32_t>(chosen->vpb, 0x1C))) chosen = &v;
            }
            if (chosen == nullptr) return 0;
            dropped_callback = field<uint32_t>(chosen->vpb, 0x20);
            dropped_vpb = chosen->vpb;
        }
        const uint32_t vpb = chosen->vpb;
        const uint32_t index = field<uint32_t>(vpb, 0x00);
        *chosen = Voice{};
        chosen->vpb = vpb;
        chosen->acquired = true;
        std::memset(guest<uint8_t>(vpb), 0, kVpbSize);
        field<uint32_t>(vpb, 0x00) = index;
        field<uint32_t>(vpb, 0x1C) = priority;
        field<uint32_t>(vpb, 0x20) = drop_callback;
        field<uint32_t>(vpb, 0x24) = user;
    }
    if (dropped_callback) call_guest(ctx, dropped_callback, {dropped_vpb});
    return chosen->vpb;
}

void AXFreeVoice(uint32_t vpb) {
    std::lock_guard lock(g_ax_mutex);
    if (Voice* v = voice_of(vpb)) {
        v->acquired = false;
        v->running = false;
        publish_state(*v);
    }
}

void AXSetVoiceState(uint32_t vpb, uint32_t state) {
    std::lock_guard lock(g_ax_mutex);
    if (Voice* v = voice_of(vpb)) {
        v->running = state != 0;
        publish_state(*v);
    }
}

struct Offsets {
    be<uint16_t> format, loop_flag;
    be<uint32_t> loop_offset, end_offset, current_offset, samples;
};

void AXSetVoiceOffsets(uint32_t vpb, const Offsets* o) {
    std::lock_guard lock(g_ax_mutex);
    Voice* v = voice_of(vpb);
    if (v == nullptr || o == nullptr) return;
    v->format = o->format;
    v->loop = o->loop_flag != 0;
    v->samples = o->samples;
    v->loop_start = to_sample(*v, o->loop_offset);
    v->end = to_sample(*v, o->end_offset);
    v->position = to_sample(*v, o->current_offset) << 16;
    std::memcpy(guest<uint8_t>(vpb + 0x34), o, sizeof(Offsets));
    publish_state(*v);
}

void AXGetVoiceOffsets(uint32_t vpb, Offsets* o) {
    std::lock_guard lock(g_ax_mutex);
    Voice* v = voice_of(vpb);
    if (v == nullptr || o == nullptr) return;
    o->format = v->format;
    o->loop_flag = v->loop ? 1 : 0;
    o->loop_offset = from_sample(*v, v->loop_start);
    o->end_offset = from_sample(*v, v->end);
    o->current_offset = from_sample(*v, v->position >> 16);
    o->samples = v->samples;
}

bool AXCheckVoiceOffsets(const Offsets* o) {
    return o != nullptr && o->end_offset >= o->current_offset;
}

int32_t AXSetVoiceSrcRatio(uint32_t vpb, float ratio) {
    if (!(ratio >= 0.0f) || ratio > 255.0f) return -1;
    std::lock_guard lock(g_ax_mutex);
    if (Voice* v = voice_of(vpb)) v->ratio = static_cast<uint32_t>(ratio * 65536.0f);
    return 0;
}

// AXPBADPCMLOOP and the ADPCM decoder state matter only for mixing.
void AXSetVoiceAdpcm(uint32_t, uint32_t) {}
void AXSetVoiceAdpcmLoop(uint32_t, uint32_t) {}
void AXSetVoiceSrcType(uint32_t, uint32_t) {}
void AXSetVoiceType(uint32_t, uint32_t) {}
int32_t AXSetVoiceDeviceMix(uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
void AXSetVoiceLpf(uint32_t, uint32_t) {}
void AXSetVoiceLpfCoefs(uint32_t, uint32_t, uint32_t) {}
void AXSetVoiceBiquad(uint32_t, uint32_t) {}
void AXSetVoiceBiquadCoefs(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}

// One-pole low-pass coefficients for a cutoff frequency at 32 kHz.
void AXComputeLpfCoefs(uint32_t frequency, be<uint16_t>* a0, be<uint16_t>* b0) {
    const double b = 2.0 - std::cos(2.0 * M_PI * frequency / 32000.0);
    const double c = b - std::sqrt(b * b - 1.0);
    if (a0) *a0 = static_cast<uint16_t>((1.0 - c) * 32768.0);
    if (b0) *b0 = static_cast<uint16_t>(c * 32768.0);
}

// AXVoiceBegin/End bracket edits of one voice against the frame update.
int32_t AXVoiceBegin(uint32_t) {
    g_ax_mutex.lock();
    return 0;
}
int32_t AXVoiceEnd(uint32_t) {
    g_ax_mutex.unlock();
    return 0;
}

uint32_t AXRegisterCallback(uint32_t callback) {
    std::lock_guard lock(g_ax_mutex);
    const uint32_t previous = g_frame_callback;
    g_frame_callback = callback;
    return previous;
}
uint32_t AXRegisterFrameCallback(uint32_t callback) {
    std::lock_guard lock(g_ax_mutex);
    const uint32_t previous = g_frame_callback2;
    g_frame_callback2 = callback;
    return previous;
}
uint32_t aux_register(int bus, uint32_t callback, uint32_t context) {
    std::lock_guard lock(g_ax_mutex);
    const uint32_t previous = g_aux_callbacks[bus][0];
    g_aux_callbacks[bus][0] = callback;
    g_aux_callbacks[bus][1] = context;
    return previous;
}
uint32_t AXRegisterAuxACallback(uint32_t callback, uint32_t context) { return aux_register(0, callback, context); }
uint32_t AXRegisterAuxBCallback(uint32_t callback, uint32_t context) { return aux_register(1, callback, context); }
uint32_t AXRegisterAuxCCallback(uint32_t callback, uint32_t context) { return aux_register(2, callback, context); }
int32_t AXRegisterDeviceFinalMixCallback(uint32_t device, uint32_t callback) {
    if (device > 1) return -1;
    g_final_mix_callbacks[device] = callback;
    return 0;
}

// TV and GamePad outputs, stereo (mode 0 = stereo for TV).
int32_t AXGetDeviceMode(uint32_t device, be<uint32_t>* mode) {
    if (mode) *mode = 0;
    return device <= 1 ? 0 : -1;
}
uint32_t AXGetMode() { return 0; }
int32_t AXSetDRCVSMode(uint32_t) { return 0; }
void AXSetMasterVolume(uint16_t volume) { g_master_volume = volume; }
uint16_t AXGetMasterVolume() { return g_master_volume; }
void AXSetAuxAReturnVolume(uint16_t) {}
void AXSetAuxBReturnVolume(uint16_t) {}
void AXSetAuxCReturnVolume(uint16_t) {}

} // namespace

CAFE_EXPORT(snd_core, AXInit, AXInit);
CAFE_EXPORT(snd_core, AXQuit, AXQuit);
CAFE_EXPORT(snd_core, AXIsInit, AXIsInit);
CAFE_EXPORT(snd_core, AXAcquireVoice, AXAcquireVoice);
CAFE_EXPORT(snd_core, AXFreeVoice, AXFreeVoice);
CAFE_EXPORT(snd_core, AXSetVoiceState, AXSetVoiceState);
CAFE_EXPORT(snd_core, AXSetVoiceOffsets, AXSetVoiceOffsets);
CAFE_EXPORT(snd_core, AXGetVoiceOffsets, AXGetVoiceOffsets);
CAFE_EXPORT(snd_core, AXCheckVoiceOffsets, AXCheckVoiceOffsets);
CAFE_EXPORT(snd_core, AXSetVoiceSrcRatio, AXSetVoiceSrcRatio);
CAFE_EXPORT(snd_core, AXSetVoiceAdpcm, AXSetVoiceAdpcm);
CAFE_EXPORT(snd_core, AXSetVoiceAdpcmLoop, AXSetVoiceAdpcmLoop);
CAFE_EXPORT(snd_core, AXSetVoiceSrcType, AXSetVoiceSrcType);
CAFE_EXPORT(snd_core, AXSetVoiceType, AXSetVoiceType);
CAFE_EXPORT(snd_core, AXSetVoiceDeviceMix, AXSetVoiceDeviceMix);
CAFE_EXPORT(snd_core, AXSetVoiceLpf, AXSetVoiceLpf);
CAFE_EXPORT(snd_core, AXSetVoiceLpfCoefs, AXSetVoiceLpfCoefs);
CAFE_EXPORT(snd_core, AXSetVoiceBiquad, AXSetVoiceBiquad);
CAFE_EXPORT(snd_core, AXSetVoiceBiquadCoefs, AXSetVoiceBiquadCoefs);
CAFE_EXPORT(snd_core, AXComputeLpfCoefs, AXComputeLpfCoefs);
CAFE_EXPORT(snd_core, AXVoiceBegin, AXVoiceBegin);
CAFE_EXPORT(snd_core, AXVoiceEnd, AXVoiceEnd);
CAFE_EXPORT(snd_core, AXRegisterCallback, AXRegisterCallback);
CAFE_EXPORT(snd_core, AXRegisterFrameCallback, AXRegisterFrameCallback);
CAFE_EXPORT(snd_core, AXRegisterAuxACallback, AXRegisterAuxACallback);
CAFE_EXPORT(snd_core, AXRegisterAuxBCallback, AXRegisterAuxBCallback);
CAFE_EXPORT(snd_core, AXRegisterAuxCCallback, AXRegisterAuxCCallback);
CAFE_EXPORT(snd_core, AXRegisterDeviceFinalMixCallback, AXRegisterDeviceFinalMixCallback);
CAFE_EXPORT(snd_core, AXGetDeviceMode, AXGetDeviceMode);
CAFE_EXPORT(snd_core, AXGetMode, AXGetMode);
CAFE_EXPORT(snd_core, AXSetDRCVSMode, AXSetDRCVSMode);
CAFE_EXPORT(snd_core, AXSetMasterVolume, AXSetMasterVolume);
CAFE_EXPORT(snd_core, AXGetMasterVolume, AXGetMasterVolume);
CAFE_EXPORT(snd_core, AXSetAuxAReturnVolume, AXSetAuxAReturnVolume);
CAFE_EXPORT(snd_core, AXSetAuxBReturnVolume, AXSetAuxBReturnVolume);
CAFE_EXPORT(snd_core, AXSetAuxCReturnVolume, AXSetAuxCReturnVolume);

} // namespace cafe::os
