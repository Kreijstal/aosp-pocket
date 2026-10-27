// SPDX-License-Identifier: GPL-3.0-only
#include "host_camera.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <sstream>

namespace pocket {
namespace {
// Must match dev.kreijstal.aosppocket.bridge.HostWire.
constexpr uint32_t kMagic = 0x504b4831, kCameras = 7, kCameraStream = 15;
constexpr uint32_t kMaxReply = 1 << 20;

bool sendAll(int fd, const void* data, size_t size) {
    auto* bytes = static_cast<const uint8_t*>(data);
    while (size) {
        ssize_t n = send(fd, bytes, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += n; size -= n;
    }
    return true;
}
bool readAll(int fd, void* data, size_t size) {
    auto* bytes = static_cast<uint8_t*>(data);
    while (size) {
        ssize_t n = recv(fd, bytes, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += n; size -= n;
    }
    return true;
}
void setTimeout(int fd, int seconds) {
    timeval timeout{seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
// Sends one request and reads the reply header and body. Returns the socket with *status set,
// or -1 with *status set to the host status or -errno.
int request(uint32_t op, const std::string& payload, std::string* body, int* status) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { *status = -errno; return -1; }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, "/dev/socket/pocket_host", sizeof(address.sun_path) - 1);
    // Opening a host camera can take a few seconds.
    setTimeout(fd, 10);
    uint32_t header[3] = {htonl(kMagic), htonl(op), htonl(static_cast<uint32_t>(payload.size()))};
    uint32_t reply[2];
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        *status = -errno; close(fd); return -1;
    }
    if (!sendAll(fd, header, sizeof(header)) || !sendAll(fd, payload.data(), payload.size()) ||
        !readAll(fd, reply, sizeof(reply))) {
        *status = -EIO; close(fd); return -1;
    }
    uint32_t length = ntohl(reply[1]);
    if (length > kMaxReply) { *status = -EPROTO; close(fd); return -1; }
    body->resize(length);
    if (length && !readAll(fd, &(*body)[0], length)) { *status = -EIO; close(fd); return -1; }
    *status = static_cast<int>(ntohl(reply[0]));
    if (*status != 0) { close(fd); return -1; }
    return fd;
}
}  // namespace

std::vector<HostCamera> hostCameras(int* status) {
    std::vector<HostCamera> cameras;
    std::string body;
    int fd = request(kCameras, "{\"format\":\"lines\"}", &body, status);
    if (fd < 0) return cameras;
    close(fd);
    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line)) {
        char id[33];
        int facing, orientation;
        unsigned width, height;
        long long duration;
        int low, high;
        if (sscanf(line.c_str(), "camera %32s %d %d", id, &facing, &orientation) == 3) {
            cameras.push_back({id, facing, orientation, {}, {}});
        } else if (!cameras.empty() &&
                   sscanf(line.c_str(), "size %u %u %lld", &width, &height, &duration) == 3) {
            cameras.back().sizes.push_back({width, height, duration});
        } else if (!cameras.empty() && sscanf(line.c_str(), "fps %d %d", &low, &high) == 2) {
            cameras.back().fpsRanges.emplace_back(low, high);
        }
    }
    return cameras;
}

int hostCameraOpen(const std::string& id, uint32_t width, uint32_t height, int fps) {
    char payload[160];
    snprintf(payload, sizeof(payload), "{\"id\":\"%s\",\"width\":%u,\"height\":%u,\"fps\":%d}",
             id.c_str(), width, height, fps);
    std::string body;
    int status;
    int fd = request(kCameraStream, payload, &body, &status);
    if (fd < 0) {
        fprintf(stderr, "POCKET_CAMERA_HOST_UNAVAILABLE id=%s status=%d\n", id.c_str(), status);
        return status < 0 ? status : -status;
    }
    // Frames arrive at the stream rate; a stalled host must not hang the capture pipeline.
    setTimeout(fd, 3);
    fprintf(stderr, "POCKET_CAMERA_HOST_STREAM id=%s size=%ux%u fps=%d open=1\n", id.c_str(), width, height, fps);
    return fd;
}

int hostCameraRead(int fd, std::vector<uint8_t>& data, int64_t* timestampNs) {
    uint8_t header[12];
    if (!readAll(fd, header, sizeof(header))) return -EIO;
    uint32_t length = (uint32_t)header[0] << 24 | header[1] << 16 | header[2] << 8 | header[3];
    uint64_t stamp = 0;
    for (int i = 4; i < 12; i++) stamp = stamp << 8 | header[i];
    if (length != data.size()) return -EPROTO;
    if (!readAll(fd, data.data(), length)) return -EIO;
    *timestampNs = static_cast<int64_t>(stamp);
    return 0;
}

}  // namespace pocket
