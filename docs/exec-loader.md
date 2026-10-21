# Native interpreter and exec continuity

This is the replacement direction for Pocket's temporary PRoot backend:
native ARM64 execution, a custom ELF interpreter/syscall layer, and unmodified
Bionic executable code. The implementation in `runtime/loader/` is an
experimental native-process backend. It now runs the real Android 9 init core
and zygote, including a forked Java child. It is not a full framework boot
backend and is not selected by the Manager APK.

## What runs

The kernel loads a small freestanding interpreter from the executable's prepared
`PT_INTERP`. It maps the pinned Android 9 linker, updates `AT_BASE` on the original
initial stack, and transfers control to that linker's entry point. The stock
linker loads stock Bionic and the executable normally. Neither stock linker's
nor Bionic's executable instructions are patched. Only the target executable's
interpreter field and the custom loader's own instance configuration are prepared.

The interpreter has no libc, dynamic dependencies, TLS accesses, preload library,
or tracing process. Its syscall handler therefore continues to work when the
stock linker initializes a different libc/TLS layout.

Additional seccomp filtering sends selected syscalls to an in-process SIGSYS
handler. Calls from the loader's fixed executable range bypass its own filter;
other host filters still apply. Unselected syscalls execute normally. This is
userspace compatibility inside the host app sandbox, not a new isolation or
privilege boundary.

Linux preserves filters across allowed fork/exec operations; see the
[kernel seccomp documentation](https://www.kernel.org/doc/html/latest/userspace-api/seccomp_filter.html).
Exec resets the handler and mappings, so the next prepared executable again
enters the interpreter at its fixed address. It installs the handler before
running the stock linker. A probe outside the bypass range detects the inherited
filter, avoiding another filter installation at every exec. The handler restores
the pre-trap signal mask before exec, retaining the application's blocked signals
without carrying a temporarily blocked SIGSYS into the next process.

Instance configuration lives in the custom interpreter's data segment rather
than environment variables. Empty-environment exec therefore retains the mapping
configuration. Guest credentials use a separate private exec-transport entry;
the interpreter consumes it before the stock linker runs, preserving the
application-visible environment, including an otherwise empty environment.
This transport is not authenticated or an isolation boundary.

## Current syscall coverage

Filesystem translation covers `openat`, `newfstatat`, `fstat`, `faccessat`,
`readlinkat`, `getcwd`, `chdir`, `mkdirat`, `unlinkat`, `renameat`/`renameat2`,
`linkat`, `symlinkat`, `fchmodat`, `fchownat`, `fchown`, `utimensat`, `truncate`
and `mknodat`. Absolute paths use the private backing root. Relative paths
retain kernel directory-FD/cwd semantics; `fchdir` works directly with the real
backing directory FD. Relative symlink targets remain relative; absolute targets
are translated when created. Readlink and getcwd reverse backing paths.

`/proc`, `/sys`, and most `/dev` nodes remain real host paths. Exceptions are
private `/dev/socket`, `/dev/__properties__`, and `/dev/input`; `/dev/hwbinder`
selects the real `/dev/binder` node for the separate Binder/HIDL bridge.
Pathname Unix sockets are translated for bind/connect, and getsockname/getpeername
reverse the pathname. Abstract and IP addresses retain their kernel ABI.
The original Pocket root is never bound into this fixture.

Boot mode starts with logical guest UID/GID 0. The get/set UID/GID and saved-ID
syscalls operate on guest state, inherited across fork and transported across
prepared exec. Guest ownership is recorded by device/inode in `.pocket-owner`
using the existing Pocket metadata format; stat reports those guest owners.
Fresh open/mkdir creations record the creator, and chown changes metadata only.
The actual kernel UID, capabilities, SELinux domain, supplementary groups,
`/proc` credentials and Unix peer credentials remain unchanged. This layer does
not enforce a new guest security boundary. Supplementary-group specialization
is still rejected by the rootless zygote profile.

Pathname copies use kernel-checked writes to a private pipe, bounded at page
boundaries. This returns EFAULT for unreadable/PROT_NONE pointers while allowing
a valid terminator immediately before an inaccessible page. Readlink replies
use a kernel-checked pipe read into the application's buffer, respecting invalid
and read-only destination pointers. There is no `/proc/self/mem` dependency.

`execve` validates that the target ELF selects this interpreter, then executes
the mapped pathname with the original argv/environment. Missing files retain
ENOENT. Unprepared ELFs and scripts return ENOEXEC instead of selecting a host
interpreter. The source also has a flags-zero `execveat` route; the current live
regression exercises `execve`, not that route. `AT_EMPTY_PATH` is unsupported.

SIGSYS is reserved: replacing its handler returns EBUSY, and signal masks keep
it deliverable, including while other handlers run. Other signal dispositions
remain native. rt_sigprocmask updates the saved signal-frame mask, rather than
losing the requested change at sigreturn. Large pathname/exec scratch buffers
use private mmap allocations, keeping the dispatcher stack frame small enough
for nested traps on small alternate signal stacks. This adds allocation
cost to translated filesystem calls; no performance ratio is claimed.
The internal getpid marker is a filter-presence probe; ordinary getpid calls
remain native.

## Reproducible test

Existing NDK r27c, the staged ARM64 Android 9 reference libraries, and
`out/boot/platform/system/bin/{sh,toybox}` are required. The builder checks ELF
architectures, builds only this experiment under `out/exec-loader/`, and does
not invoke the broad native/LineageOS build.

```sh
python tools/build_exec_loader.py --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c
rsync -a out/exec-loader/loader-test.apk tools/test_exec_loader_on_pi.py pi@juanraspberry:/home/pi/
ssh pi@juanraspberry 'python3 -u /home/pi/test_exec_loader_on_pi.py'
```

The harness requires the known virtual Cuttlefish instance. It installs a
separate non-debuggable fixture package, `dev.kreijstal.aosppocket.loadertest`.
An Activity extracts only its own assets and launches the native program via
ProcessBuilder with a cleared environment. Execution never passes through the
existing Pocket guest or its owner console. Host root is used only to retrieve
the fixture's private report. ADB servers and the Pocket instance are not reset.

The native regression checks:

- Normal untrusted-app SELinux domain, actual app UID, zero effective capabilities,
  seccomp enabled, and TracerPid zero.
- No PRoot or Bionic-patching library in the process mappings.
- All 858,956 file-backed executable Bionic bytes match the original.
- Translated libc and inline-SVC file operations, directory FDs, metadata,
  access checks, readlink truncation and reverse translation.
- Invalid/PROT_NONE path pointers, a valid path at a page boundary, invalid and
  read-only readlink buffers.
- Concurrent threads and fork inheritance.
- A filesystem trap inside a signal handler on a guarded 16 KiB alternate stack,
  with a broad handler mask that must keep SIGSYS deliverable.
- Sixteen consecutive execs with preserved argv and signal masks, alternating
  ordinary and empty environments, and no filter-count growth.
- The actual prepared Android 9 shell and Toybox reading a physical fixture file.

Evidence: [live report](exec-loader-validation.json).

## Android 9 init and zygote test

The optional build uses the pinned AOSP init parser/action/service core and
app_process with the existing reviewed rootless JNI profile. These are rebuilt
from source for ARM64 in `out/exec-loader/init-zygote`; the x86 build output and
working Pocket instance are not replaced. Stock linker and Bionic are packaged
unchanged. No PRoot, Bionic wrapper, preload trampoline or executable-byte patch
is included in this route.

```sh
python tools/build_exec_loader.py --init-zygote --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c
rsync -a out/exec-loader/loader-test.apk tools/test_exec_loader_on_pi.py pi@juanraspberry:/home/pi/
ssh pi@juanraspberry 'python3 -u /home/pi/test_exec_loader_on_pi.py --boot'
ssh pi@juanraspberry 'python3 -u /home/pi/test_exec_loader_on_pi.py'
```

The Activity supplies ANDROID_ROOT and ANDROID_DATA, prepares only its own
private directories, and recreates its volatile property area for each run.
Stdio uses /dev/null and a regular private log, as required by the real zygote
FD whitelist. Init creates the property and zygote sockets, publishes Android 9
properties, and supervises zygote plus a diagnostic Android log receiver.
The log receiver implements only the logdw datagram input, not full logd.

The independent native client queries the actual ZygoteConnection ABI protocol,
requests a Java child at guest UID/GID 39999, checks the child's file report and
thread execution, then queries the parent again. Init terminates and reaps its
services and removes its sockets. Native probes independently verify the normal
untrusted-app domain, actual app UID, zero effective capabilities, zero tracer
PID, and unchanged Bionic executable bytes.

Evidence: [native init/zygote report](init-zygote-validation.json).

## Remaining framework work

This validates init, lazy zygote startup and a real Java-child fork, not
system_server, launcher, installed applications or complete hardware support.
PID 1, kernel root, new capabilities, SELinux transitions, mount namespaces and
supplementary-group specialization are not provided by this rootless profile.
Ownership metadata does not grant host filesystem access or enforce guest DAC.
No hardware-integrity or banking-app compatibility result follows from it.

Broader filesystem calls, complete metadata lifecycle/race handling, script
dispatch and preparation of newly installed executables remain open. Apps' own
SIGSYS handlers remain unsupported. An explicit 8 KiB alternate-stack stress
test exhausted the stack with nested signal frames; 16 KiB is the tested size
for nested traps, and automatic alternate-stack growth is not implemented. The loader uses a fixed ARM64 address
layout and has only been validated on the Pi VM's 4 KiB-page kernel. Symlink
containment and relative-path escapes are not an isolation guarantee. Host
Android restrictions still apply to every actual kernel syscall.

The working 0.34 Manager still uses PRoot. It has not been silently replaced;
full framework boot must be validated before shipping the new Manager backend.
