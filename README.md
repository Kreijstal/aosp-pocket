# AOSP Pocket

A source-built foundation for a small Android-on-Android environment, intended
to run as an ordinary app on an ARM64 phone. The long-term goal is a VMOS-like
experience using an adapted AOSP userspace and the host CPU, with explicit
hardware bridges.

There are now two APKs: the original native capability/bootstrap probe and a
separate **experimental Android guest runtime** adapted around Ananbox's Anbox
and PRoot forks. The latter includes an embedded GLES display, touch forwarding,
guest-image import and guest Binder bootstrap. An AOSP 7.1.1 guest has booted
to its launcher inside the existing Android 16 LineageOS VM, with
`sys.boot_completed=1`. A separate test APK installed and rendered, and a tap
forwarded through the host Surface changed its button counter. Physical phone compatibility and
modern guest-app compatibility are not established. The guest shares the host
kernel and app UID. It is not a security boundary between guest processes.

See [the runtime build and VM evidence](docs/ananbox-runtime.md). The runtime
currently targets Ananbox's AOSP 7.1 guest image, separately from the Android 16
Toybox used by the bootstrap probe. No VMOS payload is incorporated.
Stop/Start in the same host process and touch input after that restart also
passed the [VM regression](docs/runtime-vm-validation.json).

The `0.2-windows` runtime has **Pocket Manager** and **Pocket Android** launcher
icons and separate Recents tasks. The manager contains a live miniature;
Android can open in its own full-size window or float using picture-in-picture.
A foreground service keeps the same guest running while its UI windows close.
Preview/fullscreen touch, visible PiP, launcher restoration and a fresh guest
Binder client with both windows closed passed the
[window regression](docs/runtime-windows-validation.json) on the Pi-hosted VM.
See [window controls and lifecycle](docs/ananbox-runtime.md#manager-android-window-and-floating-mode).

The `0.3-bundled` APK includes the tested AOSP 7.1.1 ARM64 guest. Tap **Start**
in Pocket Manager, or open **Pocket Android**, to extract and boot it without
a separate download/import. Setup runs in the foreground service with progress,
cancel/retry, and no temporary copy of the guest ZIP. Existing guest data is
preserved on upgrade. Manual import remains available before initial setup.
First-run setup, cancellation/retry, launcher display and forwarded touch
passed the [bundled-image VM regression](docs/runtime-bundled-validation.json).

Android 9/API 28 bring-up now has a separate source-built Binder/property bridge
and integration APK. The VM tests pass Binder/FD/virtual-UID forwarding, private
properties with change notifications and clean restart, and real Android 9 ART
execution of Java threads/reflection/exceptions plus a native child process.
These run under an ordinary host app UID; ART/platform binaries are pinned
upstream prebuilts. The rootless AOSP init core also passes service startup,
property actions, inherited sockets, crash/restart and clean shutdown tests.
The real Android 9 zygote now accepts ABI queries, forks a Java child, handles
its Binder calls and reaps it while retaining the host app sandbox. This uses
an image-only development profile with lazy preload and a JVMTI bootstrap.
A separate direct platform profile now **boots Android 9 without PRoot**, using
the real system server, SurfaceFlinger, installer and a 480 × 800 software
display. The VM regression passes the boot property, launcher first draw, APK
installation, app rendering and an injected tap that increments its button.
See [the evidence](docs/api28-direct-boot-validation.json) and
[captured test app](docs/images/api28-guest-test.png). Pocket Manager integrates this direct backend alongside Android 7, with
separate persistent data, live preview/window/PiP, touch and APK installation.
Version 0.5 adds shared host networking, contact import, host dialer/SMS intent
delegation and a permission-checked host device broker. Standard guest camera,
audio, sensor and location adapters and physical phone validation remain
pending. See [the Manager upgrade and capability matrix](docs/pocket-manager-api28.md) and
[API 28 builds, evidence and remaining boot gates](docs/api28-bringup.md).
Version 0.6 uses the host display's full pixel dimensions and density for
Android 9, fitting that unchanged guest display into the embedded card,
dedicated window or PiP with matching touch coordinates.
Version 0.7 adds an immersive full-screen viewer, preview expand control,
movable control menu, guest Back navigation and Home-to-PiP transitions.
Version 0.8 gives Android its own display page, with management controls on
Settings. The embedded card follows the guest aspect ratio and its surface
fills the card, removing the surrounding black panel.
Version 0.9 replaces the four-FPS PNG transport with anonymous shared RGBA
buffers and worker-thread native Surface presentation. Settings exposes timing
reports without root. Guest rendering still uses SwiftShader; GPU buffer
sharing and physical Samsung performance validation remain pending for 0.9.
Version 0.10 forwards completed guest compositor frames through three owned
buffers and presents them using two reusable host `AHardwareBuffer`/EGLImage
textures, with a CPU fallback. Screenshot capture is removed. Guest GLES
remains SwiftShader; the shared GPU path currently accelerates host
presentation. Settings reports the actual host renderer and presentation mode.

A separate experimental **direct backend** runs a controlled native program
with the pinned Android 9 linker/bionic without PRoot or a tracer. Its path,
directory-FD, symlink, stdio, directory, metadata, Unix-socket/FD-transfer,
thread and fork/exec tests pass on Linux and in the
Android VM. The same program under full-ptrace PRoot supplies a measured
comparison. This is a build-controlled native ABI prototype, not a complete
Android execution engine or VMOS image compatibility. See
[the implementation, limits and benchmark](docs/api28-bringup.md#experimental-direct-backend).

## What works

- A 57 KiB development APK with no requested permissions, network access, ADB
  pairing or root helper.
- A small Toybox built from **AOSP `android-16.0.0_r1`**, pinned to a source
  commit; no opaque guest ROM or VMOS binaries.
- Launching immutable native code installed from the APK as the app's own UID.
- Per-instance writable data under `files/instances/default/rootfs/data`.
- Checking real output from `echo`, `uname`, `id`, `pwd` and `cat`.
- Real capability probes for host Binder protocol access, private namespaces,
  user namespaces, tracing a disposable child, anonymous shared mappings and
  Android's public shared-memory API.
- An on-screen JSON report with a copy button, also saved app-privately.

The executable uses the host Android linker, bionic and kernel. Toybox comes
from Android 16 sources, but that does **not** make this an Android 16 guest.
The separate runtime below uses the adapted Android 7.1.1 framework.

## Build

The current workspace reuses the existing, source-built APK tooling in
`../helloworldandroid` ([androidworld](https://github.com/Kreijstal/androidworld)).
The sibling directory name can be changed with `TOOLCHAIN`. It does not modify
that project or any LineageOS output
directory. Prerequisites: Linux x86-64, JDK, Python 3, GNU Make, a C compiler for
Toybox's build generators, and Android NDK r27c.

```sh
git submodule update --init
make -j2 verify
```

For another toolchain location:

```sh
make -j2 verify NDK=/path/to/android-ndk-r27c TOOLCHAIN=/path/to/androidworld
```

`TOOLCHAIN` must contain the API 36 `android.jar`, `build/dx.jar`,
`build/apksigner.jar` and `build/tools/com/example/build/` classes described in
[androidworld](https://github.com/Kreijstal/androidworld). The local tooling
checkout used for this build was `c35c1cf3033fe42414b35570ab0ac3b97f6d8e9d`;
the binary manifest and APK builder sources were unmodified. The app and native
payload here are built from this project's sources. Development signing keys
are generated locally and are excluded from Git.

Output: `out/aosp-pocket.apk`, package `dev.kreijstal.aosppocket`, version
`0.1-bootstrap`. The initial target is the ARM64 SM-G780G on Android 13; that
physical installation has not been tested. Minimum host API is 28, target 36.
Executables have 16 KiB ELF segment alignment.

## Run

Install the APK normally and tap **Run AOSP bootstrap**, then **Copy report**.
The app does not request a wireless-debugging pairing code. ADB is optional for
developer installation and automation:

```sh
adb install -r out/aosp-pocket.apk
adb shell am start -n dev.kreijstal.aosppocket/.MainActivity --ez run_bootstrap true
```

`bootstrap_ok=true` means the native bootstrap succeeded. The separate
`android_framework_booted=false` and `guest_isolation_verified=false` fields
remain explicit. Failed capability probes are observations, not instructions
to weaken host security. No host Binder transaction or context-manager
registration is attempted.

## Verification

```sh
make -j2 verify
scp out/aosp-pocket.apk pi@juanraspberry:/home/pi/aosp-pocket.apk
ssh pi@juanraspberry python3 - < tools/validate_on_pi.py
scp pi@juanraspberry:/home/pi/aosp-pocket-bootstrap.json out/bootstrap-vm.json
```

The Pi harness first verifies the existing Cuttlefish instance and uses its
dedicated ADB server. It installs and starts only this app. External root is
used solely to retrieve its private report; the native runtime itself runs as
the ordinary app UID. It neither restarts ADB nor changes VM networking or
security settings. See [the recorded results](docs/validation.md).

## Android guest runtime

```sh
git submodule update --init
make -f runtime/Makefile native
make -f runtime/Makefile -j2 manager-verify check-profiles
```

Output: `out/manager/aosp-pocket-manager.apk`, package
`dev.kreijstal.aosppocket.runtime`. This is a separate GPL runtime prototype;
the bootstrap remains a separate Apache-2.0 APK. See the runtime document for
the additional CMake/Boost/D8 prerequisites, guest image preparation and
current limitations. The native build does not modify LineageOS source or
output directories. Modern Android hosts may need an explicit native-process
limit adjustment; the app does not make that change automatically.

See [the runtime design](docs/runtime.md) for the gaps that must be solved.

[VMOS APK analysis](docs/vmos-analysis.md) now records concrete loader, syscall,
Binder and hardware-bridge observations from the official APK, including an
offline Unicorn check of its preloader's seccomp construction. These findings
guide the next runtime experiments; they do not implement a guest framework.

## Sources and licensing

Bootstrap code: Apache-2.0, see [LICENSE](LICENSE). Toybox: upstream 0BSD,
preserved in `vendor/toybox/LICENSE` and inside the APK. The source URL, tag and
commit are recorded in [sources.lock.json](sources.lock.json); the build rejects
a different or modified Toybox checkout. The separate runtime host and adapter
are GPL-3.0-only and link Anbox's GPL core; PRoot is GPL-2.0-or-later, talloc is
LGPL-3.0-or-later and Boost is BSL-1.0. Original third-party licenses and source
headers are preserved. See [runtime/NOTICE](runtime/NOTICE). Imported guest
images retain their own upstream licenses. The runtime bundles the pinned
Ananbox reference guest with its original notices; this is an upstream prebuilt,
not a locally source-built ROM. No VMOS image or engine is included.
