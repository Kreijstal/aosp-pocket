// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// API 31/32 used a versioned Category (version at byte 0, level at byte 3).
// API 30 and API 33+ use a plain stability level; older hosts have no trailer.
// Translate only the host bootstrap objects, never guest-to-guest Parcels.
static constexpr bool pocket_api32_host_stability(int host_api, int32_t wire, bool is_null, int32_t* result) {
    if (host_api < 28) return false;
    if (host_api == 31 || host_api == 32) {
        unsigned value = static_cast<uint32_t>(wire);
        unsigned level = value >> 24;
        if ((value & 0x00ffff00u) || !(value & 255u) ||
            (is_null ? level != 0 : level != 3 && level != 12 && level != 63)) return false;
        *result = wire;
        return true;
    }
    unsigned level = host_api < 30 ? (is_null ? 0 : 12) : static_cast<unsigned>(wire);
    if (level != 0 && level != 3 && level != 12 && level != 63) return false;
    if (is_null && level != 0) return false;
    // Modern unmarked bootstrap objects are local framework IPC, not a claim
    // of VINTF compatibility. API 32 requires a declared local category.
    if (!is_null && !level) level = 12;
    *result = static_cast<int32_t>(1u | (level << 24));
    return true;
}
