// SPDX-License-Identifier: GPL-3.0-only
// Legacy primary audio HAL (audio.primary.pocket.so) for the Android 9 guest.
// Output: 48 kHz stereo PCM16 is streamed to the host Manager's AudioTrack
// through the broker's AUDIO_OUT operation; the host's blocking writes pace
// the mixer. Input: the host microphone arrives as 48 kHz mono PCM16 through
// AUDIO_IN. When the host refuses or lacks a microphone, reads return silence
// at real time, as Android does for a silenced recording, and say so once.
#include <arpa/inet.h>
#include <errno.h>
#include <hardware/audio.h>
#include <hardware/hardware.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <system/audio.h>
#include <time.h>
#include <unistd.h>

// Must match dev.kreijstal.aosppocket.bridge.HostWire.
#define HOST_MAGIC 0x504b4831u
#define HOST_AUDIO_OUT 13u
#define HOST_AUDIO_IN 14u
#define RATE 48000
#define CHANNELS 2
#define FRAME_BYTES (CHANNELS*2)
#define IN_FRAME_BYTES 2 // mono
#define PERIOD_FRAMES 960 // 20 ms

struct pocket_dev {
    struct audio_hw_device device;
    volatile bool mic_mute;
};

struct pocket_out {
    struct audio_stream_out stream;
    pthread_mutex_t lock;
    int fd;                 // host stream socket, -1 in standby
    uint64_t written;       // frames accepted since open
    struct timespec started; // wall clock of the first frame after standby
    uint64_t standby_base;  // frames written before the current run
    int reported;
};

static int64_t ns_since(const struct timespec* t) {
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return (int64_t)(now.tv_sec-t->tv_sec)*1000000000LL+(now.tv_nsec-t->tv_nsec);
}

static int send_all(int fd,const void* data,size_t size) {
    const uint8_t* p=data;
    while(size) {ssize_t n=send(fd,p,size,MSG_NOSIGNAL);if(n<0 && errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=(size_t)n;}
    return 0;
}
static int read_all(int fd,void* data,size_t size) {
    uint8_t* p=data;
    while(size) {ssize_t n=recv(fd,p,size,0);if(n<0 && errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=(size_t)n;}
    return 0;
}

// Opens a host stream; returns the socket or a negative status for logging.
// Output keeps a small send buffer, so host backpressure and latency stay near the AudioTrack buffer;
// input keeps a small receive buffer and a read timeout, so a stalled host cannot hang the RecordThread.
static int host_open(uint32_t op,int channels) {
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    struct sockaddr_un address={.sun_family=AF_UNIX};
    strncpy(address.sun_path,"/dev/socket/pocket_host",sizeof(address.sun_path)-1);
    int buffer=PERIOD_FRAMES*channels*2*2;setsockopt(fd,SOL_SOCKET,op==HOST_AUDIO_OUT?SO_SNDBUF:SO_RCVBUF,&buffer,sizeof(buffer));
    struct timeval timeout={5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    char payload[64];int length=snprintf(payload,sizeof(payload),"{\"sample_rate\":%d,\"channels\":%d}",RATE,channels);
    uint32_t header[3]={htonl(HOST_MAGIC),htonl(op),htonl((uint32_t)length)};
    uint32_t reply[2];int status;
    if(connect(fd,(struct sockaddr*)&address,sizeof(address))!=0) status=-errno;
    else if(send_all(fd,header,sizeof(header)) || send_all(fd,payload,(size_t)length) || read_all(fd,reply,sizeof(reply))) status=-EIO;
    else {
        status=(int)ntohl(reply[0]);
        // Discard the reply body; the stream carries only PCM after it.
        uint32_t remaining=ntohl(reply[1]);char skip[256];
        while(remaining && status==0) {uint32_t n=remaining<sizeof(skip)?remaining:sizeof(skip);if(read_all(fd,skip,n)){status=-EIO;break;}remaining-=n;}
        if(status==0) {
            struct timeval steady={op==HOST_AUDIO_IN?2:0,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&steady,sizeof(steady));
            return fd;
        }
    }
    close(fd);
    return status>0?-status:status;
}

static void out_close_host(struct pocket_out* out) {
    if(out->fd>=0){close(out->fd);out->fd=-1;}
}

static uint32_t out_get_sample_rate(const struct audio_stream* s) {(void)s;return RATE;}
static int out_set_sample_rate(struct audio_stream* s,uint32_t rate) {(void)s;return rate==RATE?0:-ENOSYS;}
static size_t out_get_buffer_size(const struct audio_stream* s) {(void)s;return PERIOD_FRAMES*FRAME_BYTES;}
static audio_channel_mask_t out_get_channels(const struct audio_stream* s) {(void)s;return AUDIO_CHANNEL_OUT_STEREO;}
static audio_format_t out_get_format(const struct audio_stream* s) {(void)s;return AUDIO_FORMAT_PCM_16_BIT;}
static int out_set_format(struct audio_stream* s,audio_format_t f) {(void)s;return f==AUDIO_FORMAT_PCM_16_BIT?0:-ENOSYS;}
static int out_standby(struct audio_stream* s) {
    struct pocket_out* out=(struct pocket_out*)s;
    pthread_mutex_lock(&out->lock);out_close_host(out);out->standby_base=out->written;pthread_mutex_unlock(&out->lock);
    return 0;
}
static int out_dump(const struct audio_stream* s,int fd) {(void)s;(void)fd;return 0;}
static int out_set_parameters(struct audio_stream* s,const char* kv) {(void)s;(void)kv;return 0;}
static char* out_get_parameters(const struct audio_stream* s,const char* keys) {(void)s;(void)keys;return strdup("");}
static int out_add_effect(const struct audio_stream* s,effect_handle_t e) {(void)s;(void)e;return 0;}
static uint32_t out_get_latency(const struct audio_stream_out* s) {(void)s;return 100;}
static int out_set_volume(struct audio_stream_out* s,float l,float r) {(void)s;(void)l;(void)r;return -ENOSYS;}

static ssize_t out_write(struct audio_stream_out* s,const void* buffer,size_t bytes) {
    struct pocket_out* out=(struct pocket_out*)s;
    pthread_mutex_lock(&out->lock);
    if(out->fd<0) {
        out->fd=host_open(HOST_AUDIO_OUT,CHANNELS);
        if(out->fd<0) {
            if(!out->reported) fprintf(stderr,"POCKET_AUDIO_HOST_UNAVAILABLE status=%d\n",out->fd);
            out->reported=1;out->fd=-1;
        } else {
            if(out->reported || !out->written) fprintf(stderr,"POCKET_AUDIO_HOST_STREAM open=1\n");
            out->reported=0;clock_gettime(CLOCK_MONOTONIC,&out->started);out->standby_base=out->written;
        }
    }
    int connected=out->fd>=0;
    if(connected && send_all(out->fd,buffer,bytes)) {out_close_host(out);connected=0;}
    out->written+=bytes/FRAME_BYTES;
    struct timespec started=out->started;uint64_t run=out->written-out->standby_base;
    pthread_mutex_unlock(&out->lock);
    if(!connected) {
        // Without the host, consume at real time so the mixer neither spins nor stalls.
        usleep((useconds_t)(bytes/FRAME_BYTES*1000000ULL/RATE));
    } else {
        // Host backpressure normally paces writes; never run more than 200 ms ahead of real time.
        int64_t ahead=(int64_t)(run*1000000000ULL/RATE)-ns_since(&started)-200000000LL;
        if(ahead>0) usleep((useconds_t)(ahead/1000));
    }
    return (ssize_t)bytes;
}
static int out_get_render_position(const struct audio_stream_out* s,uint32_t* frames) {
    const struct pocket_out* out=(const struct pocket_out*)s;
    *frames=(uint32_t)out->written;return 0;
}
static int out_get_next_write_timestamp(const struct audio_stream_out* s,int64_t* t) {(void)s;(void)t;return -ENOSYS;}
static int out_get_presentation_position(const struct audio_stream_out* s,uint64_t* frames,struct timespec* stamp) {
    struct pocket_out* out=(struct pocket_out*)s;
    pthread_mutex_lock(&out->lock);
    // Frames still queued in the host path are approximated by the declared latency.
    uint64_t queued=(uint64_t)RATE*out_get_latency(s)/1000;
    *frames=out->written>queued?out->written-queued:0;
    clock_gettime(CLOCK_MONOTONIC,stamp);
    pthread_mutex_unlock(&out->lock);
    return 0;
}

static int adev_open_output_stream(struct audio_hw_device* dev,audio_io_handle_t handle,audio_devices_t devices,
        audio_output_flags_t flags,struct audio_config* config,struct audio_stream_out** stream_out,const char* address) {
    (void)dev;(void)handle;(void)devices;(void)flags;(void)address;
    if(config->sample_rate!=RATE || config->channel_mask!=AUDIO_CHANNEL_OUT_STEREO || config->format!=AUDIO_FORMAT_PCM_16_BIT) {
        // Report the supported configuration so AudioFlinger can retry with it.
        config->sample_rate=RATE;config->channel_mask=AUDIO_CHANNEL_OUT_STEREO;config->format=AUDIO_FORMAT_PCM_16_BIT;
        return -EINVAL;
    }
    struct pocket_out* out=calloc(1,sizeof(*out));if(!out)return -ENOMEM;
    pthread_mutex_init(&out->lock,NULL);out->fd=-1;
    struct audio_stream* common=&out->stream.common;
    common->get_sample_rate=out_get_sample_rate;common->set_sample_rate=out_set_sample_rate;
    common->get_buffer_size=out_get_buffer_size;common->get_channels=out_get_channels;
    common->get_format=out_get_format;common->set_format=out_set_format;common->standby=out_standby;
    common->dump=out_dump;common->set_parameters=out_set_parameters;common->get_parameters=out_get_parameters;
    common->add_audio_effect=out_add_effect;common->remove_audio_effect=out_add_effect;
    out->stream.get_latency=out_get_latency;out->stream.set_volume=out_set_volume;out->stream.write=out_write;
    out->stream.get_render_position=out_get_render_position;
    out->stream.get_next_write_timestamp=out_get_next_write_timestamp;
    out->stream.get_presentation_position=out_get_presentation_position;
    *stream_out=&out->stream;
    return 0;
}
static void adev_close_output_stream(struct audio_hw_device* dev,struct audio_stream_out* stream) {
    (void)dev;struct pocket_out* out=(struct pocket_out*)stream;
    out_close_host(out);pthread_mutex_destroy(&out->lock);free(out);
}
struct pocket_in {
    struct audio_stream_in stream;
    struct pocket_dev* dev;
    pthread_mutex_t lock;
    int fd;                  // host stream socket, -1 in standby
    uint64_t read;           // frames returned since open
    struct timespec retry;   // no host reconnect before this time
    int reported;            // host unavailability already logged
};

static void in_close_host(struct pocket_in* in) {
    if(in->fd>=0){close(in->fd);in->fd=-1;}
}
static uint32_t in_get_sample_rate(const struct audio_stream* s) {(void)s;return RATE;}
static size_t in_get_buffer_size(const struct audio_stream* s) {(void)s;return PERIOD_FRAMES*IN_FRAME_BYTES;}
static audio_channel_mask_t in_get_channels(const struct audio_stream* s) {(void)s;return AUDIO_CHANNEL_IN_MONO;}
static int in_standby(struct audio_stream* s) {
    struct pocket_in* in=(struct pocket_in*)s;
    pthread_mutex_lock(&in->lock);in_close_host(in);pthread_mutex_unlock(&in->lock);
    return 0;
}
static int in_set_gain(struct audio_stream_in* s,float gain) {(void)s;(void)gain;return 0;}
static uint32_t in_get_input_frames_lost(struct audio_stream_in* s) {(void)s;return 0;}
static ssize_t in_read(struct audio_stream_in* s,void* buffer,size_t bytes) {
    struct pocket_in* in=(struct pocket_in*)s;
    pthread_mutex_lock(&in->lock);
    if(in->fd<0 && ns_since(&in->retry)>=0) {
        in->fd=host_open(HOST_AUDIO_IN,1);
        if(in->fd<0) {
            if(!in->reported) fprintf(stderr,"POCKET_AUDIO_IN_HOST_UNAVAILABLE status=%d\n",in->fd);
            in->reported=1;in->fd=-1;
            // Retry once a second rather than on every 20 ms read.
            clock_gettime(CLOCK_MONOTONIC,&in->retry);in->retry.tv_sec+=1;
        } else {
            if(in->reported || !in->read) fprintf(stderr,"POCKET_AUDIO_IN_HOST_STREAM open=1\n");
            in->reported=0;
        }
    }
    int captured=in->fd>=0 && !read_all(in->fd,buffer,bytes);
    if(in->fd>=0 && !captured) in_close_host(in);
    if(!captured || in->dev->mic_mute) memset(buffer,0,bytes);
    in->read+=bytes/IN_FRAME_BYTES;
    pthread_mutex_unlock(&in->lock);
    // The host's blocking reads pace capture; without the host, deliver silence at real time.
    if(!captured) usleep((useconds_t)(bytes/IN_FRAME_BYTES*1000000ULL/RATE));
    return (ssize_t)bytes;
}
static int in_get_capture_position(const struct audio_stream_in* s,int64_t* frames,int64_t* time) {
    struct pocket_in* in=(struct pocket_in*)s;struct timespec now;
    pthread_mutex_lock(&in->lock);*frames=(int64_t)in->read;pthread_mutex_unlock(&in->lock);
    clock_gettime(CLOCK_MONOTONIC,&now);*time=(int64_t)now.tv_sec*1000000000LL+now.tv_nsec;
    return 0;
}

static int adev_open_input_stream(struct audio_hw_device* dev,audio_io_handle_t handle,audio_devices_t devices,
        struct audio_config* config,struct audio_stream_in** stream_in,audio_input_flags_t flags,
        const char* address,audio_source_t source) {
    (void)handle;(void)devices;(void)flags;(void)address;(void)source;
    *stream_in=NULL;
    if(config->sample_rate!=RATE || config->channel_mask!=AUDIO_CHANNEL_IN_MONO || config->format!=AUDIO_FORMAT_PCM_16_BIT) {
        config->sample_rate=RATE;config->channel_mask=AUDIO_CHANNEL_IN_MONO;config->format=AUDIO_FORMAT_PCM_16_BIT;
        return -EINVAL;
    }
    struct pocket_in* in=calloc(1,sizeof(*in));if(!in)return -ENOMEM;
    pthread_mutex_init(&in->lock,NULL);in->fd=-1;in->dev=(struct pocket_dev*)dev;
    struct audio_stream* common=&in->stream.common;
    common->get_sample_rate=in_get_sample_rate;common->set_sample_rate=out_set_sample_rate;
    common->get_buffer_size=in_get_buffer_size;common->get_channels=in_get_channels;
    common->get_format=out_get_format;common->set_format=out_set_format;common->standby=in_standby;
    common->dump=out_dump;common->set_parameters=out_set_parameters;common->get_parameters=out_get_parameters;
    common->add_audio_effect=out_add_effect;common->remove_audio_effect=out_add_effect;
    in->stream.set_gain=in_set_gain;in->stream.read=in_read;
    in->stream.get_input_frames_lost=in_get_input_frames_lost;in->stream.get_capture_position=in_get_capture_position;
    *stream_in=&in->stream;
    return 0;
}
static void adev_close_input_stream(struct audio_hw_device* dev,struct audio_stream_in* stream) {
    (void)dev;struct pocket_in* in=(struct pocket_in*)stream;
    in_close_host(in);pthread_mutex_destroy(&in->lock);free(in);
}
static int adev_init_check(const struct audio_hw_device* dev) {(void)dev;return 0;}
static int adev_set_voice_volume(struct audio_hw_device* dev,float v) {(void)dev;(void)v;return -ENOSYS;}
// -ENOSYS makes AudioFlinger apply master volume in software.
static int adev_set_master_volume(struct audio_hw_device* dev,float v) {(void)dev;(void)v;return -ENOSYS;}
static int adev_get_master_volume(struct audio_hw_device* dev,float* v) {(void)dev;(void)v;return -ENOSYS;}
static int adev_set_mode(struct audio_hw_device* dev,audio_mode_t m) {(void)dev;(void)m;return 0;}
static int adev_set_mic_mute(struct audio_hw_device* dev,bool m) {((struct pocket_dev*)dev)->mic_mute=m;return 0;}
static int adev_get_mic_mute(const struct audio_hw_device* dev,bool* m) {*m=((const struct pocket_dev*)dev)->mic_mute;return 0;}
static int adev_set_parameters(struct audio_hw_device* dev,const char* kv) {(void)dev;(void)kv;return 0;}
static char* adev_get_parameters(const struct audio_hw_device* dev,const char* keys) {(void)dev;(void)keys;return strdup("");}
static size_t adev_get_input_buffer_size(const struct audio_hw_device* dev,const struct audio_config* c) {
    (void)dev;
    return c->sample_rate==RATE && c->channel_mask==AUDIO_CHANNEL_IN_MONO && c->format==AUDIO_FORMAT_PCM_16_BIT?PERIOD_FRAMES*IN_FRAME_BYTES:0;
}
static int adev_dump(const audio_hw_device_t* dev,int fd) {(void)dev;(void)fd;return 0;}
static int adev_close(hw_device_t* device) {free(device);return 0;}

static int adev_open(const hw_module_t* module,const char* name,hw_device_t** device) {
    if(strcmp(name,AUDIO_HARDWARE_INTERFACE))return -EINVAL;
    struct pocket_dev* pocket=calloc(1,sizeof(*pocket));if(!pocket)return -ENOMEM;
    struct audio_hw_device* dev=&pocket->device;
    dev->common.tag=HARDWARE_DEVICE_TAG;dev->common.version=AUDIO_DEVICE_API_VERSION_2_0;
    dev->common.module=(struct hw_module_t*)module;dev->common.close=adev_close;
    dev->init_check=adev_init_check;dev->set_voice_volume=adev_set_voice_volume;
    dev->set_master_volume=adev_set_master_volume;dev->get_master_volume=adev_get_master_volume;
    dev->set_mode=adev_set_mode;dev->set_mic_mute=adev_set_mic_mute;dev->get_mic_mute=adev_get_mic_mute;
    dev->set_parameters=adev_set_parameters;dev->get_parameters=adev_get_parameters;
    dev->get_input_buffer_size=adev_get_input_buffer_size;
    dev->open_output_stream=adev_open_output_stream;dev->close_output_stream=adev_close_output_stream;
    dev->open_input_stream=adev_open_input_stream;dev->close_input_stream=adev_close_input_stream;
    dev->dump=adev_dump;
    *device=&dev->common;
    fprintf(stderr,"POCKET_AUDIO_HAL_OPEN rate=%d channels=%d input_channels=1\n",RATE,CHANNELS);
    return 0;
}

static struct hw_module_methods_t methods={.open=adev_open};
__attribute__((visibility("default"))) struct audio_module HAL_MODULE_INFO_SYM={
    .common={.tag=HARDWARE_MODULE_TAG,.module_api_version=AUDIO_MODULE_API_VERSION_0_1,
             .hal_api_version=HARDWARE_HAL_API_VERSION,.id=AUDIO_HARDWARE_MODULE_ID,
             .name="Pocket host audio",.author="aosp-pocket",.methods=&methods},
};
