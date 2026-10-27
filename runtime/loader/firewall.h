// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
// Private, versioned socket-policy format, shared by the CLI and interpreter.
// Address bytes are in network order; all other fields are native little endian.
#define POCKET_FW_PATH "/data/misc/pocket-firewall.rules"
#define POCKET_FW_MAX 64
#define POCKET_FW_VERSION 1
enum { POCKET_FW_ACCEPT=0, POCKET_FW_DENY=1 };
struct PocketFirewallRule {
    uint32_t uid, owner, family, protocol, prefix, port, action;
    uint8_t address[16];
};
struct PocketFirewall {
    uint32_t magic, version, count, policy4, policy6;
    struct PocketFirewallRule rules[POCKET_FW_MAX];
};
#define POCKET_FW_MAGIC 0x50465731U
static inline int pocket_fw_valid(const struct PocketFirewall* f) {
    if(f->magic!=POCKET_FW_MAGIC || f->version!=POCKET_FW_VERSION ||
       f->count>POCKET_FW_MAX || f->policy4>1 || f->policy6>1)return 0;
    for(uint32_t i=0;i<f->count;i++) {
        const struct PocketFirewallRule* r=&f->rules[i];
        if((r->family!=4 && r->family!=6) || r->owner>1 || r->action>1 ||
           (r->protocol!=0 && r->protocol!=6 && r->protocol!=17) ||
           r->prefix>(r->family==4?32U:128U) || r->port>65535 ||
           (r->port && !r->protocol))return 0;
    }
    return 1;
}
static inline int pocket_fw_decide(const struct PocketFirewall* f,uint32_t uid,
                                  unsigned family,unsigned protocol,const uint8_t* address,unsigned port) {
    for(uint32_t i=0;i<f->count;i++) {
        const struct PocketFirewallRule* r=&f->rules[i];
        if(r->family!=family || (r->owner && r->uid!=uid) ||
           (r->protocol && r->protocol!=protocol) || (r->port && r->port!=port))continue;
        unsigned j;for(j=0;j<r->prefix;j++)
            if((address[j/8] ^ r->address[j/8]) & (0x80U>>(j%8)))break;
        if(j==r->prefix)return r->action;
    }
    return family==4?f->policy4:f->policy6;
}
