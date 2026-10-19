// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Build-controlled guest ABI, not an interposer for arbitrary native binaries.
// All state remains inside the host app sandbox. The standalone profile has no
// identity emulation; POCKET_DIRECT_BOOT adds guest platform metadata.
#ifdef __cplusplus
extern "C" {
#endif
int pocket_guest_init(void);
const char* pocket_guest_root(void);
#ifdef POCKET_DIRECT_BOOT
int pocket_boot_passthrough(const char*);
int pocket_boot_map_path(const char*, char*);
int pocket_boot_created_at(int, const char*);
#endif
#ifdef __cplusplus
}
#endif
