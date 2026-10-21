// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE 1
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { perror(#x); return 1; } } while (0)
int main(void) {
    struct stat actual, through_proc;
    CHECK(!fstatat(AT_FDCWD, ".", &actual, 0));
    CHECK(!stat("/proc/self/cwd", &through_proc));
    CHECK(actual.st_dev == through_proc.st_dev && actual.st_ino == through_proc.st_ino);
    int directory = open("/proc/self/cwd", O_RDONLY | O_DIRECTORY);
    CHECK(directory >= 0); close(directory);
    CHECK(!stat("/proc/self/exe", &actual) && S_ISREG(actual.st_mode));
    char path[128]; snprintf(path, sizeof(path), "/data/local/tmp/pocket-magisk-fifo-%d", getpid());
    CHECK(!mkfifo(path, 0600));
    int fifo = open(path, O_RDWR | O_NONBLOCK);
    if (fifo < 0) { unlink(path); perror("open FIFO"); return 1; }
    CHECK(!fstat(fifo, &actual) && S_ISFIFO(actual.st_mode));
    char value = 0;
    CHECK(write(fifo, "x", 1) == 1 && read(fifo, &value, 1) == 1 && value == 'x');
    close(fifo); CHECK(!unlink(path));
    CHECK(syscall(SYS_getuid) != 0);
    puts("PASS: proc magic links, real FIFO roundtrip, host kernel UID remains unprivileged");
    return 0;
}
