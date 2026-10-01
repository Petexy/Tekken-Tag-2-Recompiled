// snd_user: sound-effect processors (AXFX reverb/chorus/delay), the MIX
// channel mixer and AXART articulation. Like the voices in ax.cpp these are
// modelled for the title's control flow only: set-up succeeds, memory sizes
// are reported, and processors leave the aux buffers untouched (silence)
// until audio output is implemented. Generated from the title's import list.

#include "cafe/export.h"

namespace cafe::os {

// Working memory an effect asks the title to allocate for it.
constexpr uint32_t kEffectMemory = 0x100;

CAFE_EXPORT_RAW(snd_user, AXARTInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXARTQuit) { ctx.r[3] = 0; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpCallbackDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpGetMemSizeDpl2) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpInitDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpSettingsDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpShutdownDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpCallbackDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpGetMemSizeDpl2) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpInitDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpSettingsDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpShutdownDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXDelaySettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiCallbackDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpCallbackDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpGetMemSizeDpl2) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpInitDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpSettingsDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpShutdownDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiGetMemSizeDpl2) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiInitDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiSettingsDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiShutdownDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpCallbackDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpGetMemSizeDpl2) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpInitDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpSettingsDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpShutdownDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, MIXAssignChannel) { ctx.r[3] = 0; }
CAFE_EXPORT_RAW(snd_user, MIXInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, MIXInitInputControl) { ctx.r[3] = 0; }
CAFE_EXPORT_RAW(snd_user, MIXQuit) { ctx.r[3] = 0; }
CAFE_EXPORT_RAW(snd_user, MIXReleaseChannel) { ctx.r[3] = 0; }
CAFE_EXPORT_RAW(snd_user, MIXSetDeviceSoundMode) { ctx.r[3] = 0; }
CAFE_EXPORT_RAW(snd_user, MIXUpdateSettings) { ctx.r[3] = 1; }

} // namespace cafe::os
