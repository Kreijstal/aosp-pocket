// SPDX-License-Identifier: GPL-3.0-only
// Rootless guest property-service bring-up, not the complete AOSP init daemon.
// Uses the real Android 9 bionic property area and property-service wire ABI.
#if POCKET_GUEST_API != 28
#error Review the property protocol and platform ABI for this guest version
#endif
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include "pocket_properties.h"

bool pocketCreatePropertyInfo();

namespace {
constexpr const char* socketPath = "/dev/socket/property_service";
volatile sig_atomic_t stopped;
int serverFd = -1;
uid_t serverOwner;
PropertyChanged changed;
PropertyControl control;
#ifndef POCKET_INIT_INTEGRATION
void stop(int) { stopped = 1; }
#endif

bool receive(int fd, void* data, size_t size) {
    auto* next = static_cast<char*>(data);
    while (size) {
        ssize_t count = recv(fd, next, size, 0);
        if (count <= 0) { if (count < 0 && errno == EINTR && !stopped) continue; return false; }
        size -= count;
        next += count;
    }
    return true;
}

bool readString(int fd, std::string& result, size_t limit) {
    uint32_t length;
    if (!receive(fd, &length, sizeof(length)) || length > limit) return false;
    result.resize(length);
    return (!length || receive(fd, &result[0], length)) && result.find('\0') == std::string::npos;
}

bool legalName(const std::string& name) {
    if (name.empty() || name.front() == '.' || name.back() == '.') return false;
    char previous = 0;
    for (unsigned char ch : name) {
        if (!(isalnum(ch) || ch == '.' || ch == '_' || ch == '-' || ch == '@' || ch == ':')) return false;
        if (previous == '.' && ch == '.') return false;
        previous = ch;
    }
    return true;
}

uint32_t put(const std::string& name, const std::string& value) {
    if (!legalName(name)) return PROP_ERROR_INVALID_NAME;
    if (name.compare(0, 4, "ctl.") == 0)
        return control ? control(name.substr(4), value) : PROP_ERROR_PERMISSION_DENIED;
    if (name == "sys.powerctl" || name == "selinux.restorecon_recursive")
        return PROP_ERROR_PERMISSION_DENIED; // No host init/control forwarding.
    bool readOnly = name.compare(0, 3, "ro.") == 0;
    if (value.size() >= PROP_VALUE_MAX && !readOnly) return PROP_ERROR_INVALID_VALUE;
    const prop_info* info = __system_property_find(name.c_str());
    if (info && readOnly) return PROP_ERROR_READ_ONLY_PROPERTY;
    int result = info ? __system_property_update(const_cast<prop_info*>(info), value.c_str(), value.size()) :
        __system_property_add(name.c_str(), name.size(), value.c_str(), value.size());
    if (result != 0) return PROP_ERROR_SET_FAILED;
    if (changed) changed(name, value);
    return PROP_SUCCESS;
}

bool defaults() {
    FILE* input = fopen("/system/build.prop", "re");
    if (!input) return false;
    char* line = nullptr;
    size_t length = 0;
    bool success = true;
    while (getline(&line, &length, input) > 0) {
        char* start = line;
        while (isspace(*start)) ++start;
        if (*start == '#' || !*start) continue;
        char* equal = strchr(start, '=');
        if (!equal) continue;
        *equal++ = 0;
        char* end = start + strlen(start);
        while (end > start && isspace(end[-1])) *--end = 0;
        while (isspace(*equal)) ++equal;
        end = equal + strlen(equal);
        while (end > equal && isspace(end[-1])) *--end = 0;
        if (put(start, equal) != PROP_SUCCESS) { printf("PROPERTY_DEFAULT_FAILED name=%s\n", start); success = false; }
    }
    free(line);
    fclose(input);
    return success;
}

uid_t kernelUid() {
    FILE* status = fopen("/proc/self/status", "re");
    if (!status) return static_cast<uid_t>(-1);
    char line[256];
    unsigned int uid = -1;
    while (fgets(line, sizeof(line), status)) if (sscanf(line, "Uid: %u", &uid) == 1) break;
    fclose(status);
    return uid;
}

void request(int fd, uid_t owner) {
    timeval timeout{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ucred peer{};
    socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != owner) return;
    uint32_t command;
    if (!receive(fd, &command, sizeof(command))) return;
    std::string name, value;
    uint32_t result = PROP_ERROR_INVALID_CMD;
    if (command == PROP_MSG_SETPROP2) {
        if (!readString(fd, name, 1024) || !readString(fd, value, 65535)) result = PROP_ERROR_READ_DATA;
        else result = put(name, value);
        send(fd, &result, sizeof(result), MSG_NOSIGNAL);
    } else if (command == PROP_MSG_SETPROP) {
        char oldName[32], oldValue[PROP_VALUE_MAX];
        if (!receive(fd, oldName, sizeof(oldName)) || !receive(fd, oldValue, sizeof(oldValue))) return;
        oldName[sizeof(oldName) - 1] = 0;
        oldValue[sizeof(oldValue) - 1] = 0;
        name = oldName;
        value = oldValue;
        result = put(name, value);
        // Legacy clients synchronize by waiting for EOF, without a reply.
    } else send(fd, &result, sizeof(result), MSG_NOSIGNAL);
    printf("PROPERTY_WRITE name=%s result=%u peer_uid=%u\n", name.c_str(), result, peer.uid);
    if (name == "pocket.control.stop" && value == "1" && result == PROP_SUCCESS) stopped = 1;
}
}

int pocketPropertiesStart(PropertyChanged onChange, PropertyControl onControl) {
    stopped = 0;
    changed = std::move(onChange);
    control = std::move(onControl);
    uid_t owner = kernelUid();
    if (getuid() != 0 || owner == static_cast<uid_t>(-1)) return 1;
    if (!pocketCreatePropertyInfo()) return 2;
    chmod("/dev/__properties__/property_info", 0444);
    int initialized = __system_property_area_init();
    // Bionic returns -1 for either mapping failure or failed SELinux xattrs.
    // Validate the area directly; host SELinux still protects app-owned files.
    if (put("pocket.area.probe", "writable") != PROP_SUCCESS) {
        printf("PROPERTY_AREA_FAILED init=%d errno=%d\n", initialized, errno);
        return 2;
    }
    if (!defaults() || put("ro.property_service.version", "2") != PROP_SUCCESS ||
        put("pocket.test.sequence", "seed") != PROP_SUCCESS) return 3;
    char sdk[PROP_VALUE_MAX];
    __system_property_get("ro.build.version.sdk", sdk);
    if (strcmp(sdk, "28")) return 4;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, socketPath);
    // The caller creates/binds a private guest socket directory, not host /dev/socket.
    unlink(socketPath);
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(fd, 8)) {
        printf("PROPERTY_SOCKET_FAILED errno=%d\n", errno);
        return 5;
    }
    chmod(socketPath, 0600);
    printf("API28_PROPERTIES_READY pid=%d uid=%d kernel_uid=%u sdk=%s area_init=%d\n", getpid(), getuid(), owner, sdk, initialized);
    serverFd = fd;
    serverOwner = owner;
    return 0;
}

bool pocketPropertiesPoll(int timeout_ms) {
    pollfd event{serverFd, POLLIN, 0};
    int ready = poll(&event, 1, timeout_ms);
    if (ready < 0) return errno == EINTR;
    if (ready == 0) return true;
    if (!(event.revents & POLLIN)) return false;
    int client = accept4(serverFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (client >= 0) { request(client, serverOwner); close(client); }
    return true;
}

bool pocketPropertiesStopped() { return stopped != 0; }
uint32_t pocketPropertySet(const std::string& name, const std::string& value) { return put(name, value); }

void pocketPropertiesClose() {
    if (serverFd >= 0) close(serverFd);
    serverFd = -1;
    unlink(socketPath);
    puts("API28_PROPERTIES_STOPPED");
}

#ifndef POCKET_INIT_INTEGRATION
int main() {
    setbuf(stdout, nullptr);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGPIPE, SIG_IGN);
    int result = pocketPropertiesStart();
    if (result) return result;
    while (!pocketPropertiesStopped()) {
        if (!pocketPropertiesPoll(200)) { pocketPropertiesClose(); return 6; }
    }
    pocketPropertiesClose();
    return 0;
}
#endif
