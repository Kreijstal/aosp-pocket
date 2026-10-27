Android 10 guest port
====================

The API 29 port is experimentally supported and validated through Pocket
Manager in the Pi's dedicated Android VM. It downloads its image, boots Android
10, displays the launcher, installs and runs an APK, delivers touch input and
connects over HTTPS. It also restarts offline and reopens the installed app.
Android 9 and Android 10 have separate data directories. The host APK target
SDK is unchanged.

The separate non-debuggable package `dev.kreijstal.aosppocket.api29test` has
passed the following checks in the Pi's outer Cuttlefish Android VM under
host UID 10185. It uses its own private root and data directory.

| Component | Verified behavior |
| --- | --- |
| Android 10 image | Pinned OCI layer digest, SDK 29, flattened runtime APEX paths |
| Loader and Bionic | Real Android 10 shell and native executables run without root |
| Properties | Actual API 29 Bionic property area, read-only rejection, serial-change wakeup |
| ART | Real Android 10 ART loads the API 29 `BlendMode` framework class |
| Binder | Two-process registry, FD transfer, caller UID/PID, WorkSource read/replace/propagation |
| Init | Matching AOSP parser/action/service core, imports, triggers, sockets, UID/GID inheritance, crash restart, shutdown |
| Zygote | Matching AOSP fork JNI and Java server; a Java child sees SDK 29 and UID/GID 39999; parent remains responsive |
| HIDL | Android 10 manager 1.2 and Health 2.0 transport between distinct processes; sorted manifest listing and absent-service handling |
| Suspend HAL | Real host Android CPU wake-lock acquire/release through app-owned IPC, including duplicate release |
| Product properties | Android 10 partition-property source order and derived, read-only build fingerprint |
| Software graphics | Redroid gralloc allocation/lock/free, real EGL/GLES context and exact RGBA readback through SwiftShader |
| Display startup | Android 10 SurfaceFlinger, Pocket composer and framework detection of the 480×800, 30 Hz display |
| Flattened APEX | Six actual manifest-verified modules returned through the API 29 service; staged updates rejected |
| Storage protocol | Checkpoint capability queries, nested maintenance callback delivery and denial for ordinary guest roles |
| Network protocol | Netd/DNS-resolver version 2, LMKD query framing and rejected host VPN bypass |
| Framework boot | `sys.boot_completed=1`; activity, package, power, display, window and input services respond |
| Guest UI | Launcher screenshot, ordinary APK installation, app drawing and injected tap delivered to its button |
| Shared memory | Stock Android 10 libcutils uses memfd; readback and denied future writable mappings verified |
| Manager | Actual GuestService/DirectSession, 720×1280 shared compositor frames and host surface presentation |
| On-demand image | One HTTP download in the isolated test; per-file checks, offline restart and preserved Android 9 marker |
| Internet | Guest reports a connected network and completes an HTTPS request with status 200 |

The complete runtime report is [api29-native-validation.json](api29-native-validation.json).
Graphics and display evidence is [api29-graphics-validation.json](api29-graphics-validation.json);
that report also records the subsequent framework failure rather than claiming
a complete boot.
The later [framework boot report](api29-framework-boot-validation.json) records
successful framework startup and the subsequent black-screen UI test failure.
The [UI runtime report](api29-ui-validation.json) records the successful isolated
boot and UI test. Its `fullGuestBootVerified` field is true; this does not certify
production Manager integration or host camera/audio forwarding. The guest's
logical identities remain metadata inside the real host app sandbox. No host
capabilities, SELinux transitions, kernel mounts or namespace changes are used.

The reference is Redroid's Android 10 arm64 platform. The OCI layer digest and
the matching AOSP `android-10.0.0_r47` source commits are pinned in
`sources.lock.json`. `prepare_api29.py` verifies the layer, resolves archive
links to regular private payloads and materializes canonical `/apex` paths.
`prepare_sources()` fetches only the required pinned source checkouts and
rejects local modifications. Source adaptations are separate API 29 patches;
the pinned checkouts remain unchanged.

Android 10's Binder request header contains a WorkSource UID. The guest bridge
preserves it alongside the guest credential header and retains Parcel's
header-position bookkeeping. The parser continues to accept the API 28 and
newer host template formats. The shared native property, Binder, init and HIDL
sources also build successfully against the API 28 libraries in separate
regression output directories.

The Android 10 bootstrap is appended to the zygote's `BOOTCLASSPATH`. Android
10 did not make classes loaded by the previous DexFile bootstrap discoverable
through the boot loader. The port uses the matching stock ART with a normal
boot classpath extension. No JVMTI agent or ART method-offset patch is needed.

Build and validate locally from the repository root:

```sh
make -f runtime/Makefile api29-check
make -f runtime/Makefile api29-probe
```

The probe builder needs NDK r27c, the existing Java/manifest/signing toolchain,
the host `libpocket_runtime.so`, and the host HIDL generator. The current HIDL
generator path is `../s20fe-hardware-lab/artifacts/lineage/source/prebuilts/build-tools/linux-x86/bin/hidl-gen`.
The APK bundles a test dependency closure for this isolated fixture; it is
not the planned downloadable production image.

Run the fixture on the Pi's dedicated outer VM:

```sh
rsync -q out/api29/art-probe/api29-probe.apk pi@juanraspberry:/home/pi/api29-art-probe.apk
rsync -q tools/test_api29_on_pi.py pi@juanraspberry:/home/pi/test_api29_on_pi.py
ssh pi@juanraspberry 'python3 /home/pi/test_api29_on_pi.py --apk /home/pi/api29-art-probe.apk --art --properties --binder --init --zygote --hardware'
```

The test requires the virtual-device guard, installs only the separate fixture
package, records failures as failures, and keeps the full report on the Pi.
The working Android 9 Pocket profile is not migrated or replaced.

The full framework attempt uses `--system-server` and a separate build output
such as `out/api29/system-server-probe`. It completes Android 10's real
`SystemServer.main()` startup and reaches the boot-completed property. With
`--ui`, it also verifies launcher rendering, APK installation and delivered input.
The port supplies the matching suspend HAL/control interface
and restores all exports from the reference's `init.environ.rc` (including
`ASEC_MOUNTPOINT`, required by `installd`). Suspend wake locks call the host
app's `PowerManager`; host-wide auto-suspend/forced-suspend operations report
unsupported. Manager connects the API 29 relay and declares the normal host
WAKE_LOCK permission.

Software HALs use Android 10's SP-HAL loader. This passthrough profile adds
only the composer/allocator interface libraries and FMQ support to the SP-HAL
namespace's framework links. The image builder preserves separate framework,
VNDK, VNDK-SP and APEX dependency copies. SurfaceFlinger now initializes the
real display. The read-only service for the six pinned, flattened APEX modules
has passed runtime validation, with staged updates explicitly unsupported.
The framework fixture now stages the stock system applications,
launcher, System UI and keyboard for further boot validation.

The API 29 network adapter publishes guest UID policy to the loader's private
policy file. Local real-socket tests cover chain intersection, existing
connections, IPv6, direct syscalls and malformed policy. Filtering covers
outbound connections and writes; it does not provide kernel namespace or
receive-side firewall isolation. The dedicated outer VM's Restricted
Networking Mode must allow the fixture during real socket tests: that host
policy applies even when its INTERNET permission is granted. Validation must
restore any temporary outer VM setting change afterward.

The fixture caches unchanged signed APK assets by CRC and size, cleans up only
its own private-root executables before and after a run, and preserves native
logs on timeout. Framework checks use the guest Binder adapter and stop at the
first fatal system-server exception. The outer test permits 420 seconds for
extraction and the native watchdog; the actual framework wait remains bounded.

For a startup-configuration-only iteration, `--reuse-native` repackages existing
API 29 native outputs while rebuilding host Java and the APK. Native source
changes require their focused native rebuild first. The full `api29-probe`
Make target always rebuilds native components.

`build_api29_image.py` packages regular immutable files under `system`, `vendor`
and `apex`, replaces the test init configuration with Manager startup, and
excludes test APKs, guest data and native test executables. The image's full
manifest and archive hashes are pinned in the host APK catalog.
`build_api29_manager.py` builds an image-free host APK with separate API 28/29
catalog entries and keeps the established host JNI libraries unchanged.
Android 10 uses `api29-native-rootfs`; Android 9 retains `api28-native-rootfs`.

The [Manager validation](api29-manager-validation.json) uses its actual GuestService, DirectSession, shared
compositor frames, input channel and APK import path. It also checks HTTPS and
restarts the guest after shutting down the image download server. It passed,
with exactly one image HTTP request and a preserved Android 9 data marker.
The VM's Restricted Networking Mode was restored afterward. Its existing
lock-screen credentials were unchanged; the test activity uses the normal
show-when-locked API to keep UI validation active.

The 0.77 release candidate contains a 1,697,416-byte image-free Manager APK and
a separate 356,931,145-byte Android 10 image (1,912 immutable files). Release
artifacts, checksum list and a source overlay are in `out/api29/release`.
The experimental release is hosted at
https://github.com/Kreijstal/lineage-ur/releases/tag/pocket-api29-0.77.0.
Build its image-free host APK with `python3 tools/build_api29_manager.py
--catalog runtime/images/api29-arm64.json`. The current profile uses
SwiftShader, 64-bit guest userspace and software keymaster. Host camera/audio
forwarding and a WebView provider are not covered by this profile's validation.
