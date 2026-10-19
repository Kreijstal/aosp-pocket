// SPDX-License-Identifier: GPL-3.0-only
#include "../direct/composer_slots.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    PocketSlots state;pocket_slots_reset(&state);
    int front=-1;uint64_t seen=0,contents[3]={0};
    for(int frame=1;frame<=2000;frame++) {
        int back=pocket_slots_back(&state);assert(back>=0 && !(state.held&(1u<<back)));
        contents[back]=frame;pocket_slots_publish(&state,back);
        if(front>=0)assert(contents[front]==seen);
        if(frame%7==0) {
            int next=pocket_slots_acquire(&state,seen);assert(next>=0 && contents[next]==(uint64_t)frame);
            assert(next!=front);
            if(front>=0)assert(pocket_slots_release(&state,front)==0);
            front=next;seen=frame;
            assert(pocket_slots_acquire(&state,seen)==-1);
        }
    }
    assert(pocket_slots_release(&state,3)==-1);
    if(front>=0)assert(pocket_slots_release(&state,front)==0);
    // A slow handoff may hold two buffers; producer must keep updating the third.
    pocket_slots_reset(&state);
    pocket_slots_publish(&state,0);assert(pocket_slots_acquire(&state,0)==0);
    pocket_slots_publish(&state,1);assert(pocket_slots_acquire(&state,1)==1);
    for(int i=0;i<100;i++){assert(pocket_slots_back(&state)==2);pocket_slots_publish(&state,2);}
    assert(state.held==3 && state.latest==2 && state.sequence==102);
    assert(pocket_slots_release(&state,0)==0 && pocket_slots_release(&state,0)==-1);
    puts("PASS: 2000 compositor updates preserve held pixels, latest-frame delivery and bounded slow-consumer handoff");
}
