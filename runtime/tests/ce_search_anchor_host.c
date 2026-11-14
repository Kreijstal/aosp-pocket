// SPDX-License-Identifier: GPL-3.0-only
// Exercise the real freestanding client with the two kernel directory modes.
#define main external_runtime_baseline_main
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include "external_runtime_host.c"
#pragma GCC diagnostic pop
#undef main
#include "../loader/ce_fd_policy.h"

static int anchors[2];
static void* search_peer(void* ignored) {
    (void)ignored;
    for(unsigned round=0;round<2;round++) {
        int client=accept4(listener,NULL,NULL,SOCK_CLOEXEC);assert(client>=0);
        struct PocketExternalRequest2 request;char incoming[CMSG_SPACE(sizeof(int))];
        struct iovec input={&request,sizeof(request)};struct msghdr packet={0};
        packet.msg_iov=&input;packet.msg_iovlen=1;packet.msg_control=incoming;packet.msg_controllen=sizeof(incoming);
        assert(recvmsg(client,&packet,MSG_WAITALL)==sizeof(request));
        assert(request.header.mode==POCKET_EXT_DIR_SEARCH && !request.header.flags && !request.header.path_size);
        struct cmsghdr* rights=CMSG_FIRSTHDR(&packet);assert(rights && rights->cmsg_type==SCM_RIGHTS && rights->cmsg_len==CMSG_LEN(sizeof(int)));
        int received=-1;memcpy(&received,CMSG_DATA(rights),sizeof(received));assert(received>=0);close(received);
        struct PocketExternalReply reply={0,0};char control[CMSG_SPACE(sizeof(int))]={0};
        struct iovec output={&reply,sizeof(reply)};memset(&packet,0,sizeof(packet));
        packet.msg_iov=&output;packet.msg_iovlen=1;packet.msg_control=control;packet.msg_controllen=sizeof(control);
        rights=CMSG_FIRSTHDR(&packet);rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;rights->cmsg_len=CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(rights),&anchors[round],sizeof(int));assert(sendmsg(client,&packet,MSG_NOSIGNAL)==sizeof(reply));close(client);
    }
    return NULL;
}
int main(void) {
    unsigned initial=descriptors();char root[]="/tmp/pocket-ce-search-XXXXXX";assert(mkdtemp(root));
    strcpy(pocket_loader_config.root,root);root_size=strlen(root);external_namespace_enabled=1;
    char path[PATH_LIMIT],scratch[4*PATH_LIMIT];
    external_root_path(path,"/dev");assert(!mkdir(path,0700));external_root_path(path,"/dev/socket");assert(!mkdir(path,0700));
    external_root_path(path,"/dev/socket/pocket_efs");listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(listener>=0);
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;assert(strlen(path)<sizeof(address.sun_path));strcpy(address.sun_path,path);
    assert(!bind(listener,(struct sockaddr*)&address,sizeof(address)) && !listen(listener,2));
    char stat[4096];int fd=open("/proc/self/stat",O_RDONLY|O_CLOEXEC);assert(fd>=0);ssize_t n=read(fd,stat,sizeof(stat));close(fd);assert(n>0);
    struct PocketExternalLocator info={POCKET_EXTERNAL_LOCATOR_MAGIC,1,(uint32_t)getpid(),0,pocket_process_start(stat,n)};assert(info.start);
    external_root_path(path,"/.pocket-external-ready");fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);assert(fd>=0 && write(fd,&info,sizeof(info))==sizeof(info));close(fd);
    anchors[0]=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC);anchors[1]=open(root,O_PATH|O_DIRECTORY|O_CLOEXEC);assert(anchors[0]>=0 && anchors[1]>=0);
    pthread_t peer;assert(!pthread_create(&peer,NULL,search_peer,NULL));
    // CeStorage.directory previously always returned the first mode, causing
    // the existing DIR_SEARCH validator to reject the real transferred FD.
    assert(external_descriptor_rpc("",O_PATH|O_DIRECTORY|O_CLOEXEC,0,0,anchors[0],POCKET_EXT_DIR_SEARCH,scratch)==-EPROTO);
    long result=external_descriptor_rpc("",O_PATH|O_DIRECTORY|O_CLOEXEC,0,0,anchors[0],POCKET_EXT_DIR_SEARCH,scratch);
    assert(result>=0 && (fcntl((int)result,F_GETFL)&O_PATH) && (fcntl((int)result,F_GETFD)&FD_CLOEXEC));
    char byte;errno=0;assert(read((int)result,&byte,1)<0 && errno==EBADF);
    // Compare the production emulation gate to real kernel O_PATH behavior.
#define BAD(nr,a,b,c) do {assert(!ce_path_only_operation(nr,a));errno=0;assert(syscall(nr,result,a,b,c)<0 && errno==EBADF);}while(0)
    BAD(__NR_getdents64,(long)scratch,sizeof(scratch),0);
    BAD(__NR_fchmod,0700,0,0);BAD(__NR_ftruncate,0,0,0);
    BAD(__NR_fsync,0,0,0);BAD(__NR_fdatasync,0,0,0);
    BAD(__NR_fcntl,F_SETFL,O_NONBLOCK,0);
    struct flock lock={.l_type=F_RDLCK,.l_whence=SEEK_SET};
    BAD(__NR_fcntl,F_GETLK,(long)&lock,0);
#undef BAD
    struct stat metadata;assert(ce_path_only_operation(__NR_fstat,0) && !fstat((int)result,&metadata) && S_ISDIR(metadata.st_mode));
    assert(ce_path_only_operation(__NR_fcntl,F_DUPFD_CLOEXEC));
    int duplicate=fcntl((int)result,F_DUPFD_CLOEXEC,3);assert(duplicate>=0 && (fcntl(duplicate,F_GETFL)&O_PATH));close(duplicate);
    int saved=open(".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(saved>=0);assert(!fchdir((int)result));assert(!fchdir(saved));close(saved);close((int)result);
    assert(!pthread_join(peer,NULL));close(anchors[0]);close(anchors[1]);close(listener);
    external_root_path(path,"/.pocket-external-ready");assert(!unlink(path));external_root_path(path,"/dev/socket/pocket_efs");assert(!unlink(path));
    external_root_path(path,"/dev/socket");assert(!rmdir(path));external_root_path(path,"/dev");assert(!rmdir(path));assert(!rmdir(root));assert(descriptors()==initial);
    puts("CE_SEARCH_ANCHOR_PASS readonly_anchor_reproduces_eproto=1 real_opath_directory_accepted=1 opath_read_ebadf=1 real_fchdir=1 cloexec=1 validator_unchanged=1 metadata_gate_matches_kernel=1 fd_leaks=0");
}
