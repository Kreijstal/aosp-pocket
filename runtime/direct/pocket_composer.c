// SPDX-License-Identifier: GPL-3.0-only
// App-private Android 9 HWC 1.1 output. Uses the pinned Redroid linear gralloc,
// never a host/vendor handle. Host GPU allocation is a separate NDK operation.
#define _GNU_SOURCE
#include <hardware/hwcomposer.h>
#include <cutils/properties.h>
#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "composer_slots.h"

typedef struct {
    hwc_composer_device_1_t device;
    const gralloc_module_t* gralloc;
    const hwc_procs_t* procs;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t server_thread, vsync_thread;
    int listener, client, width, height, fps;
    bool stopping, vsync_enabled, active;
    uint8_t* memory;
    size_t bytes;
    PocketSlots slots;
    uint64_t copied_ns[POCKET_FRAME_SLOTS];
} Composer;

static uint64_t nanos(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static bool read_all(int fd,void* data,size_t bytes) {
    uint8_t* p=data;
    while(bytes){ssize_t n=recv(fd,p,bytes,0);if(n<0 && errno==EINTR)continue;if(n<=0)return false;p+=n;bytes-=n;}return true;
}
static bool write_all(int fd,const void* data,size_t bytes) {
    const uint8_t* p=data;
    while(bytes){ssize_t n=send(fd,p,bytes,MSG_NOSIGNAL);if(n<0 && errno==EINTR)continue;if(n<=0)return false;p+=n;bytes-=n;}return true;
}
static bool put32(int fd,uint32_t value){value=htonl(value);return write_all(fd,&value,4);}
static bool get32(int fd,uint32_t* value){if(!read_all(fd,value,4))return false;*value=ntohl(*value);return true;}
static bool put64(int fd,uint64_t value){return put32(fd,(uint32_t)(value>>32)) && put32(fd,(uint32_t)value);}
static bool get64(int fd,uint64_t* value){uint32_t a,b;if(!get32(fd,&a)||!get32(fd,&b))return false;*value=((uint64_t)a<<32)|b;return true;}
static int receive_fd(int socket) {
    char byte;struct iovec iov={&byte,1};
    union {struct cmsghdr alignment;char bytes[CMSG_SPACE(sizeof(int)*4)];} control;
    struct msghdr message={0};message.msg_iov=&iov;message.msg_iovlen=1;
    message.msg_control=control.bytes;message.msg_controllen=sizeof(control.bytes);
    ssize_t n;do{n=recvmsg(socket,&message,MSG_CMSG_CLOEXEC);}while(n<0 && errno==EINTR);
    int result=-1,count=0;
    if(n!=1)return -1;
    for(struct cmsghdr* c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c)) {
        if(c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS)continue;
        size_t descriptors=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);int* fds=(int*)CMSG_DATA(c);
        for(size_t i=0;i<descriptors;i++){if(!count++)result=fds[i];else close(fds[i]);}
    }
    if(count!=1 || (message.msg_flags&MSG_CTRUNC)){if(result>=0)close(result);return -1;}
    return result;
}
static void invalidate(Composer* c) {
    pthread_mutex_lock(&c->lock);const hwc_procs_t* procs=c->procs;pthread_mutex_unlock(&c->lock);
    if(procs && procs->invalidate)procs->invalidate(procs);
}
static void serve(Composer* c,int peer) {
    if(!put32(peer,0x504b4331)||!put32(peer,c->width)||!put32(peer,c->height)||!put32(peer,c->fps))return;
    // Exactly one ancillary descriptor accompanies this byte, before framed commands.
    int fd=receive_fd(peer);if(fd<0)return;
    struct stat status;void* memory=MAP_FAILED;
    size_t length=(size_t)c->width*c->height*4*POCKET_FRAME_SLOTS;
    if(fstat(fd,&status)==0 && status.st_size==(off_t)length)memory=mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    // Android 9 physical hosts can use ashmem instead of a memfd (st_size==0).
    if(memory==MAP_FAILED && fstat(fd,&status)==0 && status.st_size==0) {
        // ASHMEM_GET_SIZE is the stable driver ABI; do not interpret other file types.
        #define POCKET_ASHMEM_GET_SIZE _IO(0x77,4)
        if(ioctl(fd,POCKET_ASHMEM_GET_SIZE,NULL)==(int)length)memory=mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    }
    close(fd);if(memory==MAP_FAILED)return;
    pthread_mutex_lock(&c->lock);c->memory=memory;c->bytes=length;c->active=false;pocket_slots_reset(&c->slots);pthread_mutex_unlock(&c->lock);
    if(!put32(peer,1))goto cleanup;
    for(;;) {
        uint32_t op;if(!get32(peer,&op))break;
        if(op==1) { // Resume, keeping the last held frame stable until its replacement arrives.
            pthread_mutex_lock(&c->lock);c->active=true;pthread_mutex_unlock(&c->lock);invalidate(c);
            if(!put32(peer,1))break;
        } else if(op==2) { // Acquire a newer frame and release a previously consumed slot.
            uint64_t after;uint32_t release;
            if(!get64(peer,&after)||!get32(peer,&release))break;
            pthread_mutex_lock(&c->lock);
            if(release!=UINT32_MAX && pocket_slots_release(&c->slots,(int)release)!=0){pthread_mutex_unlock(&c->lock);break;}
            struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=100000000;
            if(until.tv_nsec>=1000000000){until.tv_nsec-=1000000000;until.tv_sec++;}
            while(c->active && !c->stopping && c->slots.sequence<=after) {
                if(pthread_cond_timedwait(&c->changed,&c->lock,&until)==ETIMEDOUT)break;
            }
            int slot=pocket_slots_acquire(&c->slots,after);
            uint64_t sequence=c->slots.sequence,copy=slot<0?0:c->copied_ns[slot];
            pthread_mutex_unlock(&c->lock);
            if(!put64(peer,slot<0?0:sequence))break;
            if(slot>=0 && (!put32(peer,(uint32_t)slot)||!put64(peer,copy)))break;
        } else if(op==3) {
            pthread_mutex_lock(&c->lock);c->active=false;pthread_mutex_unlock(&c->lock);
            if(!put32(peer,1))break;
        } else break;
    }
cleanup:
    pthread_mutex_lock(&c->lock);c->active=false;c->memory=NULL;c->bytes=0;pocket_slots_reset(&c->slots);pthread_mutex_unlock(&c->lock);
    munmap(memory,length);
}
static void* server(void* opaque) {
    Composer* c=opaque;
    for(;;) {
        int peer=accept4(c->listener,NULL,NULL,SOCK_CLOEXEC);
        if(peer<0){if(errno==EINTR)continue;break;}
        pthread_mutex_lock(&c->lock);c->client=peer;bool stop=c->stopping;pthread_mutex_unlock(&c->lock);
        if(!stop)serve(c,peer);
        pthread_mutex_lock(&c->lock);c->client=-1;pthread_mutex_unlock(&c->lock);close(peer);
        if(stop)break;
    }
    return NULL;
}
static int prepare(hwc_composer_device_1_t* device,size_t count,hwc_display_contents_1_t** displays) {
    (void)device;
    if(!count || !displays || !displays[0])return 0;
    for(size_t i=0;i<displays[0]->numHwLayers;i++)
        if(displays[0]->hwLayers[i].compositionType!=HWC_FRAMEBUFFER_TARGET)displays[0]->hwLayers[i].compositionType=HWC_FRAMEBUFFER;
    return 0;
}
static int present(hwc_composer_device_1_t* device,size_t count,hwc_display_contents_1_t** displays) {
    Composer* c=(Composer*)device;
    if(!count || !displays || !displays[0])return 0;
    hwc_display_contents_1_t* list=displays[0];list->retireFenceFd=-1;
    for(size_t i=0;i<list->numHwLayers;i++) {
        hwc_layer_1_t* layer=&list->hwLayers[i];bool complete=true;
        if(layer->acquireFenceFd>=0) {
            struct pollfd fence={layer->acquireFenceFd,POLLIN,0};int ready;
            do{ready=poll(&fence,1,1000);}while(ready<0 && errno==EINTR);
            complete=ready>0 && !(fence.revents&(POLLERR|POLLNVAL));close(layer->acquireFenceFd);layer->acquireFenceFd=-1;
        }
        layer->releaseFenceFd=-1;
        if(!complete || layer->compositionType!=HWC_FRAMEBUFFER_TARGET || !layer->handle)continue;
        pthread_mutex_lock(&c->lock);bool active=c->memory && c->active;pthread_mutex_unlock(&c->lock);
        if(!active)continue;
        // Pinned Redroid gralloc: one FD, magic 0x3141592, even pixel stride,
        // linear RGBA/RGBX client target. Validate before touching its pixels.
        const native_handle_t* handle=layer->handle;
        size_t stride=((size_t)c->width+1)&~(size_t)1;
        size_t required=stride*(((size_t)c->height+1)&~(size_t)1)*4;
        if(handle->numFds!=1 || handle->numInts<4 || handle->data[1]!=0x3141592 || handle->data[3]<0 || (size_t)handle->data[3]<required)continue;
        uint64_t began=nanos();void* pixels=NULL;
        if(c->gralloc->lock(c->gralloc,handle,GRALLOC_USAGE_SW_READ_OFTEN,0,0,c->width,c->height,&pixels)!=0 || !pixels)continue;
        pthread_mutex_lock(&c->lock);
        if(c->memory && c->active) {
            int slot=pocket_slots_back(&c->slots);
            if(slot>=0) {
                uint8_t* destination=c->memory+(size_t)slot*c->width*c->height*4;
                for(int y=0;y<c->height;y++)memcpy(destination+(size_t)y*c->width*4,(uint8_t*)pixels+(size_t)y*stride*4,(size_t)c->width*4);
                c->copied_ns[slot]=nanos()-began;pocket_slots_publish(&c->slots,slot);pthread_cond_broadcast(&c->changed);
            }
        }
        pthread_mutex_unlock(&c->lock);c->gralloc->unlock(c->gralloc,handle);
    }
    return 0;
}
static void register_procs(hwc_composer_device_1_t* device,const hwc_procs_t* procs) {
    Composer* c=(Composer*)device;pthread_mutex_lock(&c->lock);c->procs=procs;pthread_mutex_unlock(&c->lock);
}
static int event(hwc_composer_device_1_t* device,int display,int event,int enabled) {
    if(display!=0 || event!=HWC_EVENT_VSYNC || (enabled!=0 && enabled!=1))return -EINVAL;
    Composer* c=(Composer*)device;pthread_mutex_lock(&c->lock);c->vsync_enabled=enabled;pthread_mutex_unlock(&c->lock);return 0;
}
static int blank(hwc_composer_device_1_t* device,int display,int value){(void)device;(void)value;return display==0?0:-EINVAL;}
static int query(hwc_composer_device_1_t* device,int what,int* value) {
    Composer* c=(Composer*)device;
    if(what==HWC_BACKGROUND_LAYER_SUPPORTED)*value=0;
    else if(what==HWC_VSYNC_PERIOD)*value=1000000000/c->fps;else return -EINVAL;return 0;
}
static int configs(hwc_composer_device_1_t* device,int display,uint32_t* values,size_t* count) {
    (void)device;if(display)return -EINVAL;if(*count){values[0]=0;*count=1;}return 0;
}
static int attributes(hwc_composer_device_1_t* device,int display,uint32_t config,const uint32_t* attributes,int32_t* values) {
    (void)config;if(display)return -EINVAL;Composer* c=(Composer*)device;
    for(int i=0;attributes[i]!=HWC_DISPLAY_NO_ATTRIBUTE;i++) {
        switch(attributes[i]) {
            case HWC_DISPLAY_WIDTH:values[i]=c->width;break;
            case HWC_DISPLAY_HEIGHT:values[i]=c->height;break;
            case HWC_DISPLAY_VSYNC_PERIOD:values[i]=1000000000/c->fps;break;
            case HWC_DISPLAY_DPI_X:case HWC_DISPLAY_DPI_Y:values[i]=property_get_int32("ro.sf.lcd_density",320)*1000;break;
            default:return -EINVAL;
        }
    }return 0;
}
static void* vsync(void* opaque) {
    Composer* c=opaque;uint64_t next=nanos();
    for(;;) {
        next+=1000000000/c->fps;struct timespec when={(time_t)(next/1000000000),(long)(next%1000000000)};
        while(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&when,NULL)==EINTR){}
        pthread_mutex_lock(&c->lock);bool stop=c->stopping,enabled=c->vsync_enabled;const hwc_procs_t* procs=c->procs;pthread_mutex_unlock(&c->lock);
        if(stop)break;if(enabled && procs && procs->vsync)procs->vsync(procs,0,(int64_t)nanos());
        uint64_t now=nanos();if(next+1000000000/c->fps<now)next=now;
    }return NULL;
}
static int close_device(hw_device_t* device) {
    Composer* c=(Composer*)device;pthread_mutex_lock(&c->lock);c->stopping=true;pthread_cond_broadcast(&c->changed);
    if(c->client>=0)shutdown(c->client,SHUT_RDWR);pthread_mutex_unlock(&c->lock);shutdown(c->listener,SHUT_RDWR);
    pthread_join(c->server_thread,NULL);pthread_join(c->vsync_thread,NULL);close(c->listener);
    pthread_cond_destroy(&c->changed);pthread_mutex_destroy(&c->lock);free(c);return 0;
}
static int open_device(const hw_module_t* module,const char* name,hw_device_t** output) {
    if(strcmp(name,HWC_HARDWARE_COMPOSER))return -EINVAL;
    Composer* c=calloc(1,sizeof(*c));if(!c)return -ENOMEM;
    c->listener=-1;c->client=-1;c->width=property_get_int32("ro.boot.redroid_width",480);c->height=property_get_int32("ro.boot.redroid_height",800);
    c->fps=property_get_int32("ro.boot.redroid_fps",60);
    if(c->width<1 || c->height<1 || c->width>8192 || c->height>8192 || (uint64_t)c->width*c->height>16*1024*1024 || c->fps<1 || c->fps>120){free(c);return -EINVAL;}
    const hw_module_t* gralloc=NULL;int error=hw_get_module(GRALLOC_HARDWARE_MODULE_ID,&gralloc);
    if(error || !gralloc){free(c);return error?error:-ENODEV;}c->gralloc=(const gralloc_module_t*)gralloc;
    pthread_mutex_init(&c->lock,NULL);pthread_cond_init(&c->changed,NULL);pocket_slots_reset(&c->slots);
    c->listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/pocket_composer");
    unlink(address.sun_path);
    if(c->listener<0 || bind(c->listener,(struct sockaddr*)&address,sizeof(address)) || listen(c->listener,1)) {
        error=-errno;if(c->listener>=0)close(c->listener);pthread_cond_destroy(&c->changed);pthread_mutex_destroy(&c->lock);free(c);return error;
    }
    chmod(address.sun_path,0600);
    c->device.common.tag=HARDWARE_DEVICE_TAG;c->device.common.version=HWC_DEVICE_API_VERSION_1_1;c->device.common.module=(hw_module_t*)module;c->device.common.close=close_device;
    c->device.prepare=prepare;c->device.set=present;c->device.registerProcs=register_procs;c->device.eventControl=event;c->device.blank=blank;c->device.query=query;
    c->device.getDisplayConfigs=configs;c->device.getDisplayAttributes=attributes;
    if(pthread_create(&c->vsync_thread,NULL,vsync,c)!=0){close(c->listener);pthread_cond_destroy(&c->changed);pthread_mutex_destroy(&c->lock);free(c);return -EAGAIN;}
    if(pthread_create(&c->server_thread,NULL,server,c)!=0) {
        pthread_mutex_lock(&c->lock);c->stopping=true;pthread_mutex_unlock(&c->lock);pthread_join(c->vsync_thread,NULL);
        close(c->listener);pthread_cond_destroy(&c->changed);pthread_mutex_destroy(&c->lock);free(c);return -EAGAIN;
    }
    __android_log_print(ANDROID_LOG_INFO,"PocketComposer","DIRECT_COMPOSER_READY width=%d height=%d fps=%d",c->width,c->height,c->fps);
    *output=&c->device.common;return 0;
}
static hw_module_methods_t methods={.open=open_device};
hwc_module_t HAL_MODULE_INFO_SYM={.common={.tag=HARDWARE_MODULE_TAG,.module_api_version=HWC_MODULE_API_VERSION_0_1,.hal_api_version=HARDWARE_HAL_API_VERSION,
    .id=HWC_HARDWARE_MODULE_ID,.name="Pocket direct-frame composer",.author="AOSP Pocket contributors",.methods=&methods}};
