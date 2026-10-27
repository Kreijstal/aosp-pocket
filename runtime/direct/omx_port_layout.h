// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
// Android 9's ARM64 ACodec uses OMX_ANDROID_COMPILE_AS_32BIT_ON_64BIT_PLATFORMS.
// Pointer-valued port metadata is never meaningful across process boundaries.
struct PocketOmxPort32 {
    uint32_t size, version, port, direction, count, minimum, bufferSize, enabled, populated, domain;
    union {
        struct {uint32_t mime, renderer, concealment, coding;} audio;
        struct {uint32_t mime, renderer; uint32_t width,height; int32_t stride;
                uint32_t sliceHeight,bitrate,framerate,concealment,compression,color,window;} video;
        uint32_t other;
    } format;
    uint32_t contiguous, alignment;
};
static_assert(sizeof(PocketOmxPort32)==96,"Review Android 9 OMX wire layout");
