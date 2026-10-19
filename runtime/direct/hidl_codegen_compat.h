// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <hidl/HidlSupport.h>
// Modern host hidl-gen adds enum reflection metadata. Supply its uninstantiated
// primary template against Android 9 headers. This has no runtime ABI effect.
namespace android { namespace hardware { namespace details {
template<class T> struct PocketUnavailableEnumValues;
template<class T, class = std::enable_if_t<std::is_enum<T>::value>>
constexpr PocketUnavailableEnumValues<T> hidl_enum_values;
}}}
