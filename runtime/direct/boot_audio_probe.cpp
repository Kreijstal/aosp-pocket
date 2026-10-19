// SPDX-License-Identifier: GPL-3.0-only
#include <media/AudioSystem.h>
#include <soundtrigger/SoundTrigger.h>
#include <stdio.h>
#include <time.h>
using namespace android;
int main() {
    timespec before{},after{};clock_gettime(CLOCK_MONOTONIC,&before);
    float volume=-1;bool muted=true;
    if(AudioSystem::getMasterVolume(&volume)!=PERMISSION_DENIED || volume!=-1)return 1;
    if(AudioSystem::isMicrophoneMuted(&muted)!=PERMISSION_DENIED || !muted)return 2;
    if(AudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA,AUDIO_POLICY_FORCE_NONE)!=PERMISSION_DENIED)return 3;
    uint32_t modules=999;if(SoundTrigger::listModules(nullptr,&modules)!=NO_INIT || modules!=999)return 5;
    clock_gettime(CLOCK_MONOTONIC,&after);
    long long ms=(after.tv_sec-before.tv_sec)*1000LL+(after.tv_nsec-before.tv_nsec)/1000000;
    if(ms>2000)return 4;
    printf("DIRECT_AUDIO_PROFILE_PASS playback_record_policy_soundtrigger=unavailable caller_values_unchanged=1 elapsed_ms=%lld\n",ms);
    return 0;
}
