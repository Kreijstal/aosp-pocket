# VMOS architecture analysis — 2026-10-19

## Result

The inspected APK provides a useful reference for an Android-on-Android runtime:
an ARM64 AOSP-derived guest, a custom executable loader and syscall-dispatch
layer, modified guest Binder code, and host-side graphics and hardware brokers.
The next work for AOSP Pocket should establish executable loading and a private
guest IPC path. Removing packages from a normal system image will not supply
those facilities.

This is **static binary analysis plus bounded offline emulation**, not a trace
of VMOS running on the Samsung. No VMOS binary was executed or installed during
this analysis. The inspected download may differ from the user's installed
version. Decompiled code and target binaries remain research artifacts under
ignored `out/research/vmos/`; they are not incorporated into AOSP Pocket.

## Artifact

| Field | Value |
|---|---|
| Acquisition | APK link on [VMOS's official app page](https://app.vmos.com/zh/vmos/) |
| Download | `https://file.vmoscloud.com/userFile/a806ddbfc3340f17cf966c47ad0fb0ee.apk` |
| Package | `com.vmos.virtual` |
| Version | `2.0.0-alpha09`, version code `20009` |
| Host SDK | Minimum 24, target 35 |
| APK size | 333,022,446 bytes |
| APK SHA-256 | `b3cbc9858cf4e0b04ec98b176265fd4a17cdee65625a2595d449fa7c7a8eefd4` |
| Signing | APK signature verified; certificate DN identifies VMOS, Changsha, Hunan |
| Certificate SHA-256 | `9403d15076dfbfbd1587a91aec0a955aa71a813aa2593ab8e11e290b572ef683` |
| Code | Two DEX files; 238 native method declarations; 36 library entries across two ABI directories |

The certificate identifies this artifact's signing identity, not an independent
assessment of its behavior or trustworthiness.

## Guest image and engine selection

**Binary derived.** `assets/rootfs_info` declares Android 9, ARM64, HAL version 2,
guest ROM version 10075, required engine `YLINKER`, minimum engine version 216,
and optional guest root support. Guest `system/build.prop` independently declares
Android 9 / API 28 and the `generic_arm64` product. Its security-patch string is
2019-08-01; that string alone does not establish the actual patch contents.

`assets/rootfs.7z` occupies 311,979,943 bytes and contains a full Android userspace
including init, ART, `app_process64`, framework services and system libraries.
The archive tool reports 991,442,114 uncompressed bytes. VMOS's responsiveness
therefore does not establish that its guest is just a tiny launcher or that
its whole installed footprint is small.

`RomInfo.VMOSSolutionTag` distinguishes `ORIGIN`, `KERNEL` and `YLINKER`; the
last selects `libylinker32/64.so`. Other engine branches are present in the APK
but are not the engine requested by its bundled ROM. Findings about one branch
must not be generalized to all VMOS versions or configurations.

## Native startup and filesystem adaptation

**Binary derived.** The `com.vmos.core.t` startup path prepares per-instance
properties, data, sockets, host-battery files and guest storage paths. On host
API 29+, the visible branch links guest `system/bin/initlinker32/64` to engine
executables in the host application's installed native-code directory. Older
host branches contain additional prelinker/interpreter replacement logic.

`NativeUtil.forkAndExecvePolicy` selects a native startup route, configures
instance directories and delegates to native execution. The app also supplies
host-side process-creation and zygote helpers. This is substantially more than
unpacking an unmodified AOSP root filesystem and changing directory into it.

**Binary derived.** In `libylinker64.so`, `jmp_syscall_handler` at virtual address
`0x17a28` enters the larger dispatcher at `0x76f14`. The dispatcher accepts
either an explicit syscall identifier or a saved register-frame identifier.
The library imports many filesystem, process, memory and socket operations.

**Inference.** This identifies a userspace syscall-adaptation layer. The exact
interposition coverage, path-rewriting rules, guest-credential semantics and
how every guest call reaches the dispatcher remain unresolved. Compatible ARM
execution through this route is a plausible explanation for the observed speed;
no CPU execution or startup-performance measurement was made here.

## Preloader signal and seccomp handling

**Binary derived.** `assets/libprelinker64.so` exports an ELF loader, a SIGSYS
handler installer and a seccomp initializer:

| Virtual address | Identified role |
|---|---|
| `0x2458` | `__linker_init` |
| `0x2710` | ELF loading routine |
| `0x3464` | SIGSYS handler installation |
| `0x34d4` | Installed signal handler |
| `0x35d0` | seccomp filter construction |
| `0x8050` | Local `prctl` syscall wrapper |

**Synthetic offline emulation.** `tools/emulate_vmos_seccomp.py` runs only the
filter-construction routine in Unicorn with two synthetic address-range
fixtures. The `prctl` calls are intercepted before their syscall instructions:
the analysis does not install a real filter or change host process policy.

The routine requests `PR_SET_NO_NEW_PRIVS`, then `PR_SET_SECCOMP` in filter mode.
It constructs 39 classic BPF instructions. Five focused checks confirmed
address-dependent handling of `mmap`, a 4 KiB mapping exception, and special
handling of SIGSYS handler registration, including a marker value.

**Limit:** this recovered filter does not trap all syscalls. It is not evidence
that `openat`, Binder ioctls or all filesystem operations are redirected through
SIGSYS. The separate YLINKER dispatcher must be investigated independently.

## Guest Binder

**Binary derived.** Guest `system/lib64/libbinder.so` contains both normal
`android::` Binder symbols and a second `vbinder::` implementation. Examples
include `ProcessState`, `IPCThreadState`, `BpBinder`, `BBinder` and `Parcel`.
Source-path strings identify `frameworks/native/cmds/vmosvbinder/`.

The `vbinder::IPCThreadState::talkWithDriver` routine at `0x1358e0` still issues
the Binder `BINDER_WRITE_READ` ioctl (`0xc0306201`). Its presence alone does not
explain the implementation below that interface. Java startup also prepares
instance socket paths ending in `bcs`, `bis` and `bhs`.

**Unresolved:** private guest context creation, FD/handle translation, thread
semantics, death notifications and whether particular operations are proxied
through those sockets. A separate namespace in a C++ symbol is not sufficient
proof of the transport or of a security boundary. AOSP Pocket still needs a
working, independently tested guest IPC implementation.

## Graphics, input and hardware brokers

**Binary derived.** `VMTools` exposes native entry points accepting an Android
`Surface`, framebuffer capture, resizing and input events. `libvmtools.so`
imports `ANativeWindow_fromSurface`, `sendmsg`, `recvmsg` and socket operations;
it contains a native-buffer socket endpoint. Java boot code starts graphics,
audio, shared-memory and qemu-pipe helpers. An emulator graphics transport does
not by itself establish that a CPU is being emulated.

**Binary derived.** `libhalbox.so` exposes camera preview, still-image and stop
callbacks, sensor updates, location, Wi-Fi and cellular-state entry points.
This supports designing explicit host-permission-controlled hardware bridges
as separate components. Complete protocols, zero-copy behavior, pixel formats
and latency have not been established.

## Importing apps already installed on the host

**DEX/binary derived, same 2.0.0-alpha09 artifact.** This is a host APK
reader and a guest package-installation pipeline. The investigated path does
not export host app-private data or reuse the host application's process.

1. `ImportDeviceAppFragment.p(Context)` calls
   `PackageManager.getInstalledPackages(0)`, excludes VMOS itself and
   `ApplicationInfo.FLAG_SYSTEM`, reads labels and APK paths, and sorts names.
   For a single APK it records `ApplicationInfo.sourceDir`; if
   `PackageInfo.splitNames != null`, it records the APK's parent directory.
   Icons are cached by `virtual.manager.e` under `icon_cache_v1`.
   The manifest declares a MAIN/LAUNCHER package-visibility query and the
   nonstandard `com.android.permission.GET_INSTALLED_APPS` permission, which the
   fragment checks. It does not declare `QUERY_ALL_PACKAGES`. This establishes
   enumeration of packages visible to this app, not unrestricted visibility
   on every Android/OEM version.
2. `InstallAppFragment.v` turns selections into `InstallQueueInfo` entries.
   `virtual.manager.f` (Kotlin metadata name `VmAppInstaller`) queues them and
   sends each source path to SDK `p.X/Y`. Binder method `k.Z`, implementation
   `core.l.Z`, `core.s.o0`, `h0.r` and `core.t.I` lead to the running engine.
   `core.s.o0` returns an error if there is no active guest implementation.
3. `exsocket.engine.a.E` sends request type **3** with the path/user selection.
   On the LocalSocket branch, `EngineSession$d.run` opens the source file, or
   enumerates `.apk`/`.apks` files in its directory. It obtains their file
   descriptors and passes them to native `setFileDescriptorsForSend` with
   the frame header. The native library imports `sendmsg`; the receiving
   `exsocket.rom.c.m` explicitly calls `getAncillaryFileDescriptors` and
   attaches the resulting descriptor list to the request. An alternate
   socket branch uses a guest-visible path instead; it should not be
   generalized to the LocalSocket case.
4. `SampleRomService` routes request type 3 to `rom.apks.a.f`. Multiple FDs
   become one split-APK source. A single FD is classified as APK, APKS, XAPK
   or APKM using its descriptor's name. The stream wrapper, shown as
   `apks.p000.b` by JADX, uses `FileInputStream(fd)`; its actual DEX package
   contains the Unicode component U+02CB.
5. For the modern guest path, `rom.apks.a.m/k` runs guest
   `pm install-create -r`, streams every APK into
   `pm install-write -S SIZE SESSION N.apk`, and runs `pm install-commit`.
   The shell helper executes `/system/bin/sh -c`; its `SuShell` log label
   alone does not demonstrate a host `su` invocation. Completion is tracked
   using guest package-added broadcasts and the installer callback.
   Separate older/alternate installation helpers also exist; they are not
   evidence that this FD path uses those helpers.

The embedded `assets/romex_cn.apk` is package `com.android.secex`. Its manifest
declares `android.uid.system`, `INSTALL_PACKAGES`, `DELETE_PACKAGES`, and the
guest `SampleRomService`. This supports a guest-privileged installer design;
the helper's actual runtime UID was not measured. The host app does not gain
system privileges from those guest declarations.

The examined import supplies APK bytes and split APKs. No host `dataDir`,
login database, token store, or private app preferences are selected by this
path. The examined stream operations do not repack or re-sign APKs. FD passing
avoids a separate host APK export copy; it does **not** eliminate the guest
package manager's writes, extraction or compilation. APK installation can
still fail on guest SDK/ABI/dependency incompatibilities.

For AOSP Pocket, the reusable design is a host installed-app picker plus a
guest installation session. Prefer the precise `sourceDir` and
`splitSourceDirs` list over scanning an entire package directory, and report
guest installation errors. Our current Android 7.1.1/API 25 ARM64 guest still
limits which host apps can run. This section investigates VMOS; it does not
implement that picker in our runtime or validate VMOS dynamically.

Validation command:

```sh
out/research/vmos/venv/bin/python tools/inspect_vmos_app_import.py out/research/vmos/vmos-official.apk out/research/vmos/analysis
```

Passed: 12 selected methods verified directly in DEX, with no missing methods
or failed checks. Instruction evidence and check results are saved as
`out/research/vmos/analysis/app-import-dex.txt` and `app-import-checks.json`.
The guest-helper manifest results are in `app-import-rom-helper.json`.
Only the analysis tool/documentation changed; no APK rebuild was required.

## Full-screen interaction

**DEX derived, same 2.0.0-alpha09 artifact.** `VmFragment.H` checks that the
VM is running, then `S` opens `VmRenderActivity` with activity/task flags.
That Activity contains a guest surface and a temporary transition image;
its view binding declares no permanent host toolbar. `G` reapplies configured
guest width/height rather than changing guest resolution to the preview size.
`z` sets system-UI flags 5894 (immersive sticky, hide navigation, full screen,
layout stable and layout hide navigation), also reapplied on window focus.

`VmRenderActivity$m$a.invokeSuspend` hides the live surface, displays its
captured image and runs a centered circular reveal/conceal for 300 ms with a
decelerating interpolator. Back forwards Android key 4 when the SDK's `p.i()`
check allows it; otherwise it returns through the exit transition. The full
semantics of that SDK/engine check were not recovered. `onUserLeaveHint`
consults a PiP setting, uses the surface aspect ratio, and enters Android PiP.
The PiP API calls go through synthetic helper classes `f` and `h`, which are
included in the direct DEX checks rather than relying on JADX's inlined output.

Validation:

```sh
out/research/vmos/venv/bin/python tools/inspect_vmos_fullscreen.py out/research/vmos/vmos-official.apk out/research/vmos/analysis
```

Passed: ten selected methods verified directly in DEX. Private instruction
listings are `fullscreen-dex.txt` and `fullscreen-checks.json` under that
analysis directory. This establishes static behavior for the inspected APK;
VMOS was not installed or executed, and the installed phone version remains
uncompared. Pocket implements its own viewer and in-window movable control
menu. No VMOS code, graphics assets or binaries are included in its APK.

## Embedded display page

**Binary XML and DEX derived, same pinned APK.** `FragmentVmBinding.d` selects
resource `0x7f0b003d`, resolved through the APK's resource table to `res/wO.xml`.
It contains a compact icon header and a constraint-sized CardView. The guest
TextureView matches the card width and height. This layout has no Spinner or
text Button rows. Separately, `VmFragment.onViewCreated` assigns the card's
`dimensionRatio` from configured guest width/height. Its parent is a page
background, rather than a large black display slot surrounding a narrow image.

```sh
out/research/vmos/venv/bin/python tools/inspect_vmos_display_page.py out/research/vmos/vmos-official.apk out/research/vmos/analysis
```

Passed: nine checks of the binary layout and the aspect-ratio assignment in
DEX. The private artifacts are `fragment-vm.xml`, `display-page-dex.txt` and
`display-page-checks.json`. No target code was executed. Pocket 0.8 adopts the
aspect-ratio card, a focused Android page and a separate Settings activity;
its implementation and visual assets are independently written.

## What ADB activation changes in the inspected path

**Binary derived, cross-checked against DEX instructions.**
`VmActivationViewModel.u` sends this command through its embedded ADB client:

```sh
device_config put activity_manager max_phantom_processes 1024
```

The DEX constructs it from a constant command prefix plus the integer 1024;
JADX folds the concatenation into one string. It then records that value in
preferences and stops its pairing/discovery helpers. Other methods check
developer options, wireless debugging and phantom-process configuration.

This verifies one concrete activation operation. It does not prove that every
ADB-capable feature executes only that command, revoke the pairing key, or
audit the lifetime and scope of every helper. No activation command was run
during this analysis.

## Implications for AOSP Pocket

1. Keep executable loading, guest syscall adaptation, IPC and hardware bridging
   as explicit runtime components, with source builds and independent tests.
2. Establish a source-built guest executable with absolute guest paths and a
   small verified syscall surface; merely running Toybox from a private cwd
   does not establish that runtime.
3. Demonstrate a private service transaction and FD passing before attempting
   a full Android framework. Investigate existing AOSP Binder RPC as well as
   the compatibility requirements of older Binder clients.
4. Bring up adapted init/property/logging services and ART incrementally, then
   a host `Surface` display and input path.
5. Add camera, audio and sensors through explicit bridges after the basic
   framework works. Keep guest root semantics separate from actual host UID.

These are interface and design observations. No VMOS decompiled implementation
or proprietary library has been copied into the open-source runtime.

## Reproduction and validation

An additional targeted review of Android 9 Binder caller identity/token handling
reopened the existing Rizin project without full reanalysis. See
[API 28 bring-up](api28-bringup.md#vmos-reference-analysis) and finding VMOS-011.
The independently implemented API 28 bridge uses AOSP and Ananbox sources;
its real VM transaction/FD/virtual-UID tests are separate from VMOS static
analysis.

Acquired files, decompilation, symbol inventories and three reusable Rizin
projects are under `out/research/vmos/analysis/`. Machine-readable findings are
in [vmos-findings.json](vmos-findings.json).

```sh
uv venv --python /usr/bin/python3.12 out/research/vmos/venv
uv pip install --python out/research/vmos/venv/bin/python androguard==4.1.4 unicorn==2.1.4
out/research/vmos/venv/bin/python tools/inspect_vmos.py \
  out/research/vmos/vmos-official.apk out/research/vmos/analysis
out/research/vmos/jadx/bin/jadx -r -j 4 \
  -d out/research/vmos/jadx-output out/research/vmos/vmos-official.apk
out/research/vmos/venv/bin/python tools/emulate_vmos_seccomp.py \
  out/research/vmos/analysis/extracted/assets/libprelinker64.so \
  out/research/vmos/analysis/seccomp-emulation.json
```

The inventory command passed. JADX 1.5.6 produced usable relevant classes but
reported 155 errors elsewhere; the decompilation is not complete. The activation
operation was additionally checked directly in DEX instructions. Native loader
and Binder findings were checked in ELF symbols and disassembly, not inferred
only from Java names. Unicorn recovered the 39-instruction filter and all five
focused checks passed. These results are not promoted to hardware-verified.

Existing projects can be reopened without repeating analysis:

```sh
rizin -p out/research/vmos/analysis/prelinker64.rzdb
rizin -p out/research/vmos/analysis/ylinker64.rzdb
rizin -p out/research/vmos/analysis/guest-libbinder.rzdb
```

No application runtime code changed in this analysis, so an AOSP Pocket rebuild
was unnecessary. Its APK and earlier VM validation remain separate artifacts.
