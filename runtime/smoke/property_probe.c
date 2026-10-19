// SPDX-License-Identifier: GPL-3.0-only
// Regression: a newly exec'd non-root guest must map root-owned properties.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc == 1) {
        if (setresgid(39999, 39999, 39999) || setresuid(39999, 39999, 39999)) {
            perror("guest identity change");
            return 2;
        }
        char *child[] = {argv[0], "--child", NULL};
        execv(child[0], child);
        perror("guest re-exec");
        return 3;
    }
    int fd = open("/dev/__properties__/properties_serial", O_RDONLY);
    struct stat info;
    if (fd < 0 || fstat(fd, &info)) { perror("guest property fstat"); return 4; }
    close(fd);
    char api[PROP_VALUE_MAX] = {0};
    int length = __system_property_get("ro.ananbox.host.api_level", api);
    printf("{\"guest_uid\":%u,\"property_uid\":%u,\"property_gid\":%u,\"host_api\":\"%s\"}\n",
        (unsigned)getuid(), (unsigned)info.st_uid, (unsigned)info.st_gid, api);
    return getuid() == 39999 && info.st_uid == 0 && info.st_gid == 0 && length > 0 ? 0 : 5;
}
