// SPDX-License-Identifier: GPL-3.0-only
// Real service workload and independent bionic client for the AOSP init test.
#include <sys/system_properties.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>

namespace {
std::string get(const char* name) {
    char value[PROP_VALUE_MAX]; __system_property_get(name, value); return value;
}
bool set(const char* name, const std::string& value) {
    return __system_property_set(name, value.c_str()) == 0;
}
bool await(const char* name, const char* value) {
    for (int n = 0; n < 400; ++n) { if (get(name) == value) return true; usleep(50000); }
    printf("INIT_TEST_TIMEOUT property=%s expected=%s actual=%s\n", name, value, get(name).c_str());
    return false;
}
}
int main(int argc, char** argv) {
    setbuf(stdout, nullptr);
    if (argc < 2 || get("ro.build.version.sdk") != "28") return 64;
    if (!strcmp(argv[1], "once")) {
        const char* fdText = getenv("ANDROID_SOCKET_pocket_init_test");
        if (getuid() != 39999 || getgid() != 39999 || argc != 3 || strcmp(argv[2], "boot-ok") ||
            !getenv("POCKET_INIT_ENV") || strcmp(getenv("POCKET_INIT_ENV"), "from-rc") || !fdText) return 1;
        uid_t real, effective, saved;
        gid_t realGroup, effectiveGroup, savedGroup;
        if (getresuid(&real, &effective, &saved) || getresgid(&realGroup, &effectiveGroup, &savedGroup) ||
            real != 39999 || effective != 39999 || saved != 39999 ||
            realGroup != 39999 || effectiveGroup != 39999 || savedGroup != 39999) return 18;
        if (setuid(0) == 0 || errno != EPERM || setgid(0) == 0 || errno != EPERM) return 19;
        sockaddr_un address{}; socklen_t size = sizeof(address);
        if (getsockname(atoi(fdText), reinterpret_cast<sockaddr*>(&address), &size) ||
            address.sun_family != AF_UNIX || strcmp(address.sun_path, "/dev/socket/pocket_init_test")) return 2;
        printf("INIT_ONESHOT_PASS pid=%d uid=%d env=from-rc expanded=boot-ok inherited_socket=1 ids=39999/39999/39999 regain_root=denied\n", getpid(), getuid());
        return set("pocket.test.oneshot", "passed") ? 0 : 3;
    }
    if (!strcmp(argv[1], "daemon")) {
        int generation = atoi(get("pocket.test.generation").c_str()) + 1;
        if (getuid() != 39999 || !set("pocket.test.generation", std::to_string(generation))) return 4;
        printf("INIT_DAEMON generation=%d pid=%d uid=%d\n", generation, getpid(), getuid());
        if (generation == 1) return 42; // Real non-oneshot exit -> Reap/onrestart/backoff.
        pid_t child = fork();
        if (child < 0) return 5;
        if (child == 0) { for (;;) pause(); }
        if (!set("pocket.test.grandchild", std::to_string(child))) return 6;
        for (;;) pause();
    }
    if (!strcmp(argv[1], "hold")) {
        if (!set("pocket.test.hold", "running")) return 7;
        for (;;) pause();
    }
    if (!strcmp(argv[1], "stopped")) {
        for (const char* name : {"init.svc.pocket-once", "init.svc.pocket-daemon", "init.svc.pocket-hold"})
            if (get(name) != "stopped") return 14;
        if (access("/dev/socket/pocket_init_test", F_OK) == 0 || access("/dev/socket/property_service", F_OK) == 0) return 15;
        pid_t child = atoi(get("pocket.test.grandchild").c_str());
        if (child <= 1) return 16;
        for (int n = 0; n < 100; ++n) {
            if (kill(child, 0) == -1 && errno == ESRCH) {
                puts("API28_INIT_SHUTDOWN_PASS states=stopped sockets=removed grandchild=gone");
                return 0;
            }
            usleep(50000);
        }
        printf("INIT_GRANDCHILD_REMAINS pid=%d errno=%d\n", child, errno);
        return 17;
    }
    if (!strcmp(argv[1], "client")) {
        if (getuid() != 39999 || !await("pocket.test.import", "loaded") ||
            !await("pocket.test.trigger", "boot-ok") || !await("pocket.test.oneshot", "passed") ||
            !await("init.svc.pocket-once", "stopped") || !await("pocket.test.onrestart", "yes") ||
            !await("pocket.test.generation", "2") || !await("init.svc.pocket-daemon", "running")) return 8;
        if (!get("pocket.test.hold").empty()) return 9; // class_start honors disabled.
        if (!set("ctl.start", "pocket-hold") || !await("pocket.test.hold", "running") ||
            !set("ctl.stop", "pocket-hold") || !await("init.svc.pocket-hold", "stopped")) return 10;
        if (!set("ctl.restart", "pocket-daemon") || !await("pocket.test.generation", "3") ||
            !await("init.svc.pocket-daemon", "running")) return 11;
        if (set("ctl.start", "zygote") || set("ctl.interface_start", "anything") ||
            set("sys.powerctl", "reboot")) return 12;
        if (get("ro.boottime.pocket-daemon").empty()) return 13;
        puts("API28_INIT_CLIENT_PASS uid=39999 actions=1 imported=1 property_trigger=1 oneshot=1 socket=1 crash_restart=1 disabled=1 ctl_start_stop_restart=1");
        return 0;
    }
    return 64;
}
