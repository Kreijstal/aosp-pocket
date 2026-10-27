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
// Android 9 sources iterate enums with hidl_enum_iterator, which its hidl-gen
// specialized per enum. Modern generated headers publish hidl_enum_values instead.
namespace android { namespace hardware {
template<typename T> struct hidl_enum_iterator {
    constexpr auto begin() const { return std::begin(details::hidl_enum_values<T>); }
    constexpr auto end() const { return std::end(details::hidl_enum_values<T>); }
};
}}
