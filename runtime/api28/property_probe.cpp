// SPDX-License-Identifier: GPL-3.0-only
#include <sys/system_properties.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#if POCKET_GUEST_API >= 29
#include <string>
static std::string property(const char* name) {
    std::string value;
    const prop_info* info=__system_property_find(name);
    if(info)__system_property_read_callback(info,[](void* cookie,const char*,const char* text,uint32_t){
        *static_cast<std::string*>(cookie)=text;
    },&value);
    return value;
}
#endif
#ifndef POCKET_GUEST_API
#define POCKET_GUEST_API 28
#endif
#define POCKET_PROPERTY_STRING_INNER(value) #value
#define POCKET_PROPERTY_STRING(value) POCKET_PROPERTY_STRING_INNER(value)

int main(int argc, char** argv) {
    setbuf(stdout, nullptr);
    if (argc != 2) return 64;
    char value[PROP_VALUE_MAX];
    __system_property_get("ro.build.version.sdk", value);
    if (strcmp(value, POCKET_PROPERTY_STRING(POCKET_GUEST_API))) { printf("WRONG_PROPERTY_AREA sdk=%s\n", value); return 1; }
    if (!strcmp(argv[1], "check") || !strcmp(argv[1], "fresh")) {
#if POCKET_GUEST_API >= 29
        std::string fingerprint,brand=property("ro.product.brand");
        const char* keys[]={"ro.product.brand","ro.product.name","ro.product.device","ro.build.version.release",
            "ro.build.id","ro.build.version.incremental","ro.build.type","ro.build.tags"};
        const char separators[]={'/','/',':','/','/',':','/'};
        for(size_t i=0;i<8;i++){if(i)fingerprint+=separators[i-1];fingerprint+=property(keys[i]);}
        if(brand.empty() || fingerprint!=property("ro.build.fingerprint") ||
            __system_property_set("ro.build.fingerprint","tamper")==0)return 12;
        printf("API%d_PRODUCT_PROPERTIES_PASS brand=%s fingerprint=%s\n",POCKET_GUEST_API,brand.c_str(),fingerprint.c_str());
#endif
#if POCKET_GUEST_API == 32
        if (property("ro.product.cpu.abilist")!="arm64-v8a" ||
            property("ro.product.cpu.abilist64")!="arm64-v8a" || !property("ro.product.cpu.abilist32").empty() ||
            __system_property_set("ro.product.cpu.abilist","tamper")==0) return 13;
        puts("API32_CPU_ABI_PROPERTIES_PASS arm64-v8a readonly=1");
#endif
        if (__system_property_set("ro.build.version.sdk", "999") == 0) return 2;
        if (__system_property_set("ctl.start", "zygote") == 0) return 3;
        if (__system_property_set("pocket..invalid", "no") == 0) return 4;
        __system_property_get("ro.build.version.sdk", value);
        if (strcmp(value, POCKET_PROPERTY_STRING(POCKET_GUEST_API))) return 5;
        if (!strcmp(argv[1], "fresh")) {
            __system_property_get("pocket.test.sequence", value);
            if (strcmp(value, "seed")) return 11;
        }
        printf("API%d_PROPERTIES_CHECK sdk=%d uid=%u readonly=passed control=denied invalid_name=denied\n", POCKET_GUEST_API, POCKET_GUEST_API, getuid());
        return 0;
    }
    if (!strcmp(argv[1], "wait")) {
        const prop_info* info = __system_property_find("pocket.test.sequence");
        if (!info) return 6;
        uint32_t serial = 0, updated = 0;
        __system_property_read_callback(info, [](void* cookie, const char*, const char*, uint32_t value) {
            *static_cast<uint32_t*>(cookie) = value;
        }, &serial);
        puts("API" POCKET_PROPERTY_STRING(POCKET_GUEST_API) "_PROPERTY_WAIT_READY");
        timespec timeout{10, 0};
        if (!__system_property_wait(info, serial, &updated, &timeout) || updated == serial) return 7;
        __system_property_get("pocket.test.sequence", value);
        if (strcmp(value, "updated")) return 8;
        printf("API%d_PROPERTY_WAIT value=%s serial_changed=1 uid=%u\n", POCKET_GUEST_API, value, getuid());
        return 0;
    }
    if (!strcmp(argv[1], "write")) return __system_property_set("pocket.test.sequence", "updated") == 0 ? 0 : 9;
    if (!strcmp(argv[1], "stop")) return __system_property_set("pocket.control.stop", "1") == 0 ? 0 : 10;
    return 64;
}
