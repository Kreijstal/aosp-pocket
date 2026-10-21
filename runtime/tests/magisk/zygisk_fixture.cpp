// SPDX-License-Identifier: GPL-3.0-only
#include <sys/types.h>
#include "api.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

static void event(const char* name, int target = -1) {
    int fd = open("/data/local/tmp/pocket-zygisk-events", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
        dprintf(fd, "%s uid=%u target=%d pid=%d\n", name, getuid(), target, getpid());
        close(fd);
    }
}
class PocketFixture : public zygisk::ModuleBase {
    zygisk::Api* api = nullptr;
    JNIEnv* env = nullptr;
    static inline jboolean (*originalLoggable)(JNIEnv*,jclass,jstring,jint) = nullptr;
    static jboolean loggable(JNIEnv* env,jclass type,jstring tag,jint priority) {
        event("jniHook");
        return originalLoggable(env,type,tag,priority);
    }
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api=api; this->env=env; event("onLoad");
    }
    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        event("preApp", args->uid);
        JNINativeMethod methods[]={{const_cast<char*>("isLoggable"),const_cast<char*>("(Ljava/lang/String;I)Z"),reinterpret_cast<void*>(loggable)}};
        api->hookJniNativeMethods(env,"android/util/Log",methods,1);
        originalLoggable=reinterpret_cast<decltype(originalLoggable)>(methods[0].fnPtr);
        if(originalLoggable) {
            jclass type=env->FindClass("android/util/Log");
            jmethodID method=env->GetStaticMethodID(type,"isLoggable","(Ljava/lang/String;I)Z");
            jstring tag=env->NewStringUTF("PocketZygiskTest");
            env->CallStaticBooleanMethod(type,method,tag,4);
            env->DeleteLocalRef(tag);env->DeleteLocalRef(type);
        }
    }
    void postAppSpecialize(const zygisk::AppSpecializeArgs*) override { event("postApp"); }
    void preServerSpecialize(zygisk::ServerSpecializeArgs*) override { event("preServer"); }
    void postServerSpecialize(const zygisk::ServerSpecializeArgs*) override { event("postServer"); }
};
REGISTER_ZYGISK_MODULE(PocketFixture)
