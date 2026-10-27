// SPDX-License-Identifier: GPL-3.0-only
#include <android/hardware/media/omx/1.0/BpHwOmxNode.h>
#include <android/hardware/media/omx/1.0/BpHwOmxObserver.h>
#include <dlfcn.h>
#include <stdio.h>

namespace android { namespace hardware { namespace media { namespace omx { namespace V1_0 {
// Android 9 emits a zero-length FD-array fixup for an empty fence handle.
// Rust Binder rejects that fixup at the end of native_handle_t. A nullable
// HIDL handle has the same no-fence meaning and requires no FD fixup.
// Preserve handles containing real FDs or integers, and do not modify owners.
static const hidl_handle& compatibleFence(const hidl_handle& fence) {
    static const hidl_handle absent;
    const native_handle_t* handle=fence.getNativeHandle();
    return handle && handle->numFds==0 && handle->numInts==0 ? absent : fence;
}

Return<void> BpHwOmxObserver::_hidl_onMessages(IInterface* object,
        details::HidlInstrumentor* instrumentor,const hidl_vec<Message>& messages) {
    static auto original=reinterpret_cast<decltype(&_hidl_onMessages)>(dlsym(RTLD_NEXT,
        "_ZN7android8hardware5media3omx4V1_015BpHwOmxObserver16_hidl_onMessagesEPNS0_10IInterfaceEPNS0_7details16HidlInstrumentorERKNS0_8hidl_vecINS3_7MessageEEE"));
    if(!original)return ::android::hardware::Status::fromStatusT(INVALID_OPERATION);
    hidl_vec<Message> normalized(messages);
    for(auto& message:normalized)message.fence=compatibleFence(message.fence);
    auto result=original(object,instrumentor,normalized);
    if(!result.isOk())fprintf(stderr,"POCKET_MEDIA_CALLBACK_FAILED %s\n",result.description().c_str());
    return result;
}

Return<Status> BpHwOmxNode::_hidl_fillBuffer(IInterface* object,
        details::HidlInstrumentor* instrumentor,uint32_t buffer,
        const CodecBuffer& data,const hidl_handle& fence) {
    static auto original=reinterpret_cast<decltype(&_hidl_fillBuffer)>(dlsym(RTLD_NEXT,
        "_ZN7android8hardware5media3omx4V1_011BpHwOmxNode16_hidl_fillBufferEPNS0_10IInterfaceEPNS0_7details16HidlInstrumentorEjRKNS3_11CodecBufferERKNS0_11hidl_handleE"));
    if(!original)return ::android::hardware::Status::fromStatusT(INVALID_OPERATION);
    return original(object,instrumentor,buffer,data,compatibleFence(fence));
}

Return<Status> BpHwOmxNode::_hidl_emptyBuffer(IInterface* object,
        details::HidlInstrumentor* instrumentor,uint32_t buffer,
        const CodecBuffer& data,uint32_t flags,uint64_t timestampUs,const hidl_handle& fence) {
    static auto original=reinterpret_cast<decltype(&_hidl_emptyBuffer)>(dlsym(RTLD_NEXT,
        "_ZN7android8hardware5media3omx4V1_011BpHwOmxNode17_hidl_emptyBufferEPNS0_10IInterfaceEPNS0_7details16HidlInstrumentorEjRKNS3_11CodecBufferEjmRKNS0_11hidl_handleE"));
    if(!original)return ::android::hardware::Status::fromStatusT(INVALID_OPERATION);
    return original(object,instrumentor,buffer,data,flags,timestampUs,compatibleFence(fence));
}

Return<Status> BpHwOmxNode::_hidl_dispatchMessage(IInterface* object,
        details::HidlInstrumentor* instrumentor,const Message& message) {
    static auto original=reinterpret_cast<decltype(&_hidl_dispatchMessage)>(dlsym(RTLD_NEXT,
        "_ZN7android8hardware5media3omx4V1_011BpHwOmxNode21_hidl_dispatchMessageEPNS0_10IInterfaceEPNS0_7details16HidlInstrumentorERKNS3_7MessageE"));
    if(!original)return ::android::hardware::Status::fromStatusT(INVALID_OPERATION);
    Message normalized(message);normalized.fence=compatibleFence(message.fence);
    return original(object,instrumentor,normalized);
}
}}}}}
