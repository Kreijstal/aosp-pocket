# Pocket Manager: Android 9 direct backend

The Manager upgrade contains two separately stored images:

- Android 7.1.1 / API 25: existing Ananbox/PRoot backend, `files/rootfs`.
- Android 9 / API 28: pinned AOSP/Redroid framework on the direct backend,
  `files/api28-rootfs`, with persistent user data in `system-data`.

An existing API 25 installation stays selected on upgrade. A fresh installation
selects Android 9. Stop the current guest before changing the version selector.
The generic image importer still accepts only the adapted API 25 image format;
it must not inject the older Binder bootstrap into an arbitrary API 28 ROM.

`GuestService` owns the session and its foreground notification. Manager,
full-screen Android and PiP share one guest. Closing a window does not stop it.
The Stop button terminates the guest init supervisor, which stops and reaps its
service process groups before Start becomes available again.

Android 9 starts a separate `GuestControl` process after its actual
`sys.boot_completed=1` property trigger. Its inherited app-private Unix socket
serves lossless compositor screenshots, pointer events, navigation keys and APK
installation. This replaces the bounded development probe: Manager does not
install a test app or shut down automatically after boot.

The preview is letterboxed and maps touches into the guest's 480 × 800 display.
The initial viewer requests at most four PNG frames per second, pauses requests
when no window is visible, and reconnects windows to the same session. This is
an initial display transport, not a high-frame-rate graphics backend.

Use **Install APK** in Manager to select an APK through Android's document
picker. The service copies it into the guest, uses its real PackageInstaller,
reports the result and launches its launcher activity when available. The APK
must support Android 9 and the guest's ARM64 architecture.

## Build

```sh
make -f runtime/Makefile -j2 manager-verify check-profiles
```

Upgrade APK: `out/manager/aosp-pocket-manager.apk`, package
`dev.kreijstal.aosppocket.runtime`, version 0.4 / code 4. Installing it over the
old package stops that package's currently running processes, as with any app
update. Its image and user data remain in place.

A parallel development package avoids interrupting the old running guest:

```sh
make -f runtime/Makefile -j2 manager-verify \
  MANAGER_PACKAGE=dev.kreijstal.aosppocket.managerdev \
  MANAGER_OUT=out/manager-dev
```

This uses the same host and guest code with a different package/task identity
and its own private data. The APK is approximately 522 MiB because both images
are included. Only the selected image is unpacked.

## Current limits

Framework networking, audio input/output and camera are not implemented in the
Android 9 profile. ART remains interpreted. Keystore and Gatekeeper use software
implementations; no host hardware attestation or credential boundary is
provided. All guest processes share the host app's real Linux UID and SELinux
domain. Virtual guest IDs provide Android compatibility, not isolation between
guest apps. Physical Samsung validation is separate from the Pi VM tests.

The stock Android 9 Gatekeeper daemon is included with its app-private state
directory. Its [upstream software fallback](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r61/gatekeeperd/gatekeeperd.cpp)
handles the missing hardware HAL. Omitting the daemon caused a fresh guest's
setup-completion observer to crash system_server; reusing previously provisioned
data could hide that first-boot defect.

## Validation

Build/signature/image-layout checks run through `manager-verify`. The focused
profile test keeps the API 25 importer from loading an API 28 image using the
wrong backend. `tools/validate_manager_on_pi.py` uses the dedicated, guarded
Cuttlefish ADB connection and a separate Manager development package. It checks
the real guest process identity, boot endpoint, windows and continued lifetime
of the original API 25 host/PRoot/init processes.

For host UI iteration after installing a full development seed,
`tools/pack_manager_update.py` can build a small signed-code update containing
the new host DEX and native library. It retains the already unpacked private
Android 9 image and refuses a first-run setup without that seed. This is only
for `managerdev`; distribute the full `manager-verify` APK.

[Recorded VM results](api28-manager-validation.json) cover a clean-data boot,
real APK installation, four guest button taps through Manager/full-screen
windows, PiP, closing/reopening Manager, and Stop/Start. Shutdown reaped every
guest process; the original API 25 host/PRoot/init PIDs stayed unchanged. The
[VM screenshot](images/api28-manager.png) shows the live counter and RGB bars.
The full distribution contains byte-identical host DEX/native libraries and
the same 1,369 guest payload entries as the tested seed plus host update.
