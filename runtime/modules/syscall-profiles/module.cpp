// SPDX-License-Identifier: GPL-3.0-only
// Optional user-installed Zygisk module; no Pocket startup hook required.
#include <sys/types.h>
#include "sdk/zygisk.hpp"
#include "syscall_profile.h"
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int line(FILE *file, char *text, size_t size) {
    if (!fgets(text, size, file)) return -1;
    size_t length = strlen(text);
    if (!length || text[length-1] != '\n') return -1;
    text[length-1] = 0;
    return 0;
}

static int activate(JNIEnv *env, jstring process, int uid) {
    if (uid < 10000 || !process) return 0;
    const char *name = env->GetStringUTFChars(process, nullptr);
    if (!name) return -1;
    size_t length = strlen(name);
    bool valid = length && length <= 200;
    for (size_t i = 0; valid && i < length; i++) {
        char c = name[i];
        valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':';
    }
    char filename[PATH_MAX];
    int count = valid ? snprintf(filename, sizeof(filename),
        "/data/adb/pocket-syscall-profiles/%s.profile", name) : -1;
    env->ReleaseStringUTFChars(process, name);
    if (count < 0 || count >= (int)sizeof(filename)) return 0;
    FILE *file = fopen(filename, "re");
    if (!file) {
        if (errno == ENOENT) return 0; // Default remains the existing backend.
        fprintf(stderr, "POCKET_SYSCALL_MODULE_ERROR config=%s errno=%d\n", filename, errno);
        return -1;
    }
    char version[64], guest[PATH_MAX], backing[PATH_MAX], mapped[PATH_MAX];
    int bad = line(file, version, sizeof(version)) || strcmp(version, "openat-prefix-v1") ||
              line(file, guest, sizeof(guest)) || line(file, backing, sizeof(backing)) ||
              fgetc(file) != EOF || ferror(file);
    fclose(file);
    if (bad || guest[0] != '/' || backing[0] != '/') {
        fprintf(stderr, "POCKET_SYSCALL_MODULE_ERROR config=%s invalid-format\n", filename);
        return -1;
    }
    using Map = int(*)(const char *, char *);
    auto map = reinterpret_cast<Map>(dlsym(RTLD_DEFAULT, "pocket_boot_map_path"));
    const char *root = getenv("POCKET_DIRECT_ROOT");
    size_t root_size = root ? strlen(root) : 0;
    if (!map || !root_size || map(backing, mapped) ||
        strncmp(mapped, root, root_size) || mapped[root_size] != '/') {
        fprintf(stderr, "POCKET_SYSCALL_MODULE_ERROR config=%s backing-outside-image\n", filename);
        return -1;
    }
    if (pocket_syscall_profile_install(POCKET_SYSCALL_OPENAT_PREFIX_CHAINED, guest, mapped)) {
        fprintf(stderr, "POCKET_SYSCALL_MODULE_ERROR config=%s install errno=%d\n", filename, errno);
        return -1;
    }
    // The selected module stays loaded: the handler and gate must survive.
    fprintf(stderr, "POCKET_SYSCALL_MODULE_ACTIVE config=%s guest=%s uid=%d\n", filename, guest, uid);
    return 1;
}

class PocketSyscallModule : public zygisk::ModuleBase {
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    void onLoad(zygisk::Api *api, JNIEnv *env) override {this->api=api;this->env=env;}
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        int result=activate(env,args->nice_name,args->uid);
        if(result<0)_exit(125);
        if(result==0)api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }
};
REGISTER_ZYGISK_MODULE(PocketSyscallModule)
