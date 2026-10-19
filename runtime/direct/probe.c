// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include "guest_paths.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
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
static void extended_files(const char* mode) {
    const char* remove[] = {"config", "config.pending", "hard", "meta-link", "hard-link", "follow-hard", "renamed-link", NULL};
    for (int i = 0; remove[i]; ++i) {
        char path[128]; snprintf(path, sizeof(path), "/data/%s", remove[i]); unlink(path);
    }
    FILE* stream = fopen("/data/config", "we"); REQUIRE(stream);
    REQUIRE(fcntl(fileno(stream), F_GETFD) & FD_CLOEXEC);
    REQUIRE(fputs("old", stream) >= 0 && !fclose(stream));
    stream = fopen("/data/config.pending", "wxe"); REQUIRE(stream);
    REQUIRE(fputs("new-value", stream) >= 0 && !fflush(stream) && !fclose(stream));
    REQUIRE(!fopen("/data/config.pending", "wx") && errno == EEXIST);
    int data = open("/data", O_RDONLY | O_DIRECTORY | O_CLOEXEC); REQUIRE(data >= 0);
    REQUIRE(!renameat(data, "config.pending", data, "config"));
    REQUIRE(access("/data/config.pending", F_OK) == -1 && errno == ENOENT);
    REQUIRE(!faccessat(data, "config", R_OK | W_OK, 0));
    stream = fopen("/data/config", "re"); REQUIRE(stream);
    char text[32] = {0}; REQUIRE(fread(text, 1, sizeof(text), stream) == 9 && !strcmp(text, "new-value"));
    REQUIRE(!fclose(stream));
    struct stat target, alias, st;
    int link_result = link("/data/config", "/data/hard");
    bool hardlinks = link_result == 0;
    int link_error = errno;
    if (hardlinks) {
        REQUIRE(!stat("/data/config", &target) && !stat("/data/hard", &alias));
        REQUIRE(target.st_ino == alias.st_ino && target.st_dev == alias.st_dev && target.st_nlink == 2);
    } else {
        REQUIRE(link_error == EACCES || link_error == EPERM);
        // Independent descriptor-relative kernel call must deny the same link.
        REQUIRE(syscall(SYS_linkat, data, "config", data, "hard", 0) == -1 && errno == link_error);
        REQUIRE(lstat("/data/hard", &st) == -1 && errno == ENOENT);
        printf("DIRECT_HARDLINK_HOST_DENIED errno=%d raw_kernel_matches=1\n", link_error);
    }
    REQUIRE(!symlink("/data/config", "/data/meta-link"));
    int nofollow = linkat(data, "meta-link", data, "hard-link", 0);
    if (hardlinks) {
        REQUIRE(!nofollow);
        REQUIRE(!lstat("/data/meta-link", &target) && !lstat("/data/hard-link", &alias));
        REQUIRE(S_ISLNK(alias.st_mode) && target.st_ino == alias.st_ino);
    } else REQUIRE(nofollow == -1 && errno == link_error);
    int follow = linkat(data, "meta-link", data, "follow-hard", AT_SYMLINK_FOLLOW);
    if (hardlinks) {
        REQUIRE(!follow);
        REQUIRE(!stat("/data/config", &target) && !stat("/data/follow-hard", &alias) && target.st_ino == alias.st_ino);
    } else REQUIRE(follow == -1 && errno == link_error);
    REQUIRE(!rename("/data/meta-link", "/data/renamed-link"));
    REQUIRE(!lstat("/data/renamed-link", &st) && S_ISLNK(st.st_mode));
    REQUIRE(!chmod("/data/renamed-link", 0640));
    REQUIRE(!stat("/data/config", &st) && (st.st_mode & 0777) == 0640);
    REQUIRE(!fchmodat(data, "config", 0600, 0));
    REQUIRE(!stat("/data/config", &st) && (st.st_mode & 0777) == 0600);
    // These retain real ownership; they do not implement privileged guest chown.
    REQUIRE(!chown("/data/config", getuid(), getgid()));
    REQUIRE(!lchown("/data/renamed-link", getuid(), getgid()));
    REQUIRE(!fchownat(data, "config", getuid(), getgid(), 0));
    struct timespec times[2] = {{1700000000, 123000000}, {1700000001, 456000000}};
    REQUIRE(!utimensat(data, "config", times, 0));
    REQUIRE(!stat("/data/config", &st) && st.st_mtim.tv_sec == times[1].tv_sec && st.st_mtim.tv_nsec == times[1].tv_nsec);
    REQUIRE(!truncate(hardlinks ? "/data/hard" : "/data/config", 3));
    REQUIRE(!stat("/data/config", &st) && st.st_size == 3);
    REQUIRE(truncate("/data/config", -1) == -1 && errno == EINVAL);
    char canonical[4096];
    REQUIRE(realpath("/data/deep-link/../nested", canonical) && !strcmp(canonical, "/data/dir/nested"));
    char* allocated = realpath("/data/renamed-link", NULL);
    REQUIRE(allocated && !strcmp(allocated, "/data/config")); free(allocated);
    REQUIRE(realpath("/../../", canonical) && !strcmp(canonical, "/"));
    REQUIRE(!realpath("/data/does-not-exist", canonical) && errno == ENOENT);
    unlink("/data/trailing-link"); REQUIRE(!symlink("fixture/", "/data/trailing-link"));
    stream = fopen("/data/trailing-link", "r");
    if (!strcmp(mode, "direct")) REQUIRE(!stream && errno == ENOTDIR);
    else if (stream) {
        char bytes[64] = {0}; REQUIRE(fread(bytes, 1, sizeof(bytes), stream) == strlen(content) && !strcmp(bytes, content));
        REQUIRE(!fclose(stream));
        puts("DIRECT_PROOT_DIFFERENCE trailing_symlink_fopen=success expected_linux=ENOTDIR");
    } else {
        REQUIRE(errno == ENOTDIR || errno == ENOENT);
        if (errno != ENOTDIR) printf("DIRECT_PROOT_DIFFERENCE trailing_symlink_fopen_errno=%d expected_linux=%d\n", errno, ENOTDIR);
    }
    char* result = realpath("/data/trailing-link", canonical);
    if (!strcmp(mode, "direct")) REQUIRE(!result && errno == ENOTDIR);
    else if (result) {
        REQUIRE(!strcmp(result, "/data/fixture"));
        puts("DIRECT_PROOT_DIFFERENCE trailing_symlink_realpath=success expected_linux=ENOTDIR");
    } else {
        REQUIRE(errno == ENOTDIR || errno == ENOENT);
        if (errno != ENOTDIR) printf("DIRECT_PROOT_DIFFERENCE trailing_symlink_realpath_errno=%d expected_linux=%d\n", errno, ENOTDIR);
    }
    DIR* directory = opendir("/data"); REQUIRE(directory);
    bool config = false, fixture = false;
    for (struct dirent* entry; (entry = readdir(directory));) {
        config |= !strcmp(entry->d_name, "config"); fixture |= !strcmp(entry->d_name, "fixture");
    }
    REQUIRE(config && fixture && !closedir(directory));
    if (!strcmp(mode, "direct")) {
        REQUIRE(faccessat(data, "config", F_OK, AT_EACCESS) == -1 && errno == ENOTSUP);
        REQUIRE(fchmodat(data, "renamed-link", 0600, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOTSUP);
    }
    close(data);
    printf("DIRECT_FILES_PASS stdio=1 directory=1 realpath=1 access=1 rename=1 hardlink=%s metadata=1 truncate=1\n", hardlinks ? "1" : "host-denied");
}
static struct sockaddr_un unix_address(const char* path) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    REQUIRE(strlen(path) < sizeof(address.sun_path)); strcpy(address.sun_path, path);
    return address;
}
static void check_socket_name(int fd, bool peer, const char* expected) {
    struct sockaddr_un address; socklen_t size = sizeof(address);
    REQUIRE(!(peer ? getpeername(fd, (struct sockaddr*)&address, &size) : getsockname(fd, (struct sockaddr*)&address, &size)));
    REQUIRE(address.sun_family == AF_UNIX && !strcmp(address.sun_path, expected));
}
static void socket_timeout(int fd) {
    struct timeval time = {5, 0};
    REQUIRE(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &time, sizeof(time)));
    REQUIRE(!setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &time, sizeof(time)));
}
static void extended_sockets(const char* mode) {
    unlink("/data/service.sock"); unlink("/data/client.sock");
    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); REQUIRE(server >= 0);
    struct sockaddr_un address = unix_address("/data/service.sock");
    REQUIRE(!bind(server, (struct sockaddr*)&address, sizeof(address)) && !listen(server, 1));
    check_socket_name(server, false, "/data/service.sock");
    // Truncated address buffers must still report the full guest address size.
    unsigned char tiny[2]; socklen_t tiny_size = sizeof(tiny);
    REQUIRE(!getsockname(server, (struct sockaddr*)tiny, &tiny_size) && tiny_size > sizeof(tiny));
    REQUIRE(!chmod("/data/service.sock", 0600));
    pid_t pid = fork(); REQUIRE(pid >= 0);
    if (!pid) {
        alarm(10); close(server);
        int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); REQUIRE(client >= 0); socket_timeout(client);
        struct sockaddr_un local = unix_address("/data/client.sock");
        REQUIRE(!bind(client, (struct sockaddr*)&local, sizeof(local)));
        REQUIRE(!connect(client, (struct sockaddr*)&address, sizeof(address)));
        check_socket_name(client, false, "/data/client.sock");
        check_socket_name(client, true, "/data/service.sock");
        char bytes[4], control[CMSG_SPACE(sizeof(int))];
        struct iovec iov = {bytes, sizeof(bytes)};
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        REQUIRE(recvmsg(client, &msg, 0) == 4 && !memcmp(bytes, "FD28", 4));
        REQUIRE(!(msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)));
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        REQUIRE(cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS && cmsg->cmsg_len == CMSG_LEN(sizeof(int)));
        int fd; memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
        char data[64] = {0}; REQUIRE(read(fd, data, sizeof(data)) == (ssize_t)strlen(content) && !strcmp(data, content));
        close(fd); REQUIRE(write(client, "OK28", 4) == 4); close(client); _exit(0);
    }
    struct pollfd wait = {server, POLLIN, 0}; REQUIRE(poll(&wait, 1, 5000) == 1);
    int connection = accept4(server, NULL, NULL, SOCK_CLOEXEC); REQUIRE(connection >= 0); socket_timeout(connection);
    check_socket_name(connection, true, "/data/client.sock");
    struct ucred peer; socklen_t size = sizeof(peer);
    REQUIRE(!getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &peer, &size) && peer.uid == getuid() && peer.pid == pid);
    int fixture = open("/data/fixture", O_RDONLY | O_CLOEXEC); REQUIRE(fixture >= 0);
    char control[CMSG_SPACE(sizeof(int))] = {0}; struct iovec iov = {"FD28", 4};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS; cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fixture, sizeof(fixture)); REQUIRE(sendmsg(connection, &msg, 0) == 4); close(fixture);
    char answer[4]; REQUIRE(read(connection, answer, sizeof(answer)) == 4 && !memcmp(answer, "OK28", 4));
    close(connection); close(server);
    int status; REQUIRE(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    if (!strcmp(mode, "direct")) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); REQUIRE(fd >= 0);
        struct sockaddr_un abstract = {.sun_family = AF_UNIX}; memcpy(abstract.sun_path + 1, "pocket-test", 11);
        REQUIRE(bind(fd, (struct sockaddr*)&abstract, sizeof(abstract)) == -1 && errno == ENOTSUP);
        struct sockaddr_un long_path = {.sun_family = AF_UNIX};
        strcpy(long_path.sun_path, "/data/"); memset(long_path.sun_path + 6, 'x', 90); long_path.sun_path[96] = 0;
        REQUIRE(bind(fd, (struct sockaddr*)&long_path, sizeof(long_path)) == -1 && errno == ENAMETOOLONG);
        close(fd);
    }
    REQUIRE(!unlink("/data/service.sock") && !unlink("/data/client.sock"));
    puts("DIRECT_SOCKET_PASS bind=1 connect=1 names=1 peer_credentials=1 fd_transfer=1");
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
    extended_files(mode);
    extended_sockets(mode);
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
