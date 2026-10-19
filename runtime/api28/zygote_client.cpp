// SPDX-License-Identifier: GPL-3.0-only
// Independent client of the real Android 9 ZygoteConnection wire protocol.
#include <arpa/inet.h>
#include <android/log.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/system_properties.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <stdlib.h>

namespace {
uid_t kernelUid() {
    FILE* status = fopen("/proc/self/status", "re");
    if (!status) return static_cast<uid_t>(-1);
    char line[256]; unsigned uid = -1;
    while (fgets(line, sizeof(line), status)) if (sscanf(line, "Uid: %u", &uid) == 1) break;
    fclose(status); return uid;
}
bool transfer(int fd, void* bytes, size_t size, bool write) {
    char* p = static_cast<char*>(bytes);
    while (size) {
        ssize_t n = write ? send(fd, p, size, MSG_NOSIGNAL) : recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= n;
    }
    return true;
}
bool command(int fd, std::string text) { return transfer(fd, &text[0], text.size(), true); }
bool abi(int fd) {
    if (!command(fd, "1\n--query-abi-list\n")) return false;
    uint32_t count;
    if (!transfer(fd, &count, sizeof(count), false)) return false;
    count = ntohl(count);
    if (!count || count > 128) return false;
    std::string list(count, '\0');
    if (!transfer(fd, &list[0], count, false) || list != "arm64-v8a") return false;
    printf("API28_ZYGOTE_ABI value=%s\n", list.c_str());
    return true;
}
}
int main(int argc, char** argv) {
    setbuf(stdout, nullptr);
    if (argc < 2 || (strcmp(argv[1], "fork") && strcmp(argv[1], "query"))) return 64;
    char sdk[PROP_VALUE_MAX];
    __system_property_get("ro.build.version.sdk", sdk);
    if (getuid() != 39999 || strcmp(sdk, "28")) return 65;
    int fd = -1;
    for (int n = 0; n < 200; ++n) {
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return 1;
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        strcpy(address.sun_path, "/dev/socket/zygote");
        if (!connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) break;
        close(fd); fd = -1; usleep(200000);
    }
    if (fd < 0) { puts("ZYGOTE_CONNECT_TIMEOUT"); return 2; }
    timeval timeout{40, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ucred peer{}; socklen_t length = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) ||
        peer.uid != kernelUid() || peer.pid <= 1 || !abi(fd)) return 3;
    __android_log_write(ANDROID_LOG_INFO, "PocketZygoteProbe", "API28_LOG_WRITER_TEST");
    if (!strcmp(argv[1], "query")) { close(fd); return 0; }
    if (argc != 3 || strlen(argv[2]) != 16 || strspn(argv[2], "0123456789abcdef") != 16) return 64;
    const bool direct=getenv("POCKET_DIRECT_ROOT")!=nullptr;
    std::vector<std::string> arguments={"--runtime-args","--setuid=39999","--setgid=39999",
        "--runtime-flags=0","--target-sdk-version=28","--nice-name=pocket-api28-child",
        "--instruction-set=arm64","dev.kreijstal.aosppocket.guest.ZygoteChild",argv[2]};
    if (direct) {
        arguments.insert(arguments.begin()+7,{"--setgroups=1000,3010","--mount-external-write","--seinfo=platform:targetSdkVersion=28:complete"});
        arguments.push_back("app");
    }
    auto encode=[](const std::vector<std::string>& args) {
        std::string wire=std::to_string(args.size())+"\n";
        for (const auto& arg:args) wire+=arg+"\n";
        return wire;
    };
    const std::string request=encode(arguments);
    auto rejected=arguments;
    rejected.insert(rejected.begin()+7,direct?"--start-child-zygote":"--mount-external-default");
    if (direct) rejected.push_back("--zygote-socket=pocket-unused");
    const std::string unsupported=encode(rejected);
    if (!command(fd, unsupported)) return 20;
    uint32_t deniedPid; unsigned char deniedWrapper;
    if (!transfer(fd, &deniedPid, sizeof(deniedPid), false) ||
        !transfer(fd, &deniedWrapper, sizeof(deniedWrapper), false) ||
        static_cast<int32_t>(ntohl(deniedPid)) != -1 || deniedWrapper || !abi(fd)) return 21;
    puts(direct?"API28_ZYGOTE_UNSUPPORTED_CHILD_ZYGOTE rejected=1 server_alive=1":
                "API28_ZYGOTE_UNSUPPORTED_STORAGE rejected=1 server_alive=1");
    if (!command(fd, request)) return 4;
    uint32_t pid; unsigned char wrapper;
    if (!transfer(fd, &pid, sizeof(pid), false) || !transfer(fd, &wrapper, sizeof(wrapper), false)) return 5;
    pid = ntohl(pid);
    if (static_cast<int32_t>(pid) <= 1 || wrapper) { printf("ZYGOTE_FORK_REJECTED pid=%d wrapper=%d\n", pid, wrapper); return 6; }
    printf("API28_ZYGOTE_FORK parent_pid=%d child_pid=%u peer_kernel_uid=%u uid=%u\n", peer.pid, pid, peer.uid, getuid());
    bool reported = false;
    for (int n = 0; n < 800; ++n) {
        struct stat info{};
        if (!stat("/data/pocket-zygote-child.json", &info) && info.st_size > 20) { reported = true; break; }
        usleep(50000);
    }
    if (!reported || !abi(fd)) return 7;
    close(fd);
    puts("API28_ZYGOTE_FORK_PASS protocol=android9 child_report=present parent_accepts_commands=1");
    return 0;
}
