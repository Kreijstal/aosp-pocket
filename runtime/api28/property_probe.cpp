// SPDX-License-Identifier: GPL-3.0-only
#include <sys/system_properties.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    setbuf(stdout, nullptr);
    if (argc != 2) return 64;
    char value[PROP_VALUE_MAX];
    __system_property_get("ro.build.version.sdk", value);
    if (strcmp(value, "28")) { printf("WRONG_PROPERTY_AREA sdk=%s\n", value); return 1; }
    if (!strcmp(argv[1], "check") || !strcmp(argv[1], "fresh")) {
        if (__system_property_set("ro.build.version.sdk", "29") == 0) return 2;
        if (__system_property_set("ctl.start", "zygote") == 0) return 3;
        if (__system_property_set("pocket..invalid", "no") == 0) return 4;
        __system_property_get("ro.build.version.sdk", value);
        if (strcmp(value, "28")) return 5;
        if (!strcmp(argv[1], "fresh")) {
            __system_property_get("pocket.test.sequence", value);
            if (strcmp(value, "seed")) return 11;
        }
        printf("API28_PROPERTIES_CHECK sdk=28 uid=%u readonly=passed control=denied invalid_name=denied\n", getuid());
        return 0;
    }
    if (!strcmp(argv[1], "wait")) {
        const prop_info* info = __system_property_find("pocket.test.sequence");
        if (!info) return 6;
        uint32_t serial = 0, updated = 0;
        __system_property_read_callback(info, [](void* cookie, const char*, const char*, uint32_t value) {
            *static_cast<uint32_t*>(cookie) = value;
        }, &serial);
        puts("API28_PROPERTY_WAIT_READY");
        timespec timeout{10, 0};
        if (!__system_property_wait(info, serial, &updated, &timeout) || updated == serial) return 7;
        __system_property_get("pocket.test.sequence", value);
        if (strcmp(value, "updated")) return 8;
        printf("API28_PROPERTY_WAIT value=%s serial_changed=1 uid=%u\n", value, getuid());
        return 0;
    }
    if (!strcmp(argv[1], "write")) return __system_property_set("pocket.test.sequence", "updated") == 0 ? 0 : 9;
    if (!strcmp(argv[1], "stop")) return __system_property_set("pocket.control.stop", "1") == 0 ? 0 : 10;
    return 64;
}
