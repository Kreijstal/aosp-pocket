// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <hwbinder/IBinder.h>
#include <android/hidl/base/1.0/IBase.h>
#include <string>
android::sp<android::hardware::IBinder> pocketSoftwareMediaService(const std::string& descriptor);
android::sp<android::hidl::base::V1_0::IBase> pocketMediaMemoryService(const std::string& descriptor, const std::string& instance);
