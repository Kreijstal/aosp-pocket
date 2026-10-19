// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include "guest_paths.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line=%d expression=%s errno=%d (%s)\n", __LINE__, #x, errno, strerror(errno)); exit(1); } } while (0)
extern char** environ;
static const char content[] = "pocket-direct-fixture\n";

static void read_fixture_at(int dir, const char* path) {
    int fd = openat(dir, path, O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    char bytes[64] = {0};
    REQUIRE(read(fd, bytes, sizeof(bytes)) == (ssize_t)strlen(content));
    REQUIRE(!strcmp(bytes, content));
    REQUIRE(close(fd) == 0);
}
static void read_fixture(const char* path) { read_fixture_at(AT_FDCWD, path); }
static void* reader(void* arg) {
    (void)arg;
    for (int i = 0; i < 8; ++i) read_fixture("/data/fixture");
    return NULL;
}
static long status_number(const char* key) {
    // Read the real kernel surface independently of our controlled path ABI.
    int fd = syscall(SYS_openat, AT_FDCWD, "/proc/self/status", O_RDONLY | O_CLOEXEC, 0);
    REQUIRE(fd >= 0);
    char status[8192];
    ssize_t n = read(fd, status, sizeof(status) - 1);
    REQUIRE(n > 0); status[n] = 0; close(fd);
    char* line = strstr(status, key);
    REQUIRE(line);
    return strtol(line + strlen(key), NULL, 10);
}
static void identity(const char* mode) {
    long tracer = status_number("TracerPid:"), uid = status_number("Uid:");
    Dl_info library;
    REQUIRE(dladdr((void*)malloc, &library) && library.dli_fname);
    REQUIRE(uid == getuid());
    if (!strcmp(mode, "direct")) REQUIRE(tracer == 0 && pocket_guest_root());
    else if (!strcmp(mode, "proot")) REQUIRE(tracer > 0 && !pocket_guest_root());
    printf("DIRECT_ID mode=%s pid=%d uid=%ld tracer=%ld seccomp=%ld libc=%s\n",
           mode, getpid(), uid, tracer, status_number("Seccomp:"), library.dli_fname);
}
static void child(const char* mode) {
    identity(mode);
    if (!strcmp(mode, "direct")) REQUIRE(getenv("POCKET_CHILD_MARKER") && !strcmp(getenv("POCKET_CHILD_MARKER"), "1"));
    char cwd[4096]; REQUIRE(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/data"));
    read_fixture("fixture");
    read_fixture("/data/absolute-link");
    puts("DIRECT_CHILD_PASS fork_exec=1 cwd=/data");
}
static void suite(const char* mode) {
    REQUIRE(chdir("/") == 0);
    read_fixture("/data/fixture");
    int data = open("/data", O_RDONLY | O_DIRECTORY | O_CLOEXEC); REQUIRE(data >= 0);
    read_fixture_at(data, "fixture");
    read_fixture_at(data, "../../data/fixture");
    struct stat st;
    REQUIRE(!fstatat(data, "fixture", &st, 0) && st.st_size == (off_t)strlen(content));
    unlink("/data/absolute-link"); unlink("/data/relative-link"); unlink("/data/loop");
    REQUIRE(!symlink("/data/fixture", "/data/absolute-link"));
    REQUIRE(!symlink("fixture", "/data/relative-link"));
    REQUIRE(!symlink("loop", "/data/loop"));
    read_fixture("/data/absolute-link"); read_fixture("/data/relative-link");
    REQUIRE(!lstat("/data/absolute-link", &st) && S_ISLNK(st.st_mode));
    char link[64] = {0};
    REQUIRE(readlink("/data/absolute-link", link, sizeof(link) - 1) == 13 && !strcmp(link, "/data/fixture"));
    REQUIRE(open("/data/absolute-link", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
    REQUIRE(open("/data/loop", O_RDONLY) == -1 && errno == ELOOP);
    REQUIRE(open("/data/fixture/", O_RDONLY) == -1 && errno == ENOTDIR);
    REQUIRE(open("/data/fixture/missing", O_RDONLY) == -1 && errno == ENOTDIR);
    REQUIRE(open("/data/missing", O_RDONLY) == -1 && errno == ENOENT);
    REQUIRE(!mkdirat(data, "dir", 0700) || errno == EEXIST);
    unlink("/data/dir-link"); REQUIRE(!symlink("/data/dir", "/data/dir-link"));
    // '..' applies after following the symlink, not before it.
    read_fixture("/data/dir-link/../fixture");
    REQUIRE(!mkdir("/data/dir/deeper", 0700) || errno == EEXIST);
    int nested = open("/data/dir/nested", O_CREAT | O_TRUNC | O_WRONLY, 0600);
    REQUIRE(nested >= 0 && write(nested, content, strlen(content)) == (ssize_t)strlen(content)); close(nested);
    unlink("/data/deep-link"); REQUIRE(!symlink("/data/dir/deeper", "/data/deep-link"));
    read_fixture("/data/deep-link/../nested");
    int file = open("/data/dir/file", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    REQUIRE(file >= 0 && write(file, "test", 4) == 4); close(file);
    REQUIRE(open("/data/dir/file", O_CREAT | O_EXCL | O_WRONLY, 0600) == -1 && errno == EEXIST);
    REQUIRE(!unlinkat(data, "dir/file", 0));
    if (!strcmp(mode, "direct")) {
        int outside = syscall(SYS_openat, AT_FDCWD, "/", O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
        REQUIRE(outside >= 0);
        REQUIRE(openat(outside, "data/fixture", O_RDONLY) == -1 && errno == EXDEV);
        close(outside);
        REQUIRE(fstatat(data, "fixture", &st, AT_EMPTY_PATH) == -1 && errno == ENOTSUP);
        // Prove raw syscalls are outside this ABI; this is not a sandbox.
        int raw = syscall(SYS_openat, AT_FDCWD, "/data/fixture", O_RDONLY, 0);
        REQUIRE(raw < 0); // VM/host fixture exists only in the private guest root.
    }
    REQUIRE(!fchdir(data)); close(data);
    char cwd[4096]; REQUIRE(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/data"));
    char tiny[2]; REQUIRE(!getcwd(tiny, sizeof(tiny)) && errno == ERANGE);
    char* allocated = getcwd(NULL, 0); REQUIRE(allocated && !strcmp(allocated, "/data")); free(allocated);
    pthread_t thread; REQUIRE(!pthread_create(&thread, NULL, reader, NULL)); REQUIRE(!pthread_join(thread, NULL));
    pid_t pid = fork(); REQUIRE(pid >= 0);
    if (!pid) {
        char* args[] = {"/system/bin/pocket-direct-probe", "child", (char*)mode, NULL};
        // Deliberately omit the runtime environment in direct mode: exec must
        // preserve its root and pinned linker configuration, not silently escape.
        char* empty[] = {"POCKET_CHILD_MARKER=1", NULL};
        execve(args[0], args, !strcmp(mode, "direct") ? empty : environ);
        _exit(127);
    }
    int status; REQUIRE(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    identity(mode);
    // Hold for independent host inspection; host releases the process via a
    // private file, never by changing global device state.
    printf("DIRECT_READY mode=%s pid=%d\n", mode, getpid()); fflush(stdout);
    if (getenv("POCKET_DIRECT_HOLD")) {
        bool released = false;
        for (int i = 0; i < 600; ++i) {
            int fd = open("/data/release", O_RDONLY);
            if (fd >= 0) { close(fd); released = true; break; }
            usleep(50000);
        }
        REQUIRE(released);
    }
    puts("DIRECT_SUITE_PASS paths=1 dirfd=1 symlinks=1 cwd=1 thread=1 fork_exec=1");
}
static long long nanos(void) {
    struct timespec ts; REQUIRE(!clock_gettime(CLOCK_MONOTONIC, &ts));
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
int main(int argc, char** argv) {
    setbuf(stdout, NULL);
    REQUIRE(argc >= 3 && !pocket_guest_init());
    if (!strcmp(argv[1], "child")) { child(argv[2]); return 0; }
    if (!strcmp(argv[1], "suite")) { suite(argv[2]); return 0; }
    if (!strcmp(argv[1], "bench")) {
        REQUIRE(argc == 4);
        int count = atoi(argv[3]); REQUIRE(count > 0 && count <= 100000);
        long long begin = nanos();
        for (int i = 0; i < count; ++i) {
            read_fixture(argv[2]); struct stat st; REQUIRE(!stat(argv[2], &st));
        }
        printf("DIRECT_BENCH iterations=%d elapsed_ns=%lld\n", count, nanos() - begin);
        return 0;
    }
    return 64;
}
