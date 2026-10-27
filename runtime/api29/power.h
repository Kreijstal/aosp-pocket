// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <android/system/suspend/1.0/ISystemSuspend.h>
#include <stdint.h>
android::sp<android::system::suspend::V1_0::ISystemSuspend> pocketSystemSuspend();
int pocketPowerRequest(unsigned operation,uint64_t token,const char* name,uint64_t* answer);
