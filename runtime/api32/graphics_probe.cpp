// SPDX-License-Identifier: GPL-3.0-only
// Exercise 12L's typed graphics adapters and real shared buffer contents.
#include <android/hardware/graphics/allocator/2.0/IAllocator.h>
#include <android/hardware/graphics/mapper/2.0/IMapper.h>
#include <android/hardware/graphics/composer/2.1/IComposer.h>
#include <android/hardware/graphics/composer/2.1/IComposerCallback.h>
#include <atomic>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
using namespace android;
using namespace android::hardware;
namespace graphics=android::hardware::graphics;
class DisplayCallback final : public graphics::composer::V2_1::IComposerCallback {
public:
    std::atomic<uint64_t> connected{UINT64_MAX};
    Return<void> onHotplug(uint64_t display,Connection state) override {
        if(state==Connection::CONNECTED)connected.store(display);return Void();
    }
    Return<void> onRefresh(uint64_t) override {return Void();}
    Return<void> onVsync(uint64_t,int64_t) override {return Void();}
};
int main() {
    setbuf(stdout,nullptr);
    using Mapper=graphics::mapper::V2_0::IMapper;
    using Error=graphics::mapper::V2_0::Error;
    auto mapper=Mapper::getService();auto allocator=graphics::allocator::V2_0::IAllocator::getService();
    if(!mapper || !allocator)return 1;
    Mapper::BufferDescriptorInfo info{};info.width=4;info.height=4;info.layerCount=1;
    info.format=graphics::common::V1_0::PixelFormat::RGBA_8888;
    info.usage=uint64_t(graphics::common::V1_0::BufferUsage::CPU_READ_OFTEN)|
        uint64_t(graphics::common::V1_0::BufferUsage::CPU_WRITE_OFTEN);
    graphics::mapper::V2_0::BufferDescriptor descriptor;bool valid=false;
    auto status=mapper->createDescriptor(info,[&](auto error,const auto& value){valid=error==Error::NONE;descriptor=value;});
    if(!status.isOk() || !valid)return 2;
    void* buffer=nullptr;uint32_t stride=0;valid=false;
    status=allocator->allocate(descriptor,1,[&](auto error,auto actualStride,const auto& handles){
        if(error!=Error::NONE || handles.size()!=1)return;
        stride=actualStride;
        auto imported=mapper->importBuffer(handles[0],[&](auto result,auto value){valid=result==Error::NONE;buffer=value;});
        valid=valid && imported.isOk();
    });
    if(!status.isOk() || !valid || !buffer || stride<4)return 3;
    Mapper::Rect bounds{};bounds.width=4;bounds.height=4;
    void* pixels=nullptr;valid=false;
    status=mapper->lock(buffer,info.usage,bounds,hidl_handle(),[&](auto error,auto data){valid=error==Error::NONE;pixels=data;});
    if(!status.isOk() || !valid || !pixels)return 4;
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)
        memcpy(static_cast<char*>(pixels)+(y*stride+x)*4,"\x33\x66\x99\xff",4);
    valid=false;status=mapper->unlock(buffer,[&](auto error,const auto&){valid=error==Error::NONE;});
    if(!status.isOk() || !valid)return 5;
    valid=false;pixels=nullptr;
    status=mapper->lock(buffer,uint64_t(graphics::common::V1_0::BufferUsage::CPU_READ_OFTEN),bounds,
        hidl_handle(),[&](auto error,auto data){valid=error==Error::NONE;pixels=data;});
    if(!status.isOk() || !valid || !pixels)return 6;
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)
        if(memcmp(static_cast<char*>(pixels)+(y*stride+x)*4,"\x33\x66\x99\xff",4))return 7;
    valid=false;status=mapper->unlock(buffer,[&](auto error,const auto&){valid=error==Error::NONE;});
    if(!status.isOk() || !valid || mapper->freeBuffer(buffer).withDefault(Error::NO_RESOURCES)!=Error::NONE)return 8;
    puts("API32_GRALLOC_PASS typed_allocator_mapper=1 shared_memory=1 rgba_verified=1 free=1");
    auto composer=graphics::composer::V2_1::IComposer::getService();
    if(!composer)return 9;
    sp<graphics::composer::V2_1::IComposerClient> client;valid=false;
    auto created=composer->createClient([&](auto error,const auto& value){
        valid=error==graphics::composer::V2_1::Error::NONE;client=value;
    });
    if(!created.isOk() || !valid || !client)return 10;
    sp<DisplayCallback> callback=new DisplayCallback;
    auto registered=client->registerCallback(callback);if(!registered.isOk())return 11;
    for(unsigned i=0;i<200 && callback->connected.load()==UINT64_MAX;i++)usleep(10000);
    uint64_t display=callback->connected.load();if(display==UINT64_MAX)return 12;
    valid=false;
    auto configs=client->getDisplayConfigs(display,[&](auto error,const auto& values){
        valid=error==graphics::composer::V2_1::Error::NONE && values.size()>0;
    });
    if(!configs.isOk() || !valid)return 13;
    printf("API32_COMPOSER_PASS typed_client=1 hotplug=connected display=%llu configs=available\n",
        static_cast<unsigned long long>(display));return 0;
}
