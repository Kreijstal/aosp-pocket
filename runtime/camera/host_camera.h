// SPDX-License-Identifier: GPL-3.0-only
// Guest side of the host broker's camera operations (dev.kreijstal.aosppocket.bridge.HostWire).
#pragma once
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace pocket {

struct HostCameraSize {
    uint32_t width;
    uint32_t height;
    int64_t minFrameDurationNs; // host YUV_420_888 minimum frame duration, 0 if unknown
};

struct HostCamera {
    std::string id;
    int facing;      // android.lens.facing: 0 front, 1 back, 2 external
    int orientation; // android.sensor.orientation in degrees
    std::vector<HostCameraSize> sizes;
    std::vector<std::pair<int, int>> fpsRanges; // AE target fps ranges
};

// Host cameras with YUV output. *status is 0, a host status (403 without camera permission)
// or -errno for transport failures.
std::vector<HostCamera> hostCameras(int* status);

// Starts a host I420 frame stream. Returns the socket, or a negative status.
int hostCameraOpen(const std::string& id, uint32_t width, uint32_t height, int fps);

// Reads the next frame into data, which must already hold width*height*3/2 bytes.
// Returns 0, or a negative errno when the stream ended or is malformed.
int hostCameraRead(int fd, std::vector<uint8_t>& data, int64_t* timestampNs);

}  // namespace pocket
