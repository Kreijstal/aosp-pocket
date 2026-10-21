// SPDX-License-Identifier: GPL-3.0-only
// Launch an existing test app through Pocket's owner control channel, without ART.
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int exact(int fd, void* data, size_t length, int sending) {
    unsigned char* p = data;
    while (length) {
        ssize_t n = sending ? write(fd, p, length) : read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}
int main(int argc, char** argv) {
    int installing = argc == 2 && !strcmp(argv[1], "--install");
    if (argc != 2 || (!installing && (strlen(argv[1]) > 255 || strspn(argv[1],
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") != strlen(argv[1])))) return 2;
    int fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    struct sockaddr_un address = {.sun_family=AF_UNIX};
    strcpy(address.sun_path, "/dev/socket/pocket_control");
    struct timeval timeout = {.tv_sec=30};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (fd < 0 || connect(fd, (struct sockaddr*)&address, sizeof(address))) {perror("control connect");return 1;}
    uint32_t hello[4];
    if (exact(fd, hello, sizeof(hello), 0) || ntohl(hello[0]) != 0x504b543c) return 1;
    uint32_t op = htonl(installing ? 4 : 9);
    if (exact(fd, &op, sizeof(op), 1)) return 1;
    if (installing) {
        uint16_t count;
        char message[4096];
        if (exact(fd, &count, sizeof(count), 0) || ntohs(count) >= sizeof(message)) return 1;
        size_t length = ntohs(count);
        if (exact(fd, message, length, 0)) return 1;
        message[length] = 0; puts(message); close(fd); return 0;
    }
    uint16_t size = htons((uint16_t)strlen(argv[1]));
    unsigned char accepted = 0;
    if (exact(fd, &size, sizeof(size), 1) ||
        exact(fd, argv[1], strlen(argv[1]), 1) || exact(fd, &accepted, 1, 0) || accepted != 1) return 1;
    close(fd);
    puts("Existing guest app launched");
    return 0;
}
