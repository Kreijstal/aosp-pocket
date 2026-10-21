// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include "../modules/syscall-profiles/syscall_profile.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

_Thread_local volatile char profile_test_tls[64] __attribute__((aligned(64)));
static const char alias[] = "/pocket-syscall-fixture";
static char backing[PATH_MAX], real_file[PATH_MAX], virtual_file[PATH_MAX];

static void check(int ok, const char *message) {
    if (!ok) {fprintf(stderr, "FAIL: %s errno=%d (%s)\n", message, errno, strerror(errno)); exit(1);}
}
/* This SVC is separate from the adapter's permitted gate. */
static long direct_openat(int directory, const char *path, int flags) {
    register long x0 __asm__("x0") = directory;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = flags;
    register long x3 __asm__("x3") = 0;
    register long x8 __asm__("x8") = __NR_openat;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory", "cc");
    return x0;
}
static void contents(int fd) {
    check(fd >= 0, "open fixture");
    char bytes[32] = {0};
    check(read(fd, bytes, sizeof(bytes)) == 14 && !memcmp(bytes, "pocket-openat\n", 14), "read real fixture bytes");
    check(!close(fd), "close fixture");
}
static void enable(void) {
    check(!pocket_syscall_profile_install(POCKET_SYSCALL_OPENAT_PREFIX, alias, backing), "install openat profile");
}
static void disabled(void) {
    check(!pocket_syscall_profile_install(POCKET_SYSCALL_OFF, NULL, NULL), "select off profile");
    contents(openat(AT_FDCWD, real_file, O_RDONLY));
    check(direct_openat(AT_FDCWD, virtual_file, O_RDONLY) == -ENOENT, "off profile leaves alias absent");
}
static void libc_call(void) {enable();contents(openat(AT_FDCWD, virtual_file, O_RDONLY));}
static void svc_call(void) {enable();contents((int)direct_openat(AT_FDCWD, virtual_file, O_RDONLY));}
static void passthrough(void) {
    int directory = open(backing, O_RDONLY | O_DIRECTORY);check(directory >= 0, "open real directory");
    pid_t before = getpid();
    enable();
    contents(openat(directory, "fixture.txt", O_RDONLY));
    contents(openat(AT_FDCWD, real_file, O_RDONLY));
    check(getpid() == before, "unselected getpid passes through");
    check(!close(directory), "close real directory");
}
static void errors(void) {
    enable();
    check(direct_openat(AT_FDCWD, "/pocket-syscall-fixture/missing", O_RDONLY) == -ENOENT, "missing file errno");
    check(direct_openat(AT_FDCWD, (const char *)1, O_RDONLY) == -EFAULT, "invalid pointer errno");
    check(direct_openat(AT_FDCWD, virtual_file, O_RDONLY | O_DIRECTORY) == -ENOTDIR, "directory flag preserved");
    check(direct_openat(AT_FDCWD, "/pocket-syscall-fixture/link", O_RDONLY | O_NOFOLLOW) == -ELOOP, "nofollow flag preserved");
    check(direct_openat(AT_FDCWD, "/pocket-syscall-fixture/../escape", O_RDONLY) == -EPERM, "lexical escape rejected");
    check(direct_openat(AT_FDCWD, "/pocket-syscall-fixture-other/fixture.txt", O_RDONLY) == -ENOENT, "prefix boundary preserved");
    check(pocket_syscall_profile_install(POCKET_SYSCALL_OFF, NULL, NULL) == -1 && errno == EALREADY, "live profile cannot be disabled");
    char *args[] = {"missing", NULL}, *environment[] = {NULL};
    check(execve("/missing-profile-executable", args, environment) == -1 && errno == EOPNOTSUPP,
          "exec rejected before handler can be lost");
}
static void *worker(void *argument) {
    (void)argument;
    char local[PATH_MAX];snprintf(local, sizeof(local), "%s/fixture.txt", alias);
    for (int i = 0; i < 20; i++) contents((int)direct_openat(AT_FDCWD, local, O_RDONLY));
    return NULL;
}
static void inherited(void) {
    enable();
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) check(!pthread_create(&threads[i], NULL, worker, NULL), "create inherited-filter thread");
    for (int i = 0; i < 4; i++) check(!pthread_join(threads[i], NULL), "join thread");
    pid_t child = fork();check(child >= 0, "fork filtered child");
    if (!child) {worker(NULL);_exit(0);}
    int status;check(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "fork uses child's own memory");
}
static void existing_handler(int signal) {(void)signal;}
static void conflict(void) {
    struct sigaction action = {0};action.sa_handler = existing_handler;sigemptyset(&action.sa_mask);
    check(!sigaction(SIGSYS, &action, NULL), "set preexisting SIGSYS handler");
    check(pocket_syscall_profile_install(POCKET_SYSCALL_OPENAT_PREFIX, alias, backing) == -1 && errno == EBUSY, "refuse existing handler");
    struct sigaction after;check(!sigaction(SIGSYS, NULL, &after) && after.sa_handler == existing_handler, "preserve existing handler");
}
static volatile sig_atomic_t forwarded_signal;
static void chain_handler(int signal, siginfo_t *info, void *context) {
    (void)context;
    if(signal == SIGSYS && info->si_code != SYS_SECCOMP) forwarded_signal = 1;
}
static void chaining(void) {
    struct sigaction action = {0};action.sa_sigaction = chain_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;sigemptyset(&action.sa_mask);
    check(!sigaction(SIGSYS, &action, NULL), "set chainable SIGSYS handler");
    check(!pocket_syscall_profile_install(POCKET_SYSCALL_OPENAT_PREFIX_CHAINED, alias, backing), "install explicitly chained profile");
    check(!raise(SIGSYS) && forwarded_signal, "forward unrelated SIGSYS");
    contents((int)direct_openat(AT_FDCWD, virtual_file, O_RDONLY));
}
int main(int argc, char **argv) {
    profile_test_tls[0] = 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    check(argc == 2, "expected real host-writable fixture parent directory");
    check(snprintf(backing, sizeof(backing), "%s/syscall-profile-XXXXXX", argv[1]) < (int)sizeof(backing), "fixture parent length");
    check(mkdtemp(backing) != NULL, "create fixture directory");
    check(snprintf(real_file, sizeof(real_file), "%s/fixture.txt", backing) < (int)sizeof(real_file), "real path length");
    snprintf(virtual_file, sizeof(virtual_file), "%s/fixture.txt", alias);
    int fd = open(real_file, O_CREAT | O_WRONLY | O_EXCL, 0600);check(fd >= 0, "create fixture file");
    check(write(fd, "pocket-openat\n", 14) == 14 && !close(fd), "write fixture bytes");
    char link[PATH_MAX];check(snprintf(link, sizeof(link), "%s/link", backing) < (int)sizeof(link), "symlink path length");
    check(!symlink("fixture.txt", link), "create nofollow fixture");
    struct {const char *name;void (*run)(void);} cases[] = {
        {"off", disabled}, {"libc-openat", libc_call}, {"direct-svc-openat", svc_call},
        {"pass-through", passthrough}, {"errors-and-flags", errors},
        {"thread-and-fork-inheritance", inherited}, {"existing-sigsys-handler", conflict},
        {"chained-sigsys-handler", chaining},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        pid_t child = fork();check(child >= 0, "fork profile-selected process");
        if (!child) {cases[i].run();_exit(0);}
        int status;check(waitpid(child, &status, 0) == child, "wait test child");
        int passed = WIFEXITED(status) && !WEXITSTATUS(status);
        printf("%s: %s status=%x\n", passed ? "PASS" : "FAIL", cases[i].name, status);
        failures += !passed;
    }
    unlink(link);unlink(real_file);rmdir(backing);
    printf("SYSCALL_PROFILE_RESULT failures=%d kernel_uid=%u\n", failures, getuid());
    return failures ? 1 : 0;
}
