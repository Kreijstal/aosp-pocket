# Android 9 without Bionic wrappers

Version 0.34 uses PRoot syscall translation for the Android 9 Manager backend.
It does not bundle or load `libpocket_direct_boot.so`, and preserves the base
image's `libc.so` payload byte for byte. Guest Binder, HIDL, zygote and virtual
hardware implementations remain separate platform components. They do not
overwrite Bionic function entries.

This trades direct-backend performance for syscall coverage. It is not a new
kernel or security boundary: guest roles still share the host app's kernel UID,
and host permissions continue to apply. This change does not establish banking
app compatibility or hardware-backed attestation.

## Implementation

`SyscallBackend` builds one argument vector for boot and owner console commands.
The tracer receives a clean host environment. Guest platform preloads are set
only after entering the guest. Guest executables use `/system/bin/linker64` and
guest-relative library search paths. The host backing root is never bound into
the guest at its host app-data pathname.

The opt-in `POCKET_PROOT_OWNER=1` tracer extension supplies inode-keyed virtual
ownership, supplementary groups across fork/exec, and alarm-clock timerfds
without host wake capabilities. Host time changes return `EPERM`; host kernel
capabilities are not invented. Failed exec errors are published before extension
callbacks. Creation checks use the current translated entry registers,
not the previous syscall's saved registers; recreated files get fresh ownership
metadata. `PTRACE_O_EXITKILL` prevents tracees from continuing untranslated if
the tracer is killed.

The old relative `/data -> system-data` link is migrated to an empty bind
destination; `system-data` is retained. Nested bindings reverse-map directory
FDs and cwd back to `/data`, rather than exposing the backing alias.

Binder endpoint checks use the real UID in `/proc/self/status`, since traced
`getuid` reports guest credentials. The virtual configstore uses a proper
Bn/Bp HIDL transport for Java clients. Private input and hardware-event channels
avoid accessing host device-event streams. Lazy zygote initialization prepares
fonts and Java security providers under either backend.

The previous optional Magisk adapter depends on the removed Bionic module-view
interception. Version 0.34 preserves its installed files but does not activate
that adapter; `pocket.magisk.status` reports `adapter-required`. Porting its
mount view to this tracer remains separate work. Do not interpret this build
as validated Magisk/NeoZygisk support.

## Focused validation

Native builds use the prepared Android 9 platform headers and NDK r27c:

```sh
python tools/build_proot_backend.py --ndk "$NDK"
python tools/build_syscall_platform.py --ndk "$NDK"
python tools/build_syscall_platform.py --ndk "$NDK" --zygote-only
python tools/build_bionic_mode_probe.py --ndk "$NDK"
make -f runtime/Makefile out/manager-guest/pocket-control.jar
```

The host argument/environment regression passes:

```sh
javac --release 8 -d out/syscall-host-test runtime/app/src/dev/kreijstal/aosppocket/runtime/SyscallBackend.java runtime/tests/SyscallBackendTest.java
java -cp out/syscall-host-test dev.kreijstal.aosppocket.runtime.SyscallBackendTest
```

On the Pi's Android VM, `python3 /home/pi/test_proot_owner_on_pi.py` passes
ownership, credentials, failed exec, timer, host-root canary isolation, and
tracer-death checks. It compares all 858,956 file-backed executable Bionic bytes:
zero differences, with no Bionic shim loaded. See
[the native report](syscall-backend-native-validation.json).

`python3 /home/pi/test_wrapper_free_init_on_pi.py --framework --timeout 120`
reaches `POCKET_CONTROL_READY` in a separate root, then shuts down with zero
remaining processes. This establishes Android framework startup, not complete
peripheral compatibility. Missing audio hardware and unsupported host firewall
operations still produce diagnostics.

That private-launcher test requires the legacy untraced 0.32 owner console.
Under 0.34, run `python3 /home/pi/test_wrapper_free_manager_on_pi.py` and
`python3 /home/pi/test_proot_owner_on_pi.py` instead: they exercise the installed
backend without nesting tracers. Both pass. The installed Manager displays the
Android launcher; see [the report](syscall-backend-manager-validation.json) and
[VM screenshot](images/syscall-backend-manager.png).

Version 0.34 also passes a full restart from the legacy `/data` symlink layout,
with a canary file in persistent guest data preserved. See
[the migration report](syscall-backend-migration-validation.json).

Package a full update from the pinned 0.32 image with:

```sh
python tools/build_wrapper_free_manager.py --base-apk out/manager-0.32/aosp-pocket-manager-0.32.apk
```

The packaging command verifies the APK signature, absence of the old wrapper
payload, and unchanged Bionic payload.

## Pi test incident

An earlier experimental launcher exposed the original Pi Pocket root through
a writable self-bind. Guest PackageManager cleanup deleted that root, including
its guest data. No backup of that previous app state was found. The 0.32 software
image was re-extracted and booted; its previous guest data was not recovered.
The outer LineageOS VM and Samsung instance were unaffected.

The self-bind has been removed. Tests now use private executable copies with
guest-relative interpreters, verify a canary in the original root is inaccessible
and unchanged, and verify tracer death also kills its tracees. Subsequent Manager
testing backs up the restored guest's current data/preferences outside the VM.
