// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#define POCKET_NETWORK_IDENTITY_MARKER "/system/etc/pocket-network-caller32"
#define POCKET_NETWORK_IDENTITY_PROTOCOL "proc-stat-fd-v1\n"
static const unsigned char pocket_network_identity_header[8]={255,'P','K','N','E','T',1,0};
