> This document records the original bootstrap design. The implemented Ananbox/PRoot
> runtime and its guest boot evidence are documented in [ananbox-runtime.md](ananbox-runtime.md).

# Runtime design

## Intended architecture

Run compatible ARM64 guest userspace code directly on the host CPU. Adapt the
Android userspace to an unprivileged host app rather than assuming that AOSP's
normal boot sequence can run unchanged. No host root or ADB authorization is
part of the default design.

The existing bootstrap demonstrates native process execution, not all of this
architecture. In particular, it does not intercept filesystem syscalls or
provide virtual UIDs, Binder handles, properties, SELinux policy, hardware or
an Android desktop.

## Evidence from the first runtime test

The Android 16 ARM64 Cuttlefish app ran as UID/EUID 10168 in the `untrusted_app`
SELinux domain with seccomp mode 2. Combined mount/PID/IPC/UTS namespace creation
returned EPERM; user-namespace creation returned EINVAL. These are observations
from that VM, not measurements on the Samsung. The successful host Binder
version query does not provide a private Binder context for a second Android
system. Namespace support in a kernel is insufficient without usable
capabilities, SELinux policy and seccomp policy.

## Runtime work that remains

| Facility | Required implementation | Current status |
|---|---|---|
| Native execution | Launch packaged ARM64 ELF code under the app UID | Verified in VM |
| Instance storage | Writable app-private data and separate instance directories | Basic layout verified; no virtual filesystem |
| Filesystem view | Resolve guest `/system`, `/data`, APEX and executable paths without host mounts | Not implemented |
| Credentials | Define guest identity semantics without pretending the host process has root | Not implemented |
| Binder | Separate guest service namespace, transactions, handles, death notifications and FD transfer | Not implemented |
| Properties and logging | Guest-owned property area and logging endpoints | Not implemented |
| Startup | Adapt service supervision and boot dependencies; do not run unmodified root-dependent init | Not implemented |
| ART and framework | Launch adapted runtime, zygote, `system_server` and package manager | Not implemented |
| Graphics and input | Guest composition buffers to a host Surface; reverse input delivery | Not implemented |
| Hardware | Explicit camera, audio, sensor and location brokers subject to host permissions | Not implemented |
| Lifecycle | Process limits, stop/restart, background policy and saved state | Not implemented |

Independent guest security requires a separate assessment. All processes
currently share the host app UID, so the app sandbox isolates this prototype
from other apps; it does not isolate guest processes from one another.

## Why this is not a regular LineageOS build

A stock AOSP or LineageOS image expects system privileges, a kernel Binder
service manager, particular paths and boot-time setup. Removing applications
does not remove those dependencies. The relevant work is adapting those
interfaces. A minimal package selection can reduce memory and startup cost
after the runtime path works, but no full-Android startup or memory benchmark
exists yet.

## Reuse decisions

The [current VMOS APK analysis](vmos-analysis.md) supplies binary-derived
evidence for a custom YLINKER loader/dispatcher, an additional guest Binder
implementation, and separate graphics/HAL bridges. Its preloader's selective
seccomp filter was exercised only in offline emulation. This supports testing
an adapted userspace runtime; it does not establish full syscall coverage or
provide a reusable implementation of the private guest transport.

- AOSP supplies the guest userspace and first native tools. Keep upstream source
  revisions and local adaptations separate.
- Twoyi is a useful architecture/reference project and has an MPL-2.0 host app.
  Its own README requires a prebuilt guest ROM, preventing an end-to-end
  source build from the released project. This project does not import it.
- BlackBox's app-level service interception is relevant research, but its app
  sandbox is not itself a separate booted Android framework.
- Do not assume a successful Shizuku/ADB connection grants mount or namespace
  capabilities. Any privileged backend must remain explicit and separate.

## References

- [VMOS's published older guest architecture](https://github.com/vmos-dev/open-vmos-aosp_5.1#vmos-pro-android-51-rom-technical-architecture)
- [Twoyi's source/build limitations](https://github.com/twoyi/twoyi#building)
- [Android executable-code restrictions](https://developer.android.com/about/versions/10/behavior-changes-10#execute-permission)
- [AOSP Binder RPC source](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/libs/binder/)

The packaged PIE files are named `lib*.so` so Android installs their immutable
code in `nativeLibraryDir`; they are executables, not JNI libraries. The
source-built launcher sets Toybox's expected `argv[0]` before `execv`. Writable
instance directories contain data only. This intentionally avoids extracting
executable code into the writable app home directory.
