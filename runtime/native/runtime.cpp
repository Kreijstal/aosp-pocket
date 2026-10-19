// SPDX-License-Identifier: GPL-3.0-only
// JNI adapter to Ananbox's GPL Anbox renderer and input protocol.
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <fcntl.h>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include "anbox/graphics/emugl/Renderer.h"
#include "anbox/graphics/emugl/RenderControl.h"
#include "anbox/graphics/emugl/DisplayManager.h"
#include "anbox/graphics/layer_composer.h"
#include "anbox/input/manager.h"
#include "anbox/input/device.h"
#include "anbox/network/published_socket_connector.h"
#include "anbox/qemu/pipe_connection_creator.h"
#include "anbox/runtime.h"

namespace {
std::shared_ptr<anbox::Runtime> runtime;
std::shared_ptr<Renderer> renderer;
std::shared_ptr<anbox::graphics::Rect> frame;
std::shared_ptr<anbox::input::Manager> inputs;
std::shared_ptr<anbox::input::Device> touch;
std::shared_ptr<anbox::network::PublishedSocketConnector> pipe_connector;
std::array<int, 10> fingers;

// One guest compositor, several host windows. Keep the composer registered
// while no Activity is visible, so guest lifetime is independent of its UI.
class Displays final : public anbox::graphics::Renderer {
    std::mutex mutex_;
    std::map<jlong, ANativeWindow*> windows_;
    RenderableList last_;
public:
    bool draw(EGLNativeWindowType, const anbox::graphics::Rect& rectangle,
              const RenderableList& layers) override {
        std::lock_guard<std::mutex> lock(mutex_);
        last_ = layers;
        for (auto& item : windows_) renderer->draw(item.second, rectangle, layers);
        return true;
    }
    void attach(jlong id, ANativeWindow* window) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto previous = windows_.find(id);
        if (previous != windows_.end() && previous->second == window) {
            ANativeWindow_release(window);
            return;
        }
        if (previous != windows_.end()) remove(previous);
        if (!renderer->createNativeWindow(window)) {
            ANativeWindow_release(window);
            throw std::runtime_error("Host EGL display surface creation failed");
        }
        windows_.emplace(id, window);
        // Show the current guest immediately, even if its UI is idle.
        if (!last_.empty()) renderer->draw(window, *frame, last_);
    }
    void detach(jlong id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto item = windows_.find(id);
        if (item != windows_.end()) remove(item);
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        last_.clear();
        for (auto& item : windows_) renderer->draw(item.second, *frame, last_);
    }
private:
    void remove(std::map<jlong, ANativeWindow*>::iterator item) {
        renderer->destroyNativeWindow(item->second);
        ANativeWindow_release(item->second);
        windows_.erase(item);
    }
};
std::shared_ptr<Displays> displays;

std::string string(JNIEnv* env, jstring value) {
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) throw std::runtime_error("Missing JNI string");
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}
void fail(JNIEnv* env, const char* message) {
    env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), message);
}
void logger(const emugl::LogLevel& level, const char* format, ...) {
    if (level == emugl::LogLevel::TRACE) return;
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_INFO, "PocketRuntime", format, args);
    va_end(args);
}
}

extern "C" JNIEXPORT void JNICALL
Java_dev_kreijstal_aosppocket_runtime_Native_configureHost(JNIEnv* env, jclass) {
    // A captured nested-host stack stopped in SwiftShader CmdWaitEvent.
    // ANGLE supports a process-local fallback to ordinary pipeline barriers.
    // Set it before HWUI caches the ANGLE environment, and only for Pastel.
    static bool configured = false;
    if (configured) return;
    configured = true;
    char driver[PROP_VALUE_MAX] = {};
    __system_property_get("ro.hardware.vulkan", driver);
    if (std::strcmp(driver, "pastel") != 0) return;
    const char* existing = std::getenv("ANGLE_FEATURE_OVERRIDES_DISABLED");
    std::string disabled = existing && *existing ? std::string(existing) + ":" : "";
    disabled += "useVkEventForImageBarrier:useVkEventForBufferBarrier";
    if (setenv("ANGLE_FEATURE_OVERRIDES_DISABLED", disabled.c_str(), 1) != 0) {
        fail(env, "Cannot configure software renderer barriers");
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "PocketRuntime", "Pastel: disabled ANGLE VkEvent barriers in this app process");
}

extern "C" JNIEXPORT void JNICALL
Java_dev_kreijstal_aosppocket_runtime_Native_initialize(JNIEnv* env, jclass,
        jstring directory, jint width, jint height, jint dpi) {
    try {
        if (!runtime) {
            const auto path = string(env, directory);
            set_emugl_logger(logger);
            set_emugl_cxt_logger(logger);
            runtime = anbox::Runtime::create();
            renderer = std::make_shared<Renderer>();
            frame = std::make_shared<anbox::graphics::Rect>();
            frame->resize(width, height);
            auto display = anbox::graphics::emugl::DisplayInfo::get();
            display->set_resolution(width, height);
            display->set_dpi(dpi);
            if (!renderer->initialize(EGL_DEFAULT_DISPLAY))
                throw std::runtime_error("Host EGL renderer initialization failed");
            registerRenderer(renderer);
            displays = std::make_shared<Displays>();
            registerLayerComposer(std::make_shared<anbox::graphics::LayerComposer>(displays, frame, EGLNativeWindowType{}));
            inputs = std::make_shared<anbox::input::Manager>(runtime, path + "/rootfs/dev/input");
            touch = inputs->create_device();
            touch->set_name("anbox-touch");
            touch->set_driver_version(1);
            touch->set_input_id({BUS_VIRTUAL, 4, 4, 4});
            touch->set_physical_location("none");
            for (const auto item : {std::pair<int,int>{ABS_MT_SLOT, 9},
                    {ABS_MT_TOUCH_MAJOR, 127}, {ABS_MT_TOUCH_MINOR, 127},
                    {ABS_MT_POSITION_X, width - 1}, {ABS_MT_POSITION_Y, height - 1},
                    {ABS_MT_TRACKING_ID, 65535}}) {
                touch->set_abs_bit(item.first);
                touch->set_abs_max(item.first, item.second);
            }
            touch->set_prop_bit(INPUT_PROP_DIRECT);
            fingers.fill(-1);
            const auto socket = path + "/qemu_pipe";
            unlink(socket.c_str());
            pipe_connector = std::make_shared<anbox::network::PublishedSocketConnector>(socket, runtime,
                std::make_shared<anbox::qemu::PipeConnectionCreator>(renderer, runtime,
                    std::make_shared<anbox::application::SensorsState>(),
                    std::make_shared<anbox::application::GpsInfoBroker>()));
            runtime->start();
        }
        fingers.fill(-1);
        displays->clear();
    } catch (const std::exception& error) { fail(env, error.what()); }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_kreijstal_aosppocket_runtime_Native_attach(JNIEnv* env, jclass, jlong id, jobject surface) {
    try {
        if (!displays) throw std::runtime_error("Guest renderer is not initialized");
        auto window = ANativeWindow_fromSurface(env, surface);
        if (!window) throw std::runtime_error("Invalid Android Surface");
        displays->attach(id, window);
    } catch (const std::exception& error) { fail(env, error.what()); }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_kreijstal_aosppocket_runtime_Native_detach(JNIEnv*, jclass, jlong id) {
    if (displays) displays->detach(id);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_kreijstal_aosppocket_runtime_Native_touch(JNIEnv*, jclass, jint action,
        jint id, jint x, jint y) {
    if (!touch || id < 0) return;
    int slot = -1;
    for (int i = 0; i < 10; ++i) if (fingers[i] == id) slot = i;
    if (action == 0 && slot == -1) {
        for (int i = 0; i < 10; ++i) if (fingers[i] == -1) { slot = i; break; }
        if (slot >= 0) fingers[slot] = id;
    }
    if (slot < 0) return;
    std::vector<anbox::input::Event> events{{EV_ABS, ABS_MT_SLOT, slot}};
    if (action == 1) {
        events.push_back({EV_ABS, ABS_MT_TRACKING_ID, -1});
        fingers[slot] = -1;
    } else {
        if (action == 0) events.push_back({EV_ABS, ABS_MT_TRACKING_ID, id + 1});
        events.push_back({EV_ABS, ABS_MT_POSITION_X, x});
        events.push_back({EV_ABS, ABS_MT_POSITION_Y, y});
    }
    events.push_back({EV_SYN, SYN_REPORT, 0});
    touch->send_events(events);
}

// Native Parcel prefix used by this experimental upstream guest protocol.
// Android's private layout is not a stable API: validate against Java sizes
// and reject a mismatch, rather than write corrupt guest templates.
struct ParcelPrefix {
    int32_t error;
    uint8_t* data;
    size_t size, capacity, position;
    uint64_t* objects;
    size_t object_count;
};
extern "C" JNIEXPORT void JNICALL
Java_dev_kreijstal_aosppocket_runtime_Native_dumpParcel(JNIEnv* env, jclass,
        jobject parcel, jstring destination, jint java_size) {
    try {
        auto field = env->GetFieldID(env->GetObjectClass(parcel), "mNativePtr", "J");
        if (!field) return;
        auto* p = reinterpret_cast<ParcelPrefix*>(env->GetLongField(parcel, field));
        if (!p || p->size != static_cast<size_t>(java_size) || p->size > 65536 ||
                p->capacity < p->size || p->object_count != 1 || !p->data || !p->objects ||
                p->objects[0] + 24 > p->size)
            throw std::runtime_error("Unsupported host Parcel layout");
        const auto path = string(env, destination);
        FILE* output = fopen(path.c_str(), "wb");
        if (!output) throw std::runtime_error("Cannot create guest Binder template");
        bool ok = fwrite(&p->size, 8, 1, output) == 1 &&
            fwrite(&p->object_count, 8, 1, output) == 1 &&
            fwrite(p->data, 1, p->size, output) == p->size &&
            fwrite(p->objects, 8, p->object_count, output) == p->object_count;
        if (fclose(output) != 0 || !ok) throw std::runtime_error("Guest Binder template write failed");
    } catch (const std::exception& error) { fail(env, error.what()); }
}
