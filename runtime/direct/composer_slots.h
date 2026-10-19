// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#define POCKET_FRAME_SLOTS 3
typedef struct {
    int latest, cursor;
    unsigned held;
    uint64_t sequence;
} PocketSlots;
static inline void pocket_slots_reset(PocketSlots* s) {
    s->latest=-1;s->cursor=0;s->held=0;s->sequence=0;
}
static inline int pocket_slots_back(PocketSlots* s) {
    for(int n=0;n<POCKET_FRAME_SLOTS;n++) {
        int slot=(s->cursor+n)%POCKET_FRAME_SLOTS;
        if(!(s->held&(1u<<slot)) && slot!=s->latest)return slot;
    }
    // With two held slots, the remaining latest slot can be replaced while
    // the state mutex excludes acquisition. Preserve the newest completed UI.
    if(s->latest>=0 && !(s->held&(1u<<s->latest)))return s->latest;
    return -1;
}
static inline void pocket_slots_publish(PocketSlots* s,int slot) {
    s->latest=slot;s->cursor=(slot+1)%POCKET_FRAME_SLOTS;s->sequence++;
}
static inline int pocket_slots_acquire(PocketSlots* s,uint64_t after) {
    if(s->latest<0 || s->sequence<=after)return -1;
    s->held|=1u<<s->latest;return s->latest;
}
static inline int pocket_slots_release(PocketSlots* s,int slot) {
    if(slot<0 || slot>=POCKET_FRAME_SLOTS || !(s->held&(1u<<slot)))return -1;
    s->held&=~(1u<<slot);return 0;
}
