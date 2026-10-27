// SPDX-License-Identifier: GPL-3.0-only
#define OMX_ANDROID_COMPILE_AS_32BIT_ON_64BIT_PLATFORMS
#include <OMX_Component.h>
#include <stddef.h>
#include "../direct/omx_port_layout.h"
static_assert(sizeof(PocketOmxPort32)==sizeof(OMX_PARAM_PORTDEFINITIONTYPE),"wire size");
static_assert(offsetof(PocketOmxPort32,format)==offsetof(OMX_PARAM_PORTDEFINITIONTYPE,format),"format offset");
static_assert(offsetof(PocketOmxPort32,format.video.width)==offsetof(OMX_PARAM_PORTDEFINITIONTYPE,format.video.nFrameWidth),"video layout");
static_assert(offsetof(PocketOmxPort32,format.video.color)==offsetof(OMX_PARAM_PORTDEFINITIONTYPE,format.video.eColorFormat),"color layout");
static_assert(offsetof(PocketOmxPort32,format.audio.coding)==offsetof(OMX_PARAM_PORTDEFINITIONTYPE,format.audio.eEncoding),"audio layout");
static_assert(offsetof(PocketOmxPort32,alignment)==offsetof(OMX_PARAM_PORTDEFINITIONTYPE,nBufferAlignment),"trailer layout");
