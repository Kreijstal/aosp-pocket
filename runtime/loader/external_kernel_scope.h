// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// One atomic word contains pid, tid and nesting depth. No reset lock can be
// stranded by a nested signal handler; fork children atomically reclaim words
// owned by the old process. Linux pid_max is below the26-bit identity bound.
static uint64_t external_kernel_threads[64];
static long external_kernel_scope(unsigned enabled) {
    if(!guest_identity || guest_uid[1] || enabled>1)return -EPERM;
    uint64_t pid=(uint32_t)raw(__NR_getpid,0,0,0,0,0,0),tid=(uint32_t)raw(__NR_gettid,0,0,0,0,0,0);
    if(pid>=0x4000000 || tid>=0x4000000)return -EOVERFLOW;
    uint64_t key=(pid<<32)|(tid<<6);
    for(unsigned attempt=0;attempt<128;attempt++) {
        unsigned available=64;int retry=0;
        for(unsigned i=0;i<64;i++) {
            uint64_t old=__atomic_load_n(&external_kernel_threads[i],__ATOMIC_ACQUIRE);
            if((old&~63ULL)==key) {
                if(enabled && (old&63)>=32)return -EOVERFLOW;
                uint64_t next=enabled?old+1:(old&63)>1?old-1:0;
                if(__atomic_compare_exchange_n(&external_kernel_threads[i],&old,next,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
                retry=1;break;
            }
            if(available==64 && (!old || old>>32!=pid))available=i;
        }
        if(retry)continue;
        if(!enabled)return 0;
        if(available==64)return -EUSERS;
        uint64_t old=__atomic_load_n(&external_kernel_threads[available],__ATOMIC_ACQUIRE),next=key|1;
        if(old && old>>32==pid)continue;
        if(__atomic_compare_exchange_n(&external_kernel_threads[available],&old,next,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
    }
    return enabled?-EUSERS:0;
}
static int external_kernel_active(void) {
    if(!guest_identity || guest_uid[1])return 0;
    uint64_t pid=(uint32_t)raw(__NR_getpid,0,0,0,0,0,0),tid=(uint32_t)raw(__NR_gettid,0,0,0,0,0,0),key=(pid<<32)|(tid<<6);
    for(unsigned i=0;i<64;i++){uint64_t value=__atomic_load_n(&external_kernel_threads[i],__ATOMIC_ACQUIRE);if((value&~63ULL)==key && (value&63))return 1;}
    return 0;
}
