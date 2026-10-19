# Android 9 / API 28 integration

The first integration milestone runs **real Android 9 platform Binder and
bionic libraries in two separate ARM64 guest processes**, launched by an
ordinary host application. It registers a guest service, looks it up, calls it
and transfers a real file descriptor. A second round forwards virtual guest
credentials from a UID 39999 client to a UID 0 server while both retain the
host application's actual kernel UID.

The `0.2-api28-foundations` probe additionally validates **private Android 9
properties and real ART execution**. Read-only properties, cross-process change
notifications, clean shutdown and fresh restart pass. A Java program runs in
ART, exercises threads/reflection/exceptions, and launches a native child that
retains UID 39999 and reads the SDK 28 property area.

This is **runtime bring-up, not a bootable Android 9 guest**. The bootable
runtime continues to use its tested Android 7.1/API 25 image. The init core and
zygote now have rootless integration profiles. System services, launcher and
graphics integration remain unverified.
Hardware bridges are not added here.

## Separate builds and SDK checks

```sh
make -f runtime/Makefile api28-native
make -f runtime/Makefile -j2 api28-verify
make -f runtime/Makefile check-profiles
```

Outputs:

- `out/api28/native/libpocket_binder_api28.so`: source-built Binder bootstrap
  and guest credential header, compiled with `POCKET_GUEST_API=28`.
- `out/api28/native/pocket-api28-probe`: source-built registry/server/client.
- `out/api28/native/pocket-property-service-api28`: source-built rootless
  property service using the real Android 9 bionic shared-memory ABI.
- `out/api28/native/pocket-property-probe-api28`: separate native clients.
- `out/api28/native/pocket-init-api28`: source-built AOSP init core with the
  rootless entry point and shared property backend.
- `out/api28/native/pocket-init-test-api28`: real child services and an independent
  bionic control/observation client.
- `out/api28/native/pocket-app-process-api28` and `libpocket_zygote_api28.so`:
  source-built AOSP app_process and adapted native fork hooks.
- `out/api28/native/pocket-zygote-client-api28`: independent Android 9 zygote
  protocol client. `pocket-log-writer-api28` receives guest liblog diagnostics.
- `out/api28/aosp-pocket-api28-probe.apk`: independent development application,
  package `dev.kreijstal.aosppocket.api28probe`, about 77 MiB with ART assets.
- `out/api28/probe-rootfs`: ELF dependency closure, boot images and metadata;
  deliberately not an importable full guest image.

The new targets do not replace `out/runtime/aosp-pocket-runtime.apk` or its
installed guest. The probe reuses the existing source-built PRoot and host
Parcel-template JNI code. No existing S20 FE/LineageOS build is modified.

Guest SDK selection reads the guest's `system/build.prop`, independently of
the host SDK. Newly packed image metadata records `guest_sdk`; imports check
it against the extracted property. Older format-1 API 25 images remain
compatible. The boot path rejects API 28 before injecting the API 25
`HostBinder` interposer, and unsupported imports cannot replace an installed
guest. API 28 full-image boot will be enabled after its boot milestones pass.

## Sources and ABI

`sources.lock.json` records sparse AOSP `android-9.0.0_r61` Binder/core header
revisions and a pinned ARM64 [Redroid](https://github.com/remote-android/redroid-doc)
OCI image digest. The source manifest reference is
[`redroid-9.0.0`](https://github.com/remote-android/platform_manifests/tree/2955865a1ef309aae6b5280bd035d47a75115a04).

`tools/prepare_api28.py` downloads/extracts selected regular files without
starting Docker or executing the image. These Android 9 platform libraries
are **upstream prebuilts**, not locally source-built Android. Their ABI is
used for this bring-up; the Redroid image requires adaptations for rootless
Android-on-Android use. License notices accompany the probe APK.

The bridge links with `--no-undefined` against those actual libraries and the
platform C++ namespace `std::__1`, rather than accidentally using the NDK's
`std::__ndk1`. API 25 and API 28 bridges have separate source/build paths.
The new bridge does not depend on Ananbox's private API 25 `HostBinder` class.

The Android 9 bionic reference and property-info parser/serializer source are
also pinned. The property service builds the actual AOSP serialized property
trie and calls Android 9 libc's property-area/add/update APIs. Its socket
speaks the legacy and v2 property write protocols; test clients use the public
bionic read/set/wait APIs. Builds use the pinned Android 9 libc++ headers with
`-nostdinc++`, matching the platform library ABI, including init's C++17
variants and stream-based errors. Modern NDK libc++ headers are not substituted.

## Binder path

1. The host app constructs broadcast and service-lookup Parcel templates using
   its own framework. Only its package-scoped receiver receives the guest's
   manager/client handoffs.
2. The API 28 manager publishes a normal Binder object to that receiver. It
   does not issue `BINDER_SET_CONTEXT_MGR` or replace the host service manager.
3. The API 28 `ProcessState::getContextObject` interposer obtains this object
   through a callback. It uses Binder pool threads plus a bounded condition
   wait, avoiding the API 25 upstream polling/thread-pool race.
4. The probe implements the Android 9 `IServiceManager` wire operations needed
   for registration, lookup and listing. This is a minimal bring-up registry,
   not a production service manager with complete policy/death handling.
5. The client discovers the echo service through the real API 28
   `defaultServiceManager()` / `IServiceManager` implementation. A Binder call
   carries a file descriptor; the server reads and validates its bytes and
   returns its PID, UID and observed caller identity.

The bridge rebuilds template Binder objects through public Parcel methods so
the API 28 library owns their references. Host-specific stability trailers
remain in the template. Callback interface-header offsets are measured from
the actual host Parcel. The host service-lookup reply is parsed conditionally
for pre-AIDL and AIDL service-manager layouts; only host API 36 has been run.
Other Binder drivers and non-default named contexts are explicitly rejected;
HIDL/hwbinder and vndbinder are unresolved work, not silently aliased.

For guest-to-guest calls, the bridge ports the existing open-source Ananbox
approach: a virtual UID/PID prefix in interface tokens, restored using Binder's
caller-identity mechanism. Host bootstrap messages retain host-generated
headers. This changes the guest wire protocol, so every participating guest
process must load the same bridge. Virtual identities are **not authenticated
security principals**; all guests still share the host app UID/kernel.

## Private properties and ART

Every probe process binds its own `/dev/__properties__` and `/dev/socket` over
the host paths. The property service imports the reference `system/build.prop`
and advertises property-service protocol version 2. Fresh clients execute with
guest UID 39999. They verify SDK 28, rejection of read-only updates and invalid
names, rejection of `ctl.start`, shared-memory update notifications, and fresh
seed state after a clean service shutdown/restart. Host SDK properties remain
unchanged. The minimal service does not implement AOSP init actions, persistent
properties, property types or per-guest SELinux authorization.

All clients need PRoot's metadata translation, including the round that keeps
the real application UID. Otherwise bionic rejects app-owned property files
because it expects root ownership. Host file ownership never changes. An
older bind-only probe left a mode-000 guest `/dev` placeholder; setup now
repairs that directory before creating private sockets/properties.

On the tested enforcing VM, bionic cannot relabel private app files to Android
property SELinux types. `__system_property_area_init()` therefore returns -1
after successfully creating the area. The service verifies a real add/read
path instead of treating that return alone as successful initialization. The
recorded logs show both xattr failures and successful property operations.
Guest property-context SELinux separation is not provided; host app-data
isolation and enforcing SELinux remain in force. Peer checks use the actual
kernel UID from `SO_PEERCRED`, not the guest's emulated UID.

The ART test uses the pinned image's `dalvikvm64`, `libart`, core JNI libraries,
matched ARM64 boot images/VDEX files, boot-classpath JARs, ICU data and public
native-library namespace. These remain **upstream prebuilt binaries**, not a
locally rebuilt ART. The small Java test is built from `runtime/api28/ArtProbe.java`.
It runs with `-Xint` and does not establish JIT, zygote, Android application or
framework UI compatibility. Host boot-classpath/APEX environment variables are
removed before guest execution.

Two observed startup failures were fixed by including required reference data:
missing `event-log-tags` caused a `libmetricslogger` constructor to dereference
a null event-tag map, and missing `public.libraries.txt` aborted NativeLoader
initialization. The selected native namespace libraries and their dependency
closure are now included. A bounded PRoot diagnostic, enabled only by
`POCKET_DEBUG_SIGNALS` for the ART test, records ARM64 PC/LR/SP and frame pointers
without changing signal delivery. It is off for the normal API 25 runtime.

## VM validation

```sh
scp out/api28/aosp-pocket-api28-probe.apk pi@juanraspberry:/home/pi/
ssh pi@juanraspberry python3 - < tools/validate_api28_on_pi.py
```

The harness uses the existing virtual-device guard and dedicated ADB server.
It installs only the separate probe package. Root is used by the harness to
read the probe's result/status, not to execute its guest programs. The probe
itself runs the manager/client as children of an ordinary application.

Results are recorded in [api28-vm-validation.json](api28-vm-validation.json):
two distinct guest process IDs, successful registration/lookup/transaction,
FD contents `api28fd`, correct real and virtual caller identities, host app UID,
SELinux domain/mode and seccomp mode. It also records
`full_guest_boot_verified: false`.

The subsequent properties/ART milestone is recorded separately in
[api28-properties-art-validation.json](api28-properties-art-validation.json).
This preserves the earlier Binder-only evidence. The expanded probe runs the
Binder tests against the **private SDK 28 property area**, rather than reading
host SDK 36 properties, before testing ART and clean property restart.

The platform reference libraries have not been rebuilt for 16 KiB pages.
The test VM uses 4 KiB pages. Physical Samsung execution and older host Android
versions remain unverified. Package/ABI checks do not substitute for these
runtime tests.

## VMOS reference analysis

The existing Rizin project was reopened without `aaa` to inspect Android 9
Binder compatibility points in the already acquired VMOS guest library:

```sh
rizin -q -p out/research/vmos/analysis/guest-libbinder.rzdb \
  -c 'e scr.color=0;pd 18 @ 0x72dfc;pd 22 @ 0x7c48c;pd 40 @ 0x93484;q'
```

Static observations: `getCallingUid()` checks thread-local state before
reading its caller-UID member, and the ordinary `android::Parcel` token writer
sets the Android 9 strict-mode gather bit before writing its descriptor.
This does not recover VMOS's entire credential/transport mechanism. The local
listing is `out/research/vmos/analysis/api28-binder-compatibility.txt`.
Our guest credential extension uses the inspectable open-source Ananbox
approach; no VMOS implementation, image or library is included in the build.

A read-only listing of the VMOS image confirms it also supplies `dalvikvm64`,
`event-log-tags`, `public.libraries.txt` and `init.environ.rc`. The listing is
`out/research/vmos/analysis/api28-runtime-files.txt`. This is only a packaging
comparison, not execution or proof of its ART initialization mechanism. The
files bundled here come from the pinned open-source Android/Redroid reference.

## Rootless init integration

The `0.3-api28-init` development APK additionally runs the real AOSP
`android-9.0.0_r61` init core. `tools/build_api28.py` copies the pinned upstream
`system/core/init` directory into `out/api28/rootless-init`, applies
[`init-rootless.patch`](../runtime/api28/init-rootless.patch), and compiles:

- `parser.cpp`, `tokenizer.cpp`, `import_parser.cpp`, `action_parser.cpp`;
- `action.cpp`, `action_manager.cpp`;
- `service.cpp`, `descriptors.cpp`, `rlimit_parser.cpp`, `util.cpp`.

[`init_main.cpp`](../runtime/api28/init_main.cpp) provides the ordinary-process
entry point, property/control dispatch and event loop. AOSP's `Service` class
forks/executes children, publishes `init.svc.*` and `ro.boottime.*`, handles
oneshot/disabled services, executes `onrestart`, and manages restart states.
The entry point uses AOSP's five-second restart interval and keeps a zombie's
PID allocated with `waitid(WNOWAIT)` until `Service::Reap` finishes signalling
the process group. Children are stopped in reverse start order and reaped on
shutdown, with a bounded SIGTERM grace period before SIGKILL.

The property daemon and init share one backend and one private Android 9
property area. Property writes queue real AOSP actions. `ctl.start`, `ctl.stop`
and `ctl.restart` resolve only this process's service list. The standalone
property daemon continues to reject all control messages. Neither backend
forwards power control or SELinux requests to host init.

This is a **rootless init profile, not the entire AOSP init executable**.
It is not PID 1 and cannot adopt every descendant. It keeps the real host app
UID, SELinux domain and supplementary groups; guest UID/GID changes use PRoot.
AOSP's cgroup setup is replaced with POSIX child process groups. Service logs
inherit the APK's redirected output. SELinux domain transitions, capabilities,
Linux namespaces, console, cgroup settings, supplementary group changes and
unimplemented builtins are rejected. The supported service options are
`class`, `disabled`, `group` (one primary GID), `oneshot`, `onrestart`, `override`,
`rlimit`, `setenv`, `socket` (without a SELinux context), and `user`. Supported
builtins are `setprop`, `trigger`, `start`, `stop` and `class_start`.

The upstream parser normally logs some imported-file errors without returning
them to its caller. The patch accumulates these errors so the rootless entry
point refuses the entire configuration before any service starts. It never
loads the reference image's stock `init.rc` or reports `sys.boot_completed`.

The integration scenario in [`init.pocket.rc`](../runtime/api28/init.pocket.rc)
exercises import, ordered early-init/init/boot events, event/property conjunctions,
property-only triggers and argument expansion. A real UID-39999 oneshot checks
its environment and inherited Unix socket. A daemon exits with status 42,
automatically restarts, forks a descendant, and is explicitly restarted again.
The separate client starts/stops a disabled service and checks unknown/control
rejection. Shutdown checks stopped states, removed sockets and descendant death.
The PRoot adapter preserves emulated UID capabilities across GID changes and
updates real/effective/saved guest IDs before dropping UID capabilities. The
oneshot regression verifies all three IDs and that returning to virtual root
is denied after the drop. Actual host UID/SELinux/seccomp remain unchanged.
A negative imported configuration requests capabilities and a mount; it must
fail parsing and launch no child. All earlier Binder, properties and ART cases
run in the same APK before this scenario.

[VM evidence](api28-init-validation.json) records a successful run under real
host UID 10171, enforcing `untrusted_app_27` SELinux and seccomp mode 2.
The daemon's status-42 exit, two restart generations, inherited socket and
oneshot checks, five child reaps, stopped states and removed descendant are
captured in the logs. The API 25 host PID 17501 was checked separately and
remained running during the experiment.

Exact validation commands:

```sh
make -f runtime/Makefile -j2 api28-verify check-profiles
ssh pi@juanraspberry python3 - < tools/validate_api28_on_pi.py
```

Both passed. The VM-side helper uses the instance's dedicated ADB server on
port 5043, never a default server or physical-phone connection.

## Real zygote integration

The `0.4-api28-zygote` probe starts the real Android 9 `ZygoteInit`,
`ZygoteServer` and `ZygoteConnection` from the reference boot image through
the tested rootless init core. It compiles AOSP `app_process`, native fork
hooks, descriptor checks and ART's signal-chain implementation from pinned
`android-9.0.0_r61` sources. The
[`zygote-rootless.patch`](../runtime/api28/zygote-rootless.patch) replaces
privileged platform setup with the explicitly limited rootless profile.
It keeps the AOSP fork, runtime pre/post-fork hooks, descriptor checks,
signal handling, child reaping and guest `setresgid`/`setresuid` transitions.

[`init.zygote.rc`](../runtime/api28/init.zygote.rc) starts the private Binder
registry, a diagnostic `logdw` datagram receiver and app_process with an
inherited zygote socket. The receiver captures real liblog messages and kernel
sender credentials; it is not a full logd service. An independent native client
uses the Android 9 argument-count/newline protocol, queries `arm64-v8a`, and
first requests unsupported external-storage setup. That request must return
PID -1 without killing zygote. A supported request forks a real Java child as
guest UID/GID 39999. The child runs a Java thread, launches a native Binder
echo client and verifies that returning to virtual UID 0 fails with EPERM.

The host app independently reads the child's `/proc` identity and SELinux
context, checks its real parent against the socket peer, and rejects stale
child output using a fresh nonce. The VM harness also checks all four kernel
UIDs, zero effective capabilities and inherited seccomp mode 2. The host app
then terminates the verified child; zygote reaps it and must answer a fresh
ABI query. Init stops all three services with no remaining managed children.
[Recorded full-regression evidence](api28-zygote-validation.json) includes
the preceding Binder/property/ART/init cases and these zygote checks.

The profile uses interpreted ART, the pinned `/system` boot images and
`--enable-lazy-preload`. It binds a separate cache-free `/data` and deliberately
does not create a global dalvik-cache: compiler-cache generation, relocation
and full class preloading are not implemented. Android 9's image loader does
not append an arbitrary JAR through `BOOTCLASSPATH`; a supported JVMTI agent
adds the project's bootstrap/test JAR to the boot loader, then installs the
adapted native hooks and invokes the real AOSP entry point. The JVMTI plugin
makes this a debuggable development runtime. JDWP transport is disabled.

Java's Android 9 standard descriptors duplicate stdin/stdout/stderr. The
supervisor supplies null stdin and an explicit guest `/pocket-console` log
path, with append-mode output. Only that console path is added to AOSP's
descriptor whitelist; unsupported descriptors still fail the original checks.

System-server, child-zygote, supplementary-group, nonzero-capability,
SELinux-context and external-storage specialization requests are rejected
before fork. No kernel namespaces, cgroups, domain transitions or native bridge
are created. Guest IDs are PRoot metadata; all processes keep the real host
app UID and its SELinux/seccomp confinement. They share that sandbox rather
than gaining security isolation from one another.

AOSP's zygote connection moves a child to the requesting client's POSIX process
group. The regression explicitly terminates its child while zygote can reap it;
generic ownership and shutdown of arbitrary app descendants remains a boot
gate. This milestone does not start system_server, render a launcher, install
APKs or enable API 28 full-image imports in Pocket Manager. The API 25 guest
remains separate. A read-only check after this regression confirmed its host
PID 17501, PRoot PID 17995, init PID 18007, system-server PID 18325 and launcher
PID 19301 were still running.

Run the full regression with the validation commands above, or isolate zygote
iteration with:

```sh
ssh pi@juanraspberry python3 - --zygote-only < tools/validate_api28_on_pi.py
```

The [Ananbox fork comparison](ananbox-fork-review.md) records useful upstream
guest-management and Binder lifecycle work; the inspected fork does not
provide an Android 9 port or a different execution backend.

## Experimental direct backend

The independent `0.1-direct` APK, package
`dev.kreijstal.aosppocket.directprobe`, tests a source-controlled native ABI
without PRoot. It does not modify Pocket Manager, the installed API 25 guest,
or the API 28 init/zygote probe. It incorporates no VMOS code or YLINKER binary.

[`guest_paths.c`](../runtime/direct/guest_paths.c) implements GNU link-time
`--wrap` targets for controlled callers of `open/openat`, `stat/lstat/fstatat`,
`readlink/readlinkat`, `mkdir/mkdirat`, `unlink/unlinkat/rmdir`,
`symlink/symlinkat`, `chdir/fchdir/getcwd`, and `execve`. The build tool passes
the same wrapper list on Linux and Android. The adapter issues real kernel
syscalls in the guest process; no tracer, signal trap or seccomp filter is
installed. It preserves real host credentials and does not emulate guest root.

Absolute paths resolve inside `POCKET_DIRECT_ROOT`. Relative paths derive from
the kernel cwd or an actual directory FD. Resolution walks directory FDs,
interprets absolute symlink contents relative to the guest root, follows
relative links in order before applying `..`, clamps `..` at root and caps
symlink traversal at 40. Foreign directory FDs fail with EXDEV. Unsupported
metadata flags, O_TMPFILE and trailing-slash removal/link creation fail
explicitly. The regression covers creation, reading, metadata, symlink loops,
O_NOFOLLOW, missing paths, non-directory errors, nested symlink/.. ordering,
directory-relative operations, cwd buffer limits and a reader thread.

The Android APK bundles the pinned API 28 `linker64`, `libc.so`, `libdl.so` and
`libm.so` already used by the other probe. Android 9's linker does not provide
the newer command-line execution interface. The controlled executable reserves
512 bytes in PT_INTERP; extraction writes the app-private linker path there.
The comparison copy instead names `/system/bin/linker64` inside PRoot. Both
copies originate from the same built executable. A private linker configuration
and explicit library path select only the bundled libraries. The reference
loader's development `LD_CONFIG_FILE` feature supplies this configuration;
a production loader build will need an equivalent supported setup.

A real forked child execs `/system/bin/pocket-direct-probe`. Its caller supplies
a fresh environment omitting the runtime configuration. The adapter preserves
the guest root, private library path and linker configuration, while retaining
the caller's marker. The child checks its cwd, reads through a guest symlink
and reports real UID/tracer identity. The parent waits for its successful exit.
The host APK separately inspects the parent's `/proc` UID, TracerPid, SELinux,
seccomp and mappings while it waits on a private release file.

[VM evidence](direct-backend-validation.json) records real UID 10172,
`TracerPid=0`, enforcing `untrusted_app_27` and seccomp 2 in direct mode. The
maps show the private Android 9 loader, libc and libdl with no host shared
libraries. The PRoot comparison retains the same real UID and confinement but
has a nonzero tracer. It runs with `PROOT_NO_SECCOMP=1`, matching our current
working full-ptrace backend. [Linux evidence](direct-backend-host-validation.txt)
records the same path/thread/fork/exec tests under native x86-64 glibc. This
demonstrates portability of the adapter, not ARM64 execution on x86 without
CPU translation.

The benchmark times only 400 repetitions of open/read/close/stat on the same
small fixture. Three rounds alternate the baseline/direct/PRoot order. The
baseline uses pinned Android 9 bionic and a real private host pathname without
path translation. Timing excludes process startup and is a small VM workload,
not an Android boot, graphics or application benchmark. Raw per-round durations
are retained in the evidence; results must not be generalized to all workloads.

Recorded median durations for those 400 iterations:

| Mode | Duration |
|---|---:|
| Untranslated path baseline | 9.59 ms |
| Direct adapter | 24.88 ms |
| Full-ptrace PRoot | 447.28 ms |

The direct adapter was about 18.0 times faster than PRoot and 2.6 times slower
than the untranslated baseline in this run.

This is **not a complete YLINKER replacement**. Link-time wrappers cover calls
from objects built with these flags. They do not automatically redirect libc's
internal calls, arbitrary shared libraries, inline syscalls or runtime-generated
code. A negative test deliberately demonstrates that a raw syscall is outside
the path ABI. Android 9 libc startup currently reads permitted host property
areas; this backend has no private property area, virtual UID, Binder or HAL
integration. The namespace configuration restricts library lookup, not kernel
permissions. All processes share the host app sandbox; this is not an isolation
boundary against hostile guest code or filesystem races.

Build and validate:

```sh
make -f runtime/Makefile -j2 direct-verify direct-check-host
scp out/direct/aosp-pocket-direct-probe.apk pi@juanraspberry:/home/pi/aosp-pocket-direct-probe.apk
ssh pi@juanraspberry python3 - < tools/validate_direct_on_pi.py
```

Output is `out/direct/aosp-pocket-direct-probe.apk`, approximately 1.6 MiB.
The next direct-backend milestone is to route controlled platform/bionic
operations through the adapter, then port the already tested property/Binder
and init/zygote paths. Keep PRoot available until equivalent regressions pass.

## Next boot gates

1. Extend the tested zygote profile toward system-server startup. Implement
   required service/socket/property behavior and generic descendant lifecycle
   ownership. The current profile deliberately rejects system-server requests.
2. Add manager death/restart and nested Binder regression cases before treating
   the private registry as a general framework IPC layer.
3. Adapt Android 9 graphics/HAL transports and linker namespace configuration,
   then demonstrate launcher display, APK installation and forwarded touch.
4. Enable an explicitly versioned API 28 image/profile in Pocket Manager only
   after those real boot gates pass; retain the working API 25 profile.
