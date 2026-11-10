// SPDX-License-Identifier: GPL-3.0-only
#include "fuse_namespace_peer.h"
#include "../../api32/external_volume.h"
using namespace pocket::fuse32;
static unsigned descriptorCount(){DIR* directory=opendir("/proc/self/fd");assert(directory);unsigned result=0;while(readdir(directory))result++;closedir(directory);return result;}
int main() {
    unsigned initial=descriptorCount();char temp[]="/tmp/pocket-volume-XXXXXX";assert(mkdtemp(temp));std::string root=temp;
    char stat[4096];ssize_t size=readIdentityFile("/proc/self/stat",stat,sizeof(stat));assert(size>0);
    ExternalVolume::Options options;options.hostPid=getpid();options.hostUid=getuid();options.hostStart=pocket_process_start(stat,size);assert(options.hostStart);
    options.directory=root;options.identity=root+"/identity";options.locator=root+"/locator";options.device=root+"/device";
    // A model adapter exercises transaction ordering/rollback. This host test
    // deliberately does not claim a real AppFuse or MediaProvider mount.
    for(unsigned trial=0;trial<6;trial++) {
        Peer peer;peer.owner.pid=getpid();std::thread daemon;
        std::atomic<bool> callbackDone{false};unsigned attaches=0,detaches=0;uint64_t epoch=0;
        auto checking=[&](int fd) {
            if(trial==1)return -EACCES;
            int copy=dup(fd);assert(copy>=0);
            daemon=std::thread([&,copy] {
                // Require INIT before allowing the framework callback to
                // return; a sequential implementation would deadlock here.
                RequestHeader request{};assert(recv(copy,&request,sizeof(request),MSG_PEEK)==sizeof(request) && request.opcode==FUSE_INIT);
                callbackDone=true;
                if(trial==2){char packet[256];assert(recv(copy,packet,sizeof(packet),0)>0);struct {ResponseHeader h;uint32_t fields[6];} reply{{40,0,request.unique},{6,31,0,0,0,4096}};assert(send(copy,&reply,sizeof(reply),MSG_NOSIGNAL)==sizeof(reply));close(copy);}
                else peer.run(copy);
            });
            for(unsigned n=0;!callbackDone;n++){assert(n<1000);usleep(1000);}return 0;
        };
        auto adapter=[&](unsigned operation,uint64_t value) {
            assert(callbackDone && value);
            if(operation==1){assert(!attaches++ && !epoch);epoch=value;if(trial==3)return -EIO;
                if(trial!=4){uint64_t device=73;int fd=open(options.device.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0 && write(fd,&device,sizeof(device))==sizeof(device));close(fd);}return 0;}
            assert(operation==2 && epoch==value);detaches++;
            if(trial==5 && detaches==1)return -ECONNRESET;
            if(access(options.device.c_str(),F_OK)==0)assert(!unlink(options.device.c_str()));
            return 0;
        };
        {
            ExternalVolume volume(options,adapter);assert(!volume.mounted() && volume.clean());
            int result=volume.mount(checking,peer.owner);
            if(trial==1)assert(result==-EACCES && !attaches && !detaches);
            else if(trial==2)assert(result==-EPROTO && !attaches && !detaches);
            else if(trial==3)assert(result==-EIO && attaches==1 && detaches==1);
            else if(trial==4)assert(result==-ENOENT && attaches==1 && detaches==1);
            else {
                assert(!result && volume.mounted() && attaches==1 && !detaches);
                assert(volume.mount(checking,peer.owner)==-EBUSY);
                result=volume.stop();
                if(trial==5){assert(result==-ECONNRESET && !volume.clean() && !volume.mounted());
                    assert(!access((root+"/pocket_ext").c_str(),F_OK) && access((root+"/pocket_efs").c_str(),F_OK)<0);
                    assert(volume.mount(checking,peer.owner)==-EBUSY);assert(!volume.stop() && detaches==2);}
                else assert(!result && detaches==1);
            }
            assert(!volume.mounted() && volume.clean() && !volume.stop());
        }
        if(daemon.joinable())daemon.join();
        for(const char* name:{"pocket_ext","pocket_efs","locator","device"})assert(access((root+"/"+name).c_str(),F_OK)<0 && errno==ENOENT);
    }
    assert(!rmdir(root.c_str()) && descriptorCount()==initial);
    puts("EXTERNAL_VOLUME_PASS init_during_callback=1 callback_denied=1 init_failed=1 adapter_failed=1 missing_device=1 rollback=1 lost_detach_retry=1 pending_detach_broker_preserved=1 duplicate_mount_denied=1 native_brokers_stopped=1 fd_leaks=0 real_mount_claim=0");
}
