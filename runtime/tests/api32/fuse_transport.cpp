// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/fuse_transport.h"
#include <cassert>
#include <thread>
#include <future>
#include <iostream>
using namespace pocket::fuse32;
static void fault(int mode) {
    int fds[2];assert(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,fds)==0);
    Channel client(fds[0],mode==4?20:1000);
    std::thread peer([&] {
        char packet[256];ssize_t n=recv(fds[1],packet,sizeof(packet),0);assert(n>=40);
        RequestHeader request;std::memcpy(&request,packet,sizeof(request));
        ResponseHeader reply{16,0,request.unique};
        if(mode==0)reply.unique++;
        if(mode==1)reply.length=17;
        if(mode==2)reply.error=1;
        if(mode==3){send(fds[1],&reply,4,MSG_NOSIGNAL);close(fds[1]);return;}
        if(mode==4){recv(fds[1],packet,sizeof(packet),0);close(fds[1]);return;}
        if(mode==5){close(fds[1]);return;}
        assert(send(fds[1],&reply,sizeof(reply),MSG_NOSIGNAL)==16);close(fds[1]);
    });
    std::vector<char> out;int result=client.call(1,1,{10001,10002,10003},nullptr,0,&out);
    assert(result==(mode==4?-ETIMEDOUT:mode==5?-ECONNRESET:-EPROTO));
    assert(client.call(1,1,{0,0,0},nullptr,0,&out)==-ECANCELED);peer.join();
}
static void concurrent() {
    int fds[2];assert(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,fds)==0);
    Channel client(fds[0],1000);
    std::promise<void> syncReceived,readDone;
    auto entered=syncReceived.get_future();auto unblock=readDone.get_future();
    std::thread peer([&] {
        RequestHeader sync{},read{};assert(recv(fds[1],&sync,sizeof(sync),0)==40);
        assert(sync.opcode==20 && sync.node==101 && sync.uid==10001 && sync.pid==20001);syncReceived.set_value();
        assert(recv(fds[1],&read,sizeof(read),0)==40);
        assert(read.opcode==15 && read.node==202 && read.uid==10002 && read.pid==20002 && read.unique!=sync.unique);
        struct Packet {ResponseHeader h;uint64_t node;};
        Packet answer{{sizeof(Packet),0,read.unique},read.node};assert(send(fds[1],&answer,sizeof(answer),0)==sizeof(answer));
        assert(unblock.wait_for(std::chrono::seconds(1))==std::future_status::ready);
        answer={{sizeof(Packet),0,sync.unique},sync.node};assert(send(fds[1],&answer,sizeof(answer),0)==sizeof(answer));close(fds[1]);
    });
    std::promise<void> syncDone;auto synced=syncDone.get_future();
    std::thread syncing([&]{std::vector<char> out;assert(client.call(20,101,{10001,10003,20001},nullptr,0,&out)==0);
        uint64_t node=0;assert(out.size()==sizeof(node));memcpy(&node,out.data(),sizeof(node));assert(node==101);syncDone.set_value();});
    assert(entered.wait_for(std::chrono::seconds(1))==std::future_status::ready);
    std::vector<char> output;assert(client.call(15,202,{10002,10004,20002},nullptr,0,&output)==0);
    uint64_t node=0;assert(output.size()==sizeof(node));memcpy(&node,output.data(),sizeof(node));assert(node==202);
    assert(synced.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout);
    readDone.set_value();syncing.join();peer.join();
}
static void cancellation(bool malformed) {
    int fds[2];assert(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,fds)==0);
    Channel client(fds[0],1000);std::thread callers[2];
    for(auto& thread:callers)thread=std::thread([&]{std::vector<char> out;assert(client.call(15,1,{1,2,3},nullptr,0,&out)==(malformed?-EPROTO:-ECANCELED));});
    RequestHeader request;for(int i=0;i<2;i++)assert(recv(fds[1],&request,sizeof(request),0)==40);
    if(malformed){ResponseHeader bad{16,0,UINT64_MAX};assert(send(fds[1],&bad,sizeof(bad),0)==16);}
    else client.cancel();
    for(auto& thread:callers)thread.join();
    close(fds[1]);
}
int main() {
    for(int mode=0;mode<6;mode++)fault(mode);
    int fds[2];assert(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,fds)==0);
    Channel client(fds[0],1000);
    std::thread peer([&] {
        RequestHeader request;assert(recv(fds[1],&request,sizeof(request),0)==40);
        ResponseHeader error{16,-ENOENT,request.unique};assert(send(fds[1],&error,16,0)==16);
        assert(recv(fds[1],&request,sizeof(request),0)==40);
        struct {ResponseHeader h;uint64_t ino;int64_t off,length;} note{{40,2,0},1,0,0};
        assert(send(fds[1],&note,sizeof(note),0)==40);
        ResponseHeader good{16,0,request.unique};assert(send(fds[1],&good,16,0)==16);close(fds[1]);
    });
    std::vector<char> output;assert(client.call(1,1,{1,2,3},nullptr,0,&output)==-ENOENT);
    assert(client.call(1,1,{1,2,3},nullptr,0,&output)==0 && output.empty());peer.join();
    concurrent();cancellation(false);cancellation(true);
    std::cout<<"FUSE_TRANSPORT_PASS malformed=4 timeout=1 disconnect=1 error_recovery=1 invalidation=1 out_of_order=1 blocked_sync_read=1 cancel_pending=2 malformed_pending=2\n";
}
