// SPDX-License-Identifier: GPL-3.0-only
// In-process camera provider 2.4 ("legacy/0") for the 12L guest cameraserver. Each host camera with YUV
// output becomes one device@3.4 backed by 12L's external camera HAL, edited to stream host frames
// (runtime/camera/api32/port_*.py). The host decides which cameras exist; none are invented.
#include <android/hardware/camera/provider/2.4/ICameraProvider.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include "ExternalCameraDevice_3_4.h"
#include "host_camera.h"

using ::android::sp;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::camera::common::V1_0::Status;
using ::android::hardware::camera::common::V1_0::VendorTagSection;
using ::android::hardware::camera::external::common::ExternalCameraConfig;
using ::android::hardware::camera::provider::V2_4::ICameraProvider;
using ::android::hardware::camera::provider::V2_4::ICameraProviderCallback;
using ::android::hardware::camera::device::V3_4::implementation::ExternalCameraDevice;

namespace {
class PocketCameraProvider final : public ICameraProvider {
public:
    PocketCameraProvider() : mCfg(ExternalCameraConfig::loadFromCfg()) {
        int status;
        for (const auto& host : ::pocket::hostCameras(&status)) {
            std::string name = "device@3.4/legacy/" + host.id;
            sp<ExternalCameraDevice> device = new ExternalCameraDevice(name, mCfg, host);
            bool usable = !device->isInitFailed();
            fprintf(stderr, "POCKET_CAMERA_DEVICE name=%s facing=%d orientation=%d sizes=%zu usable=%d\n",
                    name.c_str(), host.facing, host.orientation, host.sizes.size(), usable);
            if (usable) mDevices[name] = device;
        }
        fprintf(stderr, "POCKET_CAMERA_PROVIDER host_status=%d cameras=%zu\n", status, mDevices.size());
    }
    Return<Status> setCallback(const sp<ICameraProviderCallback>&) override {
        // The host camera list is fixed for the provider's lifetime; there are no status changes to report.
        return Status::OK;
    }
    Return<void> getVendorTags(getVendorTags_cb cb) override {
        cb(Status::OK, hidl_vec<VendorTagSection>());
        return Void();
    }
    Return<void> getCameraIdList(getCameraIdList_cb cb) override {
        std::vector<hidl_string> names;
        for (const auto& entry : mDevices) names.push_back(entry.first);
        cb(Status::OK, names);
        return Void();
    }
    Return<void> isSetTorchModeSupported(isSetTorchModeSupported_cb cb) override {
        cb(Status::OK, false);
        return Void();
    }
    Return<void> getCameraDeviceInterface_V1_x(const hidl_string&, getCameraDeviceInterface_V1_x_cb cb) override {
        cb(Status::OPERATION_NOT_SUPPORTED, nullptr);
        return Void();
    }
    Return<void> getCameraDeviceInterface_V3_x(const hidl_string& name, getCameraDeviceInterface_V3_x_cb cb) override {
        auto device = mDevices.find(name);
        if (device == mDevices.end()) cb(Status::ILLEGAL_ARGUMENT, nullptr);
        else cb(Status::OK, device->second->getInterface());
        return Void();
    }
private:
    const ExternalCameraConfig mCfg;
    std::map<std::string, sp<ExternalCameraDevice>> mDevices;
};
}  // namespace

// Loaded by the 12L in-process HIDL service hook (runtime/api32/hidl_services.cpp) inside cameraserver.
extern "C" __attribute__((visibility("default"))) ICameraProvider* HIDL_FETCH_ICameraProvider(const char* name) {
    if (!name || strcmp(name, "legacy/0")) return nullptr;
    return new PocketCameraProvider();
}
