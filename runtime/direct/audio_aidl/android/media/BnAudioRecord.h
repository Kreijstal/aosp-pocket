// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <binder/IInterface.h>
#include <android/media/IAudioRecord.h>
namespace android { namespace media {
class BnAudioRecord : public ::android::BnInterface<IAudioRecord> {
public:
    ::android::status_t onTransact(uint32_t code,const ::android::Parcel& data,::android::Parcel* reply,uint32_t flags=0) override;
};
}}
