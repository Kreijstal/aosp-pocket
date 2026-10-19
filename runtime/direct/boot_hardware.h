// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <android/hidl/base/1.0/IBase.h>
#include <utils/StrongPointer.h>
#include <string>
namespace android { namespace hardware {
sp<hidl::base::V1_0::IBase> pocketRemoteHardwareService(const std::string&,const std::string&);
}}
