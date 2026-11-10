// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Private process-local guest events, never kernel-originated messages.
// The stock API32 observer reserves two bytes in its 1024-byte receive buffer.
enum { POCKET_UEVENT_MAX32=1022 };
int pocket_uevent_inject32(const char* record,int length);
#ifdef __cplusplus
}
#endif
