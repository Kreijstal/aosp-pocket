// SPDX-License-Identifier: GPL-3.0-only
#include <hardware/gralloc.h>
#include <stdio.h>
#include <string.h>
#include <cutils/ashmem.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
    int shared=ashmem_create_region("pocket-api29",4096);
    if(shared<0 || ashmem_get_size_region(shared)!=4096 || fcntl(shared,F_GET_SEALS)<0)return 9;
    void* writable=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,shared,0);
    if(writable==MAP_FAILED)return 10;
    memcpy(writable,"api29",6);
    if(ashmem_set_prot_region(shared,PROT_READ))return 11;
    void* readable=mmap(NULL,4096,PROT_READ,MAP_SHARED,shared,0);
    if(readable==MAP_FAILED || memcmp(readable,"api29",6))return 12;
    void* denied=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,shared,0);
    if(denied!=MAP_FAILED)return 13;
    munmap(writable,4096);munmap(readable,4096);close(shared);
    puts("API29_SHARED_MEMORY_PASS memfd=1 readback=1 future_write_mapping_denied=1");
    const hw_module_t* module=NULL;
    if(hw_get_module(GRALLOC_HARDWARE_MODULE_ID,&module))return 1;
    alloc_device_t* allocator=NULL;
    if(gralloc_open(module,&allocator))return 2;
    buffer_handle_t handle=NULL;int stride=0;
    if(allocator->alloc(allocator,4,4,HAL_PIXEL_FORMAT_RGBA_8888,
        GRALLOC_USAGE_SW_READ_OFTEN|GRALLOC_USAGE_SW_WRITE_OFTEN,&handle,&stride))return 3;
    if(!handle || handle->numFds!=1 || handle->numInts<4 || stride<4)return 4;
    printf("API29_GRALLOC_HANDLE fd_count=%d int_count=%d magic=%x bytes=%d stride=%d\n",
        handle->numFds,handle->numInts,handle->data[1],handle->data[3],stride);
    const gralloc_module_t* gralloc=(const gralloc_module_t*)module;void* pixels=NULL;
    if(gralloc->lock(gralloc,handle,GRALLOC_USAGE_SW_WRITE_OFTEN,0,0,4,4,&pixels) || !pixels)return 5;
    for(int y=0;y<4;y++)for(int x=0;x<4;x++)memcpy((char*)pixels+(y*stride+x)*4,"\x33\x66\x99\xff",4);
    if(gralloc->unlock(gralloc,handle) || gralloc->lock(gralloc,handle,GRALLOC_USAGE_SW_READ_OFTEN,0,0,4,4,&pixels))return 6;
    for(int y=0;y<4;y++)for(int x=0;x<4;x++)if(memcmp((char*)pixels+(y*stride+x)*4,"\x33\x66\x99\xff",4))return 7;
    if(gralloc->unlock(gralloc,handle) || allocator->free(allocator,handle) || gralloc_close(allocator))return 8;
    puts("API29_GRALLOC_PASS allocate=1 lock=1 rgba_verified=1 free=1");return 0;
}
