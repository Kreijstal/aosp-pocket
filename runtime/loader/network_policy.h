// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
// App-private Android policy chains. This backend supports guest user zero;
// these logical UIDs do not become host kernel credentials.
#define POCKET_NP_PATH "/data/misc/pocket-network-policy.rules"
#define POCKET_NP_MAGIC 0x504e5031U
#define POCKET_NP_UIDS 100000U
#define POCKET_NP_BYTES (POCKET_NP_UIDS/8U)
enum {POCKET_NP_DOZE=0,POCKET_NP_STANDBY=1,POCKET_NP_POWERSAVE=2,POCKET_NP_NO_INTERNET=3,POCKET_NP_CHAINS=4};
enum {POCKET_NP_RESTRICTED=4,POCKET_NP_CHAINS_V3=5};
struct PocketNetworkPolicyHeader {uint32_t magic,version,enabled,reserved;};
struct PocketNetworkPolicy {
    struct PocketNetworkPolicyHeader header;
    uint8_t members[POCKET_NP_CHAINS][POCKET_NP_BYTES];
};
// Version 3 adds Android 12L's restricted-networking allowlist. Keep the
// version 2 structure and validation unchanged for existing Android 9/10 data.
struct PocketNetworkPolicyV3 {
    struct PocketNetworkPolicyHeader header;
    uint8_t members[POCKET_NP_CHAINS_V3][POCKET_NP_BYTES];
};
static inline int pocket_np_valid(const struct PocketNetworkPolicyHeader* p) {
    return p->magic==POCKET_NP_MAGIC && p->version==2 && !(p->enabled&~15U) && !p->reserved;
}
static inline int pocket_np_v3_valid(const struct PocketNetworkPolicyHeader* p) {
    return p->magic==POCKET_NP_MAGIC && p->version==3 && !(p->enabled&~31U) && !p->reserved;
}
static inline int pocket_np_member_denies(unsigned chain,unsigned uid,int member) {
    if(chain==POCKET_NP_STANDBY || chain==POCKET_NP_NO_INTERNET)return member;
    return uid>=10000U && !member; // AOSP always allows system UIDs in whitelist chains.
}
static inline int pocket_np_denies(const struct PocketNetworkPolicy* p,unsigned uid) {
    if(!pocket_np_valid(&p->header))return 1;
    if(!p->header.enabled)return 0;
    if(uid>=POCKET_NP_UIDS)return 1;
    for(unsigned chain=0;chain<POCKET_NP_CHAINS;chain++)
        if((p->header.enabled&(1U<<chain)) && pocket_np_member_denies(chain,uid,
            !!(p->members[chain][uid/8]&(1U<<(uid%8)))))return 1;
    return 0;
}
