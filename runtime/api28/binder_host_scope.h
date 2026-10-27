// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../loader/guest_api.h"
class PocketHostBinderScope {
public:
    PocketHostBinderScope(){pocket_native_binder_host(1);}
    ~PocketHostBinderScope(){pocket_native_binder_host(0);}
    PocketHostBinderScope(const PocketHostBinderScope&)=delete;
    PocketHostBinderScope& operator=(const PocketHostBinderScope&)=delete;
};
