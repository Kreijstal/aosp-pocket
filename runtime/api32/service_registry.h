// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <binder/IBinder.h>

namespace pocket {
// A private API 32 AIDL registry. It never claims the kernel context manager.
android::sp<android::IBinder> createServiceRegistry32();
}
