// SPDX-License-Identifier: GPL-3.0-only
#include "omx_port_layout.h"
#include <media/stagefright/omx/1.0/WOmxNode.h>
#include <OMX_Component.h>
#include <string.h>
#include <stddef.h>

namespace android { namespace hardware { namespace media { namespace omx { namespace V1_0 { namespace implementation {
// The Android 9 HIDL status enum uses status_t's values, with this one alias.
static Status toStatus(status_t status) {
    return status==NOT_ENOUGH_DATA?Status::BUFFER_NEEDS_REALLOCATION:static_cast<Status>(status);
}
static OMX_INDEXTYPE toEnumIndexType(uint32_t index) {return static_cast<OMX_INDEXTYPE>(index);}
static_assert(sizeof(OMX_PARAM_PORTDEFINITIONTYPE)==112,"Review native ARM64 OMX layout");
static_assert(offsetof(OMX_PARAM_PORTDEFINITIONTYPE,format)==40,"Review port prefix");
static_assert(offsetof(OMX_VIDEO_PORTDEFINITIONTYPE,eColorFormat)
        -offsetof(OMX_VIDEO_PORTDEFINITIONTYPE,nFrameWidth)+sizeof(OMX_COLOR_FORMATTYPE)==36,
        "Review video scalar fields");

static bool nativePort(const hidl_vec<uint8_t>& bytes, OMX_PARAM_PORTDEFINITIONTYPE* native) {
    if(bytes.size()!=sizeof(PocketOmxPort32))return false;
    PocketOmxPort32 wire;memcpy(&wire,bytes.data(),sizeof(wire));
    if(wire.size!=sizeof(wire) || wire.domain>OMX_PortDomainOther)return false;
    memset(native,0,sizeof(*native));memcpy(native,&wire,40);native->nSize=sizeof(*native);
    if(wire.domain==OMX_PortDomainAudio) {
        native->format.audio.bFlagErrorConcealment=static_cast<OMX_BOOL>(wire.format.audio.concealment);
        native->format.audio.eEncoding=static_cast<OMX_AUDIO_CODINGTYPE>(wire.format.audio.coding);
    } else if(wire.domain==OMX_PortDomainVideo) {
        memcpy(&native->format.video.nFrameWidth,&wire.format.video.width,36);
    } else if(wire.domain==OMX_PortDomainOther) native->format.other.eFormat=static_cast<OMX_OTHER_FORMATTYPE>(wire.format.other);
    else return false;
    native->bBuffersContiguous=static_cast<OMX_BOOL>(wire.contiguous);native->nBufferAlignment=wire.alignment;
    return true;
}

static hidl_vec<uint8_t> wirePort(const OMX_PARAM_PORTDEFINITIONTYPE& native) {
    PocketOmxPort32 wire{};memcpy(&wire,&native,40);wire.size=sizeof(wire);
    if(native.eDomain==OMX_PortDomainAudio) {
        wire.format.audio.concealment=native.format.audio.bFlagErrorConcealment;
        wire.format.audio.coding=native.format.audio.eEncoding;
    } else if(native.eDomain==OMX_PortDomainVideo) {
        memcpy(&wire.format.video.width,&native.format.video.nFrameWidth,36);
    } else if(native.eDomain==OMX_PortDomainOther) wire.format.other=native.format.other.eFormat;
    wire.contiguous=native.bBuffersContiguous;wire.alignment=native.nBufferAlignment;
    hidl_vec<uint8_t> result;result.resize(sizeof(wire));memcpy(result.data(),&wire,sizeof(wire));return result;
}

Return<void> TWOmxNode::getParameter(uint32_t index,const hidl_vec<uint8_t>& input,getParameter_cb callback) {
    if(index==OMX_IndexParamPortDefinition && input.size()==sizeof(PocketOmxPort32)) {
        OMX_PARAM_PORTDEFINITIONTYPE native;
        if(!nativePort(input,&native)){callback(Status::BAD_VALUE,{});return Void();}
        status_t status=mBase->getParameter(OMX_IndexParamPortDefinition,&native,sizeof(native));
        callback(toStatus(status),status==OK?wirePort(native):input);return Void();
    }
    hidl_vec<uint8_t> params(input);
    auto status=mBase->getParameter(toEnumIndexType(index),params.data(),params.size());
    callback(toStatus(status),params);return Void();
}

Return<Status> TWOmxNode::setParameter(uint32_t index,const hidl_vec<uint8_t>& input) {
    if(index==OMX_IndexParamPortDefinition && input.size()==sizeof(PocketOmxPort32)) {
        OMX_PARAM_PORTDEFINITIONTYPE native;
        if(!nativePort(input,&native))return Status::BAD_VALUE;
        return toStatus(mBase->setParameter(OMX_IndexParamPortDefinition,&native,sizeof(native)));
    }
    return toStatus(mBase->setParameter(toEnumIndexType(index),input.data(),input.size()));
}
}}}}}}
