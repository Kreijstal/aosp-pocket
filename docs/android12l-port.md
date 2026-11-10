# Android 12L bring-up

The API 32 port is in development. The experimental Manager can select and
download it. Image0.78.47 with freshly source-built host libraries passes the dedicated Pi tests for a validated network,
public asynchronous DNS, unmodified Play Signal startup, Home/reopen and clean
shutdown. See the [current Signal evidence](api32-manager-signal-validation.json).
It is not yet offered by the default production catalog:
`full_guest_boot_verified` and `pocket_manager_api32_supported` remain false in
`sources.lock.json`. Image0.78.37 now passes the real framework mounted-volume lifecycle,
ordinary file/MediaStore coherence, cross-app private-storage policy and
ordinary app/software Keystore persistence across an offline outer Manager
restart. See the [mounted storage evidence](api32-volume-validation.json).
Physical keyguard unlock, encrypted mounted storage and unchanged app/software
Keystore now pass two strict cycles across an offline outer Manager restart.
See the [encrypted restart evidence](api32-keyguard-validation.json).
The preserved profile also passes the full six-cycle credential lifecycle,
including password changes/removal/restoration and return to its original
password-free state. An ordinary app also passes real AAC/AVC compression and
decode, plus EGL Surface-input AVC encoding with decoded pixels and timestamps,
twice across Home/reopen; see the [codec evidence](api32-codec-validation.json).
Image44 also passes actual decoder Surface output through SurfaceTexture and
EGL pixel readback twice, with every frame's sampled RGB values and presentation
timestamp checked.
Image 0.78.38 passes ordinary playback and microphone capture twice, including
real host PCM transport, capture timestamps and clean stream closure. Camera
enumeration exposed a startup-order defect fixed in 0.78.40, followed by an
ImageReader YUV mapping crash fixed in 0.78.42. The complete 0.78.42 image now
passes ordinary Camera2 YUV preview and decoded JPEG capture twice alongside
playback/recording across Home/reopen on the Pi virtual host. Host camera
permission granted after boot now passes metadata/403 capture-denial and
subsequent full AV checks without restarting the guest. Both advertised cameras now pass sequential preview/JPEG capture before and
after Home/reopen. See the [AV evidence](api32-av-validation.json).
Both cameras also pass MediaRecorder AVC video plus microphone AAC in MP4,
with decoded frames/PCM and overlapping timelines before and after Home/reopen.
Fresh-profile/migration reliability, default process budget, physical media and
phone, legacy and release checks remain incomplete.
Android 9/10 profiles and their data remain separate.
The new host Manager also passes the complete AV trial on image43 using the
existing CLI. Image44 moves finite owner commands into the authenticated
system-server control service and passes both ordinary AV rounds, invalid
request/guest-peer rejection and strict cleanup across three starts, with sampled
peaks32/32/31. Signal and its network/security checks also pass functionally,
but sample33, and a later full muxed-media trial samples35, so the budget gate
remains open. See the
[owner-command evidence](api32-embedded-manager-validation.json).

## Reference and source versions

`tools/prepare_api32.py` verifies and extracts Google's vanilla ARM64 API 32 SDK
image, revision 2, without mounting it. Its Android 12L build is
`SE1B.240122.005`, incremental `11418786`, security patch `2024-01-01`.
The archive SHA-256 is
`cee6aab64d859bb9a0b93628638c46e36d5e1c8364410f53a5e74f197b9fa9e6`.

Native adapters use sources pinned to `android-platform-12.1.0_r23`. This is a
version/security-aligned source baseline, not the exact emulator build manifest.
The stock Bionic, ART and framework payloads retain their original contents;
the port builds the rootless integration separately. Namespace generation uses
the stock linkerconfig and flattened APEX metadata. The fixture explicitly
exports the ART namespace required by boot JNI and clears its temporary library
search override before running ART.

## Verified gates

The dedicated Pi test VM is instance 73, with fresh data and its own Android
host. The runner rejects other instances. These tests run inside a separate,
non-debuggable Android APK under its ordinary host app UID. No phone test is
claimed.

| Gate | Evidence |
| --- | --- |
| Native loader and API 32 properties | [Native validation](api32-native-validation.json) |
| Real 12L ART and an API 32 framework class | [ART validation](api32-art-validation.json), [Android sandbox validation](api32-android-validation.json) |
| Typed Binder registry, descriptor transfer, synchronous and one-way logical caller identity, WorkSource | [Android sandbox validation](api32-android-validation.json) |
| Stock Java ZygoteConnection fork, logical UID/GID 39999, groups 1000/3003, thread creation, preserved priority and subsequent parent command | [Zygote validation](api32-zygote-validation.json) |
| Real 12L init managing Binder and zygote, service controls, crash recovery, timeouts, invalid configuration rejection and shutdown | [Init validation](api32-init-validation.json) |
| Private HIDL 1.2 manager, remote health service with actual host battery data, host wake-lock acquire/release, coexistence with the typed native registry | [Hardware validation](api32-hardware-validation.json) |
| Native suspend-control V1 callbacks, host-backed partial wake-lock timing/PID stats, duplicate-release handling and ordinary-role denial; host kernel controls unavailable | [Suspend validation](api32-suspend-validation.json) |
| Typed 12L APEX inventory, 23 module manifests, individual lookups, unknown sessions, update rejection and ordinary-role denial | [APEX validation](api32-apex-validation.json) |
| Lazy preload of stock 12L JCA providers and system font map, inherited providers, AES-GCM round trip and child font measurement | [Combined preload validation](api32-preload-validation.json) |
| Stock 12L installd typed app-data lifecycle, protected-call denial, and boot-time dexopt with real ODEX/VDEX outputs | [Combined installd validation](api32-installd-validation.json) |
| Matching 12L input reader, EventHub polling/wake pipe, actual host wake-lock release after cleanup and client death, absent kernel suspend capability | [Combined input validation](api32-input-validation.json) |
| Real framework display, stats, sensor and scheduling HIDL registrations and client lookups/pings; undeclared/mismatched service rejection and ordinary-role denial | [Framework HIDL validation](api32-framework-hidl-validation.json) |
| Typed 12L netd, network-record lifecycle, role controls, private fwmark handling and real UID UDP/socket permission enforcement | [Network validation](api32-network-validation.json) |
| Typed resolver control, real Bionic DNS and external TCP, permission denial, typed query events and exited listener cleanup | [DNS validation](api32-dns-validation.json) |
| Stock mDNS frontend/responder, real registration callback, daemon lifecycle and role denial | [mDNS validation](api32-mdns-validation.json) |
| Stock audio services, matching HIDL factories, real host PCM writes and ordinary-role denial | [Audio validation](api32-audio-validation.json) |
| Private device-event channel and stock observer JNI setup, matching, removal and field parsing | [UEvent validation](api32-uevent-validation.json) |
| Shared APEX device-data directories and system-role file write/read | [APEX data validation](api32-apex-data-validation.json) |
| Typed IVold/IStoraged, real user-data preparation, reset persistence and role denial | [Storage validation](api32-storage-validation.json) |
| Stock VINTF parser on a future host kernel; real framework boot property | [VINTF validation](api32-vintf-validation.json) |
| Zygote process UID lookup with original kernel measurements retained | [Process identity validation](api32-process-identity-validation.json) |
| Software Keymaster, stock keystore2, real JCA crypto and user 0 unlock | [Keystore validation](api32-keystore-validation.json) |
| Actual Manager download, normal service startup, host frame presentation and physical input | [Manager session validation](api32-manager-session-validation.json) |
| Private app state and AndroidKeystore decryption across Manager restart | [Manager persistence validation](api32-manager-persistence-validation.json) |
| Guest default network, real NetworkMonitor VALIDATED and network-bound HTTPS | [Validated uplink](api32-raw-dns-validation.json) |
| Ordinary-app public asynchronous DNS, positive/NXDOMAIN answers and permission revocation | [Raw DNS validation](api32-raw-dns-validation.json) |
| Unmodified Play Signal requiring API32, actual foreground launch, Home and reopen | [Manager Signal validation](api32-manager-signal-validation.json), [network-integrated rerun](api32-raw-dns-validation.json) |
| Ordinary app AAC/AVC encode/decode, decoded signal and twelve frame pixels, fresh workload after Home/reopen | [Codec validation](api32-codec-validation.json) |
| Ordinary app Camera2 YUV/JPEG capture, playback/recording repeated after Home/reopen, host PCM transport and clean microphone peer closure on Pi virtual host | [AV validation](api32-av-validation.json) |
| Five-root encrypted namespace, physical keyguard unlock, mounted media/app/software key persistence across offline Manager restart, plus legacy transport regression | [CE validation](api32-ce-storage-validation.json), [current encrypted restart](api32-keyguard-validation.json) |

The rootless zygote installs JNI compiled from the matching 12L source before
entering the stock `ZygoteInit`. It retains file descriptor validation and the
ART pre/post-fork hooks. Kernel mounts, capabilities, SELinux transitions and
privileged UID seccomp installation cannot be provided under the outer app UID.
Logical guest credentials do not create host security principals.

On the Android 16 validation host, libcutils could open the host kernel's
`trace_marker` but could not `fstat` it. This caused mandatory zygote descriptor
validation to abort. The API 32 integration disables libcutils kernel tracing
and closes its early tracing descriptor. It does not bypass descriptor checks.
The fixture captures guest liblog datagrams through its private `logdw` socket.

## Init integration

`runtime/api32/init-rootless.patch` adapts the pinned 12L parser, action queue,
service parser and service lifecycle. `init_main.cpp` supplies the rootless
supervisor entry point and supported builtins. It is not PID 1.

Property updates made by init itself go directly to its property implementation
to avoid calling back into its own single-threaded property socket. Services
receive explicit logical supplementary groups and real private Unix sockets.
The supervisor establishes owned POSIX process groups and performs service
cleanup before reaping the child's reserved PID. Kernel namespaces, capability
requests and unsupported service options are rejected during configuration
parsing, before any boot action starts.

The integration test exercises imported/property-triggered actions, argument
expansion, disabled and oneshot services, a real crash/restart, explicit control
commands, supplementary groups, a timeout and descendant cleanup. With
`--binder --zygote --init`, the real init service lifecycle launches the Binder
registry and stock Java zygote. The combined gate passed on the Pi in a
non-debuggable app with host UID 10154. Init reaped eight service lifetimes and
reported zero remaining children; the independent client also confirmed that
the service sockets and test grandchild were gone.

Rebuild and run this gate:

```sh
python3 tools/build_api32_native.py --ndk "$NDK" --init
python3 tools/build_api32_native.py --ndk "$NDK" --binder
python3 tools/build_api32_native.py --ndk "$NDK" --zygote
python3 tools/build_api32_android_probe.py --ndk "$NDK" --binder --zygote --init --reuse-native
rsync -a out/api32/android-probe/api32-probe.apk out/api32/android-probe/apk-build.json tools/test_api32_on_pi.py pi@juanraspberry:/home/pi/pocket-api32-test/
ssh pi@juanraspberry 'python3 /home/pi/pocket-api32-test/test_api32_on_pi.py --apk /home/pi/pocket-api32-test/api32-probe.apk --binder --zygote --init'
```

`--reuse-native` requires the requested adapters to have been built already.
The APK builder normally rebuilds them. A test-only thin APK update requires an
existing fixture and verifies every guest input hash; it cannot provision a
new installation. Test reports are retrieved through the APK's activity dump,
without rooting the Android host.

## Hardware service transport

Android 12L's typed native service registry and its HIDL service endpoint use
separate package-scoped host Binder handoffs. Publishing hardware services does
not replace the native registry. The host receiver retains the original handoff
keys for the Android 9/10 profiles.

The 12L adapter builds against headers generated by the pinned 12L `hidl-gen`
and the matching HIDL libraries. The initial service profile supplies the HIDL
1.2 manager, health 2.0, configstore 1.1 and system suspend 1.0 interfaces.
Unknown HALs remain unavailable. There is no fallback to a host/global hardware
service manager.

The Pi test validated a remote health result against the host BatteryManager
relay, and observed a host partial wake lock held and then released through the
suspend interface. It then repeated the native typed-registry checks after
hardware publication, followed by a Java zygote fork. The Android 9 and Android
10 native hardware builds also passed with the shared transport changes.

Build and validate the hardware gate by adding these steps/options to the init
commands above:

```sh
python3 tools/build_api32_framework.py --ndk "$NDK"
python3 tools/build_api32_android_probe.py --ndk "$NDK" --binder --zygote --init --hardware --reuse-native
```

After transferring the generated APK and metadata, append `--hardware` to the
Pi runner command.

## APEX inventory

The read-only APEX service uses headers generated from the pinned 12L AIDL and
the reference image's `apex_aidl_interface-cpp.so`. It does not reuse the
Android 9/10 transaction layout. Module names, versions and preinstalled source
package paths come from the verified reference image. The active module paths
refer to the flattened guest directories; their binary manifests are checked
before the service returns inventory.

The Pi test decoded all 23 modules through the stock typed proxy, compared
individual lookups with the complete inventory, and verified empty/unknown
session results. Mutable APEX operations are explicitly unsupported in this
image-managed profile. The service rejects inventory access by an ordinary
logical guest role. The boot notification records only the APEX service's
notification; it does not set `sys.boot_completed`.

```sh
python3 tools/build_api32_framework.py --ndk "$NDK" --apex
python3 tools/build_api32_android_probe.py --ndk "$NDK" --binder --zygote --init --hardware --apex --reuse-native
```

After transfer, append `--hardware --apex` to the Pi runner command. These tests
still do not boot `system_server` or establish a usable Android application
environment.

## Lazy zygote preload

The 12L bootstrap installs the stock
`android.security.keystore2.AndroidKeyStoreProvider` and its BC workaround when
lazy preload is selected. This registers the real provider; it does not yet
verify keystore2 service operations or hardware-backed keys.

Android 12L enables lazy typeface initialization, leaving `Typeface.DEFAULT`
unset until a font map is installed. The bootstrap calls the stock
`Typeface.loadPreinstalledSystemFontMap()` before measuring text. It does not
replace the public typefaces with fabricated values.

The combined Pi run passed all preceding gates and a real zygote child that
inherited the providers, created a thread, encrypted/decrypted data with
AES-GCM, and measured text using the installed fonts. Init reported ten reaped
service lifetimes and no remaining children. Its APK SHA-256 is recorded in the
linked report. Full framework and Manager support remain unverified.

## Stock installd and boot-time dexopt

The profile runs the reference image's stock `installd` binary. Its client uses
C++ interfaces generated from the pinned 12L `IInstalld` AIDL, including the
structured `CreateAppDataArgs` and per-item `CreateAppDataResult` types.

The combined Pi test created two packages in CE and DE directory views,
checked logical ownership, modes, cache groups and returned inodes, rejected
an invalid package name, decoded app sizes, cleared payloads while retaining
the app directories, then destroyed the data and profiles. An ordinary guest
UID was denied the protected `invalidateMounts` operation. The public quota
query is not a protected operation in 12L.

The service receives `ASEC_MOUNTPOINT=/mnt/asec`, required by its stock startup.
Its process-local integration reports no independent guest SELinux policy and
uses libselinux's disabled-policy restorecon behavior. It does not relabel app
files or modify the host policy. The adapter requires the active guest loader
and SDK 32. Host app SELinux restrictions remain enforced.

Compiler workers execute without the service's Binder/policy preloads, which
would otherwise pull platform dependencies across ART's stock linker namespace.
Other environment entries and unrelated preloads are retained. The profile
selects `dex2oat64` through `dalvik.vm.dex2oat64.enabled=1`, matching the reference
image's 64-bit emulator configuration.

The test gave stock `dex2oat64` a real smoke APK through typed `dexopt`, using
the boot-time `verify` filter. It checked real ELF ODEX and VDEX outputs and
removed the temporary code directory through `installd`. It does not yet
provide post-boot `Dex2OatBootComplete` task profiles or claim full package
installation. CE/DE directory operations do not establish FBE encryption.

```sh
make -f runtime/Makefile out/runtime/guest-test.apk
python3 tools/build_api32_framework.py --ndk "$NDK" --installd
python3 tools/build_api32_android_probe.py --ndk "$NDK" --binder --zygote --init --hardware --apex --installd --reuse-native
rsync -a out/api32/android-probe/api32-probe.apk out/api32/android-probe/apk-build.json tools/test_api32_on_pi.py pi@juanraspberry:/home/pi/pocket-api32-test/
ssh -o BatchMode=yes pi@juanraspberry 'python3 /home/pi/pocket-api32-test/test_api32_on_pi.py --apk /home/pi/pocket-api32-test/api32-probe.apk --binder --zygote --init --hardware --apex --installd'
```

All preceding gates passed in this combined run. Init reaped eleven service
lifetimes and reported no remaining children or lifecycle errors. Full
`system_server`, graphics and Manager/image integration remain unfinished.

## Normal system-server bring-up

`--system-server` adds a normal `--start-system-server` zygote service after
the component checks. It uses the stock framework JARs and native server
library. Its observer checks Android's own `sys.boot_completed` and live
activity/package/window Binder objects. The observer never sets boot completion.

The [first normal launch](api32-framework-bringup.json) entered the real
`SystemServer` and initialized ActivityManager, PowerManager and other bootstrap
services. It stopped in the DisplayManager constructor while waiting for
SurfaceFlinger at `SurfaceControl.nativeGetCompositionDataspaces`; the normal
Android watchdog subsequently killed system_server. This is startup evidence,
not a completed framework boot.

## Software graphics implementation

`tools/build_api32_graphics.py` builds SwiftShader's OpenGL ES backend from the
pinned 12L source revision, using the platform native-window/gralloc path. The
CMake integration selects that path, supplies Android logging through the C
API, and retains Android's GLES 1 library name. The driver's private C++ runtime
is linked statically; its platform boundary uses native-window/gralloc C APIs.
Stripped runtime libraries and their license notices enter the image fixture;
the build directory retains the debug artifacts.

The matching AOSP 12L allocator 2.0, mapper 2.0 and composer 2.1 passthrough
implementations use the software profile's gralloc and Pocket composer. Higher
mapper versions remain unavailable and the stock code falls back to 2.0. The
gralloc patch retains AOSP's allocation/import/lock implementation and routes
client targets through shared memory instead of a host framebuffer device.
The composer validates the pinned 12L handle structure before copying pixels.

`software-graphics.prop` selects these image implementations, a 64-bit zygote,
read-only APEX operation, memfd support and the stock GLES RenderEngine. It is
loaded as the final API 32 profile configuration before publishing read-only
properties; Android 9/10 property loading is unchanged. Stock SDK/build identity
values are retained.

The [graphics run](api32-graphics-validation.json) passed typed allocation,
import, write/read verification and freeing; hotplug/config discovery; and a
real EGL 1.4 / OpenGL ES 3.0 SwiftShader render/readback. Stock 12L SurfaceFlinger
registered its real `ISurfaceComposer` and answered the independent client.
The composer probe discovers its display through the actual hotplug callback.
The combined report remains `PARTIAL`: system_server advanced past display
initialization but failed because the initial fixture omitted the real package
installer. System application APKs, APEX application APKs, their adjacent native
libraries and the full platform JNI library set are now included when building
`--system-server`. The [full-payload run](api32-package-manager-validation.json)
published real PackageManager and ActivityManager services. Adding stock
`idmap2`/`idmap2d` and the canonical resource overlay APKs then allowed
[OverlayManager initialization](api32-overlay-validation.json) to finish.
The [input run](api32-input-validation.json) advanced through InputManager and
published a live WindowManager interface. It failed later when the fixed private
HIDL registry rejected the framework sensor-service registration. StorageManager
also crashed because its `IVold` service was absent. No working UI or Manager
image is claimed yet.

The isolated host APK also exposes a bounded `pocket-api32-progress` activity
dump for in-flight stdout, init and native logs. Its separate
`POCKET_API32_PROGRESS` key cannot be mistaken for a completed validation report.
Before runtime launch, progress snapshots contain no stale log data from an
earlier run. The runner prints a bounded summary and the path to its complete
JSON report; retrieve that file directly when collecting evidence, rather than
parsing combined SSH stdout/stderr after a crash.

```sh
python3 tools/build_api32_graphics.py --ndk "$NDK"
python3 tools/build_api32_native.py --ndk "$NDK" --init
python3 tools/build_api32_framework.py --ndk "$NDK"
python3 tools/build_api32_framework.py --ndk "$NDK" --installd
python3 tools/build_api32_android_probe.py --ndk "$NDK" --binder --zygote --init --hardware --apex --installd --graphics --system-server --reuse-native
```

After transferring the APK/metadata/runner as above, append
`--graphics --system-server` to the full component runner command. The shared
composer change also passed the Android 10 graphics rebuild.

## Rootless input reader

`build_api32_input.py` rebuilds the complete input reader and its factory from
the pinned 12L source. Its EventHub adaptation replaces privileged
`EPOLLWAKEUP` with a real host partial wake lock obtained through the private
typed suspend service. It does not forge `capget` results or grant host kernel
capabilities. The factory and EventHub are compiled together, preserving their
allocation layout. Key layouts and character maps come from the reference image.

The wake lease lasts for the EventHub lifetime, so this initial correctness
implementation keeps the host CPU awake while guest input is active. EventHub
cleanup releases it after closing the polling descriptors. A separate hardware
process owns the lease object; client Binder death drops its reference and
releases the host lock. The Pi test verified both normal release and release
after killing a separate EventHub client with SIGKILL. It also checked actual
absence of `CAP_BLOCK_SUSPEND`, successful polling and the wake pipe.

`input.prop` disables EventHub's optional V4L video-device association scan.
Pocket forwards input instead of capturing host video devices; its private
`/dev/input` directory remains watched. The first test correctly failed on an
attempt to watch the host `/dev` for V4L devices. The configured profile passed
both input tests and started the stock framework InputReader with its virtual
keyboard. Host touch forwarding and a usable guest UI remain unverified.

```sh
python3 tools/build_api32_input.py --ndk "$NDK"
python3 tools/build_api32_android_probe.py --ndk "$NDK" --binder --zygote --init --hardware --apex --installd --graphics --input --system-server --reuse-native
```

After transferring the full APK, metadata and runner, use the full component
runner command with `--graphics --input --system-server`. Native changes require
a full APK; a thin Java update cannot deliver a changed guest library or profile.

## Framework hardware registrations

The initial fixed HIDL registry rejected the stock framework's sensor-service
registration. The API 32 profile now declares four framework interfaces:
displayservice 1.0, stats 1.0, sensorservice 1.0 and schedulerservice 1.0. These
are real objects supplied by stock SurfaceFlinger and system_server, rather than
replacement sensor/display implementations. Lookup remains private to Pocket.

Registration requires the real calling host app UID, a process-start-validated
guest identity with logical UID 0 or 1000, and an actual remote interface chain
matching the declared service. The registry rejects unknown instances, changes
to its fixed health/suspend services, and duplicate publication. It links server
death to entry removal. The logical identity remains rootless guest metadata;
it is not a separate host security principal. Lazy-daemon client callbacks and
lazy unregistration remain unsupported.

The [combined Pi run](api32-framework-hidl-validation.json) verified all four
stock registrations, rejection of an incorrect interface chain, and rejection
of UID 39999 before querying its supplied object. An independent logical UID
1000 client fetched all four actual remote objects through both the typed HIDL
manager and the private direct lookup, verified their interface chains and
pinged each object. Input, graphics, zygote and
the live activity/package/window services passed again. Full boot failed later:
Watchdog killed system_server while its main thread waited for missing `netd`
in NetworkManagementService. `vold`, `storaged` and `suspend_control` were also
absent. The Android 10 hardware rebuild passed with the shared source changes.
Init cleaned up fifteen service lifetimes with zero remaining children and
zero lifecycle failures.

```sh
python3 tools/build_api32_framework.py --ndk "$NDK"
python3 tools/build_api29_framework.py --ndk "$NDK"
python3 tools/build_api32_android_probe.py --ndk "$NDK" --binder --zygote --init --hardware --apex --installd --graphics --input --system-server --reuse-native
```

After transferring the full APK, metadata and runner, run the full Pi component
command with `--graphics --input --system-server`.

## Guest netd and traffic permissions

`build_api32_network.py` generates headers from frozen netd AIDL version 7 and
links the reference image's version 7 C++ implementation. Its generated base
returns explicit unsupported errors for unimplemented operations; it does not
return a generic success response for host kernel configuration requests.

The adapter manages physical guest network records, default-network selection,
network permissions, and typed unsolicited-listener registration. Logical UID
0/1000 can manage it. NetworkStack UID 1073 has limited query/listener access;
ordinary guest roles are denied. Guest physical networks share the host app's
uplink. VPN creation, host forwarding, interface configuration and arbitrary
host firewall changes remain unsupported.

Managed policy format version 3 adds the 12L restricted-networking allowlist.
The existing version 2 layout and behavior remain supported for Android 9/10
profiles. Version 3 initially denies application INTERNET access until the
framework supplies grants. The loader checks IP socket creation and outgoing
traffic; standby, doze, power-save and restricted chains compose with these
permission rules. Unix IPC retains its existing fast path. Corrupt policy
state fails closed. Policies are published atomically in the guest data view.

The private fwmark endpoint uses the matching 12L command definitions, checks
the actual host peer UID plus process-start-validated guest identity, validates
received socket descriptors, and closes its descriptor copies. It authorizes
guest network selection without writing host socket marks. Requests to bypass
the host VPN are rejected.

The focused local commands passed:

```sh
python3 tools/build_api32_network.py --ndk "$NDK"
python3 tools/test_api29_network_policy.py
python3 tools/test_guest_firewall.py
```

The policy test checks 63 assertions, including actual UDP delivery, both policy
formats, the restricted chain and IPv4/IPv6 socket creation. The firewall
regression checks 28 real-socket assertions, including existing connections,
direct syscalls, IPv6 and malformed state. The full APK builder recompiles the
actual ARM64 loader; the Pi test distinguishes denied socket creation from
standby/restricted denial at send time.

The [final Pi run](api32-network-validation.json) passed five real delivered UDP
datagrams and four denials: three at send time and one at socket creation. It
verified typed version/hash, physical-network lifecycle and invalid requests,
ordinary-role denial, NetworkStack's limited role, unchanged host socket marks,
and rejection of VPN-bypass requests. Stock NetworkManagementService registered
its real listener and started in 25 ms. Input, graphics, zygote and all four
framework HIDL client checks passed again. Init reaped sixteen service
lifetimes with no remaining children or lifecycle failures.

The first ARM64 run exposed that `socket()` was not in the actual seccomp trap
path, despite the policy handler passing its local test. The final loader adds
an argument-filtered IP socket trap; the Pi probe now requires creation-time
denial for missing INTERNET permission. Unix socket calls keep their old path.

Full boot remains failed: ConnectivityService cannot obtain the absent native
`dnsresolver` service, so its DnsResolverServiceManager lookup returns null.
The matching DNS resolver source is now pinned and prepared for that port.

The Pi host must have a working uplink before testing network traffic. VM 73
initially had no default network; connecting its virtual `VirtWifi` network
provided one. This is isolated host test setup and does not grant Pocket root.
The runner now checks the host default network before network tests. Its
optional `--reuse-installed` mode hashes the actual installed base APK and
requires an exact candidate match before skipping installation.

Build the full fixture with `--network` added to the preceding component flags:

```sh
python3 tools/build_api32_android_probe.py --ndk "$NDK" --binder --zygote --init --hardware --apex --installd --graphics --input --network --system-server --reuse-native
```

After transferring the full APK/metadata/runner, add `--network` to the full Pi
runner command. DNS, host-uplink NetworkAgent integration and full connectivity
remain unfinished; a netd control/traffic-policy pass does not establish them.

## Host-delegated DNS

`--dns` adds a resolver service built against the reference image's frozen AIDL
version 7. Its `dnsproxyd` endpoint implements the matching getaddrinfo wire
format and delegates external lookups to the outer Android app's real resolver.
The host's network and DNS privacy policy apply. Numeric/localhost results and
IPv4-mapped address handling stay local. The service validates the actual host
peer UID and process-start-checked logical identity before handling a request.
It checks the managed UID policy before sending an external host query.

`HostDns` factors the existing host broker's DNS encoding and network snapshot
into shared code. The isolated fixture uses a DNS-only host endpoint. API 32
host DNS requests require logical UID 0/1000, so an ordinary guest app uses the
guest resolver's permission checks instead of calling the host endpoint
directly. Android 9/10 host-broker behavior is retained. Network state access
uses an ordinary host manifest permission.

Resolver control supports guest network-context lifecycle and records the
resolver parameters for that context. Supplied server addresses must match the
actual host configuration; guest search domains, DoT overrides, custom hosts
and independent NAT64 discovery are unsupported. Query execution and upstream
retry/cache policy belong to the host. No guest answer cache is retained and
flushes do not clear the host's shared cache. Raw DNS and canonical-name
requests still require additional support. A host-uplink NetworkAgent has not
yet been integrated.

The [Pi run](api32-dns-validation.json) verified the typed version/hash,
context lifecycle, host-server configuration round trip, invalid/custom
configuration rejection, and ordinary-role denial. An interpreted UID 99998
client performed two real Bionic IPv4 `getaddrinfo()` lookups that matched the
host resolver, and connected to the resulting external addresses on TCP port
443. A lookup without INTERNET permission was denied; direct host DNS calls
from that guest role were also denied. Typed query callbacks reached the
independent client. Forty exited callback clients were reclaimed, exercising
the common Binder-death listener registry now used by netd and the resolver.

Stock ConnectivityService then started in 417 ms. Full boot remains failed:
NsdService waits for the missing mDNS daemon control socket until Watchdog
kills system_server. Init cleaned up seventeen service lifetimes with no
remaining children or lifecycle failures. Storage and suspend-control services
also remain unfinished.

```sh
python3 tools/build_api32_network.py --ndk "$NDK" --dns
python3 tools/build_api32_android_probe.py --ndk "$NDK" --binder --zygote --init --hardware --apex --installd --graphics --input --network --dns --system-server --reuse-native
```

Transfer the full APK/metadata/runner and add `--dns` to the full network Pi
runner command. The shared production host Java sources also compiled, and
the existing host framing and managed-network-policy tests passed.

## Stock 12L mDNS

`--mdns` stages the reference image's `mdnsd` and `libmdnssd`, and builds the
pinned netd `MDnsSdListener`. NsdService uses `/dev/socket/mdns`; the frontend's
DNS-SD API connects separately to the responder at `/dev/socket/mdnsd`. Both
private sockets and the responder lifecycle belong to the rootless init.
Commands require a verified guest system role (logical UID 0/1000).

The [Pi run](api32-mdns-validation.json) verified ordinary-role denial, invalid
requests, an actual stock responder registration callback, registration removal,
and responder start/stop. This does not establish discovery from another LAN
device. NsdService then started in 19 ms. Full boot still fails while AudioService
waits for the absent `media.audio_policy` service; Watchdog kills system_server.
Init reaped nineteen service lifetimes with no remaining children or failures.

```sh
python3 tools/build_api32_mdns.py --ndk "$NDK"
python3 tools/build_api32_android_probe.py --ndk "$NDK" --binder --zygote --init --hardware --apex --installd --graphics --input --network --dns --mdns --system-server --reuse-native
```

Transfer the full fixture APK/metadata/runner and add `--mdns` to the full Pi
runner command. This component pass does not establish full framework boot or
Manager support.

## Stock 12L audio foundation

`--audio` adds the reference image's `audioserver` and `mediametrics`, its
matching audio/effects HIDL 7.0 wrappers, and Pocket's PCM HAL compiled against
12L headers. The wrappers run in audioserver through the private passthrough
gateway; no host hardware-service fallback is used. The profile selects the
Pocket primary HAL and disables unsupported AAudio MMAP. Its effects inventory
is empty.

Audio policy registers with ActivityManager and SensorPrivacyManager during
initialization. The fixture starts media metrics and audio early, then checks
audio readiness after the framework services have registered. The independent
client verifies both live stock Binder interface names. The fixture's separate
host audio endpoint checks the actual outer UID and process-start identity,
and permits playback only for guest system/audio roles. It exposes no host
microphone access; input capture remains unfinished.

The [Pi run](api32-audio-validation.json) verified ordinary-role denial, stock
HIDL primary initialization, the effects query, and 24,000 stereo PCM16 frames
at 48 kHz written through the shared host AudioTrack. The host recorded exactly
96,000 accepted bytes, one completed stream and zero errors. This direct HAL
test does not establish app playback or AudioFlinger mixing. Shared host
playback now handles partial AudioTrack writes without discarding unwritten
PCM. Both stock audio Binder services were live with the expected descriptors,
and framework AudioService initialized in 219 ms.

Full boot remains failed: UEventObserver throws from native setup because its
kernel-event socket is unavailable in the outer app sandbox. Init cleaned up
twenty-one service lifetimes with no remaining children or lifecycle failures.
Storage and suspend-control services also remain unfinished. The shared host
Java sources compiled, as did the Android 9 PCM HAL and Android 10 hardware
gateway regression builds.

```sh
python3 tools/build_api32_audio.py --ndk "$NDK"
python3 tools/build_api32_framework.py --ndk "$NDK"
python3 tools/build_api32_android_probe.py --ndk "$NDK" --binder --zygote --init --hardware --apex --installd --graphics --input --network --dns --mdns --audio --system-server --reuse-native
```

Transfer the full APK/metadata/runner and add `--audio` to the full Pi command.
Framework startup is required for this audio-services probe. Manager support,
full boot, application playback and capture remain separate release gates.

## Private device events for stock 12L observers

The framework fixture preloads `libpocket_uevent_api32.so` for system_server.
It supplies the matching `libhardware_legacy` device-event functions over a
process-local datagram channel. Pocket has no guest kernel-driver namespace;
this channel does not subscribe to the host kernel's netlink events. Virtual
device producers can inject records in their own process. Physical host-device
notifications still require explicit relays and are not claimed here.

The channel uses close-on-exec descriptors, resets both ends in fork children,
rejects unterminated/oversized producer records, and rejects truncated datagrams.
Producers cannot block indefinitely behind an idle consumer. The size limit
matches the stock 12L observer JNI receive buffer.

The [Pi run](api32-uevent-validation.json) verified private delivery, fork
isolation, invalid/truncated record rejection, and the stock AndroidRuntime
JNI setup, match filtering, match removal and Java event field parsing. Test
records are explicitly synthetic. The unmodified framework then passed dock
and wired-accessory initialization and reached boot phase 500. Full boot still
failed: Wi-Fi's shared configuration store could not create its missing APEX
data directory, then dereferenced the failed store list. Init reaped twenty-one
service lifetimes with no remaining children or lifecycle failures.

```sh
python3 tools/build_api32_uevent.py --ndk "$NDK"
```

Rebuild and transfer the full framework fixture. The provider and both native
and stock-JNI checks are included automatically with `--system-server`; no
Android 9/10 provider code is changed.

## Shared APEX device data

The flattened APEX service now prepares `/data/misc/apexdata` and one shared
device-data directory per verified image module before publishing its service.
The module directories use root/system ownership and mode 0771, matching 12L
init's `create_apex_data_dirs`; their parent uses root/root and mode 0711.
The service validates all module names/manifests first, opens directories with
`O_NOFOLLOW`, and preserves their existing contents. This prepares shared data
only. Per-user CE/DE storage and mutable APEX rollback remain separate work.

The [Pi run](api32-apex-data-validation.json) verified all twenty-three module
directories, a real exclusive-file write from a guest system-role child, its
readback/ownership metadata and cleanup. Guest ownership is loader metadata;
the outer app UID remains the host security boundary. The previous Wi-Fi
shared-store directory creation failure and WifiHandlerThread crash disappeared.

The framework now reaches PackageManager's `systemReady()`, where staged-session
restoration queries `StorageManagerService.supportsCheckpoint()`. Full boot
still fails because the API32 volume service (`IVold`) is absent. Init reaped
twenty-one service lifetimes with no remaining children or lifecycle failures.
The stock audio, private device channel and stock UEvent JNI checks passed again.

```sh
python3 tools/build_api32_framework.py --apex --ndk "$NDK"
```

Rebuild and transfer the full framework fixture after this native change. The
APEX probe now requires the shared-data metadata/write/read check as well as
the existing typed inventory and role-denial checks.

## Typed 12L internal storage

`--storage` builds both sides of the private IVold protocol from the pinned
12L platform AIDL, including its changed volume callbacks and incremental-FS
parcelable. All 86 methods use generated dispatch; unsupported operations return
an explicit error. IStoraged uses its matching three-method interface and the
stock no-samples performance result of zero, with no physical I/O metric claim.

The current profile supports persistent user 0 with serial 0 and plaintext
app-private files. It prepares the Android DE/CE trees and per-user APEX
subdirectories using the 12L ownership/modes, preserving existing contents and
refusing symlink directory components. Encrypted profiles are rejected before
directory preparation. Guest UID/GID ownership remains loader metadata under
the outer app's host UID. Checkpoints and native FBE are reported unavailable;
unencrypted key operations follow stock non-FBE semantics.

The framework owns the built-in private `/data` volume. This adapter announces
no fictitious external mount and provides no fabricated FUSE descriptor.
Emulated/external storage, adopted disks, destructive user removal, incremental
FS and integration with Pocket's encrypted CE backend remain unfinished.
Maintenance requests deliver their actual unsupported result through the typed
task callback, including the matching PersistableBundle serialization.

The [Pi run](api32-storage-validation.json) verified real DE/CE file writes,
readback and cleanup after a vold reset, selected directory ownership/modes,
invalid requests, ordinary-role denial, no returned FD for unsupported FUSE
operations, and the maintenance callback. The original missing-IVold crash is
gone. PackageManager finished `systemReady()` and the framework reached boot
phase 600, starting stock app processes. Full boot still failed when 12L's
VINTF kernel-level conversion aborted on the actual host Android 16 kernel tag.
SystemUI and Bluetooth also reported an unavailable SettingsProvider during
that failed run. These are not UI or app-start passes. Init reaped twenty-two
service lifetimes with no remaining children or lifecycle failures.

```sh
python3 tools/build_api32_storage.py --ndk "$NDK"
python3 tools/build_api32_android_probe.py --ndk "$NDK" --binder --zygote --init --hardware --apex --installd --graphics --input --network --dns --mdns --audio --storage --system-server --reuse-native
```

Transfer the full APK/metadata/runner and add `--storage` to the Pi command.
The fixture requires Binder, init, APEX and installd for the storage check.

## VINTF forward compatibility and boot-property milestone

The 12L `RuntimeInfo::gkiAndroidReleaseToLevel` asserts when a GKI kernel reports
an Android release newer than its enum. The isolated Android 16 host exercises
that assertion. An API 32-only interposer now returns `UNSPECIFIED` for unknown
releases, using the stock parser's existing error path. Android 12/13 mappings
are unchanged. It neither changes `uname` nor assigns a fabricated older FCM
level or successful hardware-compatibility result.

The [Pi validation](api32-vintf-validation.json) exercises the actual SDK
`libvintf` parser: known releases succeed, a future release returns an error,
integer overflow is rejected, and the current release string and numeric
kernel version remain the actual host values. The framework no longer aborts
in this conversion. Its subsequent compatibility check still reports the
unavailable kernel policy information as an error.

Stock `system_server` reached phase 1000 and set `sys.boot_completed`, with live
activity/package/window services. This is a boot-property milestone, **not a
usable UI or complete guest boot**. SettingsProvider was detached by a process
liveness check, and SystemUI, Bluetooth and other app processes then timed out
acquiring Settings. User 0 unlock had started when the observer finished. Init
reaped 21 service lifetimes with no remaining children or lifecycle failures.
Full guest boot and Manager support remain unverified.

```sh
python3 tools/build_api32_vintf.py --ndk "$NDK"
```

The normal `--system-server` build includes this provider and its parser probe.

## Framework process UID lookup

12L's ContentProviderHelper checks a provider process's state through the stock
`/proc/PID/stat` parser, then compares `Process.getUidForPid(PID)` to its guest
ProcessRecord UID. The latter is Java code calling the native `readProcLines`
parser on `Uid:` in `/proc/PID/status`. Returning the outer app's kernel UID
made this check detach the live SettingsProvider.

The API 32 zygote now registers a narrow JNI adapter for that parser. It runs
the original platform implementation first, then replaces only the requested
UID result for a numeric status path when this profile has a matching live
PID/start-time identity record and the original UID equals the host app UID.
Non-guest processes, other fields, ordinary files and parser errors retain
the platform result. Direct native reads of kernel proc files stay unchanged,
including host Binder/SO_PEERCRED authentication. These logical roles still
share one host app UID; this introduces no new kernel isolation claim.

The [Pi zygote child](api32-process-identity-validation.json) verified its own
UID 39999 and its parent's guest UID, actual host UID, stock parent/Tgid/thread
measurements, unchanged non-guest/missing-process and ordinary-file results,
and rejection of a mismatched output array. The framework run no longer logged
SettingsProvider detaches or the previous null-provider calls. Bluetooth
advanced to its keystore access. Provider round-trip correctness remains
unverified.

`--system-server` now also checks actual user unlock through UserManager after
the boot property, then attempts a temporary Settings.Global write/read/query
and cleanup. This stronger probe caught a later failure: LockSettingsService
crashes in `android.security.KeyStore.state()` while unlocking user 0 because
KeyStore2 is unavailable. The new probe received DeadSystemException and did
not pass. Init reaped 23 service lifetimes, with no remaining children or
lifecycle failures. Full guest boot, usable launcher and Manager support remain
unverified.

```sh
python3 tools/build_api32_native.py --zygote --ndk "$NDK"
```

Rebuild the full APK after changing the zygote library or probe classes. Child
Java identity evidence is collected through the private liblog socket, matching
stock ZygoteInit's stdout redirection.

## Software keystore bring-up

`--keystore` stages the immutable SDK keystore2 daemon and software Keymaster
libraries. A small entry point follows the pinned AOSP Keymaster 4.1 service
flow, selecting `SecurityLevel::SOFTWARE`. It reserves a worker before the main
thread joins: stock `(1,true)` starts no worker, which prevents Pocket's
asynchronous manager bootstrap from delivering its reply. The private HIDL
registry validates the actual 4.1/4.0/base interface chain and only exposes the
raw HAL to system and keystore roles. Other profiles do not declare this HAL.

The keystore2 process uses explicit guest-role authorization. Virtual caller
and application-key labels feed its policy boundary because restoring logical
Binder credentials clears the kernel SID. A matching live PID/start-time/UID
record is required; labels are not kernel SIDs. Stock Rust namespace ownership
and grant checks remain intact. System operations, selected LockSettings and
Wi-Fi namespaces, and ordinary app key operations have distinct rules. Raw FBE
blobs, arbitrary namespaces and device-ID/unique-ID attestation are denied.
The host app's kernel identity and SELinux policy remain unchanged.

The [Pi validation](api32-keystore-validation.json) passed remote
software HAL metadata, actual keystore2 registration/shared-secret negotiation,
frozen AIDL 1/hash, and a typed user-state query by the system role. Ordinary
UID 39999 was denied maintenance state queries, destructive key deletion,
privileged LockSettings namespace listing and raw HAL lookup. Its APP listing
remained caller-scoped even when supplied with another UID, following the stock
contract.

The JCA probe generated an AES-256 key through AndroidKeyStore, verified software
KeyInfo and no raw key export, encrypted and decrypted a fresh nonce, reopened
the KeyStore handle, rejected a modified authentication tag and removed the key.
This verifies those operations only; daemon/guest restart persistence,
credential-bound keys and hardware-backed security remain unverified.

The initial keystore run reached phase 1000 and user 0 unlocked, as observed
through the real UserManager. The earlier LockSettingsService keystore crash
was gone. That combined run still failed: the shared NetworkStack/Tethering process timed out
executing TetheringService, was killed, and system_server then crashed with
`Lost network stack`. SettingsProvider's round-trip probe received
DeadSystemException and did not pass. Native stats-service waits are a lead for
that timeout, not a proven cause. A usable launcher and full guest boot remain
unverified. Init reaped 24 service lifetimes with no remaining children or
lifecycle failures.

```sh
python3 tools/build_api32_keystore.py --ndk "$NDK"
python3 tools/build_api32_framework.py --ndk "$NDK"
python3 tools/build_api32_native.py --binder --ndk "$NDK"
```

Add `--keystore` to the full APK build and Pi test command. It requires Binder,
init, hardware and zygote. The probe requires software HAL, typed control,
role-denial and actual AndroidKeyStore crypto markers before proceeding to
the full framework checks. The control probe waits for the maintenance service
it actually uses, because keystore2 publishes its main interface first.

## Actual ART startup traces

The fixture's trace library implements ART Palette's stack-output entry point
using private files. The stock SDK ART signal catcher still generates the
Java/native thread text. A probe signals itself and requires its actual main
and sleeping worker frames before deleting its trace. Startup sampling is
limited to system-server, NetworkStack and SystemUI in this isolated fixture;
it creates no thread in the zygote parent. The host DUMP-protected diagnostic
reader returns fixed private ANR directories, checks paths/symlinks and bounds
the output. It requires neither adb root nor a guest debugger daemon.

The [Pi trace validation](api32-art-trace-validation.json) found ActivityManager
holding its global monitor while `ProcessList.writeLmkd` waits for the missing
LMK socket. Other system-server threads and NetworkStack's service publication
queue behind that monitor. Independent logs show repeated three-second OOM
adjustments. This establishes a concrete startup bottleneck; native stats waits
remain a separate lead. Full guest boot is still unverified.

The test APK builder accepts `--delta-from <prior-image-manifest.json>` to embed
changed/new guest files during iteration. It rejects removed inputs and retains
the complete current image manifest. The app verifies every input file against
that manifest before starting the guest; a missing or modified retained file
fails verification. This update requires an existing fixture and is not an
initial-install APK or a production image. The Pi run verified all image files
after applying the delta.

## Rootless guest memory policy and provider readiness

The memory supervisor uses the pinned 12L `lmkd.h` protocol on init's private
`lmkd` sequence-packet socket. It accepts authenticated logical system roles,
records live PID/start-time/UID priority entries, and applies framework memory
thresholds to actual host `MemAvailable`. It selects eligible apps in this
profile by their framework importance, preserves logical system daemons and
persistent processes, and revalidates identity before signalling through a PID
descriptor. It does not alter host OOM scores, cgroups or PSI configuration.
Kill notifications and counts describe its own actual kills. Kernel statsd
telemetry is not fabricated; accepting a STAT subscription does not establish
complete kernel LMK telemetry support.

The [Pi memory validation](api32-memory-validation.json) deliberately raised a
test threshold above actual available memory. An eligible UID 39999 child died
from SIGKILL and produced the matching notification/count. A child below the
kill threshold and a system-role child survived. Ordinary callers, a mismatched
target UID and malformed packets were rejected. Both priority packet variants,
the total-count sentinel and empty ranges passed. The probe also read the
surviving child's unchanged kernel OOM score. This tests the policy without
claiming an actual host low-memory event or PID-reuse stress coverage.

The combined fixture now passes all its component gates, stock framework boot,
actual user-0 unlock and Settings provider readiness. The latest complete native
log contains no LMK connection retries, three-second OOM adjustments, NetworkStack
ANR or `Lost network stack` crash. SystemUI ImageWallpaper and Bluetooth
AdapterService ANRs remain; this is not yet a usable launcher or Manager image.

The Settings probe uses a real external-provider reference, following the
platform `content` command. A standalone app_process tool has no registered AMS
ApplicationThread; using its ordinary context resolver was a probe error. The
external ContentProviderClient performs PUT_global/GET_global, verifies a nonce
through a real cursor query, deletes the row, verifies its absence and releases
the external reference. This establishes the external system-tool path, not
ordinary installed-app provider access.

```sh
python3 tools/build_api32_memory.py --ndk "$NDK"
```

`--network` stages the memory service and requires its policy and caller-denial
probes. The exact combined build/test commands and candidate hashes are recorded
in the validation JSON above. `full_guest_boot_verified` and Manager API 32
support remain false until the end-to-end gates below pass.

## Software Gatekeeper and installed-app validation

The SDK Gatekeeper daemon and emulator software HAL now run as private system
services. A startup-only adapter reserves a HIDL worker while the private
registry verifies the remote interface chain; credential operations remain SDK
code. The HAL is declared only for the enabled profile, and raw lookup requires
a verified root/system role. The daemon retains its stock keyguard permission
checks. Tests run after the framework permission controller is ready.

The [Gatekeeper validation](api32-gatekeeper-validation.json) passed temporary
user enrollment, rejection of a wrong password and altered SID, challenge fields,
SID read/clear and cleanup. UID 39999 could read the public SID, as the stock
contract allows, but enrollment failed and its clear request left that SID
unchanged. Raw HIDL lookup was denied. The provisioning observer no longer
crashes on a missing Gatekeeper service.

This is the SDK emulator backend. Its fast password cache is keyed by SID and
does not necessarily reject signature-only mutation after a correct password.
Its token-signature behavior does not establish secure authentication tokens.
Hardware backing, persistent throttling, daemon-restart persistence,
credential-bound keys, a lock-screen password cycle and encrypted CE integration
remain unverified.

`--apps` adds a separately signed APK with min/target SDK 32 and an ordinary
installed UID. The test uses real PackageInstaller sessions, activity launches,
an app status provider, ordinary Settings reads/queries, AndroidKeyStore and
HTTPS, then force-stops and relaunches to check process/private-state persistence.
The [combined Pi validation](api32-app-lifecycle-validation.json) passes these
gates with installed UID 10105. The two activity processes differ, the persisted
launch counter increments, and the same encrypted private value decrypts with
the non-exportable AndroidKeyStore key. Both ordinary Settings APIs and real
HTTPS with default certificate/hostname verification passed. This verifies
process restart, not a restart of the whole guest or its keystore daemon.

The package-session restorecon failure was resolved by applying the same
disabled-guest-policy semantics used by installd to the guest framework. The
framework-only library excludes installd's exec hook. The stock SELinux JNI
therefore does not attempt to relabel host app files for a nonexistent guest
kernel policy. Host kernel SELinux was `Enforcing` before and after; ordinary
permission and Gatekeeper caller-denial probes still passed. This creates no
separate guest kernel policy or security boundary.

One test activity reported a draw callback; the restarted activity had resumed
but had not reported drawing at the status snapshot. No host surface/pixels or
input path was tested. SystemUIService and Launcher Quickstep service ANRs
remain. Full guest boot/usable desktop and Manager support stay unverified.

Preparation timings show extraction taking 585 ms and complete image checksum
verification taking 80,972 ms for 2,553 inputs. Every file was still checked;
verification was not relaxed. Restricting the HWC adapter log tag to INFO keeps
frame-by-frame verbose messages from exhausting the diagnostic log limit.

```sh
python3 tools/build_api32_gatekeeper.py --ndk "$NDK"
python3 tools/build_api32_framework.py --ndk "$NDK"
python3 tools/build_api32_guest_app.py
```

Use `--apps` in both APK build and Pi test commands. It requires the framework,
software graphics, networking/DNS and keystore flags. Exact commands, hashes and
remaining limits are recorded in the app validation JSON.

## Host display and input bring-up

The loader's [nested-signal regression](api32-signal-validation.json) now passes
on the Pi and inside the Android app sandbox. An application signal could arrive
while SIGSYS was automatically blocked in the loader's handler; intercepted I/O
from the application handler then caused a fatal SIGSYS. The regression delivers
a signal during a blocked pipe write and reads a fixture from that signal's
handler. The old loader dies with signal 31; the loader using `SA_NODEFER` passes.
The original zygote crash did not capture its triggering syscall, so this proves
the reproducible loader defect without claiming that original trace identified
the exact same interruption.

The experimental `--display` fixture connects the compositor's anonymous RGBA
ring to the existing host Surface presenter. A separate installed app draws a
nonce-coded strip; the driver requires all 16 colors in both the compositor
buffer and a host Surface PixelCopy before issuing a real host tap. The app's
click counter must then increment exactly once. Frame counters alone do not
establish visible app output.

The private input service checks kernel peer PID/UID. Since a non-debuggable
Android parent can be hidden from its child's proc view, the host passes its
already-open kernel stat descriptor over SCM_RIGHTS. The service checks the
kernel fd link and expected start time before accepting input. Wrong guest
peers were rejected, and the latest run accepted the actual host's lifetime
descriptor and queued both host touch events through the stock InputManager.

The latest [integration result](api32-host-display-validation.json) passed:
all 16 current-nonce colors matched in the compositor buffer and host Surface
PixelCopy, and a real host tap reached the ordinary installed app's click
listener exactly once. The screenshot was visually inspected. The service
queued guest coordinate 240.0,195.99384 for the published button at 240,196.
The fixture also revalidated Settings, AndroidKeyStore AES-GCM/private state,
HTTPS, process restart and service cleanup; init reaped 27 services with none
remaining or failed.

Earlier runs queued events without recording a click, including one with
coordinates inside the published current-layout button. Their failure-state
dumps exposed the host UID and were denied. The API 32 [native Binder DUMP
attribution fix](api32-binder-dump-validation.json) passed real token-free
requests from root/system callers and preserved ordinary-role denial. It uses
the existing kernel-PID/start-time-checked guest identity record, without parsing
DUMP payloads as interface headers. The successful display run did not invoke
the failure dumps, so stock framework dump attribution remains unverified.
The passing tap does not establish the cause of the prior intermittent misses
or repeatable touch/keyboard behavior; those remain validation work.

The expanded [input validation](api32-host-keyboard-validation.json) passed
with the reusable host `GuestInput32` client. Three independently acknowledged
button taps and a fourth editor-focus tap reached the ordinary installed app.
Twelve hardware KeyEvents traversed the host view and stock guest InputManager;
typing `a12b`, deleting and typing `9` produced actual EditText content `a129`
and six received key-ups. The retained final host frame was inspected and shows
the changed button and text. All eight touch events and twelve key events were
accepted, none rejected. The driver waits for stable current-layout pixel proof,
rechecks geometry after its screenshot and never blindly retries a tap. Test
feedback now tracks real pauses and process starts separately from activity
recreation. This establishes the tested hardware-key sequence, without claiming
soft IME, Unicode, navigation, stress or multi-boot input reliability.

The first expanded run was killed by the Android host's low-memory killer
before input was ready: its 1.5 GiB VM exhausted swap and killed the foreground
parent app. The small-guest profile now requests four cached processes through
stock `IActivityManager.setProcessLimit`, verifies the actual value and lets
stock ActivityManager trim cached apps. Active heap limits and host memory
policy are unchanged. The subsequent complete run verified the limit and
survived; this is not a general memory-performance benchmark or a guarantee
against all low-memory kills. The driver now also preserves ApplicationExitInfo
if an observed host test process exits before reporting.

Package installation now waits for an independently registered ordinary guest
companion to receive the actual package event and match its package metadata.
PackageInstaller's success callback can precede asynchronous replacement/removal
processing; launching immediately could lose the newly started activity. A
standalone app_process receiver has no registered ApplicationThread and is
skipped by stock BroadcastQueue. The companion preserves normal package lifecycle
behavior and passed with UID 10106 while the tested app retained UID 10105.

The bootstrap DEX archive now contains only zygote bootstrap/child classes.
Standalone tools use a separate archive, so changing a tool need not change the
boot classpath archive. Both use fixed ZIP timestamps and four-byte-aligned DEX
payloads. The real zygote and standalone tools passed with this split; startup
performance gains have not been measured.

The host's default 32-child phantom-process cap also killed an initial run.
Subsequent tests temporarily used a limit of 256 on dedicated VM73 only and
restored the prior unset value after every run. This host setup requirement
must be resolved or documented for a supported runtime. Host SELinux remained
Enforcing. Full guest boot, usable launcher and Manager support stay false.

The [SystemUI startup work](api32-ui-runtime-validation.json) now prepares a real
speed-compiled artifact through stock PackageManager, installd and dex2oat.
The pinned image's 115,640-byte SystemUI odex uses `verify`; the preparation run
produced a 24,780,632-byte ARM64 ELF whose OAT header declares `speed`. Compilation
took about 123 seconds with two compiler workers. The same run revalidated core
services and touch/keyboard input, then cleaned up all guest services.
The separate fresh boot reached actual focused/drawn stock Home and Settings,
with host screenshots and successful stock WindowManager DUMP from the system
role. Its eventual SystemUI PID stayed stable through both 20-second phases,
but two earlier ImageWallpaper service ANRs correctly failed the startup gate.
The driver keeps application pixel proof separate from desktop screenshots.
Compilation and late window readiness do not establish a stable startup.

That run also recompiled the identical speed artifact instead of reusing it.
The pinned ART `OatFileAssistant` unconditionally prefers the adjacent odex when
the APK parent is writable. Pocket's app-owned backing tree made this true even
for system apps, leaving ART pointed at the bundled verification-only odex.
The [immutable-platform view](api32-readonly-validation.json) now opts API32 into
read-only platform semantics through `/.pocket-readonly-system`. It checks
kernel-resolved paths, directory descriptors and symlinks, rejects writes and
metadata mutation with EROFS, and retains writable data directories. A native
Pi regression reproduces the old W_OK bug, passes for logical root and an app
UID with the fix, and verifies the policy-disabled and external-updater paths.
Older profiles do not enable the marker. The Android app-sandbox regression
also passed for logical root and an app UID. Stock PackageManager now reused
the unchanged speed artifact in 8.7 seconds, with no recompilation, and ART's
SystemUI trace reports the loaded data-cache OAT as `speed`. This establishes
cache selection/reuse, without a general startup-speed claim.

The combined cold run again reached actual focused/drawn Home and Settings,
with host screenshots, and passed all eight touch/twelve hardware key events
(three clicks, edited text `a129`). It cleaned up all 28 guest services and
restored the temporary host phantom-process setting; host SELinux remained
Enforcing. The strict UI gate still failed: three earlier SystemUIService ANRs,
a LatinIME ANR and a Gallery boot-receiver ANR remain. Traceur also failed because
Perfetto was unavailable, and the Bluetooth stack aborted in its alarm setup.
The retained SystemUI traces include Settings/content-service Binder waits,
ART transition waits, renderer initialization and high run-queue delay; they do
not establish a single remaining cause. SystemUI startup, usable launcher,
full guest boot and production Manager support remain unverified.

The [framework artifact work](api32-framework-artifacts-validation.json)
restores the pinned platform's seven framework boot image components and the
matching preoptimized framework JAR artifacts. The earlier fixture selected only
the core ART APEX image and omitted these compiled system files. All 84 added
artifacts match the reference byte-for-byte; framework bytecode and the separate
bootstrap JAR are unchanged. The runtime now selects both core and framework
images. A real process probe requires all seven framework image/OAT mappings,
and executable mappings for the four original ELF components that contain code.
Three components contain only metadata, so requiring executable code for those
was an initial probe error, now corrected. The corrected Android app-sandbox run
verified all seven image mappings and all four executable OAT components,
followed by real framework boot/User0 unlock, Settings provider, app lifecycle,
touch/hardware keys and actual focused/drawn Home and Settings screenshots.
All 28 services were reaped with no failures or remaining children; host SELinux
remained Enforcing and the temporary host phantom limit was restored.

The strict startup gate still failed on two SystemUI KeyguardService ANRs.
Bluetooth/Gallery also had ANRs and Traceur still failed without Perfetto.
The standalone ART status query reports five system-server JAR caches as
`verify`, while the location-provider JAR selects its stock `speed` artifact.
A retained real system_server ART trace confirms that `services.jar` loaded its
older data-cache `verify` artifact, so packaging preoptimized files alone does
not establish compiled service execution. The next step is stock system-server
dexopt with explicit verification of its selected/loaded code. Startup stability,
full guest boot and Manager API32 support remain unverified.

The [trusted system-server preopt selection](api32-systemserver-dexopt-validation.json)
now invalidates only disposable service caches from earlier fixtures and selects
the pinned immutable `/system` preopts. An initial experiment compiled all six
service caches to `speed` in ordinary `/data/dalvik-cache`; stock ART rejected
that executable data code during system_server initialization. Its trust checks
remain enabled. The corrected path preserves stock verification for the three
APEX service JARs and selects the three existing `/system` artifacts as `speed`.
Stock `ZygoteInit.performSystemServerDexOpt` prepares verification where needed;
all six entries then report zero remaining dexopt. Original source/artifact
hashes are unchanged. Only the derived caches of those six service JARs are
invalidated; application data and immutable platform files are retained.

The combined run confirms an actual executable mapping of
`/system/framework/oat/arm64/services.odex` in system_server PID 30165, with its
original OAT header declaring `speed`. The cache-preparation marker reports
3.4 seconds; this is not a general boot-performance benchmark. Framework/User0,
Settings provider, ordinary app lifecycle, hardware input and focused/drawn
Home/Settings phases passed. All 28 services were reaped, with zero remaining or
failed; the host phantom limit was restored and SELinux stayed Enforcing.
Three earlier SystemUI KeyguardService ANRs still fail the strict startup gate.
The primary framework zygote still starts with `--enable-lazy-preload`, and the
run records no full class/resource preload. Normal primary-zygote preloading is
the next path to validate against those startup delays, without a current claim
that it explains every ANR. Full 12L/Manager support remains unverified.

The [normal primary-zygote preload](api32-primary-preload-validation.json)
now runs through stock `ZygoteInit` before system_server forks. The full-framework
service no longer requests lazy preloading; the separate lazy-fork smoke test
remains intact. A native protocol client requires the real `--preload-default`
reply to report that preload was already complete, and rejects zygote restarts.
The run preloaded 13,903 classes and 64+41 resources, with no resource failures;
stock timing reports 5,844 ms for preload, without a general boot-speed claim.
The class/resource counts and complete-preload protocol are required checks.
The UI gate also rejects a native log that reaches its capture limit.

This fresh boot reused all six service artifacts with unchanged hashes, zero
invalidations and zero recompilations. It again verified executable trusted
system-server code, app lifecycle, input and focused/drawn Home/Settings. All
28 services were reaped, no children remained, and the temporary host phantom
limit was restored with SELinux Enforcing. Two earlier SystemUI ANRs still fail
the startup gate (ImageWallpaper and SystemUIService). Full stock preload is
therefore verified but does not by itself establish stable startup; the remaining
traces need correlation with service waits, rendering and CPU scheduling.
Full guest boot, usable launcher and production Manager support remain unverified.

## Native suspend control

The API 32 hardware daemon now also publishes `suspend_control` and
`suspend_control_internal` through Pocket's private native Binder registry.
It runs both native Binder and HIDL thread pools, connecting the frozen public
V1 callback interface and pinned 12L internal stats interface to the existing
real host partial wake locks. The build reuses the pinned AOSP callback and
death-recipient implementation and its bounded native `WakeLockEntryList`;
it excludes the upstream kernel `SystemSuspend` backend.

The [typed Pi test](api32-suspend-validation.json) verified remote callbacks
and host lock counts `0 → 1 → 0` twice, matching client PID and elapsed native
stats, harmless duplicate release, duplicate callback rejection, and seven
protected-method denials for UID 39999. Null callbacks receive the stock frozen
AIDL `EX_NULL_POINTER` during unmarshalling. Kernel autosuspend and force-suspend
requests return false; kernel wakeup/suspend stats return unsupported. No wakeup
events or host kernel counters are fabricated. The existing input client-death
test also passed with the combined native/HIDL hardware daemon.

Run `a54484972b8b4adfa29359b42708657e` had zero native suspend-service waits,
compared with 228 previously. All 2,645 guest files were verified. Stock primary
preload, trusted executable system-server code, app lifecycle, real touch/key
input and later focused/drawn Home/Settings passed. All 28 services were reaped,
with no remaining children or lifecycle failures, the temporary host process
limit was restored, and SELinux stayed Enforcing.

The strict startup gate still failed: SystemUI had ImageWallpaper and
KeyguardService ANRs, and an earlier Launcher3 process crashed in
`SystemUiProxy.getRecentTasks()` on its `UiThreadHelper` thread. The driver now
also rejects SystemUI/Launcher3 fatal crashes, including those preceding a later
stable UI phase; its new crash check was replayed against the recorded Launcher3
failure. The original terminal report is retained unchanged. Gallery's boot
receiver ANR and Traceur's missing Perfetto failure also remain.

An early ART snapshot captured system_server's main thread in state D while
loading APK resources to start Home. The paired VM snapshot reported I/O PSI
`some avg10=36.14%`, and the trace recorded 4.092 seconds of runnable wait.
These are evidence of I/O and scheduling stalls, not proof of a single cause
for every startup failure. VM memory/CPU PSI and swap-device details were denied
to the ordinary shell; no elevated access was used. Full guest boot, usable
launcher and production Manager support remain unverified.

## Running SystemUI code and bounded report polling

The [SystemUI code test](api32-ui-code-validation.json) now requires the running
SystemUI process to have an executable OAT mapping for its actual application,
with an ELF header declaring `speed`. It checks the process command line and
rechecks the framework's PID record to reject a replaced process. Both the app
data-cache path and a matching platform preopt are recognized. The check runs
before and after the existing Home/Settings phases; creating a compiled file
alone no longer satisfies this part of the UI test.

Run `639071280392434483d0ddb5bf6de4af` verified the actual speed-cache mapping
for the later stable SystemUI PID 15014 before and after 40 seconds of drawn,
focused Home/Settings. Its 24,780,632-byte cache was reused unchanged. This
measurement does not establish code mappings for the earlier ANR PIDs 13809
and 14360. The strict startup gate still failed on those ImageWallpaper and
KeyguardService ANRs. No SystemUI/Launcher3 Java crash was recorded in this run;
that single observation does not prove the earlier Launcher3 failure fixed.
Gallery's boot-receiver ANR and Traceur's Perfetto failure remain.

The host test app also stops reading and encoding a previous large saved report
while a new guest starts. It returns a distinct short pending line until the new
report has been atomically published, then exposes the complete terminal report.
The measured pending dump was 280 bytes; the previous saved-report response was
2,347,164 bytes. These are individual transport observations, not a general
latency benchmark or proof of ANR causality. The driver kept waiting through the
pending state and consumed the correct terminal run. After a diagnostics-only
app restart, the saved terminal report remained readable without starting a guest.
An immediate observation during Activity attachment was empty; re-observation
succeeded without another restart. The driver's `reportReadRetries` includes
expected pending polls and is not itself a count of runtime failures.

All 2,645 guest hashes and required markers passed; input, native suspend
callbacks, trusted system-server code and later UI phases passed. Init reaped
28 services, with none remaining or failed; the temporary host limit was restored
and SELinux remained Enforcing. Both this and the previous run used the same
1,536 MiB, four-vCPU VM73 host. No VM memory configuration was changed and no
memory-capacity comparison is claimed. The bootstrap and native adapters were
unchanged. Full guest boot and production Manager support remain unverified.

## Normal startup and memory comparison

The [controlled memory comparison](api32-startup-memory-comparison.json) used
the identical installed APK at 1,536 and 2,560 MiB in VM73. More memory did not
eliminate startup failures: the larger run had a SystemUI KeyguardService ANR
and a Launcher3 `UiThreadHelper` crash, while later Home/Settings phases passed.
The host also rebooted, so the changed ANR count is not a general performance
or minimum-RAM result. The original complete VM configuration and 1,536 MiB
allocation were restored; data persistence and the installed APK hash were
verified after both changes. VM72 was not modified.

Investigation then found an intrusive diagnostic sampler enabled by default in
the fixture. `pocketStartStartupTrace32` requests a full ART SIGQUIT dump every
ten seconds, twelve times, in each eligible startup participant: system_server,
SystemUI and NetworkStack. Previous startup snapshots are real ART data, but
were collected under that additional workload and are not neutral performance
measurements.

Repeated startup sampling is now opt-in with `--trace-startup` in the native
fixture builder, APK builder and Pi driver. The normal init configuration
explicitly sets `POCKET_API32_TRACE_STARTUP=0`. Both fixture metadata layers record
the chosen mode, fixture reuse checks it, and the driver rejects undeclared or
mismatched modes. The native trace sink, explicit manual trace probe, bootstrap,
tools JAR, timeouts and crash/ANR gates remain unchanged.

The [normal-mode run](api32-normal-startup-validation.json),
`09c4b4c5f7c948aabc6e61f7ba2afe19`, verified all 2,645 files and required markers.
The manual ART trace probe passed with one write; no periodic private ART
snapshots remained after its cleanup. Executable code, input and later stable
Home/Settings passed, and init reaped all 28 services with none remaining/failed.
The temporary host limit was restored and SELinux stayed Enforcing.

Startup still failed on two SystemUI KeyguardService ANRs; Bluetooth's service
and Gallery's boot receiver also had ANRs. No SystemUI/Launcher3 Java crash was
recorded in this run, which does not establish a general crash fix. The native
log contains 52 failed connections to the missing private `tombstoned` endpoint,
and no ART snapshots for the actual ANR processes were available through the
requested-backtrace path. A matching private crash/intercept service is still
needed; periodic self-signals are not a replacement for it. This gap is not
proof of an exclusive cause for the ANRs. Full guest boot, usable launcher and
production Manager support remain unverified.

## Private tombstoned and requested Java backtraces

The [tombstoned validation](api32-tombstoned-validation.json) adds the matching
stock daemon, crash/intercept/Java sockets and explicit ART delegation to its
stock palette writer. The daemon runs as guest UID 1058. The scoped transport
checks real kernel peer credentials and guest PID/start-time records, enforces
interceptor roles and closes transferred descriptors on rejected requests.
These guest metadata checks do not create a kernel isolation boundary.

The first run exercised real requested Java backtraces and both denial paths,
but late standalone output failed the daemon's hard-link operation with EACCES.
The daemon adapter now handles that specific denial by reopening the actual
completed output inode and copying its bytes into an exclusive temporary file
in the same private output directory, then publishing it with an atomic rename.
The fallback validates the source FD, directory inode, regular-file size and
stock artifact name, bounds copying to 64 MiB, and propagates failures. The
stock daemon and crash dumper's executable `.text` sections remain unchanged.

Run `790619a76b10432295abe0fb0653c580` verified all 2,649 inputs. The stock
`Debug.dumpJavaBacktraceToFileTimeout` probe returned real target Java frames
in 164 ms. A second dump with no interceptor produced an actual 17,863-byte
`/data/anr/trace_00` in 440 ms; its PID, named worker and Java frames were checked,
and its target remained alive. Each denial test transferred descriptors over
24 requests plus one readiness warm-up, and the daemon FD count returned to 12.
Completed writers may exit before their completion packet is read; the adapter
retains the kernel UID check and lets the stock connection state machine enforce
that completion follows the authenticated initial request.

The strict startup gate still failed on SystemUI's SystemUIService ANR;
Bluetooth's AdapterService also had an ANR. Later focused/drawn Home/Settings, executable code
and actual input passed, with SystemUI PID 14408 stable for 40 seconds. All 30
services were reaped, with none remaining or failed; the host process setting
was restored and SELinux stayed Enforcing. No SystemUI/Launcher3 Java fatal
crash was recorded in this run. Native fatal tombstone persistence remains
unverified; observing crash requests and native logging does not establish it.

The host diagnostics reader now recognizes stock `trace_*` and text
`tombstone_*` names in the fixed profile directories, retaining symlink/path
checks and excluding binary protobuf output. Optional detail mode reads only
the newest accepted file per directory, capped at 512 KiB. A host-only update
with a byte-identical guest image was installed after cleanup and started in
saved-diagnostics mode; it did not extract or restart the guest.

That reader recovered the full 334,848-byte actual ANR file. SystemUI PID 13518's
main thread was in `BinderProxy.transact`, calling `ISliceManager.pinSlice` from
the keyguard slice view's attachment. The system_server snapshot was taken
about five seconds later, when its main loop was idle, and did not capture the
callee's `pinSlice` stack. The evidence identifies a concrete wait but does not
prove a persistent Binder deadlock. Bluetooth separately aborts after timer
initialization returns EPERM; its stock timer code requests a SCHED_FIFO callback
thread, so thread scheduling denial must be distinguished from kernel timer
denial before changing the rootless policy. Timeouts, startup ANR/crash gates,
the native log cap and normal mode's disabled periodic sampling are unchanged.
Full guest boot, usable startup and production Manager support remain unverified.

## Rootless service scheduling and hardware declarations

The [Bluetooth scheduling validation](api32-service-scheduling-validation.json)
identifies two privileged assumptions in the stock stack. Its timer requests a
SCHED_FIFO callback thread, which bionic creates before creating the kernel timer.
After that failure is addressed, the stack also treats failure to set real-time
scheduling on its main thread as fatal.

An explicit `POCKET_API32_ROOTLESS_BLUETOOTH_SCHEDULING=1` policy now adapts those
requests to actual SCHED_OTHER/priority-zero operations. It applies only in the
SDK32 fixture, under the Pocket loader, for virtual UID 1002 with the actual
`com.android.bluetooth` process name. Timer events and Bionic attributes are
copied without changing the caller's attributes. Direct scheduling adaptation
is limited to the process's own live threads, checked under `/proc/self/task`.
The adapter calls stock bionic/kernel operations and returns their actual result;
scheduling queries expose the actual ordinary policy. It grants no real-time
privilege or kernel wake-alarm permission. Other processes retain stock behavior.
The stock Bluetooth and Bionic binaries remain byte-identical to the reference.

Run `31a01661813549b2a6e17ade2eab6fcb` verified all 2,651 inputs and actual timer
callbacks. The probe checked both its calling thread and a separate callback
thread with `sched_getscheduler`/`sched_getparam`, plus rejection for another live
process, a different virtual UID and a different process name. It disabled the
opt-in policy for the stock baseline comparison: bionic's internal thread setup
also calls the public scheduler API, so resolving stock `timer_create` alone
does not bypass the scheduling adapter.

All six real Bluetooth processes observed in that run used the policy and got
past both scheduling failures. They then aborted at the stock HCI layer's check
for a Bluetooth HAL: neither the 1.1 nor 1.0 interface exists in this port. The
reference permissions XML still advertises Bluetooth and BLE, so its hardware
declarations do not match the implemented backends. Stock SystemServer uses that
feature declaration to decide whether to start Bluetooth. Correcting the
software profile's declarations and checking the framework's absent-adapter
behavior is required; real radio support requires a real backend.

The strict startup run still failed on a SystemUI ImageWallpaper ANR; Gallery's
camera boot receiver also ANRed. No SystemUI/Launcher3 Java fatal crash appeared
in this run, which does not establish a fix for the previous process-lifecycle
failure. Later focused/drawn Home/Settings, ordinary application and input checks
passed, with SystemUI PID 25079 stable for 40 seconds. Init reaped 29 services,
with none remaining or failed; the host process setting was restored and SELinux
remained Enforcing. Neither Bluetooth radio support nor complete 12L/Manager
support is claimed by the scheduling result.

## Software hardware profile and stock camera service

The [software hardware validation](api32-software-hardware-validation.json)
corrects the reference image's Bluetooth and camera feature declarations for
the currently implemented software profile. Its profile metadata declares no
HCI or camera-provider backend. The staging transform removes only the matching
feature nodes from nine permissions XML files, preserving other declarations
and copyright comments. The pinned reference is unchanged. Applying the
transform a second time produces no further XML changes.

The matching stock cameraserver now runs under native guest UID 1047 and
publishes the real `media.camera` Binder service. Its framework HIDL 2.2 service
and 2.1/2.0 aliases use the actual four-entry interface chain. The private HIDL
registry permits UID 1047 to register this camera interface only; attempting
to register an unrelated sensor service is rejected before querying its
interface chain. Existing root/system registration and app-role denial checks
remain in place. Cameraserver's executable code and the entire libcameraservice
binary retain the reference contents.

Run `7e8436f809d64a27917557ccf93d0d17` verified all 2,653 guest inputs. Actual
PackageManager calls reported all 13 declared unavailable features absent.
Stock SystemServer logged that Bluetooth hardware was not present, started no
Bluetooth process, and registered no Bluetooth manager; the framework returned
a null default adapter. This uses the platform's absent-hardware behavior, with
no package disabling or synthetic adapter.

The camera test checked a live Binder, actual Camera API1 and API2 enumeration,
and the raw typed camera interface. The stock initialized service returned
empty device lists and ILLEGAL_ARGUMENT for an unknown ID. An uninitialized
service would return DISCONNECTED instead; Camera API1's zero fallback for a
missing service does not satisfy the test. These queries run from the external
system-role tool. No camera provider, physical capture, host Camera2 bridge or
dynamic provider registration is verified by this empty-profile test.

Gallery's actual boot receiver ran, logged no back camera and that it was
disabling camera activities. No Gallery ANR was observed in this run, and the
previous Bluetooth crash loop did not occur. SystemUI still had a SystemUIService
ANR. No SystemUI/Launcher3 Java fatal crash appeared; Traceur's missing Perfetto
failure remains. Later Home/Settings, executable-code checks and touch/key input
passed, with SystemUI PID 29450 stable for 40 seconds. All 31 supervised services
were reaped, none remained or failed, the host process setting was restored and
SELinux stayed Enforcing. The strict driver exited 1 despite the guest exiting 0.
Full guest boot, usable startup and production Manager support remain unverified.

## Early SystemUI code and display-rate experiment

The optional `--observe-startup-code` diagnostic now samples only SystemUI at
approximately 2, 8 and 16 seconds after process initialization. It reads bounded
maps and main-thread scheduling statistics, without sending SIGQUIT. Executable
speed-code verification requires the mapped inode and device to match the opened
ELF. Builders record the explicit mode in metadata and reject mismatched reuse;
normal startup leaves both observation and periodic stack sampling off.

Run `640e6f3d750c4a13b2b70a15d5a93c3b` verified executable SystemUI speed code
in the initial process at all three samples, including two seconds after startup.
That process still ANRed executing SystemUIService. Missing early AOT mapping
does not explain this occurrence; mapping alone does not establish how every
instruction executes or identify a single cause of the stall.

Run `2da943bf5c744e4b9525a2855a0010d2` tested a 30 Hz software-display property
with the same 480x800 pixels and existing timeout/input gates. Both the real
DisplayManager refresh rate and the host frame-stream header reported 30 Hz.
The initial SystemUI process again had executable speed code by two seconds,
but ANRed executing KeyguardService. Settings FallbackHome also ANRed. Later
Home/Settings and all touch/key tests passed, which does not satisfy clean startup.
The frame-rate reduction was reverted because this experiment did not resolve
the startup failure. The display-rate reporting and agreement checks are retained.
These are individual diagnostic runs, not a controlled performance benchmark.

The [startup observation evidence](api32-startup-code-validation.json) records
both failed strict runs. Full guest boot and production Manager support remain
unverified. Traceur separately failed because its Perfetto command was absent;
the matching command/service integration is being tested independently.

## Stock Perfetto consumer service

The fixture now stages the matching `perfetto` command and `traced` service.
Executable sections retain the pinned reference contents. Rootless init owns
the two private stream sockets and supervises the service as guest UID 9999.
The service's socket adapter authenticates the real kernel UID and the peer's
profile identity, including PID start time. It supplies the accepted connection's
guest UID to stock Perfetto, retaining stock per-UID session ownership. Other
socket credential queries and the kernel's credentials remain unchanged.
The bounded connection table releases entries on close.

Initial run `99c800cff8d64fcdb9d45fe90f0d1fe3` verified a stock client/service
round trip: guest UID 1000 created a detached session, attached to it, stopped it
and wrote a 701-byte trace. Guest UID 39999 could not attach to that session.
Traceur's actual BOOT_COMPLETE receiver ran its stock query without the previous
Perfetto error or a captured Traceur fatal crash. The strict driver still exited
1 on a SystemUIService ANR; later Home/Settings/input passed. All 32 supervised
services were reaped, none remained or failed, the host process setting was
restored and SELinux stayed Enforcing.

Final run `1f0c00d97481432fa2440f89d064de5f` verified all 2,657 inputs with both
startup diagnostics off. Its extended check rejected 24 unregistered peers
sharing the real host app UID. The daemon's descriptor count stayed at eight,
and a subsequent stock query confirmed that it remained responsive. Session
ownership checks passed again, with 701 output bytes containing 11 framed trace
packets. Traceur again ran its boot-time query without a captured fatal crash.
The strict startup gate still failed on an initial KeyguardService ANR; later
Home/Settings/input passed with SystemUI stable for 40 seconds. There were no
captured Java fatal crashes. All 32 supervised services were reaped, with none
remaining or failed; the host process setting was restored and SELinux stayed
Enforcing. The [Perfetto validation evidence](api32-perfetto-validation.json)
records both terminal runs and the unchanged strict startup failure.

This integration does not include `traced_probes`, host kernel trace access,
ftrace or a complete system-tracing capture backend. A working consumer session
does not establish those capabilities or clean framework startup.

## Framework memory default at initial startup

The immutable `PocketMemory` product overlay now supplies the framework integer
`config_customizedMaxCachedProcesses=4`. Previously, the external readiness tool
called `setProcessLimit(4)` after BOOT_COMPLETED. The new check makes no setter
call: it reads the framework resource, verifies the absence of an override with
`getProcessLimit()`, and checks the stock ActivityManager settings dump for both
its constructor default and current limit. This uses the
[AOSP ActivityManager constructor resource](https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/android-12.1.0_r27/services/core/java/com/android/server/am/ActivityManagerConstants.java)
and the platform's immutable product-overlay path, retaining overlayable-policy
enforcement. The framework resource APK, services JAR/preopt and bootstrap retain
their previous contents.

The overlay has one integer resource and no executable code. Its build tool
archive and executable are pinned by SHA-256 in the source lock. The helper
rejects mismatched property/resource limits and a tampered cached tool. Two
consecutive complete signed overlay builds were byte-identical; this does not
establish reproducibility of the complete guest image.

Run `2f3979de7b4e4cbfbfc55d568d67ec72` verified all 2,658 staged files. The actual
ActivityManager reported constructor/default limit 4, current limit 4 and override
-1. No readiness-tool override is needed to apply the profile at initial boot.
The [early memory validation](api32-early-memory-validation.json) records these
observations and the gate replay, which rejects missing early-default evidence.

The strict driver still exited 1 on an initial SystemUIAuxiliaryDumpService ANR.
No Java fatal crash was captured. Later focused/drawn Home and Settings passed,
with SystemUI PID 20319 stable for 40 seconds; all eight touch and twelve key
events passed with no input rejections. All 32 supervised services were reaped,
none remained or failed, the host process setting was restored and SELinux stayed
Enforcing. This corrects memory-policy timing; it does not establish that memory
policy caused or resolves the remaining ANR. Full guest boot and production
Manager integration remain unverified.

## Normal runtime service startup

`--runtime-boot` now stages a separate service startup path. Its init configuration
contains the 23 runtime services and no component-test services or smoke zygote.
A standalone typed service manager publishes the private registry without the
Echo test service. A small SDK32 private-property client sends actual service
control requests; the reference Toybox does not provide `setprop`.

`pocket-runtime-boot.sh` owns startup and the service supervisor. It derives the
matching classpaths, prepares internal data, starts the native services, prepares
the trusted system-server artifacts and starts the primary zygote/framework.
Its readiness marker follows actual boot completion and live framework-service
checks. The separate `pocket-runtime-validate.sh` then runs application, input and
UI acceptance checks. Orderly stop uses `pocket.control.stop=1` and waits for init
to reap children before the owner exits; signalling the wrapper had interrupted
that wait and lost its completion report.

The build and driver record the mode explicitly and reject mismatched fixture
reuse. The old component gate body remains unchanged apart from indentation.
Normal-runtime validation checks full preload, actual app/Settings/keystore/HTTPS
and restart behavior, real host frame/input proofs, the same 40-second UI checks,
the existing capture-size limit, collector errors and actual supervisor shutdown.
It also rejects Java fatal
headers without a named process, native fatals, UI ANRs and multiple main
SystemUI/Launcher process starts. A component-suite report cannot pass these
normal-runtime gates.

The [normal runtime evidence](api32-runtime-boot-validation.json) distinguishes
the observed results. Run `dafa543f8cbf49e2a982a36c1e7880c9` had one SystemUI
process, no captured UI ANR or Java/native fatal, and successful post-boot app,
UI and input checks, but failed the shutdown gate. After the shutdown fix,
run `5a409c457658457f9e757b8cc30688f5` reaped all 23 services with none remaining
or failed and passed later UI/input checks, but an initial ImageWallpaper ANR
and SystemUI restart still failed strict startup. These separate results are
not combined into a passing run.

Run `a1188e1b698e42689e9a9d3f24906b54` tested the same installed APK with the
actual host phantom-process limit of 32 and no host-setting mutation. The host
ActivityManager explicitly logged trimming ten guest processes owned by host
PID 30208, including both shells, init, the service manager and other native
services. The guest shell exited 137 and app acceptance was incomplete. Calendar
and another unnamed process also emitted Java fatal headers while services were
being lost. The host setting remained unchanged. The process-budget issue is
therefore separate from the intermittent startup ANR already observed with a
temporary limit of 256.

This is normal service startup exercised through the isolated non-debuggable
test host, using its existing guest data. It is not a downloaded fresh image,
production Manager integration, reliable startup, or default-limit support.
The optional shared native core below reduces the process count while preserving
the existing interfaces and caller-role checks. It does not yet fit the default
process budget through the complete acceptance test.

## Shared process for UID-0 native services

`--core-services` links the private service manager, netd, DNS, memory policy,
APEX, storage and mDNS control into one native process. All seven previously ran
as guest UID 0. Six used primary GID 0; mDNS control used primary GID 1000.
The shared core uses UID/GID 0, explicitly changing mDNS control's primary GID.
Its existing UID-based caller checks remain in use, and its inherited socket
retains owner/group 0:1000 and mode 0660. The existing implementations and typed
interfaces remain in use. Other service credentials remain unchanged. Init
supplies both the LMK and mDNS sockets to the core. A component returning
unexpectedly terminates the core instead of leaving partially live service
registrations.

The production core excludes the Echo test implementation. The separate test
binary includes it so the existing Binder identity and component suite still run.
The build, staging and driver record the selected mode and reject mismatched
reuse. Component startup prepares data before launching the shared services.
Normal startup keeps its readiness checks and uses the production binary; its
init configuration has 17 service declarations instead of 23. This remains an
experimental test-host path, rather than production Manager support.

Run `c90d173c356845acac98383bcc002ea8` verified all 2,661 inputs and the existing
positive/denial checks for every merged component, including real UID traffic
rules, DNS, memory kill policy, APEX data ownership and storage lifecycle. The
guest exited 0 and init reaped 26 services with none remaining or failed. Later
Home/Settings, eight touch and twelve key events passed; the later SystemUI PID
4488 stayed stable for 40 seconds. The strict driver exited 1 because the first
SystemUI process ANRed in SystemUIService. Three main SystemUI process starts
were recorded. There were no captured Java fatal headers or native fatals. The
temporary host process limit was restored and SELinux remained Enforcing.

Run `f3429591bcef46b69f2dbed7b27a4570` verified all 2,667 normal-runtime inputs
with the actual host limit of 32 and no host-setting mutation. The production
core started once with no test Echo, and framework boot, app installation,
Settings/keystore/HTTPS/restart checks and real host input completed. SystemUI and
Settings FallbackHome ANRs still occurred. Host ActivityManager then explicitly
trimmed the two shell processes belonging to host PID 6365. The guest exited
137 during UI acceptance, without the final UI or orderly-shutdown markers.
The shared native core was not among the two processes in those trim messages.
Reducing the native service count therefore improves the architecture but does
not yet establish default-limit operation.

The [native core evidence](api32-native-core-validation.json) keeps those results
separate. Normal-runtime validation now explicitly rejects ANRs from any guest
application, including Settings. Replaying five failed reports preserves their
failure results and leaves the original reports unchanged. Timeouts, the native
log cap, full input checks and the existing component gate body remain unchanged.
Full guest boot, reliable startup, a fresh downloaded image and Manager support
remain unverified.

## Shared hardware registry and software crypto HALs

Service layout 2 moves the UID/GID-0 hardware registry into the native core. The
core now has eight components; its native and HIDL interfaces retain their
separate dispatch and caller checks. The hardware entrypoint skips repeated
stdout initialization when started inside the core. The standalone entrypoint
and the Android 9/10 branches retain their behavior.

A separate UID/GID-1000 process publishes software Keymaster 4.1 and Gatekeeper
1.0. Keymaster retains the reference software factory and key-operation library.
Gatekeeper uses the unmodified pinned AOSP software implementation and the
reference Gatekeeper library. Its ARM64 NEON scrypt source is pinned to the
matching platform tag, and its NOTICE accompanies the image. All three AOSP
known-answer vectors pass on the guest. This remains software security; the
emulator Gatekeeper token MAC is not secure and no hardware-backed claim is made.

The common crypto identity drops Keymaster's supplemental group 1026; its UID
and primary GID remain 1000. The software backend does not use DRM device access.
The existing mDNS primary-GID change from the first consolidation remains
explicitly documented above. Other remaining service credentials are unchanged.
Normal configuration now has 15 service declarations, down from 23 originally
and 17 with the first core. Build and driver metadata require layout 2 before
reusing or installing an APK.

Component run `62eb6efe565f467c82a4f84b2ce6fb5b` verified all 2,670 inputs. The
hardware registry published from the core's actual PID, and Keymaster and
Gatekeeper registered from the crypto process's actual PID. Hardware, suspend,
native/HIDL coexistence, crypto, wrong-password/altered-SID rejection, lifecycle
and caller-denial checks passed. The guest exited 0; init reaped 24 services with
none remaining or failed. The strict driver still exited 1 on an initial
SystemUIService ANR; later UI/input passed with SystemUI stable for 40 seconds.

Normal run `13fa32a118a24fe49d31c1d35f9f418a` completed the functional acceptance
sequence with the actual host limit of 32 and no host-setting mutation. App
installation, Settings/keystore/HTTPS/restart, real frame/input and later focused
Home/Settings checks passed. The guest exited 0 and init reaped 16 children with
none remaining or failed. Host ActivityManager's retained log contained no
phantom-process trim message for this run's parent. SystemUI and Settings
FallbackHome still ANRed, with three main SystemUI process starts, so strict
startup failed. There were no captured Java fatal headers or native fatals.

A transient boot snapshot contained 36 guest processes; a later UI snapshot
contained 32, with 21 host-tracked phantom records. The completed default-limit
run therefore demonstrates progress without proving a hard process ceiling or
reliable headroom. The [layout-2 evidence](api32-service-process-validation.json)
records both runs, source pins, credential changes and this distinction. Full
guest boot, stable startup, fresh-image validation and Manager support remain
unverified.

The subsequent `--renderer-worker-scheduling` experiment is off by default. It
tests lowering newly created SwiftShader workers in SurfaceFlinger from inherited
negative nice values to normal priority, using actual kernel calls. It leaves
the compositor's own control-thread priority unchanged and requires captured
worker-priority evidence. This is an independent startup investigation; it does
not change the results of the two layout-2 runs above.

Trial `7940dda759c14acbb0ce19b073f28c57` verified all 2,671 inputs. Four actual
SwiftShader workers in SurfaceFlinger PID 18885 changed from nice -8 to 0 with
no kernel error. Functional acceptance and orderly stop completed under the
unchanged default host limit; init reaped 15 children with none remaining or
failed. The initial SystemUIService ANR remained, and three main SystemUI
process starts were observed. The strict driver exited 1. The
[worker-priority evidence](api32-renderer-worker-validation.json) records the
actual change and the unresolved startup failure. The experiment stays off by
default; the result does not establish that inherited worker priority caused
the startup ANR or that the ANR is fixed.

## Downloadable 12L image and Manager preparation

`tools/build_api32_image.py` packages the layout-2 normal runtime separately
from the host APK. The experimental image contains 2,629 immutable files,
589,698,562 compressed bytes and 1,300,928,910 unpacked bytes. It includes the
ART/runtime APEX modules, system extensions, product overlays and licensing;
it excludes guest data, fixture flags, test registries and test APKs. The
renderer-worker scheduling experiment remains disabled. Catalog and manifest
metadata require SDK32, service layout 2, boot protocol `api32-v1` and
app-private storage. The APK pins the complete inventory and archive hashes.

Manager's downloader now accepts the 12L partition layout and its two required
root files without permitting archive writes into guest data or Manager state.
Preparation relocates the loader and ELF interpreters, rebuilds the bootstrap
linker configuration, and keeps native Bionic-backed `getprop` and `setprop`
tools. Its API32 environment supplies the actual host PID and start time,
without the older profiles' global library preloads. Live-session validation
is recorded separately below.

The first installed-image test exposed `ENAMETOOLONG` when a translated
property-service socket exceeded Linux's Unix socket pathname limit. API32
now uses the shorter private `files/api32` directory. The unreleased longer
directory is migrated atomically only when no old runtime is alive; conflicting
paths and encrypted profiles are refused. Android9/10 directory names remain
unchanged. A separate migration run verified preservation of existing test
data and successful execution after re-relocation.

Fresh run `d83ccc17a1f04c069ded628189cbbb93` installed the image-free probe APK
under ordinary host UID10157 in VM73 with SELinux enforcing. The app downloaded
the image, validated and installed its inventory, executed the relocated
Toybox binary, then prepared again after the runner stopped its HTTP server.
API32 data and the isolated Android9/10 profile sentinels survived. The native
`getprop` helper read SDK32 from the actual guest property area. No phone,
production profile or shared VM72 was used. The probe exited0; see the
[image-preparation evidence](api32-manager-image-validation.json).

The production image-free Android9/10 Manager still builds and passes its
distribution validator. The API32 probe is non-debuggable, contains all three
catalogs and unchanged host JNI libraries, and passes the isolated-probe
distribution checks. Host archive tests pass48 assertions, including partition
installation, root-file staging, traversal/symlink rejection, recovery and data
preservation. Production validation continues to reject the probe's loopback
HTTP catalog.

This evidence verifies image preparation. The subsequent live-session run adds
boot, control and basic APK installation coverage. The broader full-boot,
usable-launcher, Manager-support and startup-stability flags remain false;
API32 encrypted storage remains a separate implementation gate.

## Basic Manager live session

The experimental catalog now enables Android12L in Manager's profile selector.
The actual `GuestService`/`DirectSession` path starts the normal 15-service
runtime, waits for its per-start readiness token, authenticates the API32 input
channel against the host process lifetime, and uses the existing anonymous
composer buffers and host presenter. Audio and bounded guest-log adapters are
shared with the native validation fixture. Package installation and launch run
in a separate operations worker through the real 12L PackageInstaller and
12-argument activity-service ABI. Android9/10 retain their existing control
protocols and data layouts.

Two integration failures were resolved before the passing run:

- The crash-reporting adapter required an isolated-fixture marker which the
  downloadable image correctly omitted. It now requires the immutable-runtime
  marker while retaining the SDK32/loader/UID1058 checks. A source-hash audit
  proves that its remaining role/PID/start-time/FD checks match the previously
  validated implementation. Only this library changed between images0.78.1
  and0.78.2; the other2,630 image files are byte-identical.
- Stock 12L's package manager uses relative `Path.subpath()` traversal while
  creating installation parents. The empty root `data` placeholder disagreed
  with translated absolute `/data`, producing `mkdir: EEXIST`, reported by the
  framework as `INSTALL_FAILED_INSUFFICIENT_STORAGE: Failed rename`. API32 now
  connects the relative root path to the same `system-data` directory through
  a private filesystem alias. It never replaces nonempty data or unexpected
  links. This is a rootless path view, not a kernel mount. Legacy layouts are
  unchanged. A kernel-relative filesystem fixture covers traversal, repeated
  preparation, private data preservation and refusal of conflicting paths.

Run `9ec0fa9e573449108dd58d8e59789eda` passed the strict basic Manager test in
VM73 using image0.78.2 and a non-debuggable, image-free host APK. The session
used480×800 pixels at the host-selected38Hz and presented155 frames through
the AHardwareBuffer/EGL presenter. The actual Manager installed and launched
the ordinary smoke APK. An outer-Android `input tap` traversed the host
`GuestDisplay`, API32 input bridge and guest InputManager; the guest button
reported its click. Two touch packets and two navigation-key packets were
accepted with zero rejections. The test requested reopening the last installed
app and observed the running session for40seconds.

Both crash-transport negative tests passed against the actual daemon: guest
UID39999 requests and a request targeting the host process were rejected.
Each24-request test preserved the daemon's FD count at12. The guest had one
main SystemUI process start, no captured ANRs, Java fatal headers or native
fatal signals. Init reaped15 children, with zero remaining or failed; the
owner exited0 and needed no forced cleanup. Host SELinux stayed enforcing.
The host limit/monitoring overrides stayed unset; a subsequent read confirmed
the effective phantom-process limit of32. No host limit was raised.

The [live-session evidence](api32-manager-session-validation.json) records the
exact APK/image/source hashes, failing trials, physical tap, presentation and
shutdown results, and commands. The default Android9/10 Manager also rebuilds
and passes its image-free distribution validator with unchanged host JNI.

This basic test alone does not establish repeated fresh-start stability,
offline Manager restart and
application-data persistence, an unmodified third-party APK requiring API32,
phone compatibility, complete networking/storage APIs, or encrypted CE
storage. The smoke APK's minimum SDK is28; its successful installation on
the actual 12L framework is not a substitute for the required API32 app test.
The broader completion flags remain false.

## Cached-image Manager restart and ordinary API32 app persistence

The additional `--persistence-probe32` fixture exercises the same production
Manager service and controls. Its ordinary, non-debuggable app has minimum
and target SDK32, checks its actual guest SDK and ordinary UID, uses the real
API31 attribution source and install-source APIs, and stores private state
through SharedPreferences. It creates a non-exportable AES256/GCM key through
AndroidKeyStore, stores ciphertext and its IV, and verifies decryption again
after the complete guest and outer Manager processes restart. This tests
software keystore persistence; it does not implement guest CE encryption.

Run `c4a71aa073624c3095f8035739781dd7` passed both per-boot strict checks on VM73.
The first guest boot installed the app through Manager and accepted a physical
host button tap and keyboard entry. The probe requested Home and then reopened
the saved app through Manager, waiting for a new actual activity-resume event
instead of accepting an old log marker. After40seconds of observation it
stopped and reaped the guest. The controller then stopped the image HTTP
server, force-stopped only the isolated test Manager package, and launched
Manager again. The host process changed from PID3217 to5794.

The second Manager process booted its cached image with no image requests,
retained its last-app preference, and reopened the existing package without
reinstalling it. The guest UID stayed10107; its PID changed from5208 to7609,
its launch count advanced from1 to2, and its private token, text, ciphertext
digest and ability to decrypt with the same key survived. A second physical
button tap increased the persistent count from1 to2. Each boot had exactly
one main SystemUI start, no captured ANRs or Java/native fatals, and its own
unchanged2MiB native-log capture limit. Each supervisor exited0 after reaping
15 children with zero remaining or failed. Host limit/monitoring overrides
were not changed and SELinux remained enforcing.

The image source was offline; the Android host's network was not disabled.
This verifies cached-image restart, not a general disconnected-network test.
The app is a controlled API32 fixture, not an unmodified third-party app.
See the [persistence evidence](api32-manager-persistence-validation.json) for
the complete app/host identities, source and artifact hashes, input records,
per-boot gates and commands. The test changes do not alter the production
Android9/10 DEX or any host JNI library; those remain byte-identical to the
preceding default Manager build and pass the distribution validator.

Fresh isolated data also passed the same two-boot test in run
`fe5ad2b7e29e47b394d6bdebe98ca1cd`. The controller cleared only the VM73 test
Manager package, downloaded the image once, and closed its download server
before the second boot. Both boots passed the unchanged strict startup and
shutdown checks. The ordinary app retained its private state and keystore
key while its host and guest processes changed. This is one fresh-profile
repeat; it does not establish general startup reliability.

The unmodified Signal Play APK set is now available for the remaining real
application test. Aurora Store on Linux Waydroid downloaded version8.33.2
(177201) using its Samsung S20+ API33 ARM64 profile. The base and both splits
declare minimum SDK32; the base targets SDK36. All three signatures verify
for SDK32 and match Signal's certificate. The original Waydroid profile and
installed Signal application were preserved. See the
[download evidence](api32-signal-apk-download.json). Download and signature
verification alone do not establish that Signal runs inside Pocket.

## Signal Manager test and install/replacement ordering

The separate `--app-probe32` Manager downloads the hash-pinned external APK
set, verifies each file, installs all splits through the production Manager,
and observes package metadata and stock activity/window dumps from inside
12L. It also captures the actual compositor image and retains the existing
ANR, fatal, log-size and supervised-shutdown gates. The first two runs showed
Signal's welcome screen and correct ordinary app UID, version, minimum SDK32
and certificate. They exposed two observer-format errors, now corrected
against the saved stock dumps with negative focus checks.

The next run exposed an actual replacement race: PackageInstaller reported
success, Manager launched Signal, and a queued package-removal broadcast from
the replacement then force-stopped the app and removed its activity. The
foreground check correctly rejected this run. No Java/native fatal or ANR
was captured, and all three guests stopped with15children reaped and none
remaining or failed.

Image0.78.3 adds the production Pocket Services package receiver and updates
only `pocket-manager.jar`; all other0.78.2 runtime entries are unchanged.
`PackageEvents32` holds the receiver through an external provider reference,
arms it before committing the PackageInstaller session, and waits for the
matching real `PACKAGE_ADDED` or `PACKAGE_REPLACED` event before launch. It
checks the event sequence, package, update time, version and ordinary UID.
The reference is released on success and failure. The provider is protected
by DUMP permission and an explicit system-caller check. A plain tool process
cannot substitute for this AMS app: the pinned12L BroadcastQueue skips
registered receivers whose application record is absent.

Run `7428ffe3516740dba5e6292bfd185e39` passed on VM73 with image0.78.3.
The real receiver (UID10108) observed `PACKAGE_REPLACED` for Signal UID10107
with a sequence increase of2 before Manager launched it. Both captures show
Signal's RegistrationActivity resumed and focused, with its expected SDK32
minimum, SDK36 target, version177201, two splits and signing certificate.
Manager accepted the two Home key events and reopened Signal; this version
did not independently assert the intermediate launcher's foreground state.
The host presented156frames. SystemUI started once, no captured ANR or
Java/native fatal occurred, the native log remained below2MiB, and init
reaped15children with none remaining or failed before its owner exited0.
SELinux and host process-limit settings stayed unchanged. Registration,
messaging, calling and standard guest connectivity APIs remain untested.
See the [Signal Manager evidence](api32-manager-signal-validation.json) for
the failed and passing runs, exact hashes, source snapshot and commands.
Broad support and startup-reliability flags remain false.

## Standard host-uplink connectivity integration

Image0.78.4 adds `HostNetwork32` to the existing input-control process and
rebuilds the runtime tool classpath from its current sources. It uses the
version-matched12L NetworkAgent and NetworkAgentConfig interfaces. The host
broker supplies addresses, routes, DNS, MTU, metering, roaming, suspension
and congestion state. The agent represents the shared uplink as virtual
Ethernet `pocket0`, updates it when the host metadata changes, and unregisters
it when host connectivity disappears or the broker becomes unavailable.
The service count stays15. This does not configure host interfaces or routes.
Guest NetworkMonitor owns the VALIDATED capability; the adapter does not
manufacture it from the host's validation flag.

The Manager network check now records the actual default Network and its
capabilities, and performs HTTPS through `Network.openConnection`. The next
VM73 test requires these APIs to report connectivity and receive HTTP200.
It also independently checks that Home focuses the resolved default launcher
before reopening Signal. Both ordinary and system-process Java fatal headers
are now included in its strict failure scan. Runtime results for this image
remain pending in the [host-uplink evidence](api32-host-uplink-validation.json).

Run `5f5e42dc1a60454aad45ba09df87bc51` completed the real launcher/Home check
and reopened Signal, then correctly failed connectivity:12L's NetworkAgent
base class is abstract, so reflective construction raised InstantiationException.
It captured no ANR or Java/native fatal and reaped all15services before its
owner exited0. Image0.78.5 replaces this with a concrete NetworkAgent subclass
compiled against the pinned12L system API stubs. Network retirement now follows
framework unwanted/destroyed callbacks, without a fixed registration grace
period. Only `pocket-tools.jar` changed from0.78.4. A DEX class-definition check
confirmed that the adapter is concrete, inherits the platform NetworkAgent,
and packages no Android stub classes. The stubs remain a build-only dependency.
The new test checks connectivity before app installation, while retaining the
Signal/launcher checks and strict crash/ANR/shutdown gates. Default-network
selection may complete asynchronously; the same60-second check budget now
records repeated actual API results. Its0.78.5 result is recorded below.

Run `2043d50c9e6f41039f845927276ba39f` registered the concrete agent as
network100, but correctly failed default-network verification. The stock
NetworkStack's TcpSocketTracker called `INetd.getFwmarkForNetwork` while
constructing NetworkMonitor; Pocket rejected the network-stack UID1073.
The constructor propagated SecurityException, so ConnectivityService never
finished bringing the network up. Raw HTTPS returned200, but the actual
default Network stayed null. No ANR or Java/native fatal was captured and
all15services stopped cleanly.

Image0.78.6 configures the stock guest DNS-only stall detector through a held
external SettingsProvider reference before registering the agent. The
setting is read back and verified. Normal Internet DNS/HTTP validation stays
enabled. Kernel TCP stall statistics are unavailable because Pocket's shared
transport does not assign per-guest kernel fwmarks. The native read-only
fwmark query now permits UID1073 and reports EOPNOTSUPP explicitly; ordinary
apps and network mutations remain denied. The integration test executes both
native role cases. The image builder also rebuilds its layout-2 native core
from current sources, verifies the matching hardware library, and reserves
the loader interpreter before packaging it. Only the native core and runtime
tool JAR differ from0.78.5.

Run `4a28a7713e59469b9734e333e0a3b3cd` passed the0.78.6 VM73 test.
The actual guest default network was100, INTERNET was present, and
`Network.openConnection` returned HTTPS200. Both native role checks passed.
Unmodified Signal opened, Home independently focused the default launcher,
and Signal reopened. There were183presented frames, one SystemUI process,
no ANR or Java/native fatal, and all15services stopped before owner exit0.
Host SELinux and process limits stayed unchanged. Guest VALIDATED was false
at the sampled check; this result does not establish asynchronous raw DNS
or later NetworkMonitor validation. The test's pinned source snapshot is
separate from subsequent resolver work in the current tree.

The0.78.6 native logs also showed why guest VALIDATED remained false:
NetworkMonitor's public asynchronous DNS calls received ENOTSUP from the
unimplemented `resnsend` command. Image0.78.7 adds real raw DNS forwarding
through the host resolver on API29+, with bounded packet validation,
original transaction IDs, guest UID policy checks, and privileged
private-DNS bypass enforcement. DNS-only stall monitoring receives typed
raw-query events. The installed Pocket Services app exercises the public
asynchronous query/rawQuery APIs from an ordinary UID and its real main
Looper. The pending VM73 test requires positive and NXDOMAIN answers,
permission grant/revoke/restore, denied direct-host access and unauthorized
private-DNS bypass, actual NetworkMonitor VALIDATED, network-bound HTTPS,
and Signal's Home/reopen check. Local packet fixtures, including native
AddressSanitizer/UndefinedBehaviorSanitizer coverage, and packaging validation
passed. Raw private DNS on an Android28 host explicitly remains unsupported;
it never silently falls back to plaintext. See the
[raw DNS evidence](api32-raw-dns-validation.json) for exact build pins and the
live job handle. These pending runtime checks are not claimed as passes.

Run `4f2099f80f694620a5bc5d28ad3c16f3` stopped before the explicit network
check: a process exited between owned-process enumeration and the test's
`/proc/<pid>/exe` read. The helper now ignores only ENOENT/ESRCH there while
still requiring exactly one owned crash daemon and both rejection checks.
No ANR or Java/native fatal was captured; all15services stopped cleanly.
The logs independently show NetworkMonitor receiving real asynchronous DNS
answers through the privileged bypass path. HTTP validation then failed:
`getaddrinfo` still treated the bypass bit as part of the network ID.
Image0.78.8 strips and authorizes that bit, obtains real local-server packet
answers for bypass lookups, and preserves it in `getdnsnetid` responses.
The ordinary-UID native test additionally requires bypass getaddrinfo to be
denied. The Manager includes the separate legacy raw-DNS guard, and test
failures retain bounded stack traces. The new native core builds and the
expanded packet fixture passes under ASan/UBSan. The earlier failed report
and source pins are preserved alongside the successful rerun below.

Run `5cef4bf755304c02a3ff914e66f731d3` passed the0.78.8 VM73 test.
NetworkMonitor received real DNS answers and HTTP/HTTPS204 responses, then
ConnectivityService marked network100 validated. An ordinary installed app,
UID10108, exercised public asynchronous typed and raw DNS and observed a real
NXDOMAIN response. The separate native client passed Internet permission
grant/revoke/restore, direct-host denial, unauthorized bypass denial for raw
DNS and getaddrinfo, and two typed raw-query events; its temporary grant was
revoked. Network-bound HTTPS returned200. Unmodified Signal's pinned Play
splits installed and focused correctly; Home independently focused the
resolved default launcher and Signal reopened. Both screenshots were visually
inspected. The run presented159frames, captured one SystemUI start and no ANR
or Java/native fatal, and reaped all15services before owner exit0. Host
SELinux and process limits stayed unchanged. This verifies the pinned network
and app-startup scope; storage integration, slow/error cancellation, broader
startup reliability and production/phone validation remain outstanding.
The current tree also fixes the DNS diagnostic's cancellation cleanup: each
request owns a cancellation signal, all pending requests are cancelled, and
queued callbacks drain before executor shutdown. The bridge rebuild and
SDK32 signature verification pass; this follow-up was made after the pinned
0.78.8 run and awaits next-image runtime/error-path validation.

Adding the package observer exposed non-deterministic APK ZIP timestamps.
The Java-only APK builder now fixes ZIP metadata before signing. Two fresh
observer APK builds were byte-identical and verified for SDK32. Two complete
0.78.4 image builds in separate output directories also produced identical
SHA256 `d5a911533a9ed2391b6f6a436bd1dcdb5383450d1c81956ceeac7c72922a9845`.
This establishes deterministic packaging with these pinned inputs and
toolchain; it does not establish rebuilding Google's upstream SDK system
image from AOSP source.

## Encrypted storage integration

The 12L filesystem preparation source includes `/data/vendor_ce/0` as a fifth
CE root. The encrypted namespace now has a separate format 3 for this layout.
Legacy four-root stores keep format 2, where inode 7 can still be an ordinary
file. Format 3 reserves inode 7 for vendor CE and starts ordinary nodes at 8.
The namespace reads its layout from authenticated encrypted metadata. Migration
journals also record the layout before cleanup; a journal/guest mismatch aborts
while preserving plaintext. All five source roots are cleared and sealed only
after ciphertext verification. A separate profile marker enables vendor CE
routing in the loader. The native adapter requires that loader capability,
so an old interpreter cannot silently prepare plaintext vendor CE data.

VM73 run `1d40a1a99c7a46fdb5cbebe2a6b17784` passed the five-root transport
fixture with real AppFuse, migration, locked-file denial, vendor file writes,
fsync, virtual working directories, ownership checks and reopen persistence.
It also passed SQLite/WAL, shared and executable mappings, file locks, links,
and host-keystore default-slot removal/restoration. The vendor plaintext
source was empty afterward. The same APK then passed the legacy four-root
regression, run `5787ca1d985b4ce588733669e484cd47`. Both runs retained host
SELinux and process limits. The host CE JNI library is byte-identical to
Manager 0.76. The image builder's freshly compiled loader is byte-identical to
the tested loader template. See the [pinned evidence](api32-ce-storage-validation.json).

This fixture uses API28 Bionic for native file operations; it does not run
the 12L framework credential lifecycle. The typed 12L IVold adapter and combined
native core build with CE unlock, lock, auth/default-slot preparation,
fixation, unlocked-user queries and explicitly armed migration-secret capture.
Those Binder operations still need runtime validation. Experimental image
0.78.9 integrates a system-owner LockSettings helper, SP disk-secret capture,
verified migration and Manager32 encrypted startup. Credentials enter the helper
over stdin; the framework's disk secret stays inside the vold/backend handoff.
A separate nondebuggable Manager test package checks five guest boots, wrong
and old password denial, password change/removal/restoration, and private data
plus a nonexportable AndroidKeyStore key in an ordinary min/target32 app.
The first run failed the storage-capacity check before any image download or
guest boot. After clearing the stopped disposable component-test sandbox,
the same image/APK were retried on VM73; the previous Signal Manager
profile is preserved. Run `e939510f43ef4d27a39e1565b030325d` set the real
LockSettings password, captured its 32-byte SP disk secret and completed the
five-root migration. Its plain boot passed the complete fatal/ANR scan and
reaped all 15 services cleanly. The encrypted boot reached correct-password
unlock but failed ordinary app state verification. installd could not read
the inherited host SELinux label from virtual app directories, and its recovery
path tried to recreate app data. Launcher also hit EINVAL in canonicalize0:
12L Bionic opens O_PATH, then reads the final `/proc/self/fd` magic link, which
the CE handler incorrectly dereferenced as an ordinary directory.

The metadata fix preserves the actual inherited host label as read-only
encrypted inode metadata and returns namespace paths for self-fd links. It
does not load a guest SELinux policy or relabel host files. The failed started
CE profile and its report are retained. Focused namespace tests and both five-root and legacy native file transport
tests pass. VM73 runs `b7594f6e37c54db0a1cbd4d0562e8009` and
`64fdf5320da7437ab303633104f30cb2` checked read-only inherited labels,
virtual fd links and canonical paths before and after storage reopen, alongside
the existing SQLite/WAL, mmap and file-lock tests. Image0.78.10 and its separate
Manager test package `.api32ceimage2` rebuild and validate successfully. Its
loader is byte-identical to the tested fixture; host CE JNI is unchanged.
The new package preserves the failed started CE profile for inspection.
The full five-boot Manager test now passes on VM73, run
`d2cf3f83999442a3be7f61274766c641`. It set and verified a real12L password,
captured the SP disk secret through vold, migrated all five roots, rejected
wrong and old passwords, changed/removed/restored the password and verified
automatic unlock only during the password-free phase. The same ordinary
min/target32 app retained its UUID, ciphertext and nonexportable AndroidKeyStore
key across all four encrypted reboots; canonical private paths also matched.
Each complete cycle passed the fatal/ANR scan, had one SystemUI process and
reaped all15services with zero remaining/failed before its owner exited0.
Host SELinux and process settings were unchanged. The saved report and
119 source pins were independently rechecked. See the
[credential-storage evidence](api32-ce-storage-validation.json).

This validates the credential and encrypted-file lifecycle across guest boots.
Encrypted persistence across an outer Manager process restart and real keyguard
UI entry remain separate checks. Complete MediaProvider/FUSE storage, release
hosting and phone compatibility also remain unfinished.
No kernel fscrypt keys or hardware-backed encryption are claimed.

## Remaining implementation and release gates

The userspace FUSE packet transport now passes against the unmodified
packaged12L libfuse/Bionic on VM73, run `90a34c24c30245678c705fa93c55ecdb`.
It negotiated protocol7.31 and exercised lookup/read/write, valid errno
responses, caller fields, release/forget/destroy and clean thread/descriptor
shutdown. The inherited host policy denied cloning `/dev/fuse`; the library's
shared-descriptor fallback handled the packet socket. ASan/UBSan host tests
also covered malformed replies, timeout, disconnect and error recovery.
These tests use small native callbacks. They do not establish MediaProvider
policy, app filesystem routing or a mounted external volume. Those remain
the next integration work; see the
[transport evidence and session contract](api32-emulated-storage-validation.json).

The source-built12L zygote now forwards its explicit external-storage mode to
the loader before dropping logical UID0. The loader preserves the mode across
fork/exec without kernel namespace operations. Native VM73 tests passed mode
selection, invalid-value rejection, control-API mutation denial after UID drop,
and preservation of64groups and the denylist flag alongside the new field.
The zygote rebuild passes; its actual framework handoff remains unverified.

The filesystem broker's peer-identity helper also preserves the actual calling
thread ID. MediaProvider uses that ID for PendingOpenInfo during delegated
file opens; substituting a process ID would lose that context. The helper
checks kernel peer credentials, thread membership and a live PID/start-time
record instead of accepting app-supplied UID/GID fields. Native tests covered
root and dropped-UID contexts, same-process worker threads, rejected foreign
threads and GID-only changes. The loader now refreshes identity records after
successful GID changes too. This remains rootless guest metadata rather than
a separate kernel guest-security boundary. Real MediaProvider session and
app-file routing validation are still pending.

The separate host external-file bridge now passes an isolated VM73 test,
run `ee1e693740a54f76a5aea4fc8fb2ea47`. Real AppFuse descriptors were passed
to guest Bionic through the loader at logical UID19991. Shared mmap/msync,
pread/pwrite and dup stayed coherent; an EACCES exception reached the guest as
errno, and main/worker kernel thread IDs and per-open handles were preserved.
fdatasync/fsync flags and release of all four handles were checked. Another
file remained readable while a sync callback was blocked. Existing CE JNI
keeps its own ABI; the new bridge uses an independent library.

The userspace FUSE channel also now dispatches concurrent replies by unique
ID instead of waiting for each operation before sending the next. The packaged
12L libfuse test passed a read before releasing a blocked fsync, run
`9c0e659cdeb648269a81180b98ec74c6`, alongside the existing protocol, identity,
mode and descriptor-leak checks. ASan/UBSan host tests passed out-of-order
payload dispatch and cancellation/protocol failures with multiple pending calls.
Both layers use test callbacks in these proofs. Connecting them to the actual
MediaProvider session and app path routing remains unfinished; no emulated
volume is advertised yet. Exact commands and reports are in the
[external-storage evidence](api32-emulated-storage-validation.json).

The loader's MediaProvider PASS_THROUGH mapping now reaches the lower media
tree; forced `/mnt/user/0/emulated` paths remain separate. Native VM73 tests
passed this view and its non-bypass check. A separate trial now also starts the
unmodified packaged MediaProvider's real FUSE daemon and negotiates INIT before
waiting for its framework callback. Run `a3383963946542508e3b6906fda7afef`
passed user-directory metadata, stock denial of ordinary-app root/private-FUSE
access, and clean session shutdown. Its framework boot/install had one SystemUI,
no ANRs or Java/native fatals, and reaped all15services with none remaining.
The reversible test retained the plaintext profile and restored its original
loader/zygote binaries. This is a session/metadata proof; file data, MediaStore,
per-app lower views, production vold callbacks and AppFuse routing still need
integration.

The trial exposed untranslated Unix datagram destination paths: zygote's
`sendto` exit notifications targeted the host's `/data/system` socket path.
The loader now translates filesystem `sendto`/`sendmsg` destinations, with
native tests for connected sends and SCM_RIGHTS/CLOEXEC preservation. The
successful framework trial includes that fix and has no such zygote write
errors. Earlier trials still document default-host phantom trimming and startup
ANRs; one clean trial does not establish the required stability cohort. Abstract
Unix name isolation and precise PhoneFactory bind-error diagnosis remain open.

The first real file-data integration is now functionally verified in run
`b20b79c7f99a4e5ea1ef14009425fa74`: the native broker running under an
ordinary app UID (10109) opens its own `Android/media` file through the
unmodified provider. The app consumes explicitly supplied host AppFuse
descriptors and performs read/write, shared mmap/msync and fsync.
The host independently checks the backing bytes. A second direct-I/O open
observes synced writes, read-only writes are rejected, main/worker kernel
TIDs survive the callbacks, and both native handles are released. A guest
process sharing the real host UID cannot impersonate the host callback peer.
This trial still fails the strict boot gate: SystemUI/FallbackHome ANRs and
three SystemUI starts occurred. All15services were reaped and the original
guest binaries were restored. See `realProviderDataPath` in the
[storage evidence](api32-emulated-storage-validation.json).

The subsequent run `709fb3eaaaaf4d42896ca5683022401f` passes these same
data checks and the strict full-framework gate: one SystemUI, no ANRs or
Java/native fatals, zero observation errors, all15services reaped and both
file handles released. The run preserves a snapshot of all166 pinned runtime/test sources. The completed host fixture is stopped, and the retained profile's
original code and data are preserved. Host activity events identified an
extra source of load in the failed trials: replacing the foreground Manager
APK recreated an older CE probe task underneath it, which booted another
guest. The runner now waits for the host launcher before installing; the
passing trial has no CE probe restart. One strict pass does not establish
fresh-profile reliability or production mounted-storage integration.

The initial broker failed because the child's direct lookup of the real
Manager's proc stat file returned ENOENT. File callbacks now use the existing
InputControl32 lifetime-descriptor pattern: authenticate kernel socket PID/UID,
receive the parent's open kernel stat descriptor via SCM_RIGHTS, and validate
its procfs filesystem type, fd link and pinned process start time. No host dumpability or SELinux
setting changes are needed. Host sanitizer tests reject copied stat text,
missing/excess/truncated rights, stale start time, wrong peer PID, bad capability
and malformed frames, with zero descriptor leaks. Typed file operations retain
open flags and caller identity, preserve actual errors, and match Linux FUSE's
optional flush/fsync ENOSYS semantics. These checks do not establish cache
coherence, mounted storage, MediaStore or production filesystem routing.

Ordinary existing-file path opens now pass the strict framework gate in run
`643351d0de514275ae91aa4c4c5fcfcc`, using the retained 0.78.20 image and a
snapshot of 176 runtime/test sources. Java `Os.open` reaches the real provider
through the loader, authenticated app broker and host AppFuse. The app verifies
RW access, the forced/direct and sdcard alias paths, CLOEXEC, shared mmap/msync,
fsync, read-only write rejection, and cross-package/backing-alias denial. Real
provider filesystem capacity reaches `Os.fstatvfs` (2096640 blocks, 4096 bytes
per block). Main/worker callbacks retain their actual kernel TIDs and UID 10109.
All three descriptors are released, the attach accept thread and workers stop,
all 18 supervised services are reaped, and no ANR or Java/native fatal occurs.
The fixture is stopped and original code/data are preserved. This remains an
opt-in existing-regular-file route; production volume mounting is not enabled.

The statistics fix normalizes the loader checked-copy byte count to zero,
as required by `fstatfs`. Stock Bionic `fstatvfs` only converts its output when
that return is zero. Matching the host scaffold to the real pipe copy first
reproduces the failure; the sanitizer regression now also checks EFAULT for an
invalid destination. Host listener shutdown explicitly wakes blocked `accept`
before closing the socket, and the Android gate requires both its thread and
worker pool to terminate.

Startup still exceeds the host process budget. A concurrent read-only census
observes 37 guest children while the unmodified host reports
`max_phantom_processes=32`. The preceding run of the same APK lost its shell
and init supervisor to explicit host phantom trimming and failed. The later
strict pass does not resolve that scheduling-dependent failure. Process
consolidation/bounded startup and a repeated-start cohort remain release gates;
no host limit or monitoring setting was changed. Exact commands, source pins,
failed trials and cleanup evidence are in `ordinaryAppOpen` in the
[storage validation](api32-emulated-storage-validation.json).

On-demand media startup now passes the strict combined gate in run
`2e583e534c8347068b1fcb9356c9e741`. The Binder and HIDL registries request
only allowlisted init services and retain their original guest roles. Native
C++ lookups activate a missing service through the typed manager before using
the stock bounded wait; `checkService` remains observational. The private
`ServiceManagerShim` declaration comes from the pinned 12L source. The HIDL
entry shim now honors retry for declared lazy services through the stock wait
implementation and Pocket's private manager. Real cold publication, duplicate
subscription suppression, existing-service callbacks and unsubscribe pass.

The extractor previously scanned plugins in its constructor before registering
its Binder, exceeding the client's five-second lookup window. Its API32 media
preload now starts the original scan on a thread and waits for that real scan
before extraction/type queries. The guest reads an actual mono 8kHz PCM WAV,
checks its format and all 320 sample bytes, then passes the ordinary-file,
AppFuse and access-denial regression. Completion markers use fully written
temporary files and atomic rename, preserving the strict nonce check. There
is one SystemUI start, no ANR/Java/native fatal, all 17 service lifetimes are
reaped and the host fixture is stopped. All eight original patch targets and
the retained data are preserved. The run snapshots 187 runtime/test sources.

This is still an experimental reversible-image trial. Its census observes
34 guest children overall, including 33 before first observing the activation
probe, against the unchanged default limit 32. Deferred startup alone does not
establish a safe process bound. More consolidation, repeated starts, actual
codec/camera workloads and integration into the final downloadable image
remain necessary. See the [lazy-service evidence](api32-lazy-services-validation.json)
for exact commands, binary/source pins, failed trials and cleanup.

The boot shell now execs init after transient linker/classpath preparation.
Init owns the remaining startup commands as a oneshot service, reaps that child,
validates its staged run nonce, and atomically publishes readiness. Run
`fb763d006cb14322852444498edf37f0` passes both negative cases (exit 7 and exit 0
without readiness), the full framework/media/storage gate, one SystemUI, and
clean shutdown of all 18 service lifetimes. The census confirms that init is a
direct child of Manager and the shell is absent during the active workload.
The completed boot-owner trial restored its nine original code/configuration
targets and retained guest data. See the
[supervised bootstrap evidence](api32-supervised-bootstrap-validation.json).
The observed peak remains 34; eliminating the resident shell alone does not
establish host process-budget safety.

The immutable ActivityManager cache default is now two processes. Run
`d143e15ac7b1474d82c4279f0c515e6f` verifies the actual constructor/current
settings and absence of an override, alongside the strict media/storage and
bootstrap-failure gates. The sampled peak from the activation probe onward is
32, compared with 34 in the preceding four-cache run. Startup still reaches 34,
including temporary shell/printf and Traceur subprocesses; this does not prove
a safe process bound. All 11 original code/configuration files are restored and
retained guest data is preserved. See the
[cache-budget evidence](api32-cache-budget-validation.json).

The subsequent run `d732bfc5c91e46c3bc588b6168e5a469` replaces the readiness
marker's external printf with Android mksh's explicit builtin print. Both
negative bootstrap fixtures verify option-like arguments, and the normal boot
again passes the strict combined gate. Its census samples 33 children during
startup and 32 from the first activation probe onward. The peak still includes
the temporary startup shell/helper and the separate input bridge's ART/idmap2
processes. These remain consolidation candidates; neither a hard process bound
nor repeated fresh-profile reliability is established. All 11 original patch
targets are restored, the host fixture is stopped, and guest data is retained.

The input/network bridge now shares the actual system-server process. A small
version-matched native hook preserves AndroidRuntime's original child-init call
and starts the bridge only in the marked system-server child, after ART fork
hooks. It reuses the existing system context, binds its socket after fork, and
keeps the host kernel PID/UID and lifetime-descriptor authentication. Normal
source staging selects this layout and rejects stale native/JAR inputs. The
complete downloadable image is rebuilt in the subsequent source-image trial
below; its runtime result is recorded separately.

Run `633e66fed3984593804594f36fdff5e4` passes real ordinary-app key/touch
delivery, input-peer denial, NetworkMonitor validation, network-bound HTTPS,
WAV/file-data checks and the strict lifecycle gate. The bridge PID matches the
actual system_server census row, with no separate input service. Its boot peak
before media activation is 29; the complete install workload still reaches 33.

Two repeat trials then expose a cold extractor lookup missing the original
five-second client wait. Deferring its constructor's plugin scan entirely still
leaves process initialization too late. Run `2b36e670ca4e4f1c8b0c21551010f80a`
starts the extractor and waits for its real Binder before framework boot; its
plugins load on first data/type demand (823 ms in this run). Codec/player startup
remains on demand. Actual publication ordering, WAV extraction, file-data,
key/touch, peer denial and validated HTTPS all pass again, with one SystemUI,
no ANRs or Java/native fatals, and all 17 service lifetimes reaped. The run
snapshots 200 runtime/test sources and restores all 13 original patch targets.
Its sampled boot peak before activation is 27, and the full workload peak is 33.
One passing run does not establish a safe process bound or fresh-profile
reliability. Complete media/image and mounted-storage integration remain open.
Exact commands, failed trials, source pins and cleanup are in the
[system-server bridge evidence](api32-system-server-bridge-validation.json).

The normal source staging now includes media, camera, authenticated optional
ADB, stock command-line tools and microG compatibility alongside the embedded
system-server bridge. This replaces additions previously supplied only through
local patch scripts. Image `0.78.23` retains all 2,649 entries of the prior
complete image. Twelve stock executable `.text` sections match their SDK
references, and two packaging builds produce SHA-256
`ecb21b99897610fb0fc31e2182b45c5157d5b93fc8c9de10f435d1386a639d2f`.
These builds use the same staged fixture with selected native outputs reused;
they do not establish clean-room source reproducibility. The first complete
image run boots but fails an obsolete test check that reads the retired
pre-init log. Correcting the check to observe the embedded system-server log
allows run `67aab3bcb54a452a8750688504914562` to pass unchanged API32 Signal
installation, foreground launch, Home/reopen, validated network-bound HTTPS,
public async DNS and permission-denial checks. All three microG hooks and the
embedded bridge are present together. There is one SystemUI start, no ANR or
Java/native fatal, and all 17 service lifetimes are reaped. Its sampled child
peak is 30; this does not prove a hard bound or repeated/fresh-start reliability.

A subsequent fixture rebuild without `--reuse-native` invokes every adapter
builder and stages 2,685 files. Exactly one differs: the cached standalone DNS
executable. Image `0.78.24` contains that rebuilt executable, with all 2,649
archive entries verified, but has not yet been booted. Downloaded source and
compiler caches remain populated, and Manager host native libraries still come
from the pinned base APK. Media/input/storage validation now has a packaged
image mode that replaces no runtime code and checks its code hashes before
and after the session. Run `403fc1aa115b49908f777dca98a44bda` confirms unchanged
packaged code, the negative bootstrap fixtures and the two-process cache
default, but fails when cold `media.player` activation misses the unchanged
five-second client lookup. It stops cleanly, with one SystemUI, no ANR/fatal,
and a sampled peak of 30 before the failed workload. File-data and input
validation are not reached. Normal startup now starts the stock player and
waits for its Binder before framework boot, keeping the codec on demand.
Image `0.78.25` packages this change; its only payload difference from the
all-builder `0.78.24` image is the boot script. Run
`3484900f8f9442739fe7bc4671eda639` passes full download, boot, Signal and network
checks, with player publication preceding framework startup. Run
`045487726dfd49548b02a2d8cf687f51` then passes the combined media, regular-file
AppFuse, key/touch, validated network, memory default and negative bootstrap
checks against the packaged runtime without code replacement. All 13 observed
runtime code files retain their hashes. Each run has one SystemUI, no ANR or
fatal, and a clean shutdown of all 17 service lifetimes. Each sampled peak is
32; neither establishes a hard bound or repeated fresh-profile reliability.

`tools/build_host_native.py` now builds all four ARM64 Manager host libraries
and records their source hashes, commands and output hashes. The Manager builder
accepts this manifest through `--host-native-dir` and verifies the packaged
library bytes. Its resulting image-free APK passes SDK inventory, notices,
ARM64 and 16-KiB alignment validation. Current CE bytes match the pinned base;
runtime/frame allocated sections also match, while PRoot code differs and needs
legacy backend regression. Native source/dependency caches and the base APK's
resources remain inputs, so this is not a clean-room reproducibility claim.
Run `f9aa3a730c2448d0ab35b6c9e799f64e` passes Signal install/launch/Home/reopen,
validated HTTPS, async DNS and clean shutdown using the rebuilt host libraries.
It has one SystemUI and no ANR/fatal. Its sampled process peak is **34**, so the
normal process-budget gate remains open despite the functional pass. A fresh
third encryption-test profile preserves the two earlier profiles and checks
five password-lifecycle boots, physical keyguard input, then a sixth boot after
restarting the outer Manager with the image server offline. The test credentials
stay in the host driver's memory; the guest persists only its normal encrypted
credential state. This encryption regression stops on the password-free boot: decrypted canaries are readable, but Android is still asynchronously unlocking user 0 when the inspector requests the CE provider. The first three stopped cycles pass their strict gates. The inspector now waits for `UserManager.isUserUnlocked()` before the provider request; run `7dad2e51a44d4f6ab8b8b10505145028` verifies the correction: it waits 5.6 seconds for Android user unlock and preserves the original app token, cipher digest, UID and keystore key. That run still fails the shutdown gate because Manager finds surviving processes after init exits with all 16 service lifetimes reaped. The tested image is 0.78.25 and its sampled peak is 33. Run `fde5db0b580b49fc9719bdecdea0710f` then passes with an init-only reversible subreaper trial: all 16 declared service lifetimes and 14 adopted descendants are reaped, no Manager fallback cleanup is needed, the original executable is restored byte-for-byte, and the app/key remain intact. This trial still uses the retained 0.78.25 profile and samples a peak of 33. The complete 0.78.27 image now includes the new init as its only payload change from 0.78.26, retains all 2,649 entries, and declares kernel child ownership for strict validation. Packaged run `1e027ce3054347aaaf77a9cfe2b61991` passes Signal/network, one SystemUI, no ANR/fatal, all16 service lifetimes and12 adopted descendants reaped with zero fallback cleanup. Its peak is32 across160 census samples. The installer verifies the whole incoming archive and reuses2597 exact installed files, writing18,064,398 bytes; this validation/reuse/copy step takes146,900ms. Combined run `199e135fdafb4d97b60266386a689103` passes media/file/input/network and all13 unchanged code hashes, with16 service lifetimes and14 descendants reaped. Its late census covers only the workload, so it does not establish the startup peak. The preserved encrypted-profile regression against complete0.78.27 passes four strict stopped boots (password set/change/removal/restoration and unchanged app/key), then fails the physical-keyguard unlock check in run `ed1b907536e84efdacfc5465eaadab78`. The saved pre-entry frame remains the clock lock screen: the swipe did not establish password-bouncer readiness. All input packets were accepted, which does not prove password-field focus. The failed guest stops with owner exit0; its encrypted profile and owner-only Pi recovery credentials remain preserved. The outer Manager restart and restoration of the original password-free state were not reached. Full mounted storage, repeated/fresh-start budget reliability, actual media/camera workloads, legacy regressions and release validation remain open. See the [child ownership evidence](api32-child-ownership-validation.json). Real keyguard input and outer Manager restart are not yet verified on this source-built host/image combination. The failed run and encrypted profile are preserved.
See the
[source-image evidence](api32-source-image-validation.json).

The stock `media.metrics` implementation now has an entry in the existing
supervised native core. Normal startup enables it explicitly, waits for real
publication and removes its standalone service. Legacy component boots retain
the separate layout. The service runs with the core's logical UID 0 instead of
1013; the stock Binder caller checks and record limits remain. Its standalone
process-wide 128-MiB address-space limit cannot apply to the shared core and is
omitted. Activation sanitizer tests pass. Image `0.78.26` retains all 2,649
entries, changes five payload files and preserves the stock service-library
bytes. Its source-built-host Signal/network run `38411e935fbd477cbc4458eb0a29825d`
passes, with one SystemUI, no ANR/fatal and all 16 supervised service lifetimes
reaped. Its census samples 32 children at peak across 234 observations, with
no observer errors. Publication is in the actual core PID with no standalone
metrics process. Run `8eb2d6401c33443eabafac0fc7123d50` also passes the combined packaged
media, existing-file AppFuse, input, validated HTTPS and negative-bootstrap
checks. Typed stock AIDL submissions from UID 10021 replace forged UID/package
claims and reject a forbidden record. Publication is verified in the core PID
before framework startup, and all 16 service lifetimes are reaped. All 13
observed runtime-code hashes remain unchanged. Its census again samples a peak
of 32 across 52 observations with no errors. A hard process bound, repeated
fresh-start stability and complete mounted-storage integration remain unproven. See the
[media-metrics evidence](api32-core-metrics-validation.json).

- Port the version-matched native hardware, graphics, storage, networking,
  package installation and credential service interfaces required by 12L.
- Complete emulated storage/MediaProvider FUSE sessions, then verify encrypted
  CE data across outer Manager restarts and real keyguard UI password entry.
  The [pinned session contract](api32-emulated-storage-validation.json) records
  the packaged FUSE ABI, initialization ordering and remaining runtime checks.
- Complete host display/input integration and a usable launcher under the
  rootless 12L service supervisor; verify application installation, network access,
  restart persistence and an unmodified real application requiring API 32.
- Fit normal startup within the host's default process budget and resolve the
  intermittent SystemUI startup ANR without relaxing the failure gates.
- Validate repeated fresh-profile Manager starts and an unmodified
  third-party APK requiring API32.
  Publish the supported downloadable image over HTTPS after its remaining
  runtime gates pass.
- Verify the packaged Manager/image path end to end without modifying existing
  Android 9/10 profile data. Validate on the phone when it is available again.

Native/ART/zygote test passes do not establish these remaining gates.

The trusted FUSE broker now implements fresh path/handle attributes, mkdir, exclusive/create races, truncate, cross-directory rename, unlink/rmdir, and directory handles with paginated READDIRPLUS and balanced lookup references. ASan/UBSan tests exercise real temporary Linux files and pass. Real-provider run `1ae323c6189b42aea147d4ee6f7ef0c5` successfully creates three directories, then CREATE is rejected because `external_primary` has never been mounted/attached; only the internal volume is published. The failed workload retains strict failure status, while all16 service lifetimes and adopted descendants stop cleanly, all13 packaged code hashes remain unchanged, and the observed peak is32. Implementing the production volume lifecycle together with ordinary-app namespace/dirfd routing is required before this workload can pass. See the [namespace evidence and exact validation commands](api32-namespace-validation.json).

Application namespace RPC v2 now routes metadata and file mutations to the typed broker. Descriptor metadata/resize require a transferred kernel FD matching an active Manager-approved proxy; callers cannot substitute guessed slots or UIDs. Path/fd attributes agree, creation carries the real umask without changing it, unsupported lower mutations and cross-volume renames are rejected, and v1 regular-file compatibility remains supported. ASan/UBSan broker/client and legacy file/RPC/loader tests pass. Native run `62981a02a68d4c6ebac19c9d5ee3c6ae` passes on VM73 with the real ARM64 loader and pinned12L Bionic after correcting a retained fixture-only missing-tmp failure. This test uses a real-file-backed FUSE wire peer and a C dispatch adapter; it does not establish real MediaProvider policy, a host proxy mount or normal mounted-volume SIGSYS routing. Directory FD transport, trusted core/Manager embedding and the production volume lifecycle remain necessary before enabling the namespace marker in normal Manager. The real-provider external_primary failure remains open. See the [namespace RPC evidence and exact commands](api32-namespace-rpc-validation.json).

The directory VFS follow-up now passes through ordinary pinned12L Bionic calls
in an independent ARM64 guest process at logical UID10001. Run
`807b6ff345d4461abac70cccf71b71e9` exercises opendir/readdir/rewinddir,
path/FD attributes, sync and descriptor errors, plus held-dirfd open/stat/mkdir/
rename/remove after a parent rename and removal of a held child directory.
Directory capabilities use kernel socket lifetime across dup/fork/SCM_RIGHTS;
lookup-reference bundles keep relative-open ancestors alive after parent close.
ASan/UBSan directory, namespace, regular-file and legacy RPC/loader checks pass.
The component uses a real-file-backed FUSE wire peer and an explicitly synthetic
directory volume device; it does not establish real MediaProvider policy or a
kernel AppFuse mount. Normal Manager's namespace marker remains disabled. Full
path/cwd/descriptor coverage, trusted-core/Manager embedding and the actual
external_primary mount lifecycle remain open, together with the other full-image
gates above. Failed fixture trials and exact commands are retained in the
[directory validation evidence](api32-directory-validation.json).

The emulated-volume backend now has a native session owner inside the storage
core and an authenticated Manager AppFuse adapter. The shared session header
passes ASan/UBSan ordering and rollback tests. Native Pi run
`ffd26e6792ac4ac7b1be458f9d44488d` passes ordinary UID10001 data/metadata,
create/unlink and shared-mmap calls through actual host AppFuse descriptors;
revocation makes a held FD return ENOTCONN, and all provider handles/references
are released. Its FUSE peer and mount callback are models, so this does not prove
stock MediaProvider volume policy. Complete experimental0.78.28 retains all
prior entries and changes only the core, loader and backend selector. Its
image-free Manager includes the separately source-built AppFuse JNI. The full
retained-profile run `034bb49d93614fa680368973afe634fa` fails before the core
starts: namespace routing rejects bootstrap's emulated-storage symlink setup
with EOPNOTSUPP. Init reaps its only child and exits67; the sampled peak is3
and final count0. Mounted storage and the external_primary MediaStore query
remain unproven. The follow-up scopes only trusted UID0 bootstrap alias setup
and rebuilds current preparation code; native run `e7d5c294805f44fda32ba4898486697e` passes that scope and repeats
real AppFuse data/revocation checks. Corrected0.78.29 is built and audited,
changing only the loader and preparation executable from0.78.28. The corrected full
framework trial is running under its own recorded job and process observer;
mounted MediaProvider behavior remains unproven. See the [volume implementation and validation](api32-volume-validation.json).

The next0.78.29 trial (`30a3290ce0844caaa708d6eb91284913`) successfully
installs and focuses unchanged Signal, verifies actual launcher Home/reopen,
network-bound HTTPS and two mounted external_primary queries through a system
external-owner token. The initial observer used an unregistered app_process
IApplicationThread and was corrected to the pinned stock content-command
contract. The subsequent original report remains FAIL because its verifier
looked for native core stderr in framework logcat; the successful mount record
is in init console. Replaying the corrected strict gates on the unchanged
report/catalog/APK evidence passes. All16 services and13 adopted descendants
stop cleanly; no ANR, Java/native fatal or SystemUI restart is recorded. This
is mounted-framework/system-query evidence, not ordinary app CREATE/private
external-storage policy or explicit unmount/remount proof.

New O_PATH handles privately pin provider-approved lower objects and return
last-kernel-reference capabilities to apps. Kernel proc-fd links supply canonical
rename/unlink tracking without reconstructing old pathnames; no lower anchor
FD is exposed to apps, and intermediate lower symlinks are not followed. The
host ASan/UBSan path test passes, including forged carrier rejection, invalid
buffers, outside-root rejection and balanced references/FDs. Native run
`f8b533615ae54a4a9c796880e03d47e5` passes actual ARM64 Bionic realpath, aliases,
relative O_PATH, regular AppFuse proc-fd links, metadata and rename/unlink;
held data revocation and cleanup also pass. It uses the model FUSE peer, not
stock MediaProvider. The production native core rebuild passes. A registered,
unprivileged app workload now builds to test real MediaStore/path mutations
and own external-files access with the upcoming0.78.30 image. General dotdot,
symlink/cwd/poll/fstatfs, final mutation slash semantics, CE and other release
gates remain open. Exact artifacts and distinctions are in the
[volume validation evidence](api32-volume-validation.json).

Complete experimental image0.78.30 is now built and compared byte-for-byte with
0.78.29: only the source-built core/loader and their version/hash manifest
change; all other entry contents and modes match. Its image-free Manager
passes the download/native inventory validator. The dedicated Pi73 full-image
trial is running the registered ordinary-app storage workload with all709
source pins snapshotted and checked before launch. Its result is pending;
the new ordinary-app/MediaProvider requirements must pass before claiming
production path/storage compatibility.

Image0.78.30 trial `a37b696594794e80838e2a52c01d8889` fails the registered
ordinary-app workload. Its initial query passes; the saved screenshot shows a
subsequent URI open failure and cleanup SecurityException. Real provider logs
derive an internal/file URI from an external_primary insert. The observer also
used the wrong Activity dump command, so its missing report is corrected by
passing only the component matcher and retaining bounded raw diagnostics. The
original report, screenshot and failed status are preserved; all16 services
and14 descendants stop, without ANR or fatal exception.

The follow-up implements vold app-directory setup/fixup/ensure operations with
user0/app UID validation, mounted and CE-unlocked gates, safe lower dirfd walks,
symlink rejection and guest UID/GID/mode metadata. Host ASan/UBSan checks and
production-core rebuild pass. PASS_THROUGH reverse canonicalization now keeps
owned media paths in the /storage/emulated view rather than exposing /data/media.
Native run `5530e79aa15e4e29b187467c8a4a6f6d` passes actual guest ownership,
setup/fix/ensure and Bionic canonical paths for both storage and sdcard aliases,
while repeating ordinary AppFuse/path/revocation checks. It still models the
FUSE peer and does not prove real provider policy. Complete0.78.31, differing
only in core, loader and their manifest from0.78.30, is now in the full Pi73
trial. The ordinary app requires the current test nonce, and both report
observations verify it; early restoration cannot start an unrelated workload.
The effect on real provider inserts and app-private storage remains pending.

Image0.78.31 run `94867157896b4c55ad5881cd2392e599` now proves real
ContentResolver-to-path data, canonical/O_PATH metadata and registered queries
under ordinary UID10083. It fails at held O_PATH fstat after a provider rename:
stock GETATTR ignores fh and follows the stale node path. Already granted handles
now read attributes/size from their private, inode/type-verified kernel anchor;
new handles still require live provider visibility approval. Native fault runs
`4136f26eee6d4328830bf5fa1d273ccf` and `568c19e040c04965bba1133c005865b8`
pass with provider GETATTR deliberately returning ENOENT. The latter also proves
unlink(directory) returns EISDIR and rmdir(file) ENOTDIR before a provider that
assumes kernel type checks. This fixes the separate File.delete cleanup failure.

The private-directory candidate rebuilds pinned MediaProvider JNI and uses its
existing Java UID/package policy in the selected rootless PASS_THROUGH view.
Child LOOKUP checks the same policy. Java/dex/resources/manifest, C libfuse and
the original signing certificate are preserved. SDK32 signature, 16KiB ZIP/ELF
alignment and actual DEX JNI-registration/field ABI checks pass; runtime policy
and cross-UID negative canary remain unproved. Complete experimental0.78.32
changes only core, provider APK and manifest from0.78.31. Its image-free Manager
validator passes. The full actual-provider trial is being staged on isolated
Pi73 without resetting its profile or touching preserved CE3 or VM72.

The cross-UID follow-up now compiles two registered, nonprivileged API32 apps.
A separate owner seeds real Android/data/files and Android/obb nonce canaries;
the ordinary storage app must reject40 read, O_PATH, directory-handle and stat
operations through public and backing aliases. The owner independently verifies
its bytes/inode/device before and after, then removes only its current nonce.
The Manager/observer and strict Pi driver pin both actual package identities,
distinct UIDs, current action/nonce and the exact denial matrix. Fixture build
`python3 tools/build_api32_volume_app.py --out out/api32/volume-app-v3b`
and the image-free canary Manager validator pass; runtime proof is pending.
These changes are separate from the immutable first0.78.32 ordinary-storage
trial and cannot add cross-UID evidence to that earlier build.

Complete0.78.32 run `56a80ea2d6e14dee95b8fda1d1c5dbf0` passes the strict
full-image trial on Pi73. Registered ordinary UID10083 passes all8 storage
steps: mounted/query, ContentResolver bytes visible through the pathname,
canonical/O_PATH metadata, held fstat after provider rename, direct FUSE CREATE
and unlink/database coherence, and own external files. Both first/reopen
observations contain the same completed nonce proof with owned cleanup. Runtime
MediaProvider APK hash and selected policy marker match the source-built JNI.
Network-bound HTTPS/DNS, actual Home/reopen, retained image-byte reuse and clean
shutdown pass. All16 service lifetimes and13 adopted descendants stop; no ANR,
Java/native fatal or SystemUI restart is recorded. This does not prove a second
new nonce workload, cross-UID policy, unmount/remount, CE ordering, general path
semantics or full phone compatibility. The separately compiled two-UID canary
trial is next against the exact same immutable0.78.32 image; it makes no new
image-upgrade/reuse claim.

The same-image two-UID trial `11acb1b519464fe6ad62a09aedd551fb` passes
all strict gates. The registered owner UID10110 creates and reads its actual
Android/data/files and Android/obb canaries, while UID10083 is denied all40
read/O_PATH/directory/stat attempts across public and backing aliases. The owner
then rereads unchanged bytes/inode/device/size and removes both nonce files.
Both package identities and current action/nonce are pinned by the observer;
the captured driver gates replay successfully on the unchanged report and all722
source pins still match. Network/Home/reopen and shutdown pass:16 service
lifetimes and14 adopted descendants reaped, no ANR or Java/native fatal,
SELinux Enforcing and host phantom settings unchanged. Reopen retains the same
completed proof; general POSIX/ownership/redaction, remount/CE, hard process
bounds/repeated fresh boot, hardware/regressions, reproducible HTTPS release
and phone validation remain open. These passes do not establish kernel UID or
namespace isolation; Pocket guest identities still share an outer application
UID. Full Android12L support is not yet complete.

The framework lifecycle follow-up now builds a test-only owner helper that
calls the pinned IStorageManager mount/unmount API as guest system UID1000;
it never shortcuts vold. Ordinary owner/app fixtures hold a real data FD,
observe unmounted state and ENOTCONN, retain canary data across a new kernel
AppFuse device, and rerun all9 storage/private-policy operations under a new
nonce-owned phase and incremented iteration. The verifier requires two actual
provider sessions and original network/Home/reap gates. Both fixture rebuild
and image-free Manager validator pass; the old real canary report is correctly
rejected as lifecycle evidence. The same immutable0.78.32 image is staged for
the actual Pi73 lifecycle trial. Runtime lifecycle is pending; CE3/VM72 remain
untouched and reset/outer-restart/CE semantics are not claimed.

The first actual framework lifecycle run `7275f09a12604776a0e5c42b5197e1ee`
fails: framework unmount reaches state0 and the adapter detaches, but a cached
read on an existing ordinary FD still succeeds. Recovery remount and clean
shutdown succeed; the failed report is preserved. Earlier native held-revoke
checks used O_DIRECT and did not establish cached-read revocation. Pinned AOSP
vold `EmulatedVolume::doUnmount` explicitly terminates filesystem users before
unmount, which Pocket had omitted.

The host adapter now identifies exact AppFuse-device data FD/mmap holders in
the current profile, validates identity and kernel start time, signals only
verified pidfds and waits for kernel exit. ASan/UBSan real-process tests retire
FD and mmap-only children while preserving nonholders, system roles, stale
identities and a foreign profile; an isolated non-debuggable Android app also
passes pidfd_open/self signal0 using the exact rebuilt host JNI bytes, with
SELinux Enforcing. The revised full lifecycle proof follows AOSP: old holder
must be in the retired list and dead, a new process must observe unmounted and
failed path open, remount must have a different authenticated epoch, data must
persist, and the ordinary9-step/40-case workload must execute again. Kernel
device numbers may be reused after all old holders die, so epoch and process
identity—not a device-number inequality—prove a new session. Rebuilt fixtures,
JNI and image-free Manager validate; the full Android holder trial is being
staged against unchanged0.78.32. Actual retirement/remount runtime remains
pending; CE3 and VM72 are untouched.

The first holder-retirement Manager retry `ca045f983a994ba7b1c0bdd18099e9fd`
fails the existing outside-profile crash-client rejection preflight before any
volume attaches or app-holder exercise. All14 started service lifetimes and11
adopted descendants stop, without ANR or Java/native fatal. The original report
is retained. The probe now captures exact client exit/timeout/server PID and
bounded stdout before asserting the same rejection gate. Its rebuilt Manager
passes validation, and a fresh diagnostic retry is running under its recorded
exact supervisor/observer handles. This does not establish Android retirement
or full framework lifecycle. The current production API28/29 package also builds
and validates; only the three API32 external-adapter classes differ, with no
test helper in production DEX. These are packaging checks, not legacy Android
runtime regressions. Full12L, encrypted CE/restart, namespace breadth, process
stability, hardware, reproducible HTTPS release and phone gates remain open.

Diagnostic run `f9690c68b126441b9193997d0f687eff` passes both crash rejection
preflights and confirms actual Android holder retirement: UID10110 owner19872
appears in the host retired list and guest Zygote/ActivityManager logs report
its SIGKILL/death. Unmount does not return a successful control report; no
unmounted/end-session record arrives before cleanup, and the owner cleanup
reports storage unavailable. The original FAIL and nonce files are retained;
all16 service lifetimes and14 descendants stop without ANR or fatal exception.
This establishes actual scoped app retirement, not a complete volume lifecycle.

Complete experimental0.78.33 adds phase logs around the native stop sequence.
Byte-for-byte archive comparison against0.78.32 confirms the same2650 payload
entry names/modes, changing only core and version/hash manifest. Host ASan/UBSan
volume rollback/shutdown tests and the image-free Manager validator pass. The
new test captures volume-control exit versus timeout and a read-only framework
state query before any failure recovery mount. Owner diagnostics include the
actual Environment state. The strict .33 full trial is being staged with image
reuse, all original network/app/private-policy/lifecycle gates and preserved
CE3/VM72. No teardown ordering or failure requirement was relaxed.

Complete0.78.33 diagnostic run `e2e85205bf7040ed8563061bb321717b`
fails the full framework lifecycle gate. The unmount client remains alive for
120 seconds, and a separate IStorageManager query observes state5/EJECTING.
Native teardown stops the app broker but never records the adapter RPC
returning. Final host state records owner26329 retired and one adapter
revocation. Retained-image upgrade reuses2598 files and replaces52; all16
service lifetimes and14 descendants stop cleanly, without ANR or Java/native
fatal. This is stronger localization of the existing failure, not a fix.

The isolated real AppFuse fixture now follows holder-retirement semantics
instead of requiring an O_DIRECT client to survive disconnect. Its ordinary
12L client holds a cached FD and a touched mmap. Run
`bbf2d92a6f3048d9936144958068fab4` passes: client29320 exits137 and appears
in the authenticated retirement list; the core exits0, adapter RPC and every
native stop phase return, provider references/handles balance, and the
existing namespace/path/rename/coherence checks pass. SELinux stays Enforcing
and phantom settings stay null. This filesystem model does not prove full
framework or MediaProvider lifecycle. It rules out an unconditional basic
adapter-RPC failure; the same-image full trial now adds host revoke phase
logs to locate the framework-dependent wait. Original failed reports, nonce
files, CE3 and VM72 are preserved.

Same-image host trace `1c9ca05e5baf491ab10be5b5eaa835a8` reproduces
the full failure and shows retirement taking45.592 seconds, exceeding the
native RPC receive timeout20 seconds. On receive failure, the caller still
walked its zeroed ancillary input. Pinned12L Bionic CMSG_NXTHDR advances by
CMSG_ALIGN(cmsg_len); cmsg_len0 therefore repeats the same pointer forever.
The receive path now returns immediately for error/EOF and bounds ancillary
record lengths. A focused regression exercises actual timeout/EOF and FD
cleanup; the actual12L Bionic fixture also confirms its zero-length
CMSG_NXTHDR behavior.

Holder discovery now requests only cached type/inode/device metadata with
statx DONT_SYNC on API30+ hosts, avoiding synchronizing FUSE attributes during
teardown. Older hosts retain their existing syscall path; no old-host latency
improvement is claimed. Both variants pass scoped host retirement tests.
Actual Android model run `b0dc9a803c2845c5a90956c871924ed1` passes the
new receive-error regressions plus real cached FD/mmap holder retirement,
all original path/coherence gates and clean native teardown with the exact
rebuilt JNI. This remains a filesystem model, not the full framework result.

Complete image0.78.34 SHA256
`f0ff99369b21ffd790d5c97d18ef47ac8daa7a08a0ecb7e5cc87d1ac25192fd5`
contains only the changed native core and version/hash manifest relative to
0.78.33. All2650 payload hashes and entry names/modes verify. The matching
image-free Manager builds and validates. The full retained-profile trial is
running under the recorded exact supervisor; unmount/remount, fresh workload,
CE ordering, outer restart, hard process bounds, hardware, legacy runtime and
release gates remain open. An audit also found the same failed-receive
ancillary-iteration pattern in the app FD-attach and fwmark paths; those
sibling paths are not yet fixed by this immutable0.78.34 candidate.

Full0.78.34 run `79bee206d7684fc992773d7758649ea4` fails remount.
The native receive timeout now returns error11 and every stop phase runs;
the infinite ancillary loop is fixed. Holder retirement still takes46.803s.
Framework IStorageManager swallows the vold error and reports state0 after
20.997s, but native adapter ownership is uncleared and remount stays at0.
The late host reply records Broken pipe. Clean shutdown reaps16 service
lifetimes and14 descendants without ANR/fatal. Cached statx has not established
a full-profile latency improvement or successful lifecycle.

A shared bounded SCM_RIGHTS decoder now serves app headers, host attach
replies, data-broker lifetime, volume control and fwmark. Host sanitizer tests
cover timeout, EOF, actual rights, truncation cleanup, malformed lengths and
unaligned control, plus existing open/data RPC gates. Real12L Bionic model run
`fdbfe05de60b4df39332cde300299c5d` passes those checks, cached-holder
retirement and all original path/coherence/teardown gates. Its first observer
queried pidof before asynchronous process startup and saved FAIL. The exact
completed fixture was re-observed, with all unchanged gates, into a separate
PASS report without installation/restart/data reset; the original FAIL remains.
PID diagnostics are now optional and retried after a matching Activity report.

The package audit rejects unused0.78.35: its immutable fixture supplied stale
netd despite current source/client builds. The packager now rebuilds netd and
records daemon/client/source hashes. Corrected complete0.78.36 SHA256
`cafe2ae23376bcfa317a394f525a7b089c371c56fc3d723871ecd7cd5acf2084`
changes only native core, netd and the manifest; all2650 payload hashes and
entry names/modes verify. Manager carries the exact rebuilt fwmark client and
its actual idle-timeout/EOF/subsequent-request check. The matching image-free
Manager also adds host discovery timing/counts to locate the unresolved wait.
The strict volume/persistence verifiers now reject negative native stop/adapter
results even if framework reports unmounted. Full0.78.36 runtime remains
pending; no latency, remount, CE/restart, hard process-bound or release success
is claimed from these narrower checks.


Full0.78.36 run `84578ea827de4c6db9675488b68c16b6` remains FAIL.
Its actual fwmark idle-timeout/EOF/subsequent-request check passes. Holder
retirement takes37.633 seconds while scanning12110 historical identity records;
only8 are live app processes. No individual FD/maps operation exceeds100ms.
The native20-second deadline expires, and closing cleanup callbacks before the
late host completion prevents confirmed detach. The failure is not resolved by
cached statx alone. Original records, data and failure evidence are retained.

Holder discovery now enumerates live `/proc` entries first, filters the real
outer UID, then validates the corresponding owned profile identity/start time
and exact data-FD or mmap device. It preserves pidfd ownership/race checks and
falls back to identity enumeration only when proc enumeration is denied.
Identity records are validated as regular owned24-byte files before reading,
with nonblocking open so a malformed FIFO cannot stall fallback. Host
ASan/UBSan tests pass both discovery modes and cached/legacy stats, retire real
FD/mmap holders, preserve nonholders/system/foreign/stale processes, and use
inotify to prove stale records are unopened in proc mode.

Detach now retains its callback broker and channel/capabilities after an
unconfirmed adapter reply, rejects remount, and permits same-epoch retry.
The storage service preserves announced state on failed detach. Host code
separates reply-write loss from actual revocation errors, so an already
completed revocation can be confirmed idempotently. Real revoke errors remain
failures; automatic framework fault recovery is not established.

Real Android model `97a780286ed54fe1a1dbdf430e8ae003` passes with the exact
rebuilt JNI. Discovery scans63 proc entries/2 profile records and retires the
cached-FD/mmap holder in2ms. A deliberately lost detach reply produces exactly
one reply failure; the retained callback broker lets same-epoch retry confirm
balanced provider handles and detach. Core exits0 and the holder exits137.
All original path/coherence and actual Bionic receive-error/FD checks pass.
This model does not prove the real MediaProvider framework lifecycle.

Complete0.78.37 SHA256
`951197adefd97368213b9e09a17a9b6ae8e7dcb127a13ac2fef13860766dcff9`
changes only the native core and version/hash manifest relative to0.78.36.
All2650 payload hashes and ZIP entry names/modes verify; the matching
image-free app and persistence Managers pass their package validator. The
normal full-profile app trial is running under supervisor1872, token
`bd15f264a77c4a38af22257942ad02c0`, with an independent process observer.
It retains every prior lifecycle/private-policy/network/reuse/cleanup gate and
requires two actual proc-mode holder summaries below the20s RPC deadline,
with one retired holder on unmount and none at final stop. A sampled process
peak alone is not a hard process-bound proof. CE3 and VM72 remain untouched;
persistence, CE ordering, crash/restart, hardware, legacy runtime and production
release checks remain open.


Full0.78.37 run `eedace6d06d34705916fe2c0c47a9dc0` now passes the strict
normal framework lifecycle. Actual IStorageManager unmount takes807ms and
remount1292ms. Proc-mode holder discovery scans90 entries/29 profile records,
retires the cached-FD owner in264ms, and scans88 entries/27 records with no
holders in100ms at final stop. There are no negative native detach/stop results,
no adapter errors or lost replies. A new owner process sees unmounted storage
and cannot open its file; after remount the owner canaries are unchanged, the
authenticated epoch differs, and the ordinary app executes a fresh9-step
MediaStore/path workload with40 cross-app private denials. Source-built
MediaProvider, validated network/public DNS/fwmark error recovery, Home/reopen
and retained-image upgrade/reuse gates all pass.

The exact supervisor exits0. Init reaps15 service lifetimes and14 adopted
descendants with none remaining/failed, no Manager fallback cleanup, and no
ANR or Java/native fatal. The independent observer samples a peak of32 with
no errors; this is not a repeated/fresh-start hard process-bound proof.
The full report and its hash are recorded in
[volume validation](api32-volume-validation.json). Historical failures remain
unchanged. The matching0.78.37 persistence Manager has passed package
validation and its guarded offline-image outer-restart trial is being staged;
that result, CE ordering/physical keyguard, general namespace semantics,
hardware, legacy runtime, production release and phone checks remain open.


Mounted-backend0.78.37 persistence run
`dac399bfa7da40b5bd26319dbff99bad` passes. The first guest stops cleanly before
the outer Manager is restarted, changing its PID22127→25460. With the image
HTTP server stopped, the second boot preserves the ordinary app's UID,
private token/text/tap state and nonexportable software Keystore decryption;
three physical input actions and Home/reopen also pass. Both stopped cycles
have one SystemUI, no ANR or Java/native fatal, all16 service lifetimes reaped,
and14/13 adopted descendants reaped with none remaining. The supervisor
exits0, observer has170 samples/no errors, and SELinux/phantom settings stay
Enforcing/null/null. Its sampled peak34 means process-budget reliability
remains open despite these functional passes. This is normal clean restart,
not encrypted CE ordering or abrupt outer-process death/stale-endpoint recovery.

The test builder now supports finite additional non-CE Manager packages2/3,
and the ordinary-app runner can require that Android's package inventory,
including uninstalled packages with retained data, contains no chosen package
before installation. It rejects combining that fresh-sandbox requirement with
retained-image reuse and never clears an existing profile. Fresh package2
build and image-free package validation pass; comparison against the full
retained-profile Manager changes only the manifest and expected signature
entries, with identical DEX/native code/image/app/network inputs. Runtime is
not started: VM73 has1,332,322,304 bytes free while the compressed plus unpacked
image alone needs1,895,325,245 bytes, before staging/headroom. Existing profiles
and failure artifacts remain intact. The current-image Signal regression can
continue using the existing ordinary Manager profile.


Current-image Signal regression
`77aab9f08629488d98d5a4fb0d2bdb12` passes on complete0.78.37. The unchanged
Aurora Google Play8.33.2 three-APK set requires API32; version177201, exact
certificate/splits and logical UID10107 verify. Actual ActivityTaskManager
dumps show `org.signal.registration.RegistrationActivity` resumed and focused
at first launch and after genuine Home/reopen. The selected source-built
MediaProvider is mounted/queryable, validated network/public asynchronous DNS
and fwmark timeout/EOF/subsequent-request checks pass, and349 frames present.
There are no ANRs or Java/native fatals; init reaps16 service lifetimes and13
adopted descendants with none remaining. The supervisor exits0;128 observer
samples peak32 without errors, and SELinux/phantom settings remain
Enforcing/null/null. The latest report and immutable receipt are in
[Signal validation](api32-manager-signal-validation.json) and
[volume validation](api32-volume-validation.json).

The current default runner, including its new explicit sandbox receipt/manifest
checks, completes this runtime test. Fresh-package selection remains build/
package/preflight-guard validation only because of disk headroom; it has not
been run as a new Android sandbox. All runtime Java/native code in the Signal
Manager is byte-identical to the earlier full0.78.37 storage Manager; only its
app specification and expected signature entries differ. Account linking,
messaging/calls, encrypted CE/keyguard ordering, abrupt crash recovery,
broader namespace semantics, fresh-start/process-budget reliability, hardware,
legacy runtime, production HTTPS release and phone checks remain open. The
Android12L goal remains active.


The next37 encryption trial deliberately resumes the existing locked CE3
profile from terminal failure `ed1b907536e84efdacfc5465eaadab78`; its old
four strict stopped cycles, failed clock-screen image and owner-only Pi
recovery credentials remain intact. No credential change, guest APK reinstall
or data reset is requested. A read-only UiAutomation observer now returns only
SystemUI password-field identity/visibility/focus/editability/bounds, with no
password text. The fixture waits for interactive state, swipes through host
input, taps the observed field if necessary, and sends the existing password
only after visible secure entry is focused. Accessibility input/actions and
correct-password CLI verification are not used. Per-run frames/command logs
preserve the earlier failed diagnostics.

The new recovery mode requires both locked five-root and public emulated-media
canary denial, actual physical unlock, preserved ordinary app/software
Keystore, successful public-media canary read and mounted/queryable MediaProvider.
It then repeats after actual outer Manager restart with the image server offline.
Every stopped cycle retains strict native-volume/error/reply-loss, exact
service/descendant reaping, no ANR/fatal and physical input gates. The existing
Pi0600 credential file is read only and retained; no password values are
exported. The matching image-free CE3 Manager rebuild/package validator and
Python syntax checks pass. The exact supervisor17840/token
`05efd7cb35d347f999215d7d38b94a83` is running on VM73; runtime is not yet
proven. VM72 is untouched and earlier failures are not promoted by these
preparations. See the current recovery entry in
[CE validation](api32-ce-storage-validation.json).


First37 locked-profile recovery `22dcd4d87a3d4218b8e493c13b2c0f19`
fails before any password or accessibility input/query. The immutable image
upgrade completes and both five-root/public-media locked canaries are denied.
SystemUI starts; SurfaceFlinger finishes boot, then records power mode0.
The fixture requested its first nontrivial frame before sending WAKEUP, so
that frame waits60s on a sleeping screen. Accepted/rejected input counts are
both0. All15 service lifetimes and11 descendants stop cleanly; the report,
prior encrypted profile and recovery file remain intact. This does not prove
mounted-volume unlock or the new UI observer. Offline inspection of the actual
SystemUI APK verifies passwordEntry0x7f0b043a is EditText/inputType0x81 in
keyguard_password_view.xml; visibility/focus still require runtime observation.

The corrected fixture sends host WAKEUP and observes interactive state before
its initial frame. Same-image retry is running under exact supervisor22172,
token `51628f5a06684cf5a3c03da6d5e88683`, with a separate observer. It first
audits the exact terminal pre-input failure/current CE3 APK; all original
password-field, locked/unlocked media, ordinary-app/Keystore, offline outer
restart and strict cleanup requirements remain. No data reset, credential
change, existing guest APK replacement or host/VM restart is requested.


Same-image wake-first trial `02b729cff9f04ff58380a75f29f9571e` also fails
before any password input. The real InputManager accepts both WAKEUP events
with0 rejections, but PowerManager remains noninteractive for15s; locked
five-root/public-media canaries remain denied. The guest stops cleanly and
all old failures/data/credentials are retained. This identifies a real
input/power handoff issue beyond screenshot ordering. No accessibility
observer/password attempt or mounted CE restart pass is claimed.

A same-image pre-password diagnostic trial now captures host wake uptime and
actual power/input/window-policy state. It is running under supervisor27016,
token `6ff41a8c293d45439a99e72850aac388`, with an independent observer.
The added diagnostic path only reads state and runs before password entry;
normal wake/keyguard/mounted-storage/crypto/restart assertions are unchanged.
Source-built image-free CE Manager compilation/package validation and driver
syntax pass. No production wake policy has been overridden. This diagnostic
must not be counted as a successful encrypted unlock if only queueing succeeds.


The read-only diagnostic retry `3b906fb0cb764ecf9d687388f34a8399`
is terminal FAIL with clean owner exit, but captures native input, power and
window policy successfully. It supersedes the earlier input/power-handoff
hypothesis: trusted WAKEUP events arrived at563876981/563877141ms while
interactive, before lastSleep563877637ms. Observer startup crossed the
lock-screen inactivity timeout. Clock bases align and theater mode is0.
Pinned AOSP PowerManagerService returns without refreshing inactivity when
wakeUp targets an already-awake display; no production wake override is
justified by this evidence. No password was entered.

The state-based retry sends another actual host wake after observing asleep,
before the initial frame and each keyguard gesture. It retains the visible,
focused secure password-field gate, read-only accessibility attributes and
physical key input. User-unlock and mounted/queryable MediaProvider are
observed before the positive public-media canary read, using the existing
shared60s deadline. Manager build, isolated package validator and Python
syntax checks pass. Supervisor4297/token6be4a749bc7a4a9faca8947e586dc501
is running on VM73 with the same complete37 image and preserved CE3 profile.
No credential change, guest APK reinstall, data reset or VM72 access occurs.
Runtime unlock and offline outer restart remain unproven until this trial
completes; all earlier failures and original recovery credentials are retained.


State-based wake trial `b27b6bbde8334be0976bdbb5d45fa0cb` is terminal
FAIL, with owner0 and zero input rejections. It proves wake recovery and a
real nontrivial keyguard frame: the first observer saw asleep, the next wake
produced interactive state and the captured clock screen. Five-root and
public-media canaries remain denied. Three swipes then yield empty read-only
window lists; the observer omitted FLAG_RETRIEVE_INTERACTIVE_WINDOWS, required
explicitly by matching UiAutomation.getWindows. No password field was
accepted and no password keys were sent. A subsequent diagnostic frame
waited on the sleeping display and failed; this does not negate the earlier
frame/wake evidence. The next fixture sets the window opt-in and reads
interactive state again after accessibility retrieval, avoiding stale
pre-query state at the input gate. Original failed reports/data/credentials
are retained; no production power change or encrypted unlock pass is claimed.


The interactive-window opt-in rebuild/package validator and Python syntax
checks pass. Same-profile retry is now running under exact supervisor7303,
token `c6952ba2082649448e65c45f21eb1ef9`, with separate output paths and
process observer. Its launch audited the exact terminal prior attempt and
installed CE3 APK. Runtime password-field observation, encrypted media unlock
and offline outer restart remain pending; this is not a production release.


Interactive-window trial `968ed68158354944b3c77acdb609e735` proves the
first strict physical encrypted unlock on complete37: visible focused secure
password field, actual host keys, Android user unlock (5844ms observed wait),
mounted/queryable MediaProvider, unchanged ordinary app/software key and
public-media canary, with15 service lifetimes cleanly reaped. An independent
check of the saved cycle/report agrees. The full trial remains FAIL: after an
actual outer Manager PID change with image source offline, the same visible
focused field is observed and all13touch/46key events are accepted, but CE
read times out. The log shows another sleep transition and no credential-backed
unlock request. Peak33 also leaves default-process reliability unresolved.
Original data/password and both partial/failure reports remain intact.

The production API32 host input queue already wakes before touch DOWN, but
keyboard DOWN lacked that behavior. Ordinary keyboard DOWN now prepends wake;
explicit WAKEUP/POWER/SLEEP/SOFT_SLEEP retain their meaning. The test also
records typing uptime and read-only lock/power state after input, without
reading input history or password text. The image-free Manager rebuild,
package validator and driver syntax pass. Follow-up staging hit a transient
SSH/rsync reset before installation; the exact partial stage and source pin
are being rechecked before the atomic copy is resumed. No guest restart was
triggered by that transport failure. Runtime keyboard/outer-restart recovery
remains unproven.


Capped atomic transfer succeeds and remote APK/source/image hashes pass.
Keyboard wake trial runs under exact supervisor12989/token
`bc08fe58899c4e1e82cf6ad53db281a0` with independent process observer.
APK comparison confirms only outer Java DEX/signatures changed: all five
native libraries, catalog/image and ordinary CE guest app are unchanged.
Encrypted app/key and source image are preserved. Two complete strict physical
unlock cycles and offline outer restart remain runtime gates.


Keyboard-wake trial `b0bb2e37101e4fdaa58848c503190753` is terminal
FAIL before password entry: opt-in UI observations returned empty, IME-only,
then empty roots, and the final frame waited on sleep. It does not validate
ordinary keyboard wake or encrypted restart. All57 wake/gesture packets were
accepted with0 rejections; clean owner exit0, peak27. Data/password and
prior partial/full reports remain retained. The observer now retries actual
interactive-window/root metadata for a bounded3s, recycling each snapshot.
It still requires the visible focused secure SystemUI field and fresh power
state; no password text or accessibility actions are read/performed. Missing
field now fails before the diagnostic frame wait. Rebuild, package validation,
Python syntax and diff checks pass; next source-pinned same-profile trial
is staging with the original full runtime gates intact.


The bounded-window trial is running under exact supervisor15644/token
`19c3940d308a45729a2ec23eb6d85462`, run
`e3d9857762c84804b610d36e6b2cda2d`, with its own process observer. The
[compact keyguard validation index](api32-keyguard-validation.json) records
every terminal attempt, the independent first strict cycle, exact current
build/validation commands and remaining runtime/release gates. No previous
failed attempt is promoted by a later build or partial result.


Bounded-window run `e3d9857762c84804b610d36e6b2cda2d` is terminal
FAIL with clean owner exit0, zero rejected packets and peak27. It finds the
actual focused secure field after571ms;84key/12touch packets are accepted.
The native log records a credential-backed CE unlock, and canary observation
returns, but the next keyguard check sees asleep/locked. This does not pass
full physical unlock or restart. Stock source and prior native window dump
confirm keyguard DISABLE_USER_ACTIVITY; its TextWatcher normally calls the
PowerManager activity callback, with a10s bouncer timeout. The failure does
not yet prove why that callback/timing failed. Before overriding any window,
timeout or bridge power policy, the next diagnostic captures only power
state after input and aggregate encoding/submission/event uptimes. It never
reads input history/password text. That Manager rebuild, package validator
and driver syntax pass; its separate same-profile trial is staging.


The power-only post-input diagnostic is running under exact supervisor19209,
token `4ff35ac62be7473cad68da94d466f995`, on VM73 with a separate
process observer. It retains the same native/image and original encrypted
profile, full physical UI gate, mounted data/app/key checks and offline
outer restart. All prior attempts remain terminal failures where recorded;
no current production-support flag or release has been enabled.


Power trial `5af4c207e0804b59b2e24c23499af66a` is terminal FAIL, but
actual physical keyguard unlock passes and user-activity refresh is verified:
encoding takes33ms, host submission finishes567732818ms, Android records
activity567735349ms, and awake/unlocked keyguard is observed567759907ms.
No power/window override is justified by this evidence. The failure is a
real asynchronous readiness boundary: StorageManager reports mounted; the
external_primary query throws before provider SCAN_VOLUME work begins.
Android user unlock was observed after3934ms; owner stops0, peak31.

The observer now retries only the exact external_primary-not-found error
within the shared60s user/volume deadline. Security/unrelated errors still
fail immediately; no manual provider attachment or reset occurs. Actual
awake+secure+unlocked keyguard state is observed in one50ms polling process
under30s before slower CE reads, so repeated ART startup cannot consume the
normal10s bouncer timeout between checks. All physical field/input, data/key,
mounted query, offline outer restart and strict cleanup gates remain. The
Manager rebuild, package validator, Python syntax and diff checks pass;
same-image/profile readiness trial is staging. See the compact keyguard index.


Readiness trial is running under exact supervisor22262/token
`bf53df3b503f404f95857f39c82d70b8`, with an independent process observer.
It retains the complete37 image and existing encrypted profile/password;
all prior failed/partial reports remain unchanged. No production power
policy, host setting, ordinary guest package or data reset is involved.


Both readiness cycles pass in run `7e7a5d35b53d40178669e05a80425a91`.
The independent check verifies731 source pins, exact APK/image/five native
libraries, physical focused secure password input, real awake/unlocked state,
locked5-root/public-media denial and positive media read, unchanged ordinary
app/key/UID, two strict15-service shutdowns, and actual Manager PID change
with source HTTP offline. Sampled peak31 across139 observations, errors0;
Enforcing/null/null host settings and original credential file are unchanged.
Both provider queries were already ready, so the specific missing-volume
retry branch was not exercised in these passing boots. Prior failures remain
unchanged, and a sampled peak does not establish a hard process bound.

The dedicated test profile still has the synthetic test credential. A new
explicit cleanup mode physically unlocks it, authenticates its removal through
stock LockSettings, and restores the original password-free encrypted state.
A separate driver then changes the outer Manager PID and verifies automatic
CE unlock, all five readable root canaries, public media/provider and the same
app/software key without reset or reinstall. Both boots keep the image source
offline. This cleanup does not claim the full repeated credential lifecycle.
The rebuild, package validation, Python syntax and diff checks pass; restoration
is staging. The original Pi-only recovery file remains retained for history.


The explicit restoration run starts under supervisor27454/token
`8eae65edc9004bfb9d44142d6638611b`. Its first phase
`04fe2097ab9747deb4a61426b34a7446` passes physical unlock, authenticated
credential removal, five readable root canaries and strict shutdown. The
actual volume query encounters and recovers from one missing-volume response,
exercising the bounded retry path; continuous keyguard observation uses13
polls over952ms. Password-free outer restart remains in progress. The
repeatable independent command for the earlier two-cycle proof is
`python3 out/api32/verify_ce_readiness_result.py` (PASS).


Restoration `04fe2097ab9747deb4a61426b34a7446` and password-free outer
restart `8ded86137a9d4284af4693c12a66ac2b` both pass strict15-service
shutdowns. The independent check verifies732 source pins, original app/key/UID,
all five readable encrypted-root canaries and public media, mounted queries,
actual Manager/app PID changes, image source offline for both boots, one secret
request before credential removal, and unchanged historical Pi credential file.
Both boots recover one missing-volume query; waits10759/785ms. Automatic
password-free restart reports secure=false and unchanged crypto. Sampled
peak32 across128 observations, errors0. Full repeated password set/change/
remove/restore is still a separate pending gate; no production release claim.


A full repeated credential lifecycle is now prepared against the restored
current37 profile. The driver accepts only the exact strict restoration report
and installed APK, checks source/native/image pins, and requires mounted
volume, actual provider queries, all five positive canaries, public-media read
and unchanged original app/key/UID/path across password changes. All six
stopped cycles require kernel ownership/native volume/reap/no ANR/fatal gates.
Locked and initial frames now explicitly wake first; per-run frame names
preserve older artifacts. The original password-free state must return after
the final offline outer restart. New credentials are generated only on the Pi,
kept0600 during the trial, and removed only after complete success. Rebuild,
package validation, Python syntax and diff checks pass; the trial is staging.
This repeats the credential lifecycle, not plaintext migration.


The full preserved-profile credential trial is running under exact
supervisor1917/token `9fe944bd8bf2454ba0870db36233e3bf`, with its own
process observer and output directory. All six cycles, two physical keyguard
unlocks, unchanged original data/key/UID/path, mounted/public media across
each credential transition and final original password-free restoration are
required. Full-lifecycle results remain pending; earlier independent restart
and cleanup passes do not substitute for this run.


The full run is confirmed live as `0fe81ba592384d8e88333c9dd0ba1b6a`,
starting the preserved encrypted profile in the intended lifecycle mode. If
it fails, its new Pi-only0600 recovery file and exact terminal credential
phase must be inspected; the older historical credential is no longer the
active recovery source for this trial. Observation expiry does not authorize
restarting its guest.


The full preserved-profile lifecycle passes in
`0fe81ba592384d8e88333c9dd0ba1b6a`: all six strict stopped cycles, wrong
and old password rejection, password set/change/removal/restoration, automatic
unlock without a password, two actual focused physical keyguard unlocks,
unchanged original app/key/UID/path, all five root canaries/public media and
mounted provider queries across every transition. The final outer Manager
restart is offline and returns the original password-free state; the new
Pi-only test credential file is removed after success. Independent command
`python3 out/api32/verify_ce_full_lifecycle_result.py` passes against732 source
pins and exact APK/native/image/cycle hashes. Sampled peak33 over389 samples
leaves default-process reliability open; no plaintext migration was repeated.
All prior failure reports and historical recovery artifacts remain unchanged.

The next fixture is an ordinary debug-signed API32 app requesting no
permissions, with real public MediaCodec AAC/PCM and AVC/YUV round trips. It
checks EOS, compression, decoded audio frequency/amplitude, all12 video frames
and YUV pixel values, then repeats fresh work after Home/reopen. Codec work
runs in its own app UID; the system observer only reads its registered Activity
dump. App build/signature/manifest/archive and Manager build/package checks
pass; codec runtime and camera capture are not yet established.


Ordinary codec run `ee6cb7d272d8461497750c4171f33864` passes real AAC/AVC
compression and decoded PCM/YUV checks twice across Home/reopen. Independent
verification covers736 source pins, all16 native service lifetimes and13 adopted
descendants reaped, default host settings and cached complete37 image. Sampled
peak34 over94 samples is not a hard process bound. See the codec evidence.

The new ordinary audio/camera fixture finds a real gap in37, run
`526b7b5d75264dedb3a17d4816ba8d59`. Its AudioTrack counter advances96000 frames
in720ms but the host receives zero PCM; AudioRecord reads time out. The first
HAL preparation fails with-2, followed by repeated preparation/invalid-state
errors. The generated local HIDL7 stream defaults return NO_PID; stock12L
StreamHalHidl asks for the HAL PID before adopting its already-created FMQs.
The source adapter now reports the real local process PID for input/output
stream metadata; remote proxies and privileges are unchanged. Its native build
and dynamic-link checks pass, with no added preload dependencies. Complete
image0.78.38 validates all2650 entries and changes only that HIDL library.
Runtime confirmation is pending. The next fixture collects both attempts even
if a device fails, while requiring two complete passes for the combined AV gate.

Camera testing is separately limited by the outer VM73 configuration: it sets
ro.boot.qemu=1 for an older ADB-dialog workaround, but never sets the emulator
camera-selection property. The host and guest therefore advertise zero cameras.
No host boot flag, property, image, SELinux or phantom setting was changed.
The failed trial reaped all16 native service lifetimes and14 adopted descendants,
with no ANR/fatal, and restored the exact prior host camera/microphone permissions.
Its sampled peak was33 over101 observations. See [AV evidence](api32-av-validation.json).


Complete38 trial `30f98201e2284210ad09a1c7a7fb517d` confirms the local
HIDL identity fix: two fresh ordinary-app AudioTrack/AudioRecord workloads
pass across Home/reopen, with96,000 played frames and96,000 microphone
samples per attempt. The host broker writes1,935,360 PCM bytes and opens
two microphone streams; observed local stream PIDs match the real audioserver.
Independent command `python3 out/api32/verify_audio_local_identity_result.py`
passes742 source pins, unchanged fixture/five host JNI libraries, mounted
provider, all16 native lifetimes and13 descendants reaped, default settings
and restored temporary host permissions. Sampled peak34 across188 observations
is still not a hard bound. Both camera attempts report no guest cameras, so
the full AV run remains FAIL; no acoustic/physical microphone claim is made.

VM73's normal stop command could not reach its expired launcher-monitor
socket and left QEMU running; the guard stopped without copying disks or
changing configuration. The alternative Android shutdown records actual
kernel `reboot: Power down` before terminating the exact stopped VM's launcher
and remaining QEMU process. A separate stopped-disk backup is now running,
with explicit VM73/base/instance guards and per-file hashes. Only after it
completes will the old forced emulator flag be removed and the same userdata
resumed; original package UIDs, unchanged OS backing and host defaults must
verify, followed by actual camera work. Earlier failure/terminal records remain.

The successful audio data run still reports two host stream errors when
capture ends. The follow-up scopes cancellation handling to a failed socket
PCM write with a kernel-confirmed POLLHUP; real microphone initialization/read
failures remain errors. It records peer closures separately and the next test
requires zero stream errors and two confirmed closures. This host Java change
is building; no runtime cleanup pass is claimed yet.


The VM73 camera-profile restart now passes: the host exposes three cameras,
all196 original package UIDs are preserved, the OS backing disk retains its
inode/size/mtime, and SELinux/phantom defaults are unchanged. The stopped
backup contains30 files totaling28,225,320,224 bytes with per-file SHA256
receipts, including the original configuration and data overlay. Kernel
power-down was observed before terminating only the old instance73 helpers.
The first restart failed on a tombstone port held by the stale old launcher;
its failure report/log remain separate. After exact helper identity checks
and SIGTERM cleanup, the preserved boot succeeds with a new kernel boot ID
and QEMU PID. See `out/api32/vm73-camera-profile-result/independent-check.json`.
The next full AV trial uses the same38 image, ordinary guest APK and five
native host libraries, with only the scoped host Java capture-close handling
and stronger test observations changed. Actual guest camera capture is pending.

### Camera cold-start contract and confirmed clean audio close

The complete0.78.38 run `5454b7ad07c2408995e84503bcf553d4` remains a combined AV failure. Both ordinary audio attempts pass real PCM/frame/timestamp checks, with1,920,000 host playback bytes, four completed host streams, two confirmed microphone socket peer closures and zero stream errors. `python3 out/api32/verify_av_camera_clean_result.py` passes this narrower evidence check; it explicitly requires the combined failure and no guest cameras. Host camera enumeration now exposes3 cameras after the preserved VM73 profile repair.

The guest never started `pocket-cameraserver`. Exact locked Android12L framework source (`159ab48288c45267fe5e7898c6b5dd7d66c3306a`) confirms CameraManager uses Java ServiceManager.getService, which delegates to checkService and cannot invoke Pocket's native getService lazy activation. Normal fixture staging and complete image packaging now explicitly start cameraserver and await media.camera before system_server, matching the stock eager cameraserver lifetime. Focused validation confirms ordered startup/readiness, idempotent processing of current fixtures and malformed-input rejection. Full image0.78.39 build and actual camera capture validation follow; camera support is not yet verified. Original profiles, failed reports and default host policy remain preserved.

The initial0.78.39 trial `39b7ab27ce3e49638d87febbf03919fd` is a preserved failure: cameraserver's real provider enumerates two usable host cameras, but waiting for media.camera before starting framework causes a circular dependency on ActivityManager. CameraService registers its UID/privacy observers before publishing, and stock native ActivityManager::getService polls until activity exists. Init detects bootstrap readiness failure (exit67), reaps all14 started services with zero remaining children; original host permissions/default policy are restored. No Camera2 capture or guest boot pass is claimed.

The corrected generator starts cameraserver alongside framework and waits for camera registration only after framework readiness. Both source-template and frozen-fixture checks enforce this order and reject the earlier circular sequence. Complete0.78.40 image build passes, SHA256 `660b7ee99c2c131cef01c34bf428a88c4c80f21bf05bf33da081bc01b12dd286`,590,923,155 bytes,2650 validated files; only normal boot script differs from38/39. Manager build and `python3 tools/validate_download_manager.py out/api32/camera-parallel-manager-probe/api32-app-manager-probe.apk --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c --allow-image-probe32` pass. Actual40 runtime validation remains pending.

### YUV ImageReader allocation geometry

Run `bf07d9797d494f2b9a6c7aef0707ac8a` boots0.78.40, initializes two real cameras and opens host camera0 at160x120/30fps. ImageReader then hits an ART JNI abort because lockAsyncYCbCr returns BAD_VALUE and its fallback has null chroma-plane pointers. App foreground observation correctly fails; no Home/reopen or complete AV pass is claimed. All17 services and13 adopted descendants are reaped, owner exits0, original host permissions/default policy are restored. The raw report's old nativeFatal regex missed this JNI abort; the preserved report is superseded by `out/api32/camera-parallel-result/independent-check.json`, which records the actual fatal. Python and Manager test cleanup checks now reject JNI aborts/tombstone messages and named fatal signals even without libc's normal header. Actual failed40 and clean38 audio/37 Signal logs verify the tightened classifier.

The software gralloc previously computed YUV offsets from lock/crop dimensions. Opaque handles now retain the allocation width/height/format; plane offsets always use the allocation and access bounds are checked separately. The composer compiles against the same patched handle layout. The isolated unprivileged Pi/Bionic test `python3 out/api32/run_gralloc_yuv_native_linux.py` reproduces failure with the original module (exit10, child20) and passes with the corrected module: two formats, handle import in a child, verified pixels with empty/odd/full crop regions and out-of-bounds rejection. This Linux-only test supplies kernel memfd allocation because it has no Android property area; it is not an Android-sandbox or Camera2 pass. Earlier standalone attempts without an Android allocator/property area are retained and do not count as geometry validation.

The complete paired0.78.42 build is590,923,244 bytes with2650 checked entries, SHA256 `7e6a9b9886f8ac2d88a9969c374dd99307c589d9fe210daa396c944ce297ff22`. Only gralloc and its matching composer differ from40. The initial41 build is explicitly not a runtime candidate because its composer still used the earlier handle definition.42 Manager build/package validation pass; actual Camera2/AV testing follows. Seven read-only host AMS snapshots during40 each tracked one child while ps saw18..32; the separate sampled peak was34. This discrepancy/timing does not establish a hard process bound or waive the default-policy budget gate.

### Complete42 ordinary AV pass and late host-permission follow-up

Run `86840756db6441cb9a9dd39084b3ab2e` is a strict ordinary AV pass. `python3 out/api32/verify_gralloc_paired_result.py` verifies743 frozen source pins, exact42 image and Manager hashes, unchanged ordinary SDK32 APK/certificate/UID10112, actual foreground/Home/reopen, six increasing-timestamp YUV frames with nonuniform pixel values, and two decoded160x120 JPEGs (7596/8071 bytes,94/103 sampled colors). Both audio attempts play96000 frames and record96000 samples with timestamps. Host playback carries1,931,520 PCM bytes; four streams complete, two microphone peer closures, zero errors. Actual camera0 opens twice; camera1 is advertised but not captured in this test. No physical phone camera/acoustic claim is made.

All17 native service lifetimes and13 adopted descendants are reaped with zero remaining, owner exits0, no Java/native fatal or ANR, exact prior host camera/mic grants restored, SELinux Enforcing and actual phantom policy32 unchanged. The ps observer peaks37 over177 samples; eight real AMS snapshots peak27 tracked records. A hard process bound/repeated fresh-start gate remains open.

One integration gap remains when the guest boots before the outer app receives CAMERA permission: its provider freezes an empty list. Public Camera2 inventory/capabilities require no CAMERA permission, so HostBroker now exposes public static inventory before the grant while preserving explicit permission checks on stream/photo capture. A new test-only checkpoint requires real broker metadata success and actual stream403 while denied, grants host camera only after the guest is booted, then requires the same guest runId and full repeated AV success. The42 image, five native host libraries and ordinary AV APK remain unchanged; rebuilt Manager/package checks pass and actual late-grant validation follows.

The late-grant run `312b6446bae949e1907b7baad98e5d7e` passes independently verified inventory while host CAMERA is denied, actual stream403, host grant only after a booted-guest checkpoint, identical guest runId across the grant, and full ordinary AV/Home/reopen. It reuses the unchanged42 image without an image download, restores original host permissions, reaps17 services/13 descendants with zero remaining and ownerexit0, and has no ANR/native/Java fatal under the tightened classifier. `python3 out/api32/verify_camera_late_grant_result.py` passes. The sampled process peak is33; no hard budget or physical phone claim follows. The ordinary fixture is now extended to capture every advertised camera, with an explicit requireAllCameras evidence gate; prior first-camera reports are correctly insufficient for that new gate.

### All advertised cameras and fresh host-native build

Run `4598f298c4524a02ad89af87016522b7` is a strict all-camera AV pass. `python3 out/api32/verify_av_all_cameras_result.py` verifies743 source pins, all12 nonuniform increasing-timestamp YUV frames and four decoded JPEGs: camera0 at160x120, camera1 at320x240, both before and after Home/reopen. The exact42 image, production/test host Java and five native host libraries are unchanged from the late-grant pass; the ordinary SDK32 test APK expands coverage with an explicit requireAllCameras gate. Playback/recording and clean audio closure pass, all17 services/13 descendants are reaped, host grants/default policy restored, no ANR or native/Java fatal. Sampled ps peak34; no hard process-bound or physical phone claim.

`python3 out/api32/build_clean_host42.py` also completes a fresh isolated host-native build, reusing only checksum-verified dependency source archives and pinned vendor Git object stores. Boost/talloc prefixes, extracted/patched source and all native object/compiler output start fresh; compiler caches are disabled. The saved receipt is `out/api32/native-clean-42/receipt.json`; all current native source pins match. Runtime/PRoot binaries differ from the older cached build, while frame/CE/AppFuse libraries are byte-identical. These new artifacts need actual Manager runtime and legacy-backend checks before release use. This is not a clean-room rebuild of the whole Android image or a byte-reproducibility claim.

The fresh-host-native run `7c7aecbc85ab4a379ef30fd0e32fdcff` is a strict API32 all-camera AV pass. `python3 out/api32/verify_native_clean_av_result.py` verifies the fresh build receipts, exact packaged library hashes and two-library delta from the cached-host APK,743 frozen source pins, the unchanged42 image/Java/ordinary APK, all12 preview frames/four decoded JPEGs, real repeated PCM/capture/timestamps, Home/reopen and clean ownership/native shutdown under unchanged default host policy. This establishes the new runtime's API32 path; it does not exercise legacy PRoot or repeat encrypted-profile lifecycle. The same fresh-native/image/Java combination is now staged for unchanged Aurora Play Signal8.33.2 minSDK32 with real network/DNS/HTTPS/fwmark recovery checks; no account operations are authorized or claimed.

### Fresh-host-native42 Signal repeat

Run `35126d24db604567be7c59a5bae9b6bb` is a strict Signal pass on full0.78.42 with the freshly source-built host native set. `python3 out/api32/verify_native_clean_signal_result.py` passes:743 frozen source pins, exact image/Manager/library/bundle hashes, unmodified Aurora Play8.33.2 ARM64 splits with minSDK32, unchanged installed certificate and ordinary UID10107, registration screen resumed/focused before and after independently verified Home, mounted source-built MediaProvider, validated network/public async DNS/NXDOMAIN, network-bound HTTPS200 and fwmark timeout/EOF recovery. Only the app download specification and expected APK signatures differ from the fresh-native all-camera AV Manager. No phone access was needed to obtain the bundle.

All17 services and13 adopted descendants are reaped with zero remaining; the owner exits0 and no Java/native fatal or ANR is present. SELinux remains Enforcing, host sensitive permissions are all denied, phantom global overrides remain unset, and separate actual AMS settings dumps before/after both show max_phantom_processes32. The133 ps samples peak32; ten AMS snapshots peak28 tracked records. These observations do not prove a hard process bound. Account registration, messaging/calling, physical phone validation and legacy PRoot execution remain unverified. A launcher NameError occurred before process launch or APK replacement; its separate error receipt is retained. Full evidence: `out/api32/native-clean-signal-result/independent-check.json` and `docs/api32-manager-signal-validation.json#currentFreshNative07842Trial`.

### Fresh PRoot host regression

`python3 tools/build_host_proot_probe.py --host-native-dir out/api32/native-clean-42/host --out out/api32/fresh-host-proot-probe` passes and packages the exact freshly source-built tracer in a separate image-free, non-debuggable API28-target host app. The actual VM73 run passes real shell/id/cat execution, fake UID0, own-file binding, a nested binding overridden by another file and fork/exec into a second shell. Raw tracee procfs still reports the real unprivileged host UID10159, zero capabilities, active seccomp and a nonzero tracer PID; the outer app remains untraced. A deliberately incorrect parent is rejected before executing the shell. Afterward only the outer app remains under that UID, with no child processes. SELinux stays Enforcing and actual phantom max32 is unchanged. `python3 out/api32/verify_fresh_host_proot_result.py` passes.

The host denies reading its generated linker configuration; the captured warning is retained, and the host shell uses its normal fallback. This smoke is not an Android7 image boot, an Android9/10 native-profile regression or an abrupt parent/tracer-death test. Those runtime claims remain open. The original Signal/AV/CE profiles and all their reports are preserved. Evidence: `out/api32/fresh-host-proot-result/independent-check.json`.

### EGL Surface-input video round trip

Run `8afc52b35e8a4b6597568d75f2cf400c` is a strict pass on the same complete42 image, production/test host Java and five fresh host-native libraries as the Signal/all-camera AV passes. Only the downloaded ordinary codec test APK/spec changes. The no-permission SDK32 app retains UID10111 and its debug certificate; each phase repeats AAC signal and AVC byte-buffer pixel checks, then uses public EGL to draw twelve gray64x64 frames into MediaCodec.createInputSurface. SwiftShader produces buffers for c2.android.avc.encoder; the c2 decoder returns all twelve expected YUV pixels with exact100000us timestamps and EOS. Each Surface bitstream is780 bytes; first/reopened encoding takes539/404ms. These are small synthetic workload timings, not video-call or physical-phone performance claims.

`python3 tools/build_api32_codec_app.py --out out/api32/codec-surface-app-probe`, the Manager build/package validator and `python3 out/api32/verify_codec_surface_result.py` pass. The independent verifier checks747 frozen source pins, exact image/Manager/JNI/test archive hashes,48 decoded video frames total (24 Surface plus24 byte-buffer), actual app focus, intermediate Home, fresh nonce/generation after reopen, unchanged host policy and strict shutdown. All17 service lifetimes/13 adopted descendants are reaped, zero remain, ownerexit0, no ANR/native/Java fatal. The105 ps samples peak33; seven actual AMS dumps track zero phantom records. A hard process bound is not established. Evidence: `out/api32/codec-surface-result/independent-check.json`.

Camera-to-encoder/MediaRecorder integration, decoder output Surface and physical-phone media remain separate open checks. The source review for the process-budget follow-up is saved under `out/api32/budget-rootless-profile-reference`: matching12L DynamicSystem's boot receiver starts its separate service and relies on kernel guest installation/reboot; Goldfish MultiDisplay is persistent and its native adapter opens QEMU's multidisplay pipe. No image policy or host limits were changed during this codec run.

### Declarative rootless device profile in43

Complete0.78.43 keeps all existing runtime/ART/Bionic/media/camera/microG payload bytes identical to42. It adds the public device-profile JSON and a stock SystemConfig component override disabling only DynamicSystem BootCompletedReceiver, retaining the installer APK/data. It omits the four files belonging to Goldfish MultiDisplayProvider, whose persistent native service needs the QEMU multidisplay pipe. The complete2648-entry archive verifies with SHA256 `5a6c3b45df4133b8f83c0ab71f3977766a743e17f39ed57f92473edb799c1cce`; the Manager build/package validator pass. Test-only AppInspector observes actual package presence, APK hash, receiver enabled state and running package names; it never modifies them. Production host Java and all five fresh JNI libraries, ordinary AV APK/certificate/UID10112 remain unchanged.

Run `b8c0a3750b094e04a02eec3347a5ea44` passes actual policy observations and full repeated all-camera YUV/JPEG, PCM playback/microphone capture, clean peer closure, intermediate Home and fresh AV workload after reopen. `python3 out/api32/verify_rootless_device_av_result.py` passes749 frozen source pins, exact image/Manager/library hashes, package deltas and strict native cleanup. 17 service lifetimes and13 adopted descendants are reaped, zero remain, ownerexit0, host grants restored, SELinux Enforcing and actual host phantom max32 unchanged, no ANR/Java/native fatal. 175 ps samples peak32; 7 actual AMS snapshots peak26 tracked records. No sample contains DynamicSystem or Goldfish multidisplay. This first upgrade pass does not establish repeated fresh-start reliability or a hard bound. Evidence: `docs/api32-device-profile-validation.json`.

### Repeated43 startup budget and Signal rerun

The43 upgrade and two cached-image AV framework starts all pass strict policy, repeated all-camera/audio/Home/reopen and shutdown checks. The cached repeats are `9995b11ad6304d689d7dc9fcb95abd3c` and `656ab56f9bab46959f70dab3e3cc1ef1`, each independently verified by `python3 out/api32/verify_rootless_device_av_repeat_result.py --repeat N --run-id RUN_ID`. Three distinct framework session IDs and six distinct AV nonces prove new lifetimes/workloads; neither cached repeat downloads the image. The combined check `python3 out/api32/verify_rootless_device_budget_repeats.py` deliberately fails:391 samples peak32/32/33, despite all36 YUV frames/twelve JPEGs and six audio workloads passing. The source receipt preserves the budget failure. The peak33 census contains Traceur, its shell/perfetto children and a Manager-child app_process helper; the helper entrypoint is not recorded, so this alone does not establish a causal fix. Matching12L Traceur source confirms shell command execution, but its boot-completed handler explicitly avoids the tracing-state check, so the exact query trigger needs further evidence. No additional app/feature was removed or host limit changed.

Signal43 run `3a9c762dd81c4f568aba5d8732d54110` also passes the actual package/receiver policy, unmodified minSDK32 app identity/foreground, intermediate Home/reopen, mounted MediaProvider, validated default network/public DNS/NXDOMAIN, network-bound HTTPS200, fwmark error recovery and strict cleanup. `python3 out/api32/verify_rootless_device_signal_result.py` passes749 frozen pins and exact artifacts; only the app specification differs from the43 AV Manager. 130 ps samples peak33, 10 real AMS snapshots peak24 tracked records. This is a functional/network pass with the budget gate still open. All original profiles, keys and raw reports remain; no account operations or physical-phone validation are claimed.


### Owner commands in the existing framework service (44)

Image0.78.44 adds the explicit `input-control-v1` marker and changes only
`pocket-manager.jar` and `pocket-tools.jar` versus43. The2649-entry complete
archive verifies as `b198c412188daf11819d61b6681ee463b94c2a1dafd967e9fc1f8a20817ef87c`;
all native, ART/Bionic, framework and media payloads remain identical. After
existing kernel PID/UID and proc-stat-FD/start-time authentication, the private
input service dispatches a finite, bounded owner-command protocol. Its existing
system_server lazily loads the Manager jar outside the bootclasspath, preserving
PackageInstaller commit/status and matching package-broadcast readiness before
launch. A failed new transport never falls back and repeats a potentially
committed operation. Images without the marker retain the original CLI.

The new host first passes the full two-camera/audio/Home/reopen trial on old43
using the actual external CLI, run `c5b5f2e3de7d4aabb7464ef4757cde48`.
The raw candidate's inherited claim of unchanged host Java is corrected explicitly
in `out/api32/embedded-manager-compat43-scope-errata.json`; only DirectSession
changes and GuestManager32 is added in production host Java. No raw candidate or
report is rewritten. Full44 upgrade run `839ca0255c47479fabb844653b44eaf4` and
cached framework starts `e57c03fb653e46df8859f546d85b1b42` and
`3b111b15d9e54402be9b23ccaa4b4fb3` all pass both ordinary AV rounds, actual
package/receiver policy, negative guest-peer/unknown-command/invalid-argument
checks, Home/reopen, host grants/default policy restoration and strict cleanup.
`python3 out/api32/verify_embedded_manager_av_result.py` (with `--repeat` and
`--run-id` for repeats) verifies750 frozen source pins and exact artifacts.
All positive install/open markers name the existing system_server PID; detailed
censuses observe no ManagerTools32 Java VM. The397 samples peak32/32/31.

Signal44 run `f3d6b07367354615a026d803814ff121` also passes its unmodified
Aurora Play app identity/retained UID10107, registration focus/Home/reopen,
mounted source-built MediaProvider, validated default network, public async
DNS/NXDOMAIN, network-bound HTTPS200 and fwmark timeout/EOF recovery. Its network,
install and open operations all run in existing system_server PID21722. The
independent Signal verifier passes750 pins, exact artifacts and full cleanup:
17 service lifetimes/12 adopted descendants reaped, no remaining children,
ownerexit0, no ANR/Java/native fatal, host SELinux/default32 and sensitive
permissions unchanged. It samples33 across131 observations, with a peak census
including two DNS-permission test clients, a starting app and Traceur's query
shell. These processes remain counted. `python3 out/api32/verify_embedded_manager44_budget.py`
therefore returns1:528 samples across four strict functional starts peak
32/32/31/33. This proves removal of command VMs but does not close the budget
gate or prove a unique cause/universal bound. Historical43 failures remain.
Evidence: [owner-command validation](api32-embedded-manager-validation.json).
No phone/account-operation, fresh encrypted migration, legacy regression or
production release claim is made.


### Decoder Surface rendering on44

Run `30a03b7e1aa24194b024ee6df332a4a4` passes actual MediaCodec decoder
Surface output in an ordinary no-permission UID10111 application. Twelve AVC
frames per phase travel through SurfaceTexture, an external OES texture and an
EGL pbuffer; glReadPixels checks2304 sampled RGB pixels per frame against the
known changing gray signal, plus exact SurfaceTexture/presentation timestamps.
All24 output frames pass before and after Home/reopen. AAC PCM, byte-buffer AVC
and EGL Surface-input encoding retain their full repeated signal/frame/EOS
checks. The new requirement rejects historical buffer-output-only evidence.
`python3 out/api32/verify_decoder_surface_result.py --run-id 30a03b7e1aa24194b024ee6df332a4a4`
passes751 frozen source pins, exact ordinary APK/archive/host/image hashes,
authenticated owner operations/negative checks, actual device/provider state,
cached image and complete cleanup.17 service lifetimes/12 adopted descendants
are reaped, zero remain, ownerexit0, host policy unchanged and no ANR/fatal.
Image44, production host Java and five JNI libraries are unchanged.105 samples
peak33; the inspector and Traceur shell/perfetto are retained in the saved peak
census. This functional pass does not close the resource gate or establish
physical-phone/hardware-codec support.

### Camera2/MediaRecorder video on44

Run `966e530c378d460dac80efcb3832621b` passes public Camera2 feeding a
MediaRecorder input Surface, actual AVC/MP4 recording and MediaExtractor plus
MediaCodec pixel decoding on both advertised cameras, before and after
Home/reopen. Four movies contain48 checked decoded nonuniform frames, advancing
packet PTS, initial sync/EOS and the advertised160x120/320x240 dimensions.
Original AudioTrack, direct microphone and all-camera preview/JPEG checks also
pass. The first complete workload takes about37s, so the test-only observation
waits for a fresh completed recording report with a120s deadline after its
existing delay, rather than assuming a30s workload. Older preview/JPEG evidence
cannot satisfy the new recording flag.
`python3 out/api32/verify_camera_recorder_result.py --run-id 966e530c378d460dac80efcb3832621b`
passes752 frozen pins, exact artifacts and the five test/build/runner source
deltas. Only the test observer DEX and ordinary app spec change in the host APK;
production host Java/five JNI and complete44 image are identical. Host media
grants are restored, default32/SELinux remain unchanged,17 service lifetimes and
12 adopted descendants are reaped, zero remain, ownerexit0 and no ANR/fatal.
106 samples peak32; earlier33 failures remain preserved. These four recordings
are video-only: microphone AAC muxed into the same movies remains a separate
check of this MediaRecorder integration. Evidence is retained in
`docs/api32-av-validation.json#cameraRecorder44Trial`.


### Camera2/MediaRecorder muxed microphone audio on44

The first muxed trial `06aff3f006c24591941a9cb850d502ee` remains a preserved
failure. Camera0 produces valid AAC/AVC movies in both phases, while the fixture
treats a camera1 shutdown callback as a capture error before verifying its movie.
The fixture explicitly calls abortCaptures and then rejects reason1. Matching
12L source and the [Android capture-failure contract](https://developer.android.com/reference/android/hardware/camera2/CaptureFailure#REASON_FLUSHED)
identify REASON_FLUSHED as cancellation associated with the application's abort.
The corrected test accepts that reason only after deliberate shutdown, waits
for onReady after abort, and still rejects active capture, disconnect and
recorder errors, including after movie validation. Only CameraRecording.java
changes; validators, production/test host Java, five JNI libraries and image44
remain identical. No failed raw report is rewritten or reclassified as a pass.

Corrected run `354adf097c8f4aa6acbeaf8248b0f239` passes all four muxed AAC/AVC
movies across both cameras and Home/reopen. Each contains twelve decoded
nonuniform video frames and12,288 decoded48kHz mono PCM samples, with advancing
packet/decoded timestamps and independently checked audio/video overlap.
Totals are48 video frames and49,152 PCM samples. One intended shutdown-flush
callback is observed and counted on camera1 in the first phase. All original
playback, direct microphone and both-camera preview/JPEG checks remain required.
`python3 out/api32/verify_camera_recorder_audio_drain_result.py --run-id 354adf097c8f4aa6acbeaf8248b0f239`
passes753 frozen pins and exact artifacts. The host APK differs only in the
ordinary app specification/signatures. Host camera/microphone permissions are
restored; six microphone streams close normally, eight audio streams complete,
zero stream errors occur,17 service lifetimes/12 adopted descendants are reaped,
zero remain, ownerexit0 and no ANR/Java/native fatal occurs. Host default32 and
SELinux Enforcing stay unchanged.110 ps samples peak35, with eight AMS snapshots;
this new resource failure remains open alongside all historical33 failures.
The peak includes the inspector, tracing shell, transient idmap2d, Signal startup
and a starting app. No process is subtracted or unique cause claimed.
Evidence: `docs/api32-av-validation.json#cameraRecorderAudioDrain44Trial`.
The Pi software media integration now covers the named encoder/decoder Surface
and camera/MediaRecorder paths; physical-phone/audio/hardware-codec/lip-sync,
current encrypted migration, legacy and release validation remain incomplete.

The read-only full44 ActivityManager observation is now terminal and passed
(`002a8cf4b46c4ab8845cab3412703419`). Both booted and settled snapshots report
resource/constructor/current cached limit2, empty limit1 and no API override.
Only one cached empty app appears in each snapshot; other AMS processes are
active, persistent or bound. The 78-sample diagnostic peak32 does not replace
the earlier full-media peak35 failure. All17 native service lifetimes and12
adopted descendants were reaped, with no remaining guest owner. Host default32,
SELinux and permissions were unchanged. See
`api32-cache-budget-validation.json#liveActivityPolicy44Trial` for the pinned
report and independent verifier. The next experiment cohosts the pinned classic
Gatekeeper frontend with its existing UID1000 software crypto HALs; credential,
Keystore and encrypted-storage compatibility require new runtime validation.

The separate shared-Gatekeeper image45 (`dc227b523271f61284ff7f9502cd821fc86e952c868706a4461ae394a97cf813`)
changes only the crypto executable, runtime init/boot configuration and image
manifest relative to44. The source-built classic frontend retains its Binder
permission checks, Keystore token delivery, GSI behavior and existing SID state,
with absolute state paths instead of changing the crypto process CWD. The
software Keymaster/Gatekeeper algorithms and every other image payload remain
unchanged. Three focused pairing/rejection tests and the Manager APK validator
passed. The production image builder now builds and pairs this frontend; the
standalone native validator keeps its existing two-component HAL host.

Initial normal Manager boot registered the frontend and both HALs in UID1000
PID18361, but its credential test client did not execute: the fixture omitted
binding the copied ELF interpreter to the instance Pocket loader. Preserve
`out/api32/crypto-frontend-result/failure-audit.json` as a failed credential trial;
all15 actual service lifetimes and11 descendants were reaped, with no remaining
guest owner, no native/Java fatal or ANR, and unchanged host default32/policy and
permissions. Its123 process samples peaked at26 and do not establish the broader
budget gate. The corrected fixture changes only test DEX/signatures and uses
cached45 offline; that trial's state is recorded separately as
`sharedGatekeeper45LoaderTrial` in the validation JSON.

The unchanged stock software HAL has a SID-keyed fast password cache. The new
re-enrollment integration test authenticates the replacement password first,
then checks rejection of the old password against the replacement handle. It
retains the documented software cache semantics; this is not proof of immediate
old-cache invalidation, hardware-backed security or real-user CE/keyguard
migration. A one-process architecture saving does not close the prior peak35
budget failure without new complete workload measurements.

The third shared-frontend trial (`dda26cc330424d2e86fdd983409fd2aa`) passed the
independent full45 credential/startup/shutdown verifier. It waited for the
actual mounted external-volume marker, then completed native enrollment,
wrong-password and altered-SID rejection, challenge/token matching, password
re-enrollment with retained SID, SID removal, and ordinary UID39999 permission
and raw-HAL denial. Frontend and both HALs ran in UID1000 PID21978. All native
service starts matched reaps, adopted descendants and owner were reaped, and
host Enforcing/default32/permissions were unchanged. The67 process samples
peaked at31, with helpers counted. This is a scoped credential/startup pass;
full-media/Signal default-budget repeats, current CE/keyguard, namespace and
legacy-profile checks remain. Both earlier strict failures remain preserved.
The source756 snapshot, package45 and unchanged host JNI/production Java are
pinned in `sharedGatekeeper45MountedTrial`; see its exact verifier command.

Full45 now passes the complete unchanged both-camera AAC/AVC MediaRecorder
workload alongside ordinary playback, direct microphone, preview/JPEG and
verified Home/reopen (`e7d65fbf812c4800b0b99a6f66f30842`). Four MP4 recordings
produced48 decoded video frames and49152 decoded PCM samples; packet/decoded
PTS, EOS and overlapping timelines were independently checked. All16 service
lifetimes and12 adopted descendants were reaped; media permissions and host
Enforcing/default32 were restored/unchanged. The112 process samples peaked at34.

Unmodified Aurora Play ARM64 Signal8.33.2 also passes full45 install, retained
UID10107/certificate, actual registration focus/Home/reopen, public DNS/NXDOMAIN,
network-bound HTTPS200 and fwmark timeout/EOF recovery
(`5b2468ca630a4ee58e861f5df7423065`). Its121 samples peaked at31. The current
combined233-sample full-workload audit fails the32-process budget at34; the
scoped credential peak31 does not replace that evidence. All main/observer jobs
are terminal, no guest owner remains, and the installed APK/source756 pins were
checked independently. See `sharedGatekeeper45CompleteAvTrial`,
`sharedGatekeeper45SignalTrial` and `currentFull45BudgetAudit` in the validation
JSON. Prior44 peak35 and all failed trials remain intact.

The actual media log exposes readiness before initial boot broadcasts drain:
InputControl ready precedes BOOT_COMPLETED posting, owner install completion
precedes final BOOT_COMPLETED processing, and open follows it. Pinned12L primary
sources confirm sys.boot_completed is set before UserController sends the
ordered broadcast; its volatile mBootCompleted is set by the final receiver.
This identifies a startup-readiness overlap, not a proven complete explanation
of peak34. Do not call waitForBroadcastIdle as a read-only check: its stock
implementation cancels deferrals. The next candidate is real same-process
boot/user/storage readiness through authenticated owner control, preserving
early display/input for locked CE/keyguard and all services/tracing. The
read-only source/log review is `out/api32/boot-readiness-review/evidence.json`;
it has not changed production behavior. A separate same-UID/GID0 installd/core
consolidation review records fork, environment, policy and data-lifecycle
requirements; it too is unimplemented.

The app-readiness implementation is now paired with
`/system/etc/pocket-app-readiness32` (`boot-broadcasts-and-volume-v1`). The lazy
Manager JAR reads the actual system-server class loader from ZygoteInit, then
LocalServices' real AMS/UserController volatile `mBootCompleted`, user-unlock
state and the selected primary emulated volume. Owner install/open/dialer/contact
operations wait boundedly for actual app readiness; input/display startup and
legacy standalone CLI behavior remain available. No broadcast deferrals or
feature flags are changed, and no extra ART process runs the check.

Image46 passed the focused actual-state/early-input/temporary-credential trial
(`70975967c8b0439f9607a007ced89428`). It observed mount-pending/boot-pending,
mounted/boot-pending, then mounted/boot-complete in the same system_server PID3581.
Four successful injection replies covered WAKEUP and HOME down/up while readiness
was false before and after input. The117 samples peaked at30; full native/external
mount teardown and credential checks passed. This scoped pass does not close the
broader resource or locked-CE/keyguard gate.

The first full46 workload stopped before install because userUnlocked=false was
incorrectly treated as necessarily requiring credentials. Preserve that FAIL
(`1e45e33f8b2b4a19ba8ad9072d9992ef`) and its32 samples/peak27; it is not full media
or resource evidence. All14 native lifetimes/11 descendants stopped cleanly,
with no fatal/ANR and host permissions restored. Image47 corrects the predicate:
automatic user unlock can remain in progress, while a secure/device-locked user
that has not unlocked receives the explicit unlock error. Readiness itself still
requires actual user unlock, boot broadcast completion and mount. Only the
Manager JAR/manifest differ from46; all native/platform payloads match exactly.
The complete unchanged media trial now passes with source757
(`bf335cfed1f24ee6afb35a99a327eb69`). It observed the real automatic unlock
transition, primary mount and final boot-broadcast completion in system_server
PID10179. Both cameras passed muxed AAC/AVC recording and decoded media checks
before and after Home/reopen. All16 native service lifetimes and12 adopted
descendants were reaped; host permissions and default policy were preserved.
The172 process samples peaked at30. One full run does not prove the repeated
resource bound; encrypted-profile behavior and an operation submitted while
readiness is still pending remain unproven.

The shared Gatekeeper frontend also now has canonical compiler source-path
mapping. Two fresh builds in different output directories produced identical
frontend objects and identical executable SHA256
`0836be1da15207589330d2f0ca7b119449611a39b66f59939f3625ade2b48a4a`.
This proves that output-directory case only, not full cross-toolchain image
reproducibility. See `out/api32/readiness-crypto-repro-check.json` and the image
package comparisons for the pinned unchanged algorithms and payloads.


Unmodified Aurora Play Signal8.33.2 also passes image47 install, retained UID10107
and certificate, actual registration focus, Home/reopen, public DNS/NXDOMAIN,
network-bound HTTPS200 and fwmark timeout/EOF recovery
(`4eb874f210d54f169a5c20920b3408a1`). All16 native lifetimes and12 adopted
descendants were reaped. Its130 process samples peaked at32. The frozen full47
media/Signal cohort totals302 samples across two starts, peaks32 and passes the
sampled budget check. It does not prove a hard bound or repeated-start stability.
The earlier full45 peak34 and full44 peak35 remain preserved.

A separate host candidate fixes the response deadline for owner operations:
readiness and PackageInstaller/broadcast delivery can each consume120 seconds,
so the socket now allows300 seconds. The independent input connection retains
its existing deadline. A full AV test option preloads its ordinary APK, submits
the install during actual pending startup, requires the real wait-to-ready
receipt from the same system_server, and checks accepted HOME input within that
wait. It then runs all existing media, foreground/Home/reopen, authorization and
cleanup checks. This candidate changes only DEX in the Manager APK: the image,
five host JNI libraries, diagnostic assets and ordinary AV app remain identical.
Its focused build, distribution validator and Python syntax check pass. Runtime
proof now passes in `fdaee20791244ba6907def0b6f0a1094`: real startup wait46011ms,
with all four WAKEUP/HOME injection replies accepted1806ms after the wait began.
The same system_server PID20477 observed automatic user unlock, primary mount
and final boot-broadcast completion before installation. Both complete media
rounds pass with48 decoded AVC frames and49152 decoded AAC samples. All16
native lifetimes and12 adopted descendants were reaped, no owner remains, and
permissions/host defaults were preserved. Its120 samples peaked at30. This
proves the actual pending-operation path and responsive input, rather than the
worst-case300s response deadline or a hard process bound. See `out/api32/readiness47-pending-package-check.json`
for exact commands and results.


The encrypted-profile upgrade review verifies the existing six strict cycles
from image37 (`0fe81ba592384d8e88333c9dd0ba1b6a`), original password-free
restoration and removal of its temporary credentials. Current-image encrypted
upgrade and the secure-locked readiness branch remain unverified. The CE driver now validates the latest full-lifecycle predecessor, all six raw
cycle hashes/strict checks, unchanged app/key fields and the actual installed
APK before this upgrade. Do not
substitute a restoration flag for a real authless-boot observation or reset the
profile. See `out/api32/readiness47-ce-upgrade-review.json`.


The first current-image CE3 attempt stopped before boot or any credential change:
`d140c380a1e04bcba2ee237c82e26bb4`, "Existing encryption trial requires deliberate
resume". The previous successful trial retained its restart checkpoint. This
failure and its unused private recovery file remain preserved; it is not CE or
resource proof. The fixture now accepts an explicit verified completed-run nonce,
observes actual authless boot and unchanged app/key data, then archives that exact
checkpoint without overwrite and records its SHA256 before changing credentials.
The driver accepts only that exact pre-credential rejection when replacing its
installed APK. Compilation, syntax and image-free distribution checks pass;
the continuation `b7625ee6ca134398a9fd7f8451931b01` is running on VM73, with the
same image/five host native libraries and production Java as the pending-install
AV pass. Its original host workflow failed after five successful guest cycles, as detailed below; the raw failure is preserved.


Image47 now passes the preserved encrypted-image upgrade and six guest lifecycle
cycles. The five-boot guest result passed, including a focused physical password
unlock, but the host verifier then hit a Python scoping error (`storage` inside
the readiness checker). The raw host FAIL remains preserved. Corrected helpers
were replayed against every recorded locked/ready/storage/UI state and the five
strict cycle files. An explicit outer-only continuation then kept the installed
APK, loaded only this exact run's owner0600 recovery file on the Pi, actually
restarted the Manager and completed the sixth strict cycle. The restored report
retains every original cycle hash and links the failed host report by SHA256.

Both physical password unlocks, the unchanged ordinary app/software Keystore,
five-root and public-media canaries, mounted/queryable MediaProvider, real
locked-operation rejection and post-unlock boot/user/mount readiness pass. The
original password-free state was restored and the used recovery file removed;
the earlier unused checkpoint-rejection file remains private and preserved.
All jobs are terminal and no guest owner remains. Verify with
`python3 out/api32/verify_readiness47_ce3_outer_resume_result.py` (PASS) and
`python3 out/api32/verify_readiness47_ce3_terminal_state.py` (PASS).

The combined536 CE process samples peak33 (first five boots33, outer restart30),
so resource validation fails despite the functional pass. A source build of all
13 pinned installd implementation units, its full AIDL interface and real static
async-safe logging now passes ARM64 linking and16KiB ELF alignment checks. The
new dependency headers are pinned to the same12.1.0_r23 source baseline. This is
preparation for same-UID/GID0 installd/core integration; it has not been installed
or runtime-validated. Keep scoped SELinux/exec-worker behavior and initialize
globals before other core threads when implementing it. The full build command
and source/binary hashes are in
`out/api32/installd-source48/independent-build-check.json`.

The 0.78.48 candidate source-builds the complete pinned 12L installd into the
existing UID0/GID0 native core. All thirteen implementation units, daemon
startup, generated IInstalld and Bionic async-safe logging are linked. Only
startup is patched: directory/global initialization happens before core
threads, private installd state is initialized before publishing, and the
component reuses the established Binder pool. App-data, permission, quota and
dexopt method bodies retain their upstream implementation.

This selection deliberately changes the common libbinder default minimum
scheduler priority from nice19 to nice0 before any Binder threads start,
matching standalone installd. `disableBackgroundScheduling` is process-wide,
so it cannot safely be changed later by a service thread. Other core endpoints
share that minimum when this component is selected. The older standalone
layout and default core build retain their existing selection behavior.
Installd uses explicitly named guest-policy and exec hooks; the combined ELF
exports no generic execv/SELinux interposers. ART exec children receive a new
environment without the Binder, HIDL or policy daemon preloads, with unrelated
preloads and environment retained and without mutating the parent environment.
Crypto remains a separate UID1000 process.

`python3 tools/build_api32_core.py --ndk
/home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c
--reuse-generated --installd --out out/api32/installd-core48` passed. The image
builder independently rebuilt the same core bytes (SHA256
c710cefcf63f8a94f59415537ec1b2efe45257172d77a27c564b29125e7f20c7).
`out/api32/installd-core48/independent-build-check.json` verifies complete
object linkage, scoped hooks and ARM64 16KiB PT_LOAD alignment. This is a
scoped two-output comparison, not whole-image reproducibility proof.

`cc -std=c11 -O2 -Wall -Wextra -Werror -pthread
runtime/tests/host-native/installd_worker_env_host.c -o
out/api32/installd-worker-env-host && out/api32/installd-worker-env-host` passed
five actual fork/exec cases, including concurrent parent environment
observation. `python3 -m unittest tools.tests.test_api32_installd_layout
tools.tests.test_api32_crypto_layout` passed six tests covering paired startup,
identities, preloads, mixed versions, duplicates and idempotence.

Image48 SHA256 a9229e62f9e2431f20b72a68933b42815d0cae435ac7d90b4d641d4fd37b3e25
contains the same 2650 runtime entries as47; only pocket-core-api32, its init
configuration and boot script change, plus the manifest. The standalone
installd executable remains available but is not declared or started in this
selected layout. Both a published readiness marker and the typed installd
service wait remain mandatory. Image packaging and native compilation do not
prove runtime behavior or close the prior CE sampled peak33/default32 gate.

The complete image48 VM73 trial `7778cd944b35417e9d7c89d2ba0dfb81` passed.
`python3 out/api32/verify_installd_core48_av_result.py --run-id
7778cd944b35417e9d7c89d2ba0dfb81` independently verified all763 frozen source
pins, the actual live core executable/PID17388 serving installd at logical
UID/GID0, system-role typed CE/DE app-data ownership/cache groups/modes/size/
clear/destroy, invalid-name rejection, UID39999 caller denial, and actual
stock boot-time dex2oat with odex ELF/vdex verification and cleanup. These
checks ran before the unchanged full two-round ordinary-app media workload:
both Camera2 devices, preview/JPEG, muxed microphone AAC/AVC MP4, 48 decoded
recording frames, 49152 decoded AAC samples, PCM playback/microphone, focused
Home/reopen and mounted private MediaProvider all passed. The full workload
retained its app UID/certificate and the five validated42 source-built JNI.

All15 native service lifetimes and12 adopted descendants were reaped. Across
200 all-process samples, including test clients and tracing, the observed
peak was32 at the unchanged host default32. Seven AMS snapshots were retained.
This is one-start sampled evidence, not a hard bound or a replacement for the
preserved image47 six-cycle CE peak33 failure. Signal48 and CE48 still need
separate repeats. `python3 out/api32/verify_installd_core48_terminal_state.py`
passed: no guest owner remains, all supervisors/observers are terminal,
installed Manager hashes match, the CE3 Manager is unchanged, SELinux remains
Enforcing, max_phantom_processes remains32 with unchanged flags, host camera/
microphone are denied again, and reverse ports/test servers are gone. Raw
reports and independent checks are under `out/api32/installd-core48-av-result`.
No production support flag, commit, release or phone action was made.

The unchanged Aurora Play ARM64 Signal8.33.2/API32 repeat on image48 also
passed, run `530366892b7b4edda5ae19d0813460ea`.
`python3 out/api32/verify_installd_core48_signal_result.py --run-id
530366892b7b4edda5ae19d0813460ea` verifies all763 identical runtime/tool source
pins, retained app UID10107/certificate/splits, actual focused registration
screen, Home/reopen, mounted source-built provider, public asynchronous
DNS/NXDOMAIN, network-bound HTTPS200, fwmark error/recovery and strict native/
adopted-descendant teardown. Shared installd again publishes from the
UID0/GID0 core; the credential frontend remains in its separate UID1000 host.
Signal's test Manager has the exact same DEX/manifest/five JNI/image as the
passing48 media trial; only its app specification changes and the optional
native-test assets are omitted. No Signal account registration, messages or
calls were performed.

Signal48 peaked29 in132 samples. Together the two image48 starts have332
samples and peak32, with every helper/tracing process included. The combined
receipt `out/api32/installd-core48-media-signal-budget-check.json` explicitly
leaves CE48 and a hard bound unverified, and preserves the prior full47 CE
peak33 failure. `python3
out/api32/verify_installd_core48_signal_terminal_state.py` passed; no guest
owner or live supervisor/observer remains, host default32/flags/Enforcing and
denied camera/microphone are unchanged, test ports are closed, the installed
Signal-test Manager hash matches28eaa87904a47dce86da40660a13b322e129c89e16151af0b0a5cf81ce8b71f2,
and the preserved CE3 Manager hash remains unchanged.

A read-only audit also records the remaining ordinary post-boot dexopt
scheduling check in `out/api32/installd-core48-postboot-dexopt-review.json`.
Pinned installd requests Dex2OatBootComplete before its background nice
change; the stock aggregate includes CPU/blkio cgroup joins and40ms timer
slack. The successful native compiler test intentionally covers boot-time
execution only. This audit implements no scheduling substitute and makes no
post-boot compilation claim. The full pinned installd/async-safe source cache
remains clean against its pinned Git revisions; the upstream-clean receipt
is `out/api32/installd-core48-upstream-clean-check.json`.

The next image48 CE3 trial reuses the exact native installd test body from the
passing media fixture (SHA25669b68067de29621675327dae3b8e74d48cbe303324022b9550cc72f4e8f825b1),
now in the shared test base class. The CE probe invokes it after verifying the
preserved original app/key and before changing credentials. Only four test/
packager source files change relative to the complete48 media/Signal source
snapshot; production host Java, the image and all five validated JNI remain
unchanged. The packager now replaces an existing CE test asset rather than
adding a duplicate ZIP entry. Both the app fixture and CE fixture rebuild;
CE package1f9e52e99d725b14d5c9b374339c169666494ab77740eb5cb352dcad59f27cdc
passes signing and package/delta checks. `out/api32/installd-core48-ce3-package-check.json`
records the exact package scope. Trial b0111ff7db22438ea3fb0d3e122803cf uses
the successful six-cycle47 report as its explicit predecessor. No profile or
guest app is reset/reinstalled; new test credentials are held only in this
run's owner0600 Pi recovery file and must survive any failure until deliberate
recovery/restoration. The runtime result is pending.

While the CE trial runs, read-only storage preflight establishes that VM73
uses QEMU (`qemu_cli`), `use_existing`, configured8192MiB f2fs userdata and
qcow2-v3 overlays. f2fs tools are1.16.0. A private ARM64 qemu-img11.1.2 and its
sole missing libaio dependency were extracted from configured, root-owned
repository SHA256 metadata without installing system packages or changing a
global library path. The executable/version and package/library hashes are
in `out/api32/vm73-qemu-img-ready.json`. The matching qemu-img/qemu-io chain
passed a16MiB scratch backing+overlay conversion: both distinct data patterns
and zero ranges survive, qemu-img compare succeeds, and both input hashes
remain unchanged (`out/api32/vm73-qcow-preservation-test.json`).

These results do not resize real userdata. Public host properties report
file encryption, `/data` is mounted from dm-105 and the vendor fstab names a
metadata-encryption key directory. Decrypted offline f2fs access has not been
established. No keys/dm tables were read and no current VM disk, backing image,
config or VM lifetime was changed. `out/api32/vm73-storage-preserved-data-preflight.json`
separates proven tool/format facts from the still-required coherent backup,
rollback, GPT/backing-chain and encrypted-filesystem resize path. Do not use
the plaintext scratch conversion as proof that real encrypted /data can be
safely resized.

The image48 encrypted-profile trial b0111ff7db22438ea3fb0d3e122803cf passed in
one uninterrupted host workflow. `python3
out/api32/verify_installd_core48_ce3_result.py --run-id
b0111ff7db22438ea3fb0d3e122803cf` independently verifies all763 frozen source
pins, all six raw cycle hashes/strict shutdowns, two focused UI password
unlocks, wrong/changed/removed password behavior, actual locked owner app-open
rejection and unlocked boot/user/mount readiness from each same system_server.
Original app UID10106/token/cipher digest/canonical path/software Keystore
key and all five root canaries plus public media persist through the47-to48
encrypted image update. The predecessor checkpoint is archived by its own
hash after live original app/key verification; the old Pi report remains
unchanged. Actual image-install statistics report2596 reused files and54
replaced files (runtime-patched inputs included), with the complete2650-entry
inventory and byte accounting verified. No plaintext migration or guest-app
reinstall/data reset occurred.

The complete native installd permission/app-data/dexopt diagnostic also passes
through encrypted CE storage: root core PID28640, logical UID/GID0, ordinary
caller rejected, system-role CE/DE create/cache groups/modes/size/clear/destroy,
invalid name rejected, actual boot-time dex2oat odex/vdex and temporary data
cleanup. All six boots show shared installd in the matching core; crypto is
still in a distinct UID1000 process. Service lifetimes reaped per cycle are
14/14/14/15/14/14; adopted descendants are11/11/11/11/11/12, all with zero
remaining. The outer Manager really restarts while the image server is
offline, repeats focused UI unlock and data/key checks, restores original
password-free access, and only then removes this run's owner0600 recovery
file.

CE48 sampled peak29 across562 observations, with all guest/helper/tracing
processes counted. Together with the full48 media and Signal trials there
are eight distinct guest starts and894 samples, overall peak32 at unchanged
default32. `out/api32/installd-core48-full-workload-budget-check.json` records
this finite sampled success and preserves the earlier full47 CE peak33
failure as a separate historical result. It does not claim a hard process
bound. `python3 out/api32/verify_installd_core48_ce3_terminal_state.py` passes:
all supervisors/observers are terminal, no guest owner remains, installed APK
hashes match, the predecessor report is unchanged, the new recovery file is
gone, Enforcing/default32/flags and denied host media permissions are intact,
and test servers/reverse ports are closed.

Post-boot ordinary compiler scheduling, a separate current-backend fresh
plaintext-to-CE migration, namespace reset/death/stale-endpoint work, complete
legacy Android9/10 regressions and eventual physical-phone/delivery checks
remain open. Source support flags remainfalse; no commit, publication or phone
operation was performed.


Post-boot compiler control on unchanged image48

The distinct system-UID post-boot IInstalld request passes with flags10
(DEXOPT_PUBLIC | DEXOPT_BOOTCOMPLETE), actual odex/vdex creation and temporary
output removal, followed by the complete two-round ordinary camera/audio
workload, Home/reopen and strict native cleanup. Run
`b9673d0d1fd946f4acf18082c9bd3d08`; independent command
`python3 out/api32/verify_installd_postboot48_av_result.py --run-id b9673d0d1fd946f4acf18082c9bd3d08`
passes. All128 observed process samples remain at or below30 with the real
host maximum32 and Enforcing unchanged. The terminal check also passes.

This control targets UID1000 and does not prove an ordinary app compiler
worker priority. Pinned libprocessgroup source logs per-action task-profile
errors but returns success; successful compilation therefore cannot prove
kernel cgroup placement. A separate UID12347 test is running with actual
host-side live worker executable, identity, PPID and kernel nice observations,
artifact owner/group/mode checks, and unchanged core priority. Only the
native probe, shared test helper and packager changed; production runtime,
image48 and five host JNI files remain unchanged.


Ordinary post-boot compiler observer failures

Run `44cbd06f8e54447bb1dc9b3c18064ff3` performs actual boot-time and
post-boot compilation successfully for system UID1000 and target UID12347.
For each operation it verifies ELF odex/vdex content, owner1000, target
group, mode0644 and removal of its temporary directory. It fails before
media testing because the host observer Future times out during shutdown.
The original scan walks historical identity files with no cancellation
check inside that scan. Its complete report, frozen source and failure
audit are retained under `out/api32/installd-postboot-app48-av-result`.

Run `9fbd1969baff4e238b9ea9f3378b3b59` repeats the successful native
compilation/ownership/cleanup and records unchanged core nice0. Its
observer stops promptly but finds no live worker through per-thread
`children` files. A separate host-shell check finds
`/proc/1/task/1/children` unavailable. This run also fails before media;
its report/failure audit remain in
`out/api32/installd-postboot-children48-av-result`. Both guest owners stop
with exit0 and all native descendants are reaped; their sampled peaks30
and29 do not constitute full-workload passes.

The replacement observer enumerates actual live `/proc` PIDs, checks
cancellation per entry, requires kernel stat PPID equal to the core, then
checks the exact ART executable, authenticated live UID12347/start time
and actual host-kernel priority. Interrupting its polling sleep permits
prompt shutdown. The rebuilt APK passes compilation/signature checks and
the distinct live-proc trial is running. Production Java, five host JNI
libraries and image48 are unchanged. No ordinary-worker scheduling or
new media pass is claimed until the complete result is independently
verified.


Ordinary post-boot compiler and complete media repeat pass

Run `6f6983eb987148c2acacfd0f8c1eab0d` passes on unchanged image48.
The actual dex2oat64 PID32704 has kernel parent29913 (the live core),
verified live guest UID12347/start identity, exact installed ART executable
and actual host-kernel nice10. The core remains nice0 before/after.
Boot-time system and post-boot system/ordinary requests all create valid
odex/vdex with owner1000, target group1000/12347, mode0644, and fully remove
their unique temporary directories. The host can read actual cgroup
membership, which remains top-app for cpuset/CPU; timer-slack reads are
unavailable. No kernel cgroup-profile or timer-slack application is claimed.

The same run completes both ordinary camera/audio rounds, four muxed
AAC/AVC recordings and48 decoded video frames, Home/reopen and strict
service/descendant cleanup. All121 process observations stay at or below30
with unchanged default32, Enforcing, flags and restored media permissions.
`python3 out/api32/verify_installd_postboot_proc48_av_result.py --run-id 6f6983eb987148c2acacfd0f8c1eab0d`
and `python3 out/api32/verify_installd_postboot_proc48_terminal_state.py`
both pass. The terminal check finds no guest owner, matching installed APK
and preserved CE APK hashes, denied host media permissions, closed servers
and removed reverse ports. The exact narrow build/signature command and
results are retained in `out/api32/installd-postboot-proc48-package-check.json`.

The full48 media/Signal/six encrypted cycles plus the two post-boot full-AV
passes now cover ten distinct starts and1143 finite observations, peak32.
`out/api32/installd-postboot48-full-workload-budget-check.json` keeps each
run distinct and preserves historical47 peak33 and both observer failures.
This is a sampled resource result, not a hard bound. The next open work is
actual framework reset and abrupt outer-owner death/recovery, separate fresh
plaintext-to-CE migration, current legacy regressions, and eventual phone
and on-demand delivery validation. Support flags remainfalse.


Framework storage reset trial and stock callback contract

A new optional volume fixture requests an actual framework reset through
IStorageManager.setDebugFlags while reapplying the identical guest
persist.sys.adoptable value. It registers the actual framework volume
listener first; Manager additionally requires a new mounted epoch, real
old descriptor-owner retirement, original private canaries and fresh
complete mounted provider/private40 workload. Production runtime/image/JNI
are unchanged. The earlier real unmount/remount report remains accepted
and is explicitly rejected as evidence for this distinct reset mode.

First run `bad1cd36c8ad43ab9148493212b41db5` fails its test-only callback
expectation. Native output shows the second actual reset, two successful
mounts, retired holder7046, final mounted state2, correct cleanup and
unchanged host policy. It receives recreated-volume callbacks0->1 and1->2,
then waits for old teardown callbacks that stock Android intentionally
removes. The pinned source queues old state changes onto mHandler, delivers
listeners later in onVolumeStateChangedAsync, and removes pending handler
messages during StorageSessionController.onReset. Source hashes and the
reason are retained in `out/api32/framework-reset48-callback-contract-review.json`;
the failed raw report and audit remain intact. No post-reset file
persistence/full-reset pass is claimed for this failed run.

The corrected test requires the recreated checking/mounted callbacks and
retains independent extra native reset, changed epoch, actual old owner
retirement, unchanged guest property and complete post-reset file/provider
proof. The corrected helper and APK rebuild/signature pass; exact commands
are in `out/api32/framework-reset-contract48-package-check.json`. A distinct
current-source trial is running on VM73; abrupt outer Manager death, fresh
plaintext migration and remaining legacy/phone/delivery gates remain open.


Current image48 actual framework reset and file preservation pass

Corrected run `53ef421a11b24f10960c6848b6595e16` passes the full
framework reset, ordinary mounted provider/private40, network and
Home/reopen workload. The actual IStorageManager.setDebugFlags request
reapplies the same empty guest persist.sys.adoptable value and completes
in2663ms. It receives recreated volume callbacks0->1 and1->2, and native
output independently records the second actual vold reset and second
successful mounted session. Mounted epoch changes from851379328537255093
to7028363496576650002. Held descriptor ownerPID12172 is really retired;
new ownerPID13246 has no held old descriptor and independently reads the
same two canary contents and inodes195720/195892. The virtual device changes
from70 to54, reflecting the fresh mounted epoch. Complete fresh ordinary
provider/private-path/FD work runs again after reset.

`python3 out/api32/verify_framework_reset_contract48_app_result.py --run-id 53ef421a11b24f10960c6848b6595e16`
passes, as does
`python3 out/api32/verify_framework_reset_contract48_terminal_state.py`.
Source763 is pinned; production Java, image48 and all five host JNI files
are unchanged. All15 service lifetimes and14 adopted descendants are
reaped, with no owner/fallback cleanup/ANR/fatal left behind. All138 samples
remain at or below30 under unchanged Enforcing/default32 and flags. Both
observers are terminal without errors. The terminal check confirms exact
installed Manager and unchanged CE3 APK hashes, no guest owner, denied host
media permissions, removed reverse ports and offline test servers. The
first incorrect callback-expectation failure remains preserved separately.

The unchanged production runtime now has eleven distinct full workload
starts and1281 finite process samples across media, Signal, six encrypted
cycles, system/ordinary post-boot dexopt, and framework reset; peak32.
`out/api32/framework-reset48-full-workload-budget-check.json` records each
frozen test APK/source scope and retains all historical failures. This
remains a sampled result, not a hard process bound. Actual abrupt outer
Manager death/stale endpoint recovery is still unverified, as are a
separate fresh migration, current legacy profiles, eventual phone and
delivery gates. Source support flags remainfalse.


Abrupt Manager crash recovery first runtime trial and driver corrections

A test-only checkpoint protocol now records a live full guest with an
ordinary app holding a real mounted descriptor, two private canary files
and a non-exportable AndroidKeyStore key that decrypts persisted
ciphertext. It fsyncs an owner0600 checkpoint and authenticated signal
record, then sends SIGKILL9 to the actual host Manager PID. A pre-service
validation hook observes stale endpoints before production startup can
clean them. Recovery uses the same APK/profile with the fixture server
and reverse route already offline; it must preserve canary inode/content,
UID, original key and ciphertext across a new host PID and mounted epoch.
The final cleanup removes only this nonce's test key/canaries. Production
Java, image48 and five source-built host JNI files remain unchanged.

First run `d13c1008247840948eae45ef92204199` kills ManagerPID14417 and
recovers inPID18728. The guest functional report passes file/key/network/
Home/reopen and strict final cleanup; all26 old process identities are
gone, with zero old guest processes still live at pre-start observation.
The first driver incorrectly treats a restarted package/service process
as an activity, so a guarded explicit intent opens the absent recovery
activity in that same process without killing/reinstalling anything. Its
receipt is preserved. The driver also evaluates request accounting before
finalizing that field and requires a new install owner operation on the
recovery boot, contrary to the no-reinstall requirement. Original raw
driver status remainsFAIL, with guestPASS, peak31 and322 samples. Audit:
`out/api32/manager-crash48-app-result/failure-audit.json`.

The corrected driver always sends the singleTask resume intent after
confirming the exact old Manager PID has disappeared, snapshots requests
before evaluating recovery, and requires the first boot's real authorized
install plus the new system-server owner's open/network operations while
rejecting any new install operation. It preserves both boots' peer-denial
and invalid-owner-command checks. Python compilation and real previous
framework-reset/compiler authorization/namespace regression predicates
pass. The original terminal report and its frozen driver remain unchanged. A
new, independently guarded nonce/trial is exercising the corrected driver;
no full automated crash pass is claimed yet.


Activity-removal acknowledgment in the abrupt-recovery driver

The immediate-resume trial `12a75d2eb1d9422e8078f0699a5b90b7` exposes
an Android host activity-death race: resume START returns3 at01:19:57.652,
then the old singleTask activity is forcibly removed as dead at57.677.
The package process exists without the test activity. The exact new
launcher function is tested on that real state: it observes exact component
absence, opens the activity without killing/reinstalling a process, and
receives new recovery-PID28263 acknowledgment. Its source/receipt and raw
host logs are preserved. The guest and original driver then finishPASS,
including original files/non-exportable key and strict cleanup, peak30
over392 samples. This remains an assisted trial, separately audited at
`out/api32/manager-crash-owner48-app-result/supplemental-launcher-audit.json`;
it is not claimed as a standalone driver pass.

The final driver waits for exact component absence before sending an
intent, or acknowledges a matching already-restored recovery activity. It
records every launch attempt and requires a new current-run host PID
acknowledgment, preserving timeout/transient-error evidence without
killing/reinstalling anything. Python compilation and actual prior owner-
authorization/reset acceptance regressions pass. A fresh standalone trial
with the identical APK/image/five JNI and only this Python driver changed
is running. `docs/api32-crash-recovery-validation.json` records current
status and both distinct prior trial scopes.


Standalone abrupt Manager SIGKILL and offline file/key recovery pass

Run `160b1ac256b84225894020ebe4ebecc3` passes end to end with the
acknowledged-activity driver and no manual/supplemental launcher. Real
ManagerPID3374 self-SIGKILLs with the ordinary owner holding a mounted
descriptor. The fixture server and reverse route are already offline.
The driver observes exact component absence, opens the singleTask activity
and receives new recovery-PID7741 acknowledgment. The pre-service hook
sees the same old volume socket inode/type before production cleanup.
All26 old PID/start identities are gone; none remain live at pre-start,
so this run does not claim observation of a live orphan-cleanup path.

The mounted epoch changes from-1109557664130948602 to3966043662980268260.
Original canary UID/inodes/contents and the original non-exportable
AndroidKeyStore key decrypting identical saved ciphertext survive. The
APK is not reinstalled or force-stopped after the crash. Each boot performs
real mounted provider/private40 and network/DNS/HTTPS/fwmark work plus
Home/reopen. Only this nonce's key/canaries are removed after verification.
Recovery then reaps15 native service lifetimes and13 adopted descendants,
with no fallback cleanup/remaining owner, ANR or fatal.

`python3 out/api32/verify_manager_crash_ack48_app_result.py --run-id 160b1ac256b84225894020ebe4ebecc3`
passes, as does `python3 out/api32/verify_manager_crash_ack48_terminal_state.py`.
All763 source pins match, with production Java/image48/five JNI unchanged.
Both observers terminate without errors;241 samples peak31 under unchanged
default32/Enforcing/flags. The final state check confirms no guest owner,
matching installed APK and unchanged CE3 APK hashes, denied host media
permissions, removed reverse routes and offline test servers. The earlier
failed and assisted crash trials remain separate and unchanged.

The passing full-workload aggregate now covers13 starts and1522 finite
observations at peak32. The receipt also lists the five preserved failed/
assisted scopes: across all listed current48 scopes there are20 starts
and2556 observations, also peak32. Historical47 peak33 remains preserved.
No hard process bound or physical-phone/production-support claim is made.
A distinct fresh plaintext-to-CE migration, general namespace edge cases,
current Android9/10 regressions and eventual phone/delivery checks remain
open. Main source support flags remainfalse.


### Native parent and final-symlink routing, images49/50 (VM73)

The ordinary app now loads its own source-built ARM64 JNI probe and exercises
real openat/dotdot, symlink-before-dotdot, private-target denial, trailing
slash, poll/fstatfs, proc FD, cwd and renamed/unlinked-handle cases. The
complete mounted MediaProvider/private40/network/Home/reopen workload remains
required; no diagnostic helper or workload was removed for process-budget
acceptance.

The original image48 run `0d50cd260c5f4eb78c94ecdea0296413` fails with EINVAL
at relative dirfd dotdot. Its raw report/source snapshot and failure audit
remain in `out/api32/namespace49-app-result`;105 samples peak31. The anchored,
provider-authorized parent helper and final-symlink UNLINK fix are built into
image49. Real VM run `f8d6318ff94146a9abae9608a5c5e338` then passes the relative
parent case but fails the next absolute symlink-before-dotdot case with EPERM.
Its raw report is retained in `out/api32/namespace-parents49-app-result`;
210 samples peak29. Both runs remove only their nonce's canaries, stop all
native owners/descendants and finish with unchanged default32/Enforcing,
host media permissions and retained CE3 APK. These are scoped failures,
not full namespace/Home-reopen passes.

A focused host reproduction also shows existing and dangling final symlinks
in an internal directory returning unhandled despite pointing into lower
external storage. Following operations now route the actual target to the
broker; nofollow metadata, unlink, rename and exclusive-create semantics
remain on the link itself. Unsupported following mutations also route to
provider policy. Parent-containing absolute paths can be prepared only as
explicit external namespace probes; an unhandled probe is rejected before
the raw syscall fallback. Legacy/disabled mapping keeps the old rejection.
The parent helper also rejects pre-initialization requests with ENODEV.

`out/api32/external-symlink-path50-host`,
`out/api32/external-runtime50-host`,
`out/api32/external-namespace-rpc50-host` and
`out/api32/fuse-namespace-parents50-host` pass. The actual in-process broker
fixture checks following/dangling internal-link stat, unsupported mutations,
nofollow operations, live caller/descriptor authentication, bad ancillary
rights and balanced handles/lookup references. An existing fixture race in
excess-rights rejection was diagnosed with GDB: an unknown partial header
receives the common EPROTO prefix rather than a full namespace reply. The
fixture accepts only that malformed-rights error prefix in that case.
Exact build/check commands and scope are recorded in
`out/api32/namespace-routing50-package-check.json`.

Image0.78.50 builds with2650 entries. Only `system/bin/pocket-core-api32` and
`system/bin/pocket-loader` differ from49. SHA256:
`a77a36c792d537cf5b0a965ff85879e127d96b92c3003d3095810d7576cb6add`.
`loader-build.json` now records all22 loader source/build inputs, the exact
compiler command and NDK properties hash, and is bound into image inputs.
The normal Manager test APK is51db3a85dfc62e86fd9d32b33642fd7ccec141270a94713350e7ce97d7c048e4;
all five host JNI libraries and production Java remain unchanged. The
current768-file source snapshot is
`out/api32/namespace-routing50-app-source-snapshot`.

The new mounted VM trial is running, not accepted yet. Its job and exact
process handle are in `out/api32/namespace-routing50-app-job.json`; poll it
with `python3 out/api32/poll_namespace_routing50_app.py`. An observation
expiration is not a terminal condition and must not trigger a restart.
The earlier full image48 AV/Signal/six-CE/reset/crash receipts remain their
original scopes; this production-loader/core change has not repeated them.

The same trial reports only pre-service marker existence: the retained
normal API32 profile has guest CE disabled and no five-root CE marker.
Evidence is `out/api32/namespace-routing50-storage-markers.json`. No keys,
passwords or historical credentials were read. This provides a candidate
for an explicit retained-plaintext-profile migration test, preserving its
existing apps/UIDs/keys/data; no conversion or profile/storage change has
been performed. Host encrypted /data is a separate, unchanged layer.
Independent fresh canaries and recovery safeguards are still required.

General namespace completion, actual new plaintext-to-CE migration, affected
full-workload repeats, Android9/10 regressions and eventual phone/delivery
checks remain open. Both main guest API32 support flags remainfalse; no
production, publication or physical-phone claim is made.


### Descriptor-based filesystem metadata, image51 (VM73)

Image50 run `81b7178a9147444fbc5bdb97c4b76b5e` passes the real ordinary-app
relative dirfd parent, symlink-before-parent, private symlink denial and
trailing-slash checks. It fails the next combined poll/fstatfs stage. That
frozen probe does not expose the exact failing assertion or both filesystem
types, so it is not evidence of a separate poll pass. Raw report and audit:
`out/api32/namespace-routing50-app-result`. All15 service lifetimes and13
adopted descendants are reaped;12 descendants receive native shutdown
signals, with zero remaining/query errors.185 samples peak29; both
observers terminate without errors and the terminal guard confirms no guest
owner, unchanged policy/media grants and preserved CE3 APK. The guest CE
marker absence is also present in the terminal report.

The directory carrier's kernel socket filesystem is not the emulated volume.
Version2 STATFS now carries one actual directory/O_PATH descriptor by
SCM_RIGHTS, with no client-provided UID or inode. The broker authenticates the
caller and registered capability, asks the live provider for FUSE_STATFS and
returns its fields through the existing validated statfs decoder. Regular
AppFuse proxy version1 queries are retained. A real kernel check confirms
that valid O_PATH directory descriptors support fstatfs; inventing EBADF for
metadata-only handles would be incorrect.

The real-file-backed FUSE peer now supplies actual fstatvfs values and full
kernel timestamps. Its broker fixture uses the real lower-anchor setup,
checks ordinary and O_PATH directory/file descriptors, forged/extra rights,
current-caller errors and a bad output buffer, while retaining all cursor,
namespace, IPC and reference-lifetime checks. A held, proved descriptor keeps
fstat metadata authority across a UID change; a new unauthorized pathname
lookup is still denied. The fixture asserts both distinctions explicitly.
`out/api32/external-namespace-rpc51-host` prints DIRECTORY_RPC_PASS and
EXTERNAL_NAMESPACE_RPC_PASS. The regular metadata decoder regression
`out/api32/external-runtime51-host` also passes. Exact commands and scope are
in `out/api32/namespace-statfs51-package-check.json`.

The ordinary JNI probe keeps all nine groups and adds failureLine plus
file/directory filesystem type and block-size diagnostics. Image0.78.51
builds with2650 entries; only the core and loader differ from50. SHA256:
`e12a54e782e7cbec01c474035671c58e73ceb09ba3a487a7fb77d17a375b620c`.
The test Manager APK is5df28eb18be08f884230b90bb29ac1c7cfcad8daa8d008a81bdb5d1bd8ac9b4a;
production Java/five host JNI libraries remain unchanged. Source snapshot:
`out/api32/namespace-statfs51-app-source-snapshot` (768 files).

SSH reset during the first APK upload. The existing candidate pin, stopped
VM state, profile APKs and partial staging files were reauthenticated before
resuming immutable transfers; no guest was restarted to recover the upload.
The resumed stage is complete, and a new exact VM73 job is running:
`out/api32/namespace-statfs51-app-job.json`. Poll with
`python3 out/api32/poll_namespace_statfs51_app.py`; runtime acceptance is
pending. No code/data/profile reset or credentials change was performed.
General namespace/cwd/proc and ownership/transform coverage, actual new
plaintext-to-CE migration, affected full workload/legacy repeats and eventual
phone/delivery checks remain open. Both production support flags remainfalse.


### Proc FD directory descendants, image52 (VM73)

Image51 run `7af62a29c06441bea6d2cec2542114bf` passes five complete native
groups, including real polling and matching file/directory FUSE metadata
(type0x65735546, block size4096). FailureLine102 then identifies the exact
next error: opening `/proc/self/fd/<held-directory>/canary` returns ENOTDIR.
Before that line, duplicate flags, actual directory rename, relative child
open/inode preservation and exact proc readlink already succeed. Those
partial facts are retained separately from the incomplete group. The raw
report/audit is `out/api32/namespace-statfs51-app-result`;139 samples peak29,
15 native service lifetimes and13 adopted descendants reaped, zero remaining
or query errors, no ANR/fatal. The terminal guard confirms unchanged host
policy/media permissions and the retained CE3 APK.

Own process/current-task proc directory descendants now become relative
namespace operations carrying the actual registered SCM_RIGHTS directory
proof. Both operands of rename/link parsing use that mechanism. A held
inode is used directly after rename; no old pathname or provider inode is
accepted from the app. Exact proc readlink behavior remains in its existing
path. For nonlink descendants, readlink resolves provider existence/privacy
before returning EINVAL; missing paths retain ENOENT. Other process FD paths
are not reinterpreted as this process's table.

The real broker host fixture now exercises self, thread-self, numeric
process and actual worker-TID descendant forms, relative mkdir/rename/remove,
held access after rename, nonlink readlink errors and foreign-process
non-aliasing, while keeping all filesystem/FD/cursor/lifetime checks.
`out/api32/external-namespace-rpc52-host` prints DIRECTORY_RPC_PASS and
EXTERNAL_NAMESPACE_RPC_PASS. Exact commands/results and scope are in
`out/api32/namespace-proc52-package-check.json`.

Image0.78.52 builds with2650 entries. Only `system/bin/pocket-loader` differs
from51: the entire native core is byte-for-byte unchanged. SHA256:
`618c34fc57e94c9ed8d11a1e867ffe718bfffd5c848f48c97b107a07d9859d8f`.
The test Manager APK isfe346b6d0468c97c04f35f1f0513c1a510aad8c79dad781e50fbf6d90a556bda;
production Java and the five host JNI libraries remain unchanged. The
768-file snapshot is `out/api32/namespace-proc52-app-source-snapshot`.
The same nine-group ordinary JNI fixture and complete mounted provider,
private40/network/Home-reopen requirements are retained. The new VM73
job/result remain unverified until terminal collection and acceptance.

This change does not establish complete procfs/exact FD reopen or cwd,
unlinked-handle, ownership/transform semantics. Actual plaintext-to-CE,
affected full workload and legacy repeats, eventual phone and delivery
checks remain open. No profile/data reset, root operation, policy relaxation,
publication or production-support flag change was performed.


### Directory cwd search, image53 (VM73)

Actual image52 run `687f7b5fe7774d2a948b0bf7ee606603` passes six complete
native namespace groups. This includes the formerly failing proc FD child
open, after directory duplication and rename. The next failure is the
combined fchdir/getcwd check at source line109: errno20 (ENOTDIR). Raw FAIL
and independent scope audit are retained in
`out/api32/namespace-proc52-app-result`. All own canaries were removed;
15 services and13 adopted descendants were reaped with zero remaining or
query errors. The167 process samples peak at30; six AMS samples have no
observer errors. Terminal verification confirms no guest owner remains,
Enforcing/default32 unchanged, host camera/microphone grants denied and
the CE3 APK preserved. This is not a full namespace or Home/reopen pass.

Image53 adds a directory-only search handoff. The broker authenticates the
actual caller and registered descriptor, resolves its live lower anchor
back to the provider and checks X_OK before duplicating an O_PATH directory.
The calling process performs kernel fchdir and closes that temporary FD;
broker workers never change cwd. Kernel cwd lifetime therefore survives
descriptor close/fork without a cached string or hidden global carrier.
Path chdir uses the same authorization. getcwd translates the backing prefix
only in the enabled external namespace; other visible-path behavior stays
unchanged. No lower regular-file data FD is exposed by this operation.

Focused host tests pass for live cwd, held rename, inherited cwd after
fork/descriptor close, O_PATH directory versus regular-file types,
execute/current-caller denial without cwd change, malformed and forged
proofs, balanced references and zero descriptor leaks. Existing loader
protocol and visible-path regressions pass. ARM64 image/Manager builds and
image-free APK validation pass; exact commands and source/package digests
are in `out/api32/namespace-cwd53-package-check.json`. Only the guest core
and loader differ from52. Production Java, the five host JNI libraries and
the full nine-group ordinary JNI fixture are unchanged.

The full VM73 mounted retest is live under supervisor24469, token
`d119abe0112447baae0c6b530215de7e`; the exact job and latest live observation
are retained in `out/api32/namespace-cwd53-app-job.json` and
`out/api32/namespace-cwd53-live-state.json`. Deleted-directory cwd search
is still unimplemented: the live provider cannot authorize a removed path
with its current pathname-based access check. The original strict check
remains required. Exact proc descriptor reopen, ownership/transforms, actual
plaintext-to-CE migration, affected full/legacy repeats and eventual phone
and delivery gates also remain open. Both production support flags arefalse.


### Removed private directory cwd, image54 (VM73)

Image53 run `bfdab3ee68924ec1b7d5e6afbe088be3` passes eight complete native
namespace groups, adding cwd-relative operations and retained file access
after unlink. The remaining failure is source line126, fchdir on a removed
directory, returning ENOENT. Its raw FAIL and independent audit remain in
`out/api32/namespace-cwd53-app-result`. All own canaries were removed,
15 services and14 adopted descendants reaped (one signalled), zero remaining
or query errors. The180 process samples peak at29; six AMS samples have no
observer errors. Terminal verification proves no live guest owner remains,
with Enforcing/default32, denied host media grants and CE3 APK preserved.

The provider now retains an inode/type-checked O_PATH anchor for a selected
package-private directory node, including metadata-only LOOKUP references.
Search of a removed directory uses that actual inode and current execute
permissions, after the existing native and Java MediaProvider UID/package
policy. It does not reopen a removed name or inherit authority from the
original opener. The anchor closes with the final provider node reference.
Live paths and public directories keep their provider access behavior. The
broker sends ACCESS on the authenticated held provider node for removed
directories, avoiding an impossible pathname-based GETATTR first.

Host tests pass for normal and O_PATH removed-directory fchdir, kernel
getcwd ENOENT, changed caller denial, actual execute revocation, rename and
replacement-inode protection, balanced provider references and zero FD leaks.
The standalone anchor test exercises filesystem mechanics; it does not prove
MediaProvider package policy. The actual mounted trial must establish that
policy. All original nine native app checks remain, and the ordinary JNI
fixture additionally requires O_PATH directory fchdir/getcwd after removal.

Image0.78.54 and the Manager build and validate. Exact commands, source and
package checks are in `out/api32/namespace-deleted54-package-check.json`.
Only the guest core and the MediaProvider APK differ from53; within that APK
only libfuse_jni.so changes, preserving the original signing certificate and
all Java/resources/C libfuse. Loader, production Manager Java and five host JNI
libraries are unchanged. The guarded VM73 full mounted trial is live under
supervisor29480, token `acb2532c63cf4c849ca9fa10c99d0380`; the exact job and
latest process observation are in `out/api32/namespace-deleted54-app-job.json`
and `out/api32/namespace-deleted54-live-state.json`.

Exact proc descriptor reopen, broader ownership/transforms, actual retained
plaintext-to-CE migration, affected full/legacy repeats, eventual phone and
delivery remain open. Both production support flags remainfalse. No profile
reset, root operation, host policy relaxation or publication was performed.


### Full namespace pass and exact proc FD metadata, image55 (VM73)

Image54 run `7560a2eaff064150a8ac3c5cab4c96b8` passes the complete original
nine-group ordinary namespace workload, including retained normal and O_PATH
directory cwd after removal. Mounted provider operations, the40 private-path
denials, network/DNS/fwmark recovery and actual Home/reopen also pass. Native
work executes once per process/phase: the reopened observation retains
PID19730, phase initial, iteration1, so it is not counted as a second native
execution. All own canaries were removed;15 services and13 adopted descendants
were reaped, zero remaining/query errors. The192 process samples peak at31
and seven AMS samples have no observer errors; no ANR/fatal occurred. The
independent scope check and terminal guard are retained in
`out/api32/namespace-deleted54-app-result/independent-check.json` and
`out/api32/namespace-deleted54-terminal-state.json`. Host Enforcing/default32,
denied media grants, retained profiles/data/keys and the CE3 APK are preserved.

Exact own process/task proc FD following stat and directory chdir still
exposed the socket carrier to the kernel. Loader55 sends actual descriptor
proof through existing fstat/search authorization instead. Nofollow stat
keeps the real proc symlink. Host tests cover self/thread/numeric forms,
invalid flags and buffers, regular proxies, normal/O_PATH removed directory
metadata and cwd, and retained nofollow behavior. The ordinary app retains
all nine checks and adds proc stat/type/inode/device checks, lstat symlink,
live and deleted proc directory cwd plus deleted regular-file metadata and
ENOTDIR chdir.

Image55, the native fixture and Manager build and validate; exact commands
and source/package checks are in `out/api32/namespace-procstat55-package-check.json`.
Only the loader differs in the image: core and original-certificate provider
are byte-identical to54. Production Java and five host JNI libraries remain
unchanged. The full mounted retest is live under supervisor2400, token
`7641706300fd48249ba88c970be7692b`; its exact job and live observations are
`out/api32/namespace-procstat55-app-job.json` and
`out/api32/namespace-procstat55-live-state.json`. This change does not
implement exact proc FD reopen or establish complete procfs/ownership/transform
semantics. Actual retained plaintext-to-CE, affected current full/legacy
repeats, eventual phone and delivery remain open. Both production support
flags remain false.


### Exact proc FD reopen, image56 (VM73)

Image55 run `97ea7e78c8ed4f5db392591e948537a5` passes the full original
nine-group namespace workload with the added exact proc following-stat and
chdir checks. Mounted provider/private40/network/DNS/fwmark and actual
Home/reopen pass. The independent check and terminal guard are in
`out/api32/namespace-procstat55-app-result/independent-check.json` and
`out/api32/namespace-procstat55-terminal-state.json`. All own canaries were
removed,15 services and13 adopted descendants reaped, zero remaining/query
errors,173 samples peak29 and seven AMS samples without errors. No ANR/fatal
occurred. Native work remains once per process/phase; cached reopen proof
is not counted as a second native execution.

A distinct version2 FD_REOPEN operation now authenticates the current caller
and actual registered source descriptor. A following exact proc FD open gets
a new provider handle and directory cursor, with requested close-on-exec
behavior. O_PATH cloning carries only existing metadata authority; a data
reopen still requires current provider authorization. Live data opens refresh
provider lookup/OPEN, verify the held inode before optional truncation, and
never create or truncate a replacement of an old name. Existing proc
nofollow/exclusive behavior remains with the kernel.

The selected private provider view retains O_PATH anchors for regular files
as well as directories, including LOOKUP-only references. Removed private
files reopen inside the provider after its current UID/package checks and
real kernel access checks. Removed private directories get new independent
DIR handles and empty enumeration. No lower regular-file FD is returned to
an app. Public media retains its ordinary OnFileOpen redaction/transform
path; the private fallback rejects transforms. Actual opened private inode
identity is checked for live and removed opens. The original media signing
certificate and Java/resources/C libfuse payloads remain unchanged.

Focused host tests pass for independent directory descriptions, close-on-exec,
metadata versus data authority, changed caller denial, real read/execute
revocation, rename, retained file bytes after unlink, replacement-inode
protection, empty removed directories and balanced references/FDs. The
80-case Linux reference is `out/api32/proc-reopen55-kernel-reference.json`;
it is a filesystem reference, not Android package-policy proof. The expanded
ordinary JNI app keeps all nine original groups and requires live and removed
file/metadata/directory reopen, independent offsets/cursors, inode identity,
empty enumeration and cwd lifetime; its explicit procFdReopenVerified must
be true. The provider and app build, and ARM64 image56 builds. Manager
packaging/validation pass. The full VM73 mounted trial passed as run
`c79e150f68284e27b02010aaefdb8f4a`: all nine original groups plus the explicit
reopen proof, private40, network/DNS/fwmark and Home/reopen. The private
provider logged the removed-file handle path. All canaries were removed;
15 services and13 adopted descendants were reaped, with zero remaining or
query errors. There were162 process samples with peak29 and seven AMS samples.
The independent check and terminal no-owner guard are retained in
`out/api32/namespace-reopen56-app-result/independent-check.json` and
`out/api32/namespace-reopen56-terminal-state.json`. Native work runs once per
process/phase; Home proof reuse is not a second native execution. Exact commands
and package checks are in `out/api32/namespace-reopen56-package-check.json`. The initial native
prototype and its input snapshot are preserved separately; the selected
provider is `out/api32/mediaprovider-reopen56-checked-native`.

Broader ownership/transform coverage, actual retained plaintext-to-CE,
affected current full and legacy repeats, eventual phone and delivery gates
remain open. Enforcing/default32, denied host media grants and retained
profiles/data/keys are preserved. Both production support flags remain false.


### Full audio/camera repeat on image56

The unchanged ordinary AV APK from the passing48 workload is running against
the exact cached image56 and all770 source pins validated by namespace56. This
retains both all-camera rounds, Camera2 YUV/JPEG, microphone AAC plus Surface
AVC MP4 recording, actual decoded frames/PCM/timestamps/EOS, playback/capture,
Home/reopen and the typed source-built installd data/permission/dexopt checks.
The original app build sources remain in the48 snapshot; it is not presented
as a fresh rebuild with the later generic packaging helper. The new Manager
package preserves production Java and all five JNI libraries.

The exact supervisor and observers are recorded under
`out/api32/av-repeat56-app-*-job.json`; the result is pending. The image is
already cached, so historical install statistics do not prove a new download
or upgrade in this trial. Enforcing/default32 and complete process accounting
remain required, with host media grants restored after strict shutdown.
