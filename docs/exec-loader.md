# Native interpreter and exec continuity

This is the replacement direction for Pocket's temporary PRoot backend:
native ARM64 execution, a custom ELF interpreter/syscall layer, and unmodified
Bionic executable code. The implementation in `runtime/loader/` is an
experimental native-process foundation. It is not yet a full Android boot
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
configuration without inserting environment variables into the application.

## Current syscall coverage

`openat`, `newfstatat`, `faccessat`, and `readlinkat` translate absolute paths into
the private fixture root. `/proc`, `/sys`, and `/dev` currently remain host paths.
Paths already inside the backing root are accepted directly. Relative paths
retain kernel directory-FD/cwd semantics. Backing paths returned by readlink,
including `/proc/self/exe` and `/proc/self/fd`, translate back to guest paths.
This is necessary for the stock linker's directory and library resolution.

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

Replacing SIGSYS's handler returns EBUSY for now. Other sigactions pass through.
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
- Sixteen consecutive execs with preserved argv and signal masks, alternating
  ordinary and empty environments, and no filter-count growth.
- The actual prepared Android 9 shell and Toybox reading a physical fixture file.

Evidence: [live report](exec-loader-validation.json).

## Remaining Android boot work

Guest UID/GID/groups, file-ownership metadata, private property/socket/device
paths, cwd/directory-FD namespace behavior, broader filesystem operations, and
init/zygote integration are not implemented here. The fixture reads the host's
permitted property areas and reports its real kernel UID. No hardware integrity
or banking-app compatibility result follows from this test.

Script dispatch and preparation of arbitrary newly installed executables remain
open. Apps' own SIGSYS handlers and nested signal behavior need further work.
The loader uses a fixed ARM64 address layout and has only been validated on the
Pi VM's 4 KiB-page kernel. Symlink containment and filesystem races are not an
isolation guarantee. No performance ratio has been measured for this backend.

The working 0.34 Manager still uses PRoot. It has not been silently replaced
with this incomplete experiment; full guest boot must be validated before that
integration is shipped.
