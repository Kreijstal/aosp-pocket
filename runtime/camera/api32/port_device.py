# SPDX-License-Identifier: GPL-3.0-only
"""Pocket edits to the 12L external camera HAL device (device@3.4): host cameras instead of V4L2 nodes."""
import sys
from pathlib import Path

def edit(path, pairs):
    text = path.read_text()
    for old, new in pairs:
        if text.count(old) != 1:
            raise SystemExit(f'{path.name}: expected one match for {old[:70]!r}, found {text.count(old)}')
        text = text.replace(old, new)
    path.write_text(text)

root = Path(sys.argv[1]) / 'camera/device/3.4/default'
edit(root / 'ExternalCameraDevice.cpp', [
('#include <regex>\n', '#include <regex>\n#include <set>\n'),
('''ExternalCameraDevice::ExternalCameraDevice(
        const std::string& devicePath, const ExternalCameraConfig& cfg) :
        mCameraId("-1"),
        mDevicePath(devicePath),
        mCfg(cfg) {
    std::smatch sm;
    if (std::regex_match(mDevicePath, sm, kDevicePathRE)) {
        mCameraId = std::to_string(mCfg.cameraIdOffset + std::stoi(sm[1]));
    } else {
        ALOGE("%s: device path match failed for %s", __FUNCTION__, mDevicePath.c_str());
    }
}''', '''// Pocket: one device per host camera; the camera id is the host's, and no V4L2 node exists.
ExternalCameraDevice::ExternalCameraDevice(
        const std::string& devicePath, const ExternalCameraConfig& cfg,
        const ::pocket::HostCamera& host) :
        mCameraId(host.id),
        mDevicePath(devicePath),
        mCfg(cfg),
        mHost(host) {
}'''),
('''    unique_fd fd(::open(mDevicePath.c_str(), O_RDWR));
    if (fd.get() < 0) {
        int numAttempt = 0;
        do {
            ALOGW("%s: v4l2 device %s open failed, wait 33ms and try again",
                    __FUNCTION__, mDevicePath.c_str());
            usleep(OPEN_RETRY_SLEEP_US); // sleep and try again
            fd.reset(::open(mDevicePath.c_str(), O_RDWR));
            numAttempt++;
        } while (fd.get() < 0 && numAttempt <= MAX_RETRY);

        if (fd.get() < 0) {
            ALOGE("%s: v4l2 device open %s failed: %s",
                    __FUNCTION__, mDevicePath.c_str(), strerror(errno));
            mLock.unlock();
            _hidl_cb(Status::INTERNAL_ERROR, nullptr);
            return Void();
        }
    }

    session = createSession(
            callback, mCfg, mSupportedFormats, mCroppingType,
            mCameraCharacteristics, mCameraId, std::move(fd));''',
'''    // Pocket: the session opens the host stream when it configures one.
    session = createSession(
            callback, mCfg, mSupportedFormats, mCroppingType,
            mCameraCharacteristics, mHost.id, unique_fd());'''),
('''        // init camera characteristics
        unique_fd fd(::open(mDevicePath.c_str(), O_RDWR));
        if (fd.get() < 0) {
            ALOGE("%s: v4l2 device open %s failed", __FUNCTION__, mDevicePath.c_str());
            return DEAD_OBJECT;
        }
''', '''        // init camera characteristics from the host camera description
        const int fd = -1;
'''),
('ret = initCameraControlsCharsKeys(fd.get(), &mCameraCharacteristics);','ret = initCameraControlsCharsKeys(fd, &mCameraCharacteristics);'),
('ret = initOutputCharsKeys(fd.get(), &mCameraCharacteristics);','ret = initOutputCharsKeys(fd, &mCameraCharacteristics);'),
('    const uint8_t facing = ANDROID_LENS_FACING_EXTERNAL;\n',
 '    // Pocket: the host camera\'s facing and orientation; the guest shares the host\'s display.\n    const uint8_t facing = static_cast<uint8_t>(mHost.facing);\n'),
('    const int32_t orientation = mCfg.orientation;\n','    const int32_t orientation = mHost.orientation;\n'),
('''    int fd, ::android::hardware::camera::common::V1_0::helper::CameraMetadata* metadata) {
    initSupportedFormatsLocked(fd);''','''    int, ::android::hardware::camera::common::V1_0::helper::CameraMetadata* metadata) {
    initSupportedFormatsLocked();'''),
('''            case V4L2_PIX_FMT_MJPEG:
                hasColor = true;''','''            case V4L2_PIX_FMT_MJPEG:
            case V4L2_PIX_FMT_YUV420:
                hasColor = true;'''),
('''            case V4L2_PIX_FMT_MJPEG: hasColor = true; break;''',
 '''            // Pocket: host frames are I420; without this the device lacks BACKWARD_COMPATIBLE
            // and cameraserver counts it as no normal camera, so apps cannot open it.
            case V4L2_PIX_FMT_MJPEG:
            case V4L2_PIX_FMT_YUV420: hasColor = true; break;'''),
('''        initOutputCharskeysByFormat(metadata, V4L2_PIX_FMT_MJPEG, halFormats,''',
 '''        initOutputCharskeysByFormat(metadata, V4L2_PIX_FMT_YUV420, halFormats,'''),
('''void ExternalCameraDevice::getFrameRateList(
        int fd, double fpsUpperBound, SupportedV4L2Format* format) {
    format->frameRates.clear();
''', '''// Pocket: frame rates are the host's AE target maxima this size can sustain, up to fpsUpperBound.
void ExternalCameraDevice::getFrameRateList(
        const ::pocket::HostCamera& host, const ::pocket::HostCameraSize& size,
        double fpsUpperBound, SupportedV4L2Format* format) {
    format->frameRates.clear();
    double sizeLimit = size.minFrameDurationNs > 0 ? 1e9 / size.minFrameDurationNs : 1000.0;
    std::set<int> rates;
    for (const auto& range : host.fpsRanges) {
        if (range.second > 0 && range.second <= fpsUpperBound + 0.01 && range.second <= sizeLimit + 0.5) {
            rates.insert(range.second);
        }
    }
    for (int rate : rates) {
        format->frameRates.push_back({/*durationNumerator*/1, /*durationDenominator*/static_cast<uint32_t>(rate)});
    }
}

#if 0  // V4L2 enumeration, replaced by the host description above
static void v4l2FrameRateList(int fd, double fpsUpperBound, SupportedV4L2Format* format) {
'''),
('''                frameInterval.width, frameInterval.height);
    }
}

void ExternalCameraDevice::trimSupportedFormats(''','''                frameInterval.width, frameInterval.height);
    }
}
#endif

void ExternalCameraDevice::trimSupportedFormats('''),
('''std::vector<SupportedV4L2Format> ExternalCameraDevice::getCandidateSupportedFormatsLocked(
    int fd, CroppingType cropType,
    const std::vector<ExternalCameraConfig::FpsLimitation>& fpsLimits,
    const std::vector<ExternalCameraConfig::FpsLimitation>& depthFpsLimits,
    const Size& minStreamSize,
    bool depthEnabled) {
    std::vector<SupportedV4L2Format> outFmts;
''','''std::vector<SupportedV4L2Format> ExternalCameraDevice::getCandidateSupportedFormatsLocked(
    const ::pocket::HostCamera& host, CroppingType cropType,
    const std::vector<ExternalCameraConfig::FpsLimitation>& fpsLimits,
    const std::vector<ExternalCameraConfig::FpsLimitation>& depthFpsLimits,
    const Size& minStreamSize,
    bool depthEnabled) {
    (void)depthFpsLimits; (void)depthEnabled;
    std::vector<SupportedV4L2Format> outFmts;
    // Pocket: the host streams tightly packed I420 at any of its YUV_420_888 sizes.
    for (const auto& size : host.sizes) {
        // Disregard h > w formats so all aspect ratio (h/w) <= 1.0; odd sizes cannot hold I420.
        if (size.height > size.width || (size.width % 2) || (size.height % 2)) continue;
        if (size.width < minStreamSize.width || size.height < minStreamSize.height) continue;
        SupportedV4L2Format format {
            .width = size.width,
            .height = size.height,
            .fourcc = V4L2_PIX_FMT_YUV420
        };
        updateFpsBounds(host, size, cropType, fpsLimits, format, outFmts);
    }
    trimSupportedFormats(cropType, &outFmts);
    return outFmts;
}

#if 0  // V4L2 enumeration, replaced by the host description above
static std::vector<SupportedV4L2Format> v4l2CandidateFormats(int fd) {
    std::vector<SupportedV4L2Format> outFmts;
'''),
('''        fmtdesc.index++;
    }
    trimSupportedFormats(cropType, &outFmts);
    return outFmts;
}

void ExternalCameraDevice::updateFpsBounds(
    int fd, CroppingType cropType,''','''        fmtdesc.index++;
    }
    trimSupportedFormats(cropType, &outFmts);
    return outFmts;
}
#endif

void ExternalCameraDevice::updateFpsBounds(
    const ::pocket::HostCamera& host, const ::pocket::HostCameraSize& size, CroppingType cropType,'''),
('''    getFrameRateList(fd, fpsUpperBound, &format);
    if (!format.frameRates.empty()) {''','''    getFrameRateList(host, size, fpsUpperBound, &format);
    if (!format.frameRates.empty()) {'''),
('''void ExternalCameraDevice::initSupportedFormatsLocked(int fd) {
    std::vector<SupportedV4L2Format> horizontalFmts = getCandidateSupportedFormatsLocked(
        fd, HORIZONTAL, mCfg.fpsLimits, mCfg.depthFpsLimits, mCfg.minStreamSize, mCfg.depthEnabled);
    std::vector<SupportedV4L2Format> verticalFmts = getCandidateSupportedFormatsLocked(
        fd, VERTICAL, mCfg.fpsLimits, mCfg.depthFpsLimits, mCfg.minStreamSize, mCfg.depthEnabled);''',
'''void ExternalCameraDevice::initSupportedFormatsLocked() {
    std::vector<SupportedV4L2Format> horizontalFmts = getCandidateSupportedFormatsLocked(
        mHost, HORIZONTAL, mCfg.fpsLimits, mCfg.depthFpsLimits, mCfg.minStreamSize, mCfg.depthEnabled);
    std::vector<SupportedV4L2Format> verticalFmts = getCandidateSupportedFormatsLocked(
        mHost, VERTICAL, mCfg.fpsLimits, mCfg.depthFpsLimits, mCfg.minStreamSize, mCfg.depthEnabled);'''),
])

edit(root / 'include/ext_device_v3_4_impl/ExternalCameraDevice_3_4.h', [
('#include "ExternalCameraDeviceSession.h"\n', '#include "ExternalCameraDeviceSession.h"\n#include "host_camera.h"\n'),
('    ExternalCameraDevice(const std::string& cameraId, const ExternalCameraConfig& cfg);\n',
 '    // Pocket: host describes the host camera this device streams from.\n'
 '    ExternalCameraDevice(const std::string& cameraId, const ExternalCameraConfig& cfg,\n'
 '                         const ::pocket::HostCamera& host);\n'),
('    void initSupportedFormatsLocked(int fd);\n', '    void initSupportedFormatsLocked();\n'),
('    static void getFrameRateList(int fd, double fpsUpperBound, SupportedV4L2Format* format);\n',
 '    static void getFrameRateList(const ::pocket::HostCamera& host, const ::pocket::HostCameraSize& size,\n'
 '                                 double fpsUpperBound, SupportedV4L2Format* format);\n'),
('    static void updateFpsBounds(int fd, CroppingType cropType,\n',
 '    static void updateFpsBounds(const ::pocket::HostCamera& host, const ::pocket::HostCameraSize& size,\n'
 '            CroppingType cropType,\n'),
('''    static std::vector<SupportedV4L2Format> getCandidateSupportedFormatsLocked(
            int fd, CroppingType cropType,''','''    static std::vector<SupportedV4L2Format> getCandidateSupportedFormatsLocked(
            const ::pocket::HostCamera& host, CroppingType cropType,'''),
('    const ExternalCameraConfig& mCfg;\n    std::vector<SupportedV4L2Format> mSupportedFormats;\n',
 '    const ExternalCameraConfig& mCfg;\n    const ::pocket::HostCamera mHost;\n    std::vector<SupportedV4L2Format> mSupportedFormats;\n'),
])
