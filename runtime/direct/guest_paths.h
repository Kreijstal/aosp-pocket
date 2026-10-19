// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Build-controlled guest ABI, not an interposer for arbitrary native binaries.
// All state remains inside the host app sandbox. No UID or permission emulation.
int pocket_guest_init(void);
const char* pocket_guest_root(void);
