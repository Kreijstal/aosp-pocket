// SPDX-License-Identifier: GPL-3.0-only
#include <android/hardware/audio/7.0/IDevicesFactory.h>
#include <android/hardware/audio/effect/7.0/IEffectsFactory.h>
#include <binder/IServiceManager.h>
#include <hardware/audio.h>
#include <hardware/hardware.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

using namespace android;
namespace audio=hardware::audio::V7_0;
int main(int argc,char** argv) {
    const bool denied=argc==2 && !strcmp(argv[1],"denied");
    const uid_t role=denied?39999:1000;
    if(getuid()!=0 || setgid(role) || setuid(role))return 1;
    if(denied) {
        int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return 10;
        sockaddr_un address{};address.sun_family=AF_UNIX;
        strcpy(address.sun_path,"/dev/socket/pocket_audio");
        timeval timeout{5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        uint32_t request[]={htonl(0x504b4831),htonl(13),0},reply[2];
        bool rejected=connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 &&
            send(fd,request,sizeof(request),MSG_NOSIGNAL)==sizeof(request) &&
            recv(fd,reply,sizeof(reply),MSG_WAITALL)==sizeof(reply) && ntohl(reply[0])==403 && reply[1]==0;
        close(fd);if(!rejected)return 11;
        puts("API32_AUDIO_ROLE_DENIED_PASS uid=39999 playback=denied");return 0;
    }
    if(argc==2 && !strcmp(argv[1],"services")) {
        for(const char* name:{"media.audio_policy","media.audio_flinger"}) {
            sp<IBinder> service;
            for(int i=0;i<600 && !service;i++) {
                service=defaultServiceManager()->checkService(String16(name));
                if(!service)usleep(50000);
            }
            if(!service || service->pingBinder()!=OK)return 2;
            const String16 expected(!strcmp(name,"media.audio_policy")?
                "android.media.IAudioPolicyService":"android.media.IAudioFlingerService");
            const auto descriptor=service->getInterfaceDescriptor();
            if(descriptor!=expected)return 12;
            printf("API32_AUDIO_SERVICE_LIVE name=%s descriptor=%s\n",name,String8(descriptor).c_str());
        }
        puts("API32_AUDIO_SERVICES_PASS stock_audioserver=12L flinger=live policy=live");
        return 0;
    }
    auto factory=audio::IDevicesFactory::getService();
    if(!factory)return 3;
    bool initialized=false;
    auto opened=factory->openDevice("primary",[&](auto result,const auto& device) {
        if(result!=audio::Result::OK || !device)return;
        auto checked=device->initCheck();
        initialized=checked.isOk() && checked==audio::Result::OK;
    });
    if(!opened.isOk() || !initialized)return 4;
    auto effects=hardware::audio::effect::V7_0::IEffectsFactory::getService();
    if(!effects)return 5;
    bool queried=false;
    auto descriptors=effects->getAllDescriptors([&](auto result,const auto& values) {
        queried=result==hardware::audio::effect::V7_0::Result::OK;
        printf("API32_AUDIO_EFFECTS_QUERY count=%zu\n",values.size());
    });
    if(!descriptors.isOk() || !queried)return 6;
    const hw_module_t* module=nullptr;audio_hw_device_t* device=nullptr;
    if(hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID,"primary",&module) ||
       audio_hw_device_open(module,&device))return 7;
    audio_config config{};config.sample_rate=48000;config.channel_mask=AUDIO_CHANNEL_OUT_STEREO;
    config.format=AUDIO_FORMAT_PCM_16_BIT;
    audio_stream_out* output=nullptr;
    int result=device->open_output_stream(device,1,AUDIO_DEVICE_OUT_SPEAKER,AUDIO_OUTPUT_FLAG_PRIMARY,
        &config,&output,"");
    if(result || !output){audio_hw_device_close(device);return 8;}
    // A bounded PCM pulse exercises the real host stream; no microphone access.
    int16_t pcm[1920];
    for(size_t i=0;i<1920;i++)pcm[i]=((i/2)%96<48)?512:-512;
    bool wrote=true;
    for(int i=0;i<25;i++)if(output->write(output,pcm,sizeof(pcm))!=sizeof(pcm)){wrote=false;break;}
    output->common.standby(&output->common);
    device->close_output_stream(device,output);audio_hw_device_close(device);
    if(!wrote)return 9;
    puts("API32_AUDIO_HAL_PASS wrapper=stock_7.0 primary=initialized effects=queried pcm_frames=24000");
    return 0;
}
