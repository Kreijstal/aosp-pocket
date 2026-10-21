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
Only one window owns its display at a time. The Android window (including PiP)
takes precedence over Manager; Manager hides its SurfaceView and shows a return
link while that window is visible. Closing Android's window hands its display
back to Manager. Changing windows does not create another guest or mirror its
screen. This ownership rule applies to both backends.
The Stop button terminates the guest init supervisor, which stops and reaps its
service process groups before Start becomes available again.

Version 0.15 replaces the text-only **Install APK** chooser with a separate
installer page inspired by VMOS's app-import layout. **Device apps** shows a
four-column icon grid with app names, selection marks and search. Selecting
one app shows its icon, package, version, total APK size and split count above
**Install in Android**. The existing installation path includes its base APK
and split APKs. One package is selected per installation.

**APK file** opens Android's document picker, then previews the chosen APK's
actual icon and metadata before confirmation. Archive parsing runs on a worker;
the temporary preview copy is deleted after its icon and label are read. The
original document URI supplies installation bytes. Canceling does not install
anything. Opening this page does not stop the running guest. No VMOS code or
assets are bundled. [VM UI checks](api28-app-import-validation.json) cover the
grid, selection, search, result delivery and archive-resource preview; this is
local VM evidence, with Samsung validation still pending.

Version 0.16 fixes input freezing while an APK is imported. Previously copying
the host APKs and waiting for the guest's PackageInstaller callback ran on the
same worker and socket as touch and navigation events. Input queued behind the
entire installation. Slow operations now use a separate serial worker and
connection; touch and navigation retain their ordered input channel. Guest
display transport is unchanged. Status distinguishes copying individual APKs
from waiting for installation.

The focused ARM64 Pi VM regression deliberately blocks both the source stream
and the installer response. With the old production host classes, navigation
cannot arrive during either three-second observation window. With 0.16 it
arrives in milliseconds during both blocked phases, and installation completes
after each delay is released. This tests the actual `DirectSession` against a
synthetic socket peer; it is not proof of Revolut compatibility on Samsung.

```sh
make -f runtime/Makefile install-input-probe
rsync out/install-input/probe.apk pi@juanraspberry:/home/pi/pocket-install-input-test.apk
ssh pi@juanraspberry python3 - < tools/validate_install_input_on_pi.py
```

Version 0.17 also fixes a guest system-server crash after an app requesting
`CHANGE_NETWORK_STATE` is installed. Android 9's `PermissionMonitor` sends legacy netd
commands such as `network permission user set NETWORK 10016` from its
`PACKAGE_ADDED` receiver. Rejecting this command raised an uncaught
`IllegalStateException` in `NetworkManagementService.setPermission`, killing
`system_server` after PackageInstaller had already reported success. The
network stub now validates and records virtual UID permission set/clear
commands and returns the expected success response. It does not reconfigure
host firewall rules or provide separate kernel-UID networking isolation.
The fixed image includes the 0.16 input-queue change.

`make -f runtime/Makefile check-network-permissions` covers the exact failing
command, grant updates/removal, UID limits and rejection of malformed batches
without partial updates. `network-permission-probe` builds a small new app
requesting `CHANGE_NETWORK_STATE` to exercise the real package-added broadcast independently
of Revolut's code. See [installation regression evidence](api28-install-hang-validation.json).

Android 9 starts a separate `GuestControl` process after its actual
`sys.boot_completed=1` property trigger. Its inherited app-private Unix socket
serves pointer events, navigation keys and APK installation. Version 0.9 also
used it for frames; 0.10 gives composer output its own private endpoint.
This replaces the bounded development probe: Manager does not
install a test app or shut down automatically after boot.

Android 9 takes the host display's full portrait pixel dimensions and density
when its service starts. Window controls do not subtract pixels from the guest
display. These values configure the private image's boot properties before
init starts; the control handshake checks the framework's actual size and DPI.
The embedded card, dedicated window and PiP scale this same display to fit
their available area while preserving its aspect ratio. Resizing a window does
not change the guest resolution or restart Android. Touches map from the
visible surface back into the full guest pixel coordinates. For example, a
1080 × 2400 guest in a 320-pixel-wide card occupies about 320 × 711 pixels.
The embedded card no longer has an arbitrary 300 dp height cap.
Version 0.9 replaces the four-FPS PNG screenshot transport with two anonymous
RGBA buffers. The host allocates shared memory using Android's public NDK
shared-memory API and passes its descriptor to the guest through the private
Unix socket. Pixel data does not travel through that socket or phone storage.
A request identifies the back buffer and a sequence number; the guest completes
its capture and copy before acknowledging ownership. The host presents that
buffer directly to an `ANativeWindow` on its worker thread. A front-buffer lock
protects window handoff, transition snapshots and unmapping at shutdown.
There are no per-frame PNG encode/decode operations, host Java Bitmap draws,
or accumulating UI callbacks. Presentation targets 60 FPS without a catch-up
queue; achieved throughput depends on capture and host presentation costs.
The producer pauses when no window owns a valid Surface. Embedded, full-screen
and PiP windows use the same buffers and guest process.

This remains a software-rendered SwiftShader guest, with compositor readback
and CPU copies into shared memory and the host Surface. It is **not** zero-copy
GPU buffer sharing or evidence of VMOS-equivalent performance. A host Bitmap is
created only for an explicit window transition snapshot. API 25 keeps its
existing renderer. Both sides require display protocol v3 and reject an older
handshake rather than interpreting it as pixel data.

On an unrooted phone, open **Settings** after using Android for a few seconds.
The last visible sample shows presented FPS, guest capture time and native
presentation time. **Copy display report** copies the JSON report without ADB,
root or instrumentation. The private `logs/display-performance.json` also
records geometry, anonymous buffer size and the zero pixel-byte socket count.
Samples measure frame submissions, not unique content changes or panel scanout.
The pinned compositor configuration remains 30 Hz; a 60-FPS producer ceiling
does not imply 60 different guest frames. Capture/presentation means are
cumulative for the session and include startup stalls.

The [shared-display VM regression](api28-shared-display-validation.json) passed
anonymous descriptor transfer, rootless guest boot, exact RGB pixels, three
real guest taps, hidden-display pause/resume, exclusive PiP and clean teardown.
The original API 25 guest was restored from its existing data. Its sampled
throughput was 6.34 presented FPS at 720 × 1280, with cumulative mean capture
300.7 ms and native presentation 6.6 ms. This is not a Samsung benchmark or a
measured before/after speedup. [Android](images/api28-shared-display.png) and
[Settings](images/api28-shared-display-settings.png) screenshots show the tested UI.

Frame sizes scale with guest pixels. Invalid geometry and sizes over
16 megapixels are rejected; two RGBA slots use eight bytes per guest pixel.
Android 7 retains its saved display geometry.

Version 0.10 replaces the screenshot producer with a source-built HWC 1.1
module, loaded through the guest's existing composer 2.1 adapter. Only the
Manager image selects `hwcomposer.pocket`; the original platform probe retains
its Redroid configuration. Non-target layers request GLES client composition.
The completed client target is copied through the pinned linear Redroid
mapper into a three-slot anonymous pool. No `SurfaceControl.screenshot`, Bitmap
normalization, PNG codec or per-frame pixel socket payload remains.
The control handshake is v4; the compositor endpoint has its own versioned
header and checks the shared region size. The composer uses the Manager's
60 Hz configuration; achieved frame rates
remain workload- and device-dependent.

The consumer holds a front slot while displaying or snapshotting it. Acquisition
holds its replacement before the previous front is released. The composer may
replace its newest unheld slot under the state mutex when a slow consumer holds
two slots. This retains the latest content without an accumulating frame queue.
Pause disables pool copies; resume requests one invalidation so even a static
scene refreshes. A retained frame is redrawn when the host Surface changes,
including when no new guest frames arrive. Socket close disables the producer
before its mapping is released. Three RGBA slots use 12 bytes per guest pixel.

Host presentation now imports two persistent `AHardwareBuffer` objects as
EGLImage textures. The CPU copies received RGBA rows into the selected hardware
buffer; GLES samples that same allocation into the host Surface. GPU fences
protect reuse, and an unlock fence is honored before sampling. Textures and
hardware buffers survive window handoff. Missing host EGL/allocator support or
repeated presentation failure uses the native CPU Surface fallback. The
reported backend and fallback reason identify the selected path.

This is **shared GPU presentation**, with two CPU copies remaining: guest client
target → anonymous pool, then pool → host hardware buffer. Guest GLES still
uses SwiftShader. It does not import Samsung/host allocator handles into the
Android 9 mapper and does not accelerate guest GLES command execution. An
arbitrary guest image or graphics allocator needs its own compatibility work.
The pinned gralloc's even-pixel stride is also established from its allocator
instructions at `0x19c0`–`0x19cc`, not inferred from the test host's 720-pixel
width. No Samsung performance measurement is implied by VM validation.

The ordinary display probe animates before its first tap, then becomes static.
This measures changing frames first and tests subsequent idle window handoffs
without relying on an animation to refresh them. The buffer ownership test
also exercises a slow consumer and 2,000 producer updates.

The [0.10 VM regression](api28-composer-gpu-validation.json) passed the shared
GPU path with no CPU fallback, exact red/green/blue and middle-gray pixels,
three guest taps, hidden copying pause/resume, exclusive PiP and clean teardown.
After the first tap, the static screen produced zero additional frames over
the idle check; subsequent window handoffs passed. The original API 25 guest
was restored from existing data. The 720 × 1280 animated sample measured
11.86 FPS, 5.34 ms mean composer copy and 306.96 ms cumulative mean presentation
on the software host renderer. Startup stalls are included in that mean; this
is neither a Samsung measurement nor a controlled before/after speedup test.
[Display](images/api28-composer-gpu.png) and
[Settings](images/api28-composer-settings.png) show the tested output.

Version 0.8 puts the guest on a dedicated **Android** page. Its only page
navigation is Android/Settings; a small expand control opens the existing
full-screen guest. Start/Stop, image import, version selection, APK install,
Open last app, peripheral grants and diagnostics live on **Settings**. Settings
contains no guest preview. Opening the last app or choosing an APK returns to
the Android page. The idle page offers one Start Android action.

The embedded card itself fits the guest aspect ratio and the SurfaceView fills
that card's exact bounds. There is no larger black container around a smaller
image. Page margins use the page background, and the complete guest screen is
preserved. Fitting happens once at the card boundary, avoiding another rounding
step that could leave a one-pixel border. Switching pages pauses invisible
frame requests and leaves the guest process running.

The full-screen control introduced in version 0.7 opens the existing guest in its own
immersive task. The guest fills the available display with no permanent host
toolbar. A movable **Android controls** button snaps to either edge and opens
Back, Home, Recents, Manager, Float and Close window actions. It stays inside
the app window and does not require permission to draw over other apps.
Manager returns to the embedded preview; Close window removes only the viewer.
On the Android 9 backend, the host Back action goes to the guest Activity;
Back while the control menu is open dismisses the menu. Leaving with host Home
enters PiP when the host supports it. PiP hides the control handle, and the
Android launcher entry restores the full-screen window. Android 7 retains its
existing touch navigation; the added guest navigation menu requires Android 9.
On Android 13+ hosts the viewer explicitly opts into the platform Back dispatcher
and registers an `OnBackInvokedCallback`; older hosts use `onBackPressed`.
The callback API is isolated behind an SDK check. The manifest validator also
checks the encoded opt-in attribute, following Android's
[platform Back guidance](https://developer.android.com/guide/navigation/custom-back/predictive-back-gesture).
When a guest frame is available, the Android 9 viewer uses a 300 ms circular
snapshot transition on entry and return, with unchanged guest pixels and one
display owner throughout.

Use **Install APK** in Manager to select an APK through Android's document
picker. The service copies it into the guest, uses its real PackageInstaller,
reports the result and launches its launcher activity when available. The APK
must support Android 9 and the guest's ARM64 architecture.
Use **Open last app** to reopen the most recently imported APK. An update's
commit callback can precede package-removal broadcast processing, which can
stop its auto-launched activity. Explicitly reopening it after installation
avoids reinstalling; automatic update-launch ordering remains a limitation.

## Build

```sh
make -f runtime/Makefile -j2 manager-verify check-profiles check-display
```

Upgrade APK: `out/manager/aosp-pocket-manager.apk`, package
`dev.kreijstal.aosppocket.runtime`, version 0.8 / code 8. Installing it over the
old package stops that package's currently running processes, as with any app
update. Its image and user data remain in place.

A parallel development package avoids interrupting the old running guest:

```sh
make -f runtime/Makefile -j2 manager-verify \
  MANAGER_PACKAGE=dev.kreijstal.aosppocket.managerdev \
  MANAGER_OUT=out/manager-dev
```

This uses the same host and guest code with a different package/task identity
and its own private data. The APK is approximately 529 MiB because both images
are included. Only the selected image is unpacked.

## Current limits

Standard guest camera, audio, sensor, location and radio adapters remain
unimplemented in the Android 9 profile. The host device broker described below
does not make those Android APIs functional automatically. ART remains
interpreted. Keystore and Gatekeeper use software
implementations; no host hardware attestation or credential boundary is
provided. All guest processes share the host app's real Linux UID and SELinux
domain. Virtual guest IDs provide Android compatibility, not isolation between
guest apps. Physical Samsung validation is separate from the Pi VM tests.

## Host peripherals in 0.5

Open **Host peripherals** in Manager to grant Android permissions, import
contacts and run explicit device checks. Host permissions are separate from
permissions granted inside the guest. Camera and microphone access is scoped
to a request; permission failures and unavailable host devices are reported.

| Interface | Guest integration | Remaining limits |
| --- | --- | --- |
| Network | Android 9 NetworkAgent, private netd metadata, DNS proxy and ordinary host-UID sockets | Shares the host app's default uplink and VPN policy; no independent guest routing, firewall or VPN exemption. Reverse DNS is not implemented. |
| Contacts | Stock ContactsProvider and Contacts app; explicit host-to-guest snapshot import | Names, phone numbers and email addresses only. Repeated imports update managed copies and remove copies absent from a complete new snapshot. Unrelated guest contacts and all host contacts stay untouched. |
| Calls and SMS | Guest `tel:` DIAL/CALL/VIEW and `sms:`/`smsto:` SENDTO intents open host dialer or message composer | Host confirmation is required. CALL is deliberately delegated as DIAL. No independent guest SIM, modem, IMS registration or call audio path. Explicit intents targeting another guest dialer do not use this adapter. |
| Battery | Existing health relay | Subject to host Android visibility. |
| Camera | Broker Camera2 enumeration and one-shot YUV_420_888 capture | Guest CameraManager/HAL adapter and continuous video remain pending. Supported sizes come from the host; plane bytes, strides, crop and available capture metadata are preserved. YUV is not sensor RAW. |
| Microphone and playback | Broker PCM16 capture/playback, bounded to five seconds per request | Guest AudioRecord/AudioTrack/AudioFlinger adapter remains pending. No codec, zero padding or invented microphone samples. |
| Sensors | Broker request-scoped SensorManager samples with original timestamps and accuracy | Guest SensorManager/HAL streaming adapter remains pending. |
| Location | Broker newest accessible last-known host fix, with provider and age | No invented fix; continuous guest LocationManager integration remains pending. |
| Phone state | Broker permitted TelephonyManager readings, with per-field errors | No privileged identifiers, carrier provisioning, attestation keys or privileged IMS state. |
| NFC, Bluetooth, USB and other devices | Unimplemented | No forwarding claim. |

The binary `HostWire` protocol runs over an app-private filesystem socket.
Requests and replies have bounded lengths; camera planes and PCM use binary
payloads following JSON metadata. `HostDevices` is the guest-side SDK. It is not
a sandbox boundary between guest apps: they share the same real host UID.
The guest control process accesses ContactsProvider using Android 9's
external-provider lifetime API, because it is not an AMS-managed application.
This uses its existing virtual guest system role, not host root privileges.

The host phone/message apps keep ownership of real SIM/IMS services. This is
delegation, not an IMS implementation inside the guest. The host app requests
ordinary READ_PHONE_STATE, not privileged phone-state access.

Host-broker results in the Pi VM are observations of Cuttlefish host APIs, not
physical SM-G780G validation. In particular, successful PCM transport with zero
sample power does not prove microphone audibility. The older API 25 backend
does not use these new Android 9 bridges.

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
the new host DEX and native library. With `--base` and `--image`, it also uses
the new manifest and includes guest payloads changed from the full seed.
It retains the already unpacked private
Android 9 image and refuses a first-run setup without that seed. This is only
for `managerdev`; distribute the full `manager-verify` APK.

`check-display` covers aspect-ratio fitting in portrait/landscape and smaller
windows, native-resolution frame limits, boot-property projection and the
versioned size/density handshake. `tools/validate_display_on_pi.py` exercises
an ordinary guest app's real display metrics, forwarded touches in embedded
and dedicated windows, exclusive PiP and return to Manager. It tests guests
sequentially to fit the 2 GiB outer VM, stopping the original through its own
UI and restoring it from its existing data afterwards. Stage the development
APK as `/home/pi/aosp-pocket-manager-dev.apk`, the display probe as
`/home/pi/pocket-display-test.apk`, and `tools/validate_manager_on_pi.py` as
`/home/pi/aosp-pocket-validate-manager.py`, then run:

```sh
make -f runtime/Makefile -j2 display-probe check-display
ssh pi@juanraspberry python3 -u - < tools/validate_display_on_pi.py
```

[Version 0.8 display-page evidence](api28-display-page-validation.json) verifies
the [focused Android page](images/api28-display-page.png) and
[separate Settings page](images/api28-settings-page.png). The embedded card and
SurfaceView both occupy `[59,54][660,1124]` on the 720 × 1280 test host:
601 × 1070, compared with version 0.7's 340 × 605 preview. Pixel inspection
confirms that the guest's full-width red reference bar reaches both card edges
and the surrounding page has its normal background colour. The Surface adds
zero padding on all four sides; guest Android's own status/navigation bars
remain part of the guest image. The native guest resolution/DPI stayed
720 × 1280 at 240 DPI through Settings, full screen and PiP handback. Three
real taps reached the same guest app without a system-server restart.

```sh
ssh pi@juanraspberry python3 -u - --clean-page --reuse-probe < tools/validate_display_on_pi.py
```

The first run exposed capitalised accessibility button labels under the new
host theme; the test now compares action labels case-insensitively. No APK
change was needed for that test correction. The final run stopped/reaped the
development guest and restored the original from existing data. Production and
development APKs have matching host DEX/native code and all 1,376 guest payloads.
Only VM host evidence is claimed; physical Samsung testing remains pending.

[Version 0.7 full-screen evidence](api28-fullscreen-validation.json) verifies
the [preview expand control](images/api28-fullscreen-embedded.png), a
[720 × 1280 guest filling the whole host window](images/api28-fullscreen.png),
the movable control handle, menu Back dismissal, Manager return and
[exclusive PiP](images/api28-fullscreen-pip.png). A real Back event reached the
ordinary guest Activity (`BACK_RECEIVED`), host Home entered PiP, and reopening
the Android launcher entry restored full screen. The same guest system server
survived all transitions and three button taps. The final focused run reused
the previously installed display probe after adding the platform Back callback:

```sh
ssh pi@juanraspberry python3 -u - --fullscreen --reuse-probe < tools/validate_display_on_pi.py
```

Omit `--reuse-probe` to also install/update the diagnostic APK through the file
picker. The test closes its development host after clean guest shutdown, then
restores the original API 25 guest from its existing image/data. These results
cover the Android 16 Cuttlefish host; physical Samsung and Android 9 host tests
remain pending. [VMOS reference findings](vmos-analysis.md#full-screen-interaction)
are independently checked static DEX observations, not a dynamic VMOS trace.

[Version 0.6 display evidence](api28-host-display-validation.json) records a
720 × 1280, 240 DPI host and an ordinary guest app reporting the same pixel
dimensions and resource density. Three button taps reached that app across
the embedded view, dedicated window and PiP handback without restarting its
system server. The [embedded preview](images/api28-host-display-embedded.png),
[dedicated window](images/api28-host-display-dedicated.png) and
[exclusive PiP](images/api28-host-display-pip.png) show the same guest display
fitted without stretching. These are Cuttlefish observations; physical phone
display validation is still pending.

[Recorded VM results](api28-manager-validation.json) cover a clean-data boot,
real APK installation, four guest button taps through Manager/full-screen
windows, PiP, closing/reopening Manager, and Stop/Start. Shutdown reaped every
guest process; the original API 25 host/PRoot/init PIDs stayed unchanged. The
[VM screenshot](images/api28-manager.png) shows the live counter and RGB bars.
The [exclusive-display check](images/api28-manager-pip.png) confirms that Manager hides its guest surface
while PiP owns the display, that its return link opens the existing Android
window, and that closing that window restores interactive display ownership to
Manager without restarting the guest.
Those recorded window tests describe version 0.4 with 1,369 guest payloads.
Version 0.5 adds Contacts/ContactsProvider and the host phone intent adapter,
for 1,376 guest payloads. Host-bridge validation is a separate test:

```sh
make -f runtime/Makefile -j2 manager-verify check-profiles check-host-wire host-network-probe
rsync out/manager-guest/host-network-test.apk pi@juanraspberry:/home/pi/pocket-network-test.apk
ssh pi@juanraspberry python3 -u - --network-app < tools/validate_host_bridges_on_pi.py
```

The integration harness targets only the separate development package in the
guarded VM and checks that the original API 25 host/PRoot/init PIDs remain
unchanged. Runtime permission grants made by the test harness are administrative
test setup; the broker and guest operations still run under the ordinary host
app's Linux UID. Test contacts must be synthetic fixtures, never a personal
phonebook committed to a validation report.

The 2 GiB outer Cuttlefish VM ran out of memory during concurrent Android 7/9
device testing on 2026-10-19. Android's process-exit record reports LOW_MEMORY
for the original API 25 host process (PID 17501); its children also exited.
That run fails the preserved-session requirement even though its image and
data were not replaced. `--original-stopped` explicitly permits a subsequent
sequential test when the old guest is already absent, and records preservation
as untested rather than claiming unchanged PIDs. It never stops that guest.
Use that mode only after the old guest is already stopped. On this constrained
VM, testing guests sequentially avoids repeating the concurrent-memory failure.
`--app-only --network-app` resumes the ordinary-app network/SMS checks from a
saved successful core checkpoint and records that reuse explicitly.

[Version 0.5 host-bridge evidence](api28-host-bridges-validation.json) records
ordinary guest UID 10017 using the host app's real UID 10174, connected-network
reporting and HTTPS 200; host dialer and SMS conversation handoff; contact
import without duplicate managed copies; and four real permission-denial
responses. Broker tests captured a 176 × 144 YUV frame, four accelerometer
samples and 12,000 PCM frames with matching playback progress. The microphone
samples were all zero, so this verifies transport rather than audible capture.
These broker checks do not imply working standard guest camera/audio HALs.
Cleanup verified that removing the host fixture also removes its managed guest
copies. The development guest reaped all children and closed its broker, then
the original API 25 guest booted again from its existing image and data. Its
original pre-OOM session was not preserved.

### Termux bootstrap and native execution (0.14)

The direct backend resolves guest shebang interpreters, including their single
optional argument, and translates ELF64 RUNPATH and caller library directories.
Downloaded Android executables receive a cached copy beside the original with
the pinned guest linker as PT_INTERP. Their original bytes remain unchanged;
the same directory preserves `$ORIGIN`. Cache reuse checks source identity and
timestamps, and source execute permission is checked even when a cache exists.
The source directory must be writable to create this cache. This supports
dynamically linked 64-bit Bionic programs; it does not intercept raw syscalls
inside arbitrary static programs.

Execution retains the runtime's library paths, current guest identity, groups
and session log even when a native launcher calls `clearenv()`. Normal runtime
diagnostics go to the session log rather than contaminating command output.
Bionic's null-path `utimensat` call for `futimens` operates on its existing file
descriptor. PTY operations use host `/dev/ptmx` and `/dev/pts` under the ordinary
host app UID and its existing kernel permissions.

[Recorded local VM results](api28-termux-validation.json) cover Termux 0.118.3
starting with an absent prefix, extracting its bootstrap, completing the real
package maintainer scripts and opening a [Bash terminal](images/api28-termux.png).
The actual Termux shell retained `untrusted_app_27`, zero capabilities, seccomp
and no tracer. Native checks also verified clean command output, cache reuse,
invalidation after a source timestamp change, execute-permission enforcement
and unchanged original executable bytes. Host regression tests cover shebang
arguments, recursion, missing interpreters, ELF search paths and FD timestamps:

```sh
python3 tools/build_direct.py --host
python3 tools/validate_direct_host.py
```

The test used x86-64 Android 9 inside the local Cuttlefish VM. Root belonged to
the external setup/inspection harness. Its separate test image omitted large
fonts and used a lower private-guest storage reserve to fit alongside the VM's
existing system. The ARM64 0.14 APK retains the full image; only its manifest
and direct runtime payload differ from the previous distribution. Its shim
was rebuilt against that distribution's exact libc with all 48 hook prologues
verified by `tools/build_exec_runtime.py`. Physical Samsung validation is pending.


## Termux administration (0.18)

Settings now has **Enable Termux administration**, **Copy connection token**,
**Save Termux client**, and **Revoke connection token**. Administration is off
when the Manager process starts. Enabling it starts an app-owned listener on
`127.0.0.1:2228`; port conflicts are reported without binding to another address.
The guest need not be running to enable the listener or query its status.

Save the client to Downloads, copy the connection token, then run in Termux:

```sh
pkg install python
termux-setup-storage
python ~/storage/downloads/pocketctl.py pair
# Paste the token at the hidden prompt.
python ~/storage/downloads/pocketctl.py status
python ~/storage/downloads/pocketctl.py start
python ~/storage/downloads/pocketctl.py shell
python ~/storage/downloads/pocketctl.py shell -c 'id; getprop ro.build.version.sdk'
python ~/storage/downloads/pocketctl.py exec -- /system/bin/pm list packages
python ~/storage/downloads/pocketctl.py push ./example.apk /data/local/tmp/example.apk
python ~/storage/downloads/pocketctl.py pull /data/local/tmp/example.apk ./copy.apk
python ~/storage/downloads/pocketctl.py install ./example.apk
python ~/storage/downloads/pocketctl.py stop
```

`start` and `stop` request lifecycle transitions; `status` reports readiness.
Shell/exec require a prepared Android 9 direct image. From 0.21 the owner console
also works while the guest is stopped, for file access and recovery; commands
that use Android services still require a running guest. Administration keeps
its foreground notification while the guest is stopped so Termux can start it
again. Disabling administration closes the listener and current console sessions.
Revocation closes current sessions and replaces the key; pair again afterward.
The token is stored privately by each app, and never passed as a command-line
argument. Treat it as authority over Pocket and its guest data.

The client uses a challenge-response protocol with mutual HMAC-SHA256
verification and fresh 32-byte nonces. The server accepts at most four clients,
bounds request sizes, and times out unauthenticated/request handshakes. Stdin,
stdout, stderr and exit status are framed independently; binary file transfers
are not text encoded. Push writes its destination directly; an interrupted push
can leave a partial file. Pull replaces its local destination only on success.

This is a pipe-based root console, not an SSH server or PTY. Basic interactive
shell input works; terminal programs/job control and terminal resizing are not
implemented. Ctrl-C closes the client connection and stops its foreground
command. A deliberately detached guest process may outlive the connection.
`install` handles a single APK; use guest package-manager commands for split APK
sessions. The API 25 PRoot backend does not expose this root-console launcher.

The shell starts with virtual guest UID/GID 0, a guest-only path and a clean
Android 9 runtime environment. The real Linux UID/SELinux domain remains the
ordinary Manager app's. It provides no host root, kernel privileges or new
hardware attestation guarantees. It does not alter boot state or integrity
results. Data changes persist; subsequent image setup/updates may restore
bundled system/vendor files, so preserve local system modifications separately.

Focused validation:

```sh
make -f runtime/Makefile check-admin
python3 tools/build_manager_update.py \
  --base-apk out/manager-0.17/aosp-pocket-manager-0.17.apk \
  --out out/manager-0.18 --version-code 18 --version-name 0.18
```

The update builder emits a complete installable APK with the current host code
and client while retaining the known-good base APK's guest/native ABI. It does
not recompile or replace that base's guest firmware or native libraries.
Protocol test evidence is written to `out/admin-tests/protocol-validation.json`.
Host protocol tests alone do not establish root-shell behavior on Android.

## AndroidKeyStore provider registration (0.19)

The offline probe in `runtime/tests/key-attestation` ran as ordinary guest UID
10018 on the Samsung-hosted Android 9 image. All three cases (plain EC,
challenge attestation, StrongBox) failed before generation because the
`AndroidKeyStore` JCA provider was absent. A running keystore daemon and the
software-only Keymaster service did not establish that apps could use them.

The direct profile uses lazy Zygote preloading, which omits Android 9's
`warmUpJcaProviders()` call. The bootstrap now installs the stock
`AndroidKeyStoreProvider`, including its BC workaround provider, before apps
are forked. It checks provider availability without opening the keystore
service during this early startup phase. This changes neither the software
backend's security level nor verified-boot state.

The update builder can explicitly replace guest Java while retaining native
payloads from a known-good complete APK:

```sh
make -f runtime/Makefile out/manager-guest/pocket-control.jar key-attestation-probe
python3 tools/build_manager_update.py \
  --base-apk out/manager-0.18/aosp-pocket-manager-0.18.apk \
  --out out/manager-0.19 --version-code 19 --version-name 0.19 \
  --guest-control-jar out/manager-guest/pocket-control.jar
```

Both commands passed; APK signature verification passed. Payload comparison
confirmed that only the manifest and guest bootstrap/control JAR changed
(apart from signing metadata). Device validation of 0.19 is pending.

TEESimulator is not included in 0.19. Its upstream source at
`2fda3b1c67f3f00f1a1d343cee760eeb26a190bc` supports Android 10 and newer.
The Android 10/11 interceptor uses asynchronous Binder callbacks; Android 9's
`IKeystoreService` returns key characteristics, operation results and
attestation chains synchronously, with different transaction ordinals.
Lowering its minimum SDK or installing the existing module is insufficient.
An Android 9 adapter and startup integration require separate validation.
The source checkout for that investigation is `out/teesimulator-source`;
the pinned Android 9 AIDL reference is
`out/key-attestation/IKeystoreService-api28.aidl`.

## Experimental Android 9 attestation backend (0.22)

`runtime/attestation/teesimulator-api28.patch` ports the pinned upstream legacy
Keystore router to Android 9. It adds the Android 9 transaction ordinals,
synchronous reply envelopes, UID checks, software characteristics, private-key
existence/deletion handling, and StrongBox-unavailable responses. Authentication-
bound and symmetric keys retain the normal platform path. The adapter builds
against Android 9's actual Binder headers and the complete distribution APK's
libraries; it does not reuse the newer upstream Parcel/BBinder object layouts.
The source commit and Cargo lockfile are pinned in this repository.

The library loads only into the guest `keystore` executable, and only when an
owner-provisioned regular file at `/data/misc/keystore/pocket-teesim.json` has
mode 0600. It starts after the guest Keystore service is registered and reads
that configuration once per process. There is no Magisk/root injector or
additional control socket. The distribution contains no signing keys or keybox.
The existing host app sandbox remains the isolation boundary; logical guest
UIDs and guest root do not create a hardware-protected credential boundary.

Focused build and validation:

```sh
make -f runtime/Makefile check-teesim-api28
```

This passed under qemu-user with the pinned Android 9 Binder libraries and the
real upstream Rust TA. Tests cover synchronous generation, characteristics and
export replies; cross-UID generation rejection; StrongBox refusal; an EC public
key and three-certificate chain; certificate signatures; exact caller challenge;
SOFTWARE security levels; and a TA begin/update/finish signature verified with
the attested public key. The signer is a newly generated local test CA, not a
Google-trusted identity. Parcel buffers stay alive until the test process exits
because Android 9's Parcel destructor eagerly opens a kernel Binder driver.
These results establish native wire/crypto behavior. A separate device test on
the unrooted SM-G780G also passed: ordinary guest app UID 10018 generated EC
keys, signed and verified data, and obtained a three-certificate chain rooted in
the deployed local test CA. The certificate extension contained its exact
32-byte challenge and SOFTWARE attestation/Keymaster levels. StrongBox requests
returned unavailable (-68). This is software simulation inside Pocket, not
hardware-backed or Google-trusted attestation.

The first live runs exposed two additional routing requirements. Pocket restores
logical guest Binder identities only for application transaction codes, so the
interceptor's internal forwarding code must stay within that range. Android 9
also uses `android.security.IKeystoreService` as its interface descriptor; newer
versions use `android.security.keystore.IKeystoreService`. Version 0.22 includes
both fixes. The successful device test used the 0.21 host with the corrected
guest library, which is packaged in 0.22. Earlier reports using Android's default
AOSP software attestation chain do not establish that this backend was used.

Public test evidence can be checked without transferring signing keys:

```sh
python3 tools/validate_attestation_report.py \
  out/key-attestation/samsung-0.21-descriptor-fixed-report.json \
  --root out/teesim-api28/deployed-test-root.pem
```

The validator checks the complete certificate signatures against the deployed
public root, challenge, software levels, ordinary app UID, sign/verify results,
and StrongBox refusal. Private configuration and signing keys remain outside
the distribution APK. Some earlier runs lost guest init to SIGKILL (137); the
cause is unresolved, and a successful later boot does not establish long-term
stability.

An owner can prepare a SOFTWARE profile using an existing signing identity:

```sh
python3 tools/make_pocket_attestation_config.py \
  --keybox /private/path/keybox.xml --package dev.kreijstal.pocketattestationtest \
  --uid 10018 --out /private/path/pocket-teesim.json
```

Use the actual package UID from the guest `/data/system/packages.list`.
The output is private mode 0600 and existing output files are refused. It
describes an unlocked/unverified software guest and does not claim hardware
security. With the guest ready, create a private destination before transferring
the file through the already-paired Termux owner console:

```sh
python ~/storage/downloads/pocketctl.py exec -- /system/bin/sh -c \
  'umask 077; set -C; : > /data/misc/keystore/pocket-teesim.json'
python ~/storage/downloads/pocketctl.py push \
  /private/path/pocket-teesim.json /data/misc/keystore/pocket-teesim.json
python ~/storage/downloads/pocketctl.py stop
python ~/storage/downloads/pocketctl.py start
```

The library activates on that next startup. **Disable experimental attestation
backend** in Pocket Settings renames and preserves the configuration and stops
the guest. This recovery action works even if the guest cannot boot; start it
again afterward. No configuration is enabled automatically by installation.

The inherited legacy implementation keeps simulated keys in process memory;
they do not survive Keystore restarts. Its application-ID construction is also
minimal (package name/version placeholder, without signing-certificate digests).
Use temporary-key experiments while those upstream limitations remain. Hardware
authentication, Google trust, Play Integrity and banking compatibility are not
established by these tests.

### Reproducing the older VM's attestation claims

The old Cuttlefish experiment recorded a Google-rooted key-attestation chain
and a locked-bootloader claim, not a demonstrated `MEETS_STRONG_INTEGRITY`
verdict. On 2026-10-21 the same existing signing identity was tested through
the Pocket owner console using `pf-phone2`. Its private configuration targets
only `dev.kreijstal.pocketattestationtest` (guest UID 10018), with a simulated
TEE level and locked/verified boot claims. No physical boot state or Android
system properties were changed. This Android 9 adapter uses generation mode;
the older Cuttlefish setup used patch mode and retained platform key blobs.

The ordinary guest app reported successful EC generation and sign/verify,
`insideSecureHardware=true`, and a four-certificate chain ending in the old
Google root. StrongBox correctly remained unavailable (-68). These are
reproduced claims from a software simulator, not evidence of physical TEE
enforcement. After reconnecting the SSH tunnel, independent validation of the
full report passed: all certificate signatures, exact caller challenge, TEE
attestation/Keymaster levels, and locked/verified RootOfTrust claims matched.
The three issuer certificates exactly match the old EC keybox chain.

The old root's public key matches Google's published attestation root, but
Google's current revocation list marks both the old EC and RSA signing
certificates `REVOKED`, reason `KEY_COMPROMISE`. Thus reproducing that chain
cannot establish a valid trusted attestation. The check used the public
endpoints documented by Google:

- <https://android.googleapis.com/attestation/root>
- <https://android.googleapis.com/attestation/status>
- <https://developer.android.com/privacy-and-security/security-key-attestation>

The validator now supports explicit expected claims and RSA-signed certificate
chains, while its defaults still require SOFTWARE. To inspect this experiment's
report after retrieval, use the exported **public** root:

```sh
python3 tools/validate_attestation_report.py \
  out/key-attestation/samsung-previous-vm-profile-report.json \
  --root /private/path/previous-vm-root.pem \
  --expected-security-level 1 --chain-length 4 \
  --expected-device-locked true --expected-boot-state 0
```

Passing this command checks reported claims and cryptography only; it does not
override certificate revocation or establish a Play Integrity verdict. The
private identity and configuration are outside the repository and APK, and the
normal software profile was preserved separately on the phone before testing.
