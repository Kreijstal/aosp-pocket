// SPDX-License-Identifier: GPL-3.0-only
// API7's AudioFlinger client requires the HAL PID when it adopts the PCM FMQs.
// Generated local HIDL stream defaults return NO_PID; only the remote Binder
// dispatch normally supplies the real PID. Pocket runs these stock streams in
// audioserver itself. Supply the actual process identity for these two local
// stream implementations, without changing remote proxies or scheduling policy.
#include <android/hardware/audio/7.0/IStreamIn.h>
#include <android/hardware/audio/7.0/IStreamOut.h>
#include <stdio.h>
#include <unistd.h>

namespace android { namespace hardware { namespace audio { namespace V7_0 {
namespace {
template<class Callback> Return<void> localIdentity(const void* object,const char* stream,Callback callback) {
    ::android::hidl::base::V1_0::DebugInfo info{};
    info.pid=getpid();
    info.ptr=reinterpret_cast<uint64_t>(object);
    info.arch=::android::hidl::base::V1_0::DebugInfo::Architecture::IS_64BIT;
    fprintf(stderr,"API32_AUDIO_LOCAL_STREAM_IDENTITY stream=%s pid=%d remote=0\n",stream,info.pid);
    callback(info);
    return Void();
}
}
Return<void> IStreamIn::getDebugInfo(getDebugInfo_cb callback) {
    return localIdentity(this,"input",callback);
}
Return<void> IStreamOut::getDebugInfo(getDebugInfo_cb callback) {
    return localIdentity(this,"output",callback);
}
}}}}
