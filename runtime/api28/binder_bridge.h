// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <binder/IBinder.h>

namespace pocket {
// Publish a userspace service manager through the host app's scoped receiver.
// Does not claim or replace the host kernel's Binder context manager.
android::status_t publishManager(const android::sp<android::IBinder>& manager);
}
