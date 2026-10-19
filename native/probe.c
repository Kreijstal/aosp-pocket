// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static void quoted(const char *s) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    if (*p == '"' || *p == '\\') printf("\\%c", *p);
    else if (*p < 32) printf("\\u%04x", *p);
    else putchar(*p);
  }
  putchar('"');
}

// Never change the Java process or a long-lived native process's namespaces.
// Each privilege probe runs in a disposable child and reports errno or a signal.
static int child_check(int kind) {
  if (kind == 0) return unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS);
  if (kind == 1) return unshare(CLONE_NEWUSER);
  if (kind == 2) return ptrace(PTRACE_TRACEME, 0, NULL, NULL);
  errno = EINVAL;
  return -1;
}

static void capability(const char *name, int kind) {
  int fd[2];
  printf(",\"%s\":", name);
  if (pipe(fd)) { printf("{\"probe_error\":%d}", errno); return; }
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    close(fd[0]);
    alarm(2);
    int result = child_check(kind);
    int code = result == 0 ? 0 : errno;
    ssize_t ignored = write(fd[1], &code, sizeof(code));
    (void)ignored;
    _exit(0);
  }
  close(fd[1]);
  if (pid < 0) { close(fd[0]); printf("{\"probe_error\":%d}", errno); return; }
  int status = 0, code = -1;
  ssize_t bytes = read(fd[0], &code, sizeof(code));
  close(fd[0]);
  // A successful PTRACE_TRACEME makes the parent a tracer; the child immediately
  // exits without exec/stop, so no other process is attached or modified.
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  if (WIFSIGNALED(status)) printf("{\"available\":false,\"signal\":%d}", WTERMSIG(status));
  else if (bytes != sizeof(code)) printf("{\"probe_error\":\"no child result\"}");
  else {
    printf("{\"available\":%s,\"errno\":%d,\"detail\":", code == 0 ? "true" : "false", code);
    quoted(code == 0 ? "operation succeeded in disposable child" : strerror(code));
    putchar('}');
  }
}

int main(int argc, char **argv) {
  if (argc >= 4 && !strcmp(argv[1], "exec-toybox")) {
    // APK native executables must be named lib*.so. Pass Toybox its real
    // multiplexer name as argv[0], without modifying AOSP's command dispatcher.
    const char *executable = argv[2];
    argv[2] = "toybox";
    execv(executable, argv + 2);
    perror("execv toybox");
    return 127;
  }
  if (argc != 1) return 2;
  struct utsname uts;
  if (uname(&uts)) return 1;
  printf("{\"uid\":%u,\"euid\":%u,\"pid\":%d,\"machine\":", getuid(), geteuid(), getpid());
  quoted(uts.machine);
  printf(",\"kernel\":"); quoted(uts.release);
  printf(",\"seccomp_mode\":%d", prctl(PR_GET_SECCOMP, 0, 0, 0, 0));
  char context[256] = {0};
  int fd = open("/proc/self/attr/current", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t n = read(fd, context, sizeof(context)-1);
    if (n > 0) context[n] = 0;
    close(fd);
  }
  printf(",\"selinux_context\":"); quoted(context);
  fd = open("/dev/binder", O_RDONLY | O_CLOEXEC);
  int code = fd < 0 ? errno : 0;
  struct binder_version version = {0};
  if (fd >= 0) {
    if (ioctl(fd, BINDER_VERSION, &version)) code = errno;
    close(fd);
  }
  printf(",\"host_binder\":{\"available\":%s,\"errno\":%d,\"protocol\":%d,\"private_guest_context\":false}",
         code == 0 ? "true" : "false", code, version.protocol_version);
  capability("private_namespaces", 0);
  capability("user_namespace", 1);
  capability("trace_own_child", 2);
  // Shared mappings are needed for graphics/Binder, but this is only an anonymous
  // mapping check; it does not establish cross-process graphics support.
  void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  printf(",\"anonymous_shared_mapping\":%s", map != MAP_FAILED ? "true" : "false");
  if (map != MAP_FAILED) munmap(map, 4096);
  puts("}");
  return 0;
}
