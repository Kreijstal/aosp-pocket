// SPDX-License-Identifier: GPL-3.0-only
// Declarations matching Android 9 aidl-cpp output for IAudioRecord.aidl.
// The implementation is the prebuilt one inside libaudioclient/libaudioflinger.
#pragma once
#include <binder/IBinder.h>
#include <binder/IInterface.h>
#include <binder/Status.h>
#include <media/MicrophoneInfo.h>
#include <vector>
namespace android { namespace media {
class IAudioRecord : public ::android::IInterface {
public:
    DECLARE_META_INTERFACE(AudioRecord)
    virtual ::android::binder::Status start(int32_t event,int32_t triggerSession)=0;
    virtual ::android::binder::Status stop()=0;
    virtual ::android::binder::Status getActiveMicrophones(::std::vector<::android::media::MicrophoneInfo>* activeMicrophones)=0;
    enum Call {
        START=::android::IBinder::FIRST_CALL_TRANSACTION+0,
        STOP=::android::IBinder::FIRST_CALL_TRANSACTION+1,
        GETACTIVEMICROPHONES=::android::IBinder::FIRST_CALL_TRANSACTION+2,
    };
};
}}
