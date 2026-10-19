# Experimental Android guest runtime

The original bootstrap APK executes AOSP Toybox. The separate runtime APK is
the implemented next step: it hosts Ananbox's modified Anbox graphics/input
backend and PRoot, and imports an AOSP guest filesystem. A guest has reached
its launcher on the Pi-hosted Android 16 VM, with guest Android 7.1.1 and
`sys.boot_completed=1`. A black Surface or a running `init` alone is not a
successful framework boot.

## Sources

- Anbox backend: `vendor/anbox`, pinned to
  `fbe569a8a3a42cfdf7276422ba5da128cfb2e124` from
  <https://github.com/Ananbox/anbox>.
- PRoot: `vendor/proot`, pinned to
  `8c60d0287ce9ece3f21df1dc60de55e33f6bdba3` from
  <https://github.com/Ananbox/proot>.
- Host reference investigated:
  <https://github.com/Ananbox/ananbox>, commit
  `e65bec867cc1b56fd424c740fbda5bf31d17bc43`, plus its v0.0.2 APK.
  Our host UI, Java bridge and JNI adapter are built from the sources in
  `runtime/`; the reference APK is not repackaged into our app.
- Guest source manifest:
  <https://github.com/Ananbox/platform_manifests/tree/anbox> selects AOSP
  `android-7.1.1_r13` with guest framework, Binder and hardware adaptations.
- Current test filesystem: the upstream v0.0.2 `rootfs_arm64.7z` release,
  recorded in `sources.lock.json`. **This guest image has not been built
  locally from source.** VMOS images and libraries are not used.

A guest launcher, separate test APK installation/rendering and forwarded touch
have been verified. Updating the Android guest, adding a convenient APK-install
flow and implementing hardware bridges follow this baseline. The separate APK
was installed through the developer test transport, not an integrated app-store
UI. This old guest does not establish compatibility with modern apps.

## Build

Prerequisites: Linux x86-64, JDK, Python 3.12+, GNU Make, patch, CMake, Ninja,
host clang/clang++, Android NDK r27c and 7-Zip. The Java APK builder comes from the
same `TOOLCHAIN` as the bootstrap. D8 desugars Java lambdas; `R8` defaults to the
existing LineageOS checkout's **read-only** `prebuilts/r8/r8.jar`. Override it
with another D8/R8 distribution when building elsewhere.

```sh
git submodule update --init
make -f runtime/Makefile native
make -f runtime/Makefile -j2 verify
# Optional overrides:
make -f runtime/Makefile verify NDK=/path/to/ndk TOOLCHAIN=/path/to/androidworld R8=/path/to/r8.jar
```

`tools/build_runtime_native.py` downloads the pinned Boost 1.83.0 and talloc
2.4.1 sources. It cross-builds the dependencies, applies the local PRoot patch
to a build copy, then builds the Anbox backend and JNI adapter. No vendored
source, existing ROM checkout or existing build output is changed.

The build also downloads pinned guest platform headers and, if necessary, the
reference guest image. `tools/build_guest_binder.py` builds only the guest Binder
bootstrap fix against that image's existing library ABI with `--no-undefined`.
It does not rebuild the guest framework. Its generated C++ configuration uses
the guest's `std::__1` namespace and leaves the NDK untouched.

Output: `out/runtime/aosp-pocket-runtime.apk`. Native libraries have 16 KiB
LOAD alignment; C++ and Boost are linked statically. The APK includes the
guest launch script, guest Binder fix, licensing notices and, since
`0.3-bundled`, the pinned AOSP 7.1.1 ARM64 reference guest filesystem.
The full APK is approximately 267 MiB. The bundled image is an upstream
prebuilt, not a locally built AOSP ROM. The build prepares the pinned image
automatically; `BUNDLED_GUEST=/path/to/compatible-pocket.zip` overrides it.

The experimental host targets API 28 because this guest bootstrap uses hidden
ActivityManager/Parcel interfaces. Its private Parcel layout and broadcast
transaction signature are checked and an unsupported layout fails visibly.
This is a compatibility constraint to remove, not a modern stable public API.

## First run and optional import

Install the full APK and tap **Start** in Pocket Manager, or open the
**Pocket Android** launcher icon. If no guest is installed, the service extracts
the bundled image directly from the installed APK, displays progress and boots
it. No separate image file, network download or storage permission is needed.
The extracted guest is about 594 MiB in addition to the APK. Setup continues
when UI windows close; **Stop** cancels it, removes partial files and permits
retry. An upgrade or repeated Start never replaces an existing guest.

Guest file assets carry a `.payload` suffix inside the APK and regain their
original names during extraction. Android's APK installer scans `.bc` entries
even inside assets and selects a 32-bit host ABI for legacy RenderScript;
exposing the guest's `.bc` names caused `INSTALL_FAILED_NO_MATCHING_ABIS`.
The APK validator checks this packaging constraint and the complete image
manifest. Extraction does not change guest file contents.

Manual **Import guest** remains available before installing a guest. It accepts
the compatible Pocket ZIP format; arbitrary VMOS or stock AOSP images do not
become compatible just by being zipped. Updating/replacing an installed guest
is not implemented yet.

To prepare a manual import, extract a trusted ARM64 Ananbox rootfs and convert it:


```sh
python3 tools/pack_guest_image.py /path/to/rootfs out/aosp-pocket-guest-arm64.zip
```

Some desktop 7-Zip builds refuse `sbin/ueventd` and `sbin/watchdogd` links to
`../init`, leaving empty files. Check the extraction exit status. In this
specific upstream image, restore those two links before conversion. The
converter preserves file modes and symlink targets in a versioned layout
manifest, and rejects non-file/directory/symlink nodes.

Put the resulting ZIP in Downloads and use **Import
guest**. Import writes regular files before symlinks, checks paths and duplicate
entries, bounds expanded data to 2 GiB, and activates the completed import by
rename. Only use trusted guest images: they contain executable programs, and
PRoot is not a security boundary. Updating/replacing an installed guest is not
implemented yet.

Tap **Start** after a manual import. Guest bootstrap no longer requests coarse
location: the Binder broadcast now uses `OP_NONE` rather than accidentally
checking `COARSE_LOCATION`. The host does not implement or forward GPS here.
**Stop** kills the tracked PRoot process; kernel EXITKILL cleans up its tracees
without relying on guest shutdown handlers. There is no `killall init`, host
security change, or ADB pairing flow. ADB/root can be used by an external test
harness to inspect the
app's own files, but the app runs as an ordinary untrusted-app UID.

## Manager, Android window and floating mode

Runtime version `0.2-windows` has two launcher entries from one APK:

- **Pocket Manager** imports/starts/stops the guest and shows a small,
  interactive live preview. **Open Android ↗** opens its separate screen;
  **Close manager** removes only the manager task.
- **Pocket Android** opens the same running guest in its own full-size task
  and Recents card, with its own label/icon. **Manager** switches tasks;
  **Close window** removes only this screen. Opening it starts Android if
  needed.

**Float** puts the Android screen into Android's picture-in-picture mode.
The system provides the floating window's move/resize/expand controls. PiP
touches operate those system controls; expand the window to interact with
Android, or use the manager's interactive preview. This uses the
[platform PiP API](https://developer.android.com/develop/ui/views/picture-in-picture),
without an overlay permission. PiP availability depends on the host.

`GuestService` owns the tracer, Binder receiver and boot monitoring. Its
foreground notification opens the Android screen or stops Android. Closing
either or both UI tasks leaves a running guest in that service. **Stop** in
the manager/notification terminates the guest. Host process death still
terminates its traced children; this does not provide persistence across an
Android process kill or host reboot.

The logical display dimensions come from the host's first display profile,
with space reserved for the viewer controls, and are saved app-privately.
Switching windows does not resize/reboot Android. Each Surface uses that fixed
buffer size, scales with letterboxing, and maps touch back to guest pixels.

The runtime's manifest adapter sorts binary XML attributes by resource ID,
as Android's styled-attribute reader requires. Keeping source order made the
package manager silently skip the viewer's configuration/theme/orientation
settings, recreating it during PiP entry. APK validation now checks attribute
ordering and both activities' configuration masks in the encoded manifest.

The test VM also had a stale SystemUI PiP display cache: 1280×720 at 160 dpi,
although its current window-manager override was 720×1280 at 240 dpi. This
placed PiP outside the visible screen. Restarting only the outer SystemUI
refreshed the cache; the Pocket host and guest init PIDs stayed unchanged.
The window regression checks PiP bounds against the actual host display size,
so an off-screen floating window cannot count as a pass. The APK does not
restart SystemUI or change host display settings.
Refreshing SystemUI can restore the host's lock screen; unlock the host
normally before continuing interactive validation.

The generated Binder bootstrap broadcast now uses `appOp=-1` (`OP_NONE`).
The previous zero accidentally requested `COARSE_LOCATION` app-op checks,
which Android denied after a background transition and could leave newly
started guest processes waiting for their service-manager handoff. The
broadcast remains package-targeted and its receiver remains non-exported.
A shared native compositor presents the same guest layers to the active
surfaces and caches its latest layers for attaching an idle guest immediately.

## Android native-process limit

Android 12+ hosts may trim an ordinary app's native children once the system's
phantom-process cap is reached. On this test VM the default cap was 32. Its
ActivityManager log explicitly recorded `Trimming phantom processes` killing
our PRoot, guest init, logd and service manager. The rest of the guest then
malfunctioned because translation was gone. This was a host resource limit,
not a nested-KVM restriction.

For this dedicated test VM only, `activity_manager/max_phantom_processes` was
raised to **256**. The previous setting was `null` (no explicit override),
recorded in `out/pi-phantom-limit-before.json` and on the Pi at
`/home/pi/pocket-phantom-limit-before.json`. The effective limit was checked
with `dumpsys activity settings`.

When a physical host needs the same resource allowance, configure it explicitly
from a shell/ADB session; the app does not set it or pair ADB automatically:

```sh
adb -s HOST_SERIAL shell device_config put activity_manager max_phantom_processes 256
# After stopping the guest, restore the default (the test VM's original state):
adb -s HOST_SERIAL shell device_config delete activity_manager max_phantom_processes
```

A host that already permits enough native children may not need this step.
The local PRoot patch also enables `PTRACE_O_EXITKILL`: if the tracer dies,
the kernel kills its traced guests instead of letting them continue without
filesystem/identity translation. This requires a kernel supporting EXITKILL.
The app also passes its PID to PRoot, which checks its parent and installs
`PR_SET_PDEATHSIG=SIGKILL` before launching any guest. Terminating the host app
process therefore terminates its tracer and traced children. UI window closure
only detaches that display from the foreground service.
The main VM's ADB server, security policy and networking are not changed.

## Runtime path

The host creates a Surface-backed Anbox/emugl renderer, a private `qemu_pipe`
socket, and an Anbox touchscreen event endpoint. The guest's adapted graphics,
input and Binder code consumes those interfaces. Touch forwards pointer IDs,
down/up, movement and cancellation; this is not a remote VNC connection.

The guest service-manager bootstrap uses the upstream Binder-handoff broadcast
protocol. Generated templates target only our host package; the receiver is
not exported to other app UIDs on the tested Android 16 host (the explicit
non-exported registration requires API 33+). Earlier Android hosts have not
been validated. No guest context manager is registered in the
host's global service manager. This still shares the host Binder driver and
app UID; complete service isolation has not been established.

The runtime prepares guest paths, starts its own immutable PRoot executable
from the APK and executes guest `/init`. All guest identity/capability changes
are emulated; guest root is not host root. `/dev`, `/proc` and `/sys` are bound
for compatibility and are filtered only by the host app's ordinary access
controls. Hardware access/forwarding is not implemented here.

## Android host compatibility findings

On the existing Android 16 ARM64 LineageOS Cuttlefish VM, the unmodified v0.0.2
reference imported its image, started guest `init`, `servicemanager`, and
`surfaceflinger`, then repeatedly lost zygote to SIGSYS. The PRoot log recorded
`si_syscall: 143` (`setregid`, AArch64). No guest `system_server` was verified.

Enabling `PROOT_NO_SECCOMP=1` exposed another SIGSYS, this time syscall `-1`.
That is PRoot's syscall placeholder for fully emulated operations. The host
app seccomp filter remains enabled throughout these tests.

The local compatibility patch:

- Adds two missing declarations required by current Clang.
- Makes optional diagnostic libunwind backtraces conditional, rather than
  adding a second unwinder solely for crash diagnostics.
- Uses permitted `getpid` as the Android placeholder for fully emulated calls;
  original guest syscall identity remains available to exit handlers.
- Handles that placeholder in guest `brk` emulation and preserves skipped-call
  return values for existing exit handlers.
- Enables kernel EXITKILL so a killed tracer cannot leave untraced guests.
- Restores the default SIGTERM action in the tracer. This Android libcore's
  `Process.destroyForcibly()` delegates to `destroy()`, whose native method
  sends SIGTERM; the upstream PRoot loop ignored that signal. EXITKILL now
  cleans the guest when the app terminates its tracked tracer.
- Keeps guest property-area files owned by virtual root when an app performs
  `fstat`, rather than changing the reported owner to each calling app. No
  host ownership or other guest-file metadata is changed.

`runtime/guest.sh` uses full ptrace syscall interception, so emulated legacy
identity calls can be translated before reaching the host seccomp filter.
This is expected to be slower than PRoot's seccomp-assisted path. The modified
guest did reach zygote and `system_server` while retaining host UID 10170,
`Seccomp: 2` and `u:r:untrusted_app_27` for guest processes.

A second startup deadlock was traced to `HostBinder::getSVMObj()`: its blocking
`handlePolledCommands()` could wait forever after a Binder pool thread had
already received the callback. SurfaceFlinger then never published its service.
`runtime/guest-binder-bootstrap.cpp` overrides only that bootstrap method, using
a Binder-pool receiver and a condition variable. With this fix, SurfaceFlinger
registered and the guest reached its launcher and `sys.boot_completed=1`.

Restart also required clearing the host's previous manager and accepting the
newly published, pinged Binder. `isBinderAlive()` alone retained a cached true
result for the old guest's proxy; the second guest then could not register its
services. The receiver now queues early bootstrap requests until the new
manager arrives and resets that state at each start.

The APK installs this source-built interposer and patches only its private
guest's `init.environ.rc`. An original environment file is preserved. The host clears the old guest property area and boot marker before startup.
A small second PRoot process queries the guest's own `getprop` against that
private property area. The host writes a status marker only after observing
`sys.boot_completed=1` for the current running guest. An earlier init-action
marker experiment did not fire on this guest and was removed. This does not grant host root or change the host filter.

A subsequent app-launch stall was traced to the compiler's sleep loop in
`libcutils`: it waited for `ro.ananbox.host.api_level`. PRoot's generic fake-UID
handling reported property files as owned by the compiler/app UID. Android 7
bionic rejects property areas unless both UID and GID are zero; see the
[upstream ownership check](https://android.googlesource.com/platform/bionic/+/android-7.1.1_r13/libc/bionic/system_properties.cpp).
The local patch preserves virtual root ownership only for descriptors resolving
inside this guest's `/dev/__properties__/` directory. This allows newly exec'd
processes to initialize their own property mappings.

Native backtraces also exposed graphics/connection stalls. The host adaptation
destroys removed connections outside the registry mutex, closes each stream
queue under its own mutex, and releases the global GLES decoder lock during
RenderControl processing. Presentation retains its own window-lifetime guard
while releasing the renderer's resource mutex so other contexts can submit.
For a SwiftShader renderer, presentation uses an EGL pbuffer, reads its rendered
RGBA pixels and posts them through `ANativeWindow`; hardware renderers retain
the EGL window path. This avoids the nested test driver's Vulkan queue-present
wait, at the cost of CPU readback/copying. It is not a performance result for
physical phones.

A repeat boot still stopped in SwiftShader's `CmdWaitEvent`, while readback
waited for a fence. A subsequent press on a host button blocked the UI in
HWUI's `syncAndDrawFrame`. Android preloads hardware rendering before
`onCreate`, so setting the ANGLE environment there was too late for HWUI.
The host controls now explicitly disable hardware acceleration in the manifest;
the guest Surface has its own independent renderer. The build checks the actual
binary manifest attribute, using a local adapter for the shared manifest
encoder without changing that encoder's checkout.

On a host identifying its Vulkan driver as `pastel`, the app sets ANGLE's
process-local `ANGLE_FEATURE_OVERRIDES_DISABLED` before guest EGL initialization,
disabling `useVkEventForImageBarrier` and
`useVkEventForBufferBarrier`. ANGLE then uses its pipeline-barrier fallback.
This changes no system properties or other apps. The subsequent full guest
boot/install/render/touch regression passed; Stop/Start is checked separately
with the optional restart regression below.

The input adaptation also snapshots live connections instead of treating their
monotonically assigned IDs as array indexes. IDs become sparse after disconnect
or guest restart. Input connections now observe EOF, and a disconnect racing
with delivery removes that client instead of crashing the host. Event storage
is released after synchronous delivery.

## Checks run

Runtime `0.3-bundled` passed `make -f runtime/Makefile -j2 verify`, including
signing, the full bundled guest inventory, and the asset naming rule that
prevents Android's RenderScript scanner from forcing a 32-bit host ABI.
The signed full APK is 279,763,561 bytes (about 267 MiB).

`ssh pi@juanraspberry python3 - < tools/validate_bundled_on_pi.py` passed on
2026-10-19. APK installation preserved the prior guest; the harness retained
that guest by rename for recovery before exercising a new setup. Start
installed the bundled image without a separate ZIP, cancellation removed partial
files, retry survived manager closure, and the guest reached boot completion.
Opening the Android window again retained its processes. The guest launcher
was then verified as the resumed activity and captured in
`out/pocket-bundled-launcher.png`.

`ssh pi@juanraspberry python3 - < tools/validate_guest_on_pi.py` also passed
against this freshly extracted image: separate test APK installation/rendering,
a tap forwarded through the outer Surface, property access from a fresh guest
UID, and continued host UID 10170/seccomp mode 2/untrusted-app confinement.
See [runtime-bundled-validation.json](runtime-bundled-validation.json).
The former VM guest remains under
`files/rootfs-pre-bundled-1792404051`; no prior guest data was deleted.
The new APK was copied to Samsung Downloads and its installer opened through
Termux. Installation/first boot on the physical Samsung remain user-driven
and unverified.

`make -f runtime/Makefile -j2 verify`: passed APK signing verification and
`tools/validate_runtime_apk.py` (ARM64, static C++/Boost linkage, 16 KiB LOAD
alignment, licensing assets and scoped launch script).

`python3 tools/build_runtime_native.py --ndk ~/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c`:
passed source checks and the narrow PRoot/Anbox/JNI build.

The APK installed successfully on the existing Pi-hosted VM as UID 10170.
The ZIP integrity/layout check passed for 1,699 entries, including both required
init symlinks. Physical Samsung installation is not yet verified.

```sh
make -f runtime/Makefile -j2 verify smoke
make -f runtime/Makefile check-host
scp out/runtime/guest-test.apk pi@juanraspberry:/home/pi/pocket-guest-test.apk
scp out/runtime/property-probe pi@juanraspberry:/home/pi/pocket-property-probe
ssh pi@juanraspberry python3 - < tools/validate_guest_on_pi.py
# Also exercise Stop/Start in the same app process before installing/testing:
ssh pi@juanraspberry python3 - --restart < tools/validate_guest_on_pi.py
# With Pocket Manager open and the guest running, exercise window transitions:
ssh pi@juanraspberry python3 - < tools/validate_windows_on_pi.py
# First-run setup regression. Preserves the existing guest by rename;
# installs the full APK, cancels/retries setup and boots without manual import:
scp out/runtime/aosp-pocket-runtime.apk pi@juanraspberry:/home/pi/aosp-pocket-runtime.apk
ssh pi@juanraspberry python3 - < tools/validate_bundled_on_pi.py
```

The guest regression uses a temporary forward to the runtime's **loopback-only**
port 5559 on the existing dedicated ADB server. Android 7 requires the tested
`--no-streaming` push-install path; the modern streaming install returned
`Failure [null]` on this guest. It verifies the nested device
is Android 7.1.1/API 25 before installing the included test APK. It checks guest
boot completion, installs/opens that APK, sends a touch to the outer host
Surface, and checks that the guest button counter increments. It also checks
the boot marker and host UID/seccomp/SELinux context. The temporary forward and
transport are removed afterward; the ADB server is never restarted.
Input commands explicitly select display 0 on the scaled test VM.

`make -f runtime/Makefile check-host`: passed the focused connection-registry
test, including destruction callbacks that re-enter the registry and sparse
connection IDs. The earlier destructor-callback test against the original
header timed out, reproducing the lock problem.

`ssh pi@juanraspberry python3 - < tools/validate_windows_on_pi.py`: passed on
2026-10-19 with runtime `0.2-windows`. It verified both launcher activities,
distinct manager/Android tasks, foreground service ownership, scaled touch in
the preview and full window, visible PiP bounds, restoration through the
Android launcher entry, and closing both UI tasks without replacing host PID
9056 or guest init PID 10400. A new guest `service check activity` Binder client
also succeeded with both windows closed. The newest task IDs were rechecked
after restoration: Android 267, manager 268. See
[runtime-windows-validation.json](runtime-windows-validation.json).
Screenshots are saved as `out/pocket-manager-preview.png`,
`out/pocket-android-window.png`, `out/pocket-floating-android.png` and
`out/pocket-android-restored.png`. This remains VM validation; no physical
Samsung installation is claimed.

The end-to-end guest check passed on 2026-10-19: Android 7.1.1/API 25,
`sys.boot_completed=1`, a separately installed test APK, and a tap sent through
the outer Surface that changed the guest button from 0 to 1. A fresh guest
UID 39999 could read the virtual-root-owned property area. Guest processes
retained host UID 10170, seccomp mode 2 and the untrusted-app SELinux domain.
The outer screenshot showed the guest's RGB bands; pixel samples were exactly
`(255,0,0)`, `(0,255,0)` and `(0,0,255)`.

The final `--restart` run passed too: Stop removed every guest/tracer process,
the same host PID booted a fresh guest, then APK installation, property access
and the forwarded button tap all passed. The captured framebuffer contained
three correctly ordered RGB bands, 80 rows each. The machine-readable result
is recorded in [runtime-vm-validation.json](runtime-vm-validation.json).

Killing only the tracked tracer was also verified to remove all guest
processes while leaving the host app alive. Machine-readable test output and
the VM screenshot are saved under `out/pocket-runtime-validation.json` and
`out/pocket-guest-test.png`; these generated artifacts are excluded from Git.

There is no audio, camera, GPS, sensor or telephony forwarding in this runtime
yet. The guest audio HAL reports that its Anbox audio server is unavailable.
Complete guest isolation and Android 16 guest support are not implemented.
