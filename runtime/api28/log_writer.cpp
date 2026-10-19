// SPDX-License-Identifier: GPL-3.0-only
// Diagnostic receiver for Android 9 liblog's real logdw datagram protocol.
// This is not logd: it has no log reader/control API, retention or audit support.
#include <private/android_logger.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main() {
    setbuf(stdout, nullptr);
    const char* text = getenv("ANDROID_SOCKET_logdw");
    if (!text || !*text) return 64;
    char* end; long value = strtol(text, &end, 10);
    if (*end || value < 0 || value > 1048576) return 64;
    int fd = static_cast<int>(value), on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on))) return 1;
    puts("API28_LOG_WRITER_READY protocol=android9-logdw");
    for (;;) {
        char packet[16384], control[CMSG_SPACE(sizeof(ucred))];
        iovec iov{packet, sizeof(packet)};
        msghdr message{};
        message.msg_iov = &iov; message.msg_iovlen = 1;
        message.msg_control = control; message.msg_controllen = sizeof(control);
        ssize_t size = recvmsg(fd, &message, 0);
        if (size < 0) { if (errno == EINTR) continue; return 2; }
        if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || size < static_cast<ssize_t>(sizeof(android_log_header_t) + 3)) continue;
        const ucred* peer = nullptr;
        for (cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS && c->cmsg_len >= CMSG_LEN(sizeof(ucred)))
                peer = reinterpret_cast<ucred*>(CMSG_DATA(c));
        if (!peer) continue;
        android_log_header_t header;
        memcpy(&header, packet, sizeof(header));
        if (header.id == LOG_ID_EVENTS || header.id == LOG_ID_SECURITY || header.id == LOG_ID_STATS) continue;
        const char* payload = packet + sizeof(header);
        size_t remaining = size - sizeof(header) - 1;
        const char* tag = payload + 1;
        size_t tagSize = strnlen(tag, remaining);
        if (tagSize >= remaining) continue;
        const char* body = tag + tagSize + 1;
        size_t bodySize = strnlen(body, remaining - tagSize - 1);
        printf("GUEST_LOG pid=%d kernel_uid=%u tid=%u id=%u priority=%u tag=%.*s %.*s\n",
               peer->pid, peer->uid, header.tid, header.id, static_cast<unsigned char>(*payload),
               static_cast<int>(tagSize), tag, static_cast<int>(bodySize), body);
    }
}
