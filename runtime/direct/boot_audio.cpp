// SPDX-License-Identifier: GPL-3.0-only
// Explicit no-audio bring-up profile. Preserve the platform clients' real
// unavailable-service errors instead of waiting forever for absent daemons.
// POCKET_GUEST_AUDIO builds run the real audioserver; only sound trigger,
// which has no HAL, stays unavailable.
#include <media/AudioSystem.h>
#include <media/IAudioFlinger.h>
#include <media/IAudioPolicyService.h>
#include <soundtrigger/SoundTrigger.h>
#include <stdio.h>
#include <atomic>
namespace android {
#ifndef POCKET_GUEST_AUDIO
const sp<IAudioFlinger> AudioSystem::get_audio_flinger() {
    static std::atomic<bool> reported{false};
    if(!reported.exchange(true)) fprintf(stderr,"DIRECT_AUDIO_UNAVAILABLE service=media.audio_flinger profile=no_audio\n");
    return nullptr;
}
const sp<IAudioPolicyService> AudioSystem::get_audio_policy_service() {
    static std::atomic<bool> reported{false};
    if(!reported.exchange(true)) fprintf(stderr,"DIRECT_AUDIO_UNAVAILABLE service=media.audio_policy profile=no_audio\n");
    return nullptr;
}
#endif
const sp<ISoundTriggerHwService> SoundTrigger::getSoundTriggerHwService() {
    static std::atomic<bool> reported{false};
    if(!reported.exchange(true)) fprintf(stderr,"DIRECT_AUDIO_UNAVAILABLE service=media.sound_trigger_hw profile=no_audio\n");
    return nullptr;
}
}
