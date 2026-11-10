// SPDX-License-Identifier: GPL-3.0-only
// A fixture-only init socket handoff. Full framework boot uses the supervisor.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int control_socket(const char* name) {
    struct sockaddr_un address={0};
    address.sun_family=AF_UNIX;
    if (snprintf(address.sun_path,sizeof(address.sun_path),"/dev/socket/%s",name)>=(int)sizeof(address.sun_path)) return -1;
    if (unlink(address.sun_path) && errno!=ENOENT) return -1;
    int fd=socket(AF_UNIX,SOCK_STREAM,0);
    if (fd<0) return -1;
    if (bind(fd,(struct sockaddr*)&address,sizeof(address)) || chmod(address.sun_path,0660) || listen(fd,16)) {
        close(fd);return -1;
    }
    char key[64],value[32];
    snprintf(key,sizeof(key),"ANDROID_SOCKET_%s",name);
    snprintf(value,sizeof(value),"%d",fd);
    if (setenv(key,value,1)) {close(fd);return -1;}
    return fd;
}
int main(void) {
    struct stat marker;
    if (getuid()!=0 || stat("/.pocket-api32-fixture",&marker)) return 64;
    umask(0077);
    if (control_socket("zygote")<0 || control_socket("usap_pool_primary")<0) {
        perror("API32_ZYGOTE_INIT_SOCKETS");return 65;
    }
    char* const args[]={"/system/bin/pocket-app-process-api32","-Xzygote","-Xnorelocate","-Xnoimage-dex2oat",
        "-Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art","-Xms16m","-Xmx192m","/system/bin",
        "--zygote","--socket-name=zygote","--enable-lazy-preload",NULL};
    execv(args[0],args);
    perror("API32_ZYGOTE_EXEC");return 66;
}
