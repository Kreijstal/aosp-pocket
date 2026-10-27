# Experimental Magisk userspace support

The Magisk fork is at <https://github.com/Kreijstal/Magisk/tree/pocket-api28>,
based on upstream Magisk 30.7, commit
`e8a58776f1d7bdf852072ad0baa6eceb9a1e4aac`.

Pocket provides compatibility adapters for an optional Magisk userspace runtime
inside the Android 9 guest. The distribution APK contains no Magisk core,
Magisk Manager, BusyBox, or upstream module installer. No Samsung boot partition, host kernel, SELinux policy or
host root installation is changed. Logical guest root remains confined to the
manager app's actual kernel UID and SELinux domain; it is not an additional
security boundary against guest code issuing raw syscalls.

## Reproducible build

```sh
make -f runtime/Makefile magisk-pocket
make -f runtime/Makefile magisk-pocket-manager
```

The second builds the support-only 0.25 Pocket APK without fetching or compiling
Magisk. The first separately builds the optional test/runtime archive,
`out/magisk-pocket/pocket-magisk-optional-runtime.zip`, and device-test artifacts.
Neither is embedded in the distribution APK. ONDK r29.5 builds that optional
Magisk source and its submodules. Versioned
Termux ARM64 BusyBox, libandroid-selinux and PCRE2 packages provide dynamically
linked shell tools that can use Pocket's path translation. The stock static
Magisk BusyBox cannot do so. Dependency license notices accompany the optional
archive. A Magisk Manager app is a separate user installation.

`runtime/magisk/magisk-pocket.patch` contains the corresponding source changes;
`tools/build_magisk_pocket.py` fetches, builds and prepares the exact assets.
No administration token, attestation signing identity or private configuration
is bundled or committed.

## Boot and recovery

After data preparation, the guest starts the Magisk preparation service, a
foreground init-supervised daemon and post-fs-data handling before zygote.
That daemon starts only when an external runtime has been installed at
`/data/adb/magisk/magisk`. Otherwise initialization proceeds directly to zygote,
without exposing a stale Magisk CLI. The current optional runtime is a Pocket
adaptation; this does not make the stock boot-image installer work unmodified.
Late-start scripts follow, with the boot-complete callback after Android boots.
Module state remains under `/data/adb/modules` and `/data/adb/modules_update`.
The actual manager and module protocols are retained.

Preparation enables owner writes on the private guest property-area files.
Kernel root normally overrides their read-only permissions; Pocket's logical
root cannot. This lets real `resetprop -n` map the guest areas for writing.
It does not change host property files or provide isolation between guest
processes sharing the same host app UID.

Guest owner-console recovery remains available while Android is stopped:

```sh
python pocketctl.py exec -- /system/bin/toybox touch /data/adb/pocket-magisk.disabled
```

Restart the guest to boot without Magisk. Remove that file to re-enable it.
This preserves modules and their data. Diagnostics are in `/logs/magisk.log`.

With the guest stopped, the authenticated owner can also recover console access
without executing any guest binary:

```sh
python pocketctl.py reset-module-view
```

This clears the active view marker. Module files remain installed and can be
disabled or repaired from the restored base console before the next boot.

## Guest Zygisk and file overlays

Zygisk follows Magisk's existing database setting. Pocket explicitly installs
the actual upstream app/server specialization wrappers after registering its
Android 9 JNI backend. The adapter reuses ART's native-method introspection
callbacks, with offsets and instruction bytes generated from the pinned image
and checked before use. The upstream module ABI, daemon protocol, JNI/PLT APIs
and module callbacks are retained. The library remains mapped in forked apps;
the native-bridge-specific self-unloader is not used in this backend.

Magic Mount compatibility is provided by a merged private filesystem view,
published before zygote starts. It merges module `system/` files, routes
`system/vendor/` to the guest vendor tree, and supports `skip_mount`, symlinks
and `.replace` directories. The resolver presents that tree to guest file
operations, directory enumeration, executable lookup and dynamic loading.
The base image is preserved; disabling a module removes its changes on restart.
Hard links are attempted for baseline files, with private copies when host
policy rejects links. This fallback can add approximately one guest image's
worth of storage and boot-time copying.

These are userspace compatibility mechanisms. They do not create kernel mounts
or namespaces, implement kernel modules, load host SELinux policy, support Riru,
or provide device-node whiteouts. Riru modules are skipped. Modules shipping
`sepolicy.rule` load; their rules are stored but not applied (guest SELinux is
disabled). Arbitrary module-issued mount syscalls remain subject to host
permissions. Full third-party module and Specter compatibility is unverified.

## Physical-device validation

On the unrooted Samsung SM-G780G, using the paired `pf-phone2` owner console:

- PASS: actual daemon version handshake and privileged database query.
- PASS: `magisk su -c '/system/bin/toybox id -u'` returns logical guest UID 0.
- PASS: an ordinary guest UID is denied the privileged database API.
- PASS: official Magisk module installer extracts the offline fixture ZIP into
  `modules_update`; the next boot upgrades it and executes both module scripts.
- PASS: real FIFO read/write and `/proc/self/cwd`/`exe` traversal regression tests;
  the actual kernel UID stays unprivileged.
- PASS: `resetprop -n` creation/update and direct deletion are visible through
  the ordinary guest libc `getprop` client. A module's post-fs-data property
  is visible to its service script after restart.
- PASS: module file replacement/addition and directory enumeration, followed
  by restoring the baseline after disabling the module and restarting.
- PASS: actual Android 9 zygote app fork, Zygisk `onLoad`, pre/post app
  specialization, and `hookJniNativeMethods` interception of `Log.isLoggable`.
- PASS: initialization without an installed Magisk core; no stale CLI remains.
- PASS: support-only 0.25 APK signature and embedded-runtime exclusion checks.
- PASS: installed support-only 0.25 reaches `sys.boot_completed=1`, displays
  `com.android.launcher3/.Launcher`, and processes `BOOT_COMPLETED`. The optional
  core and `/sbin/magisk` are absent; `pocket.magisk.status=absent`.
- PENDING: full Android UI boot with an externally installed backend and Magisk
  manager UI.

The focused device test is `tools/test_magisk_pocket.py`, run in phone Termux
beside the generated payload tar, fixture ZIP and native test clients. It uses
a small actual AOSP init profile, the real Magisk daemon and no network or
attestation keys. The complete focused test passed on 2026-10-21; the receipt
is summarized in `docs/magisk-pocket-validation.json`. This test does not
establish full Android UI integration.

`tools/test_magisk_features.py` separately exercises overlays and the actual
AOSP zygote with an externally installed offline Zygisk fixture. It checks file replacement/addition,
directory enumeration, rollback after disabling the module, app specialization
callbacks and the public JNI-hook API. Its minimal zygote profile matches
production's JDWP setting and console handling: stdin is `/dev/null`, and output
uses `/pocket-console`. Piped stdin becomes a duplicated Java descriptor that
Android 9's fork checks correctly reject.

The direct runtime fixes keep host `/proc` magic-link traversal in the host
tree and translate dynamically called FIFO-creation functions into the guest
filesystem. FIFO creation still uses the real kernel operation; character and
block devices retain the host's actual permission checks.

## Native-loader backend

The Manager's native backend runs unchanged Bionic through `pocket-loader`, so
the resolver above no longer exists. Version 0.45 restores the adapter there:

- The loader maps guest `/system` and `/vendor` into `/.pocket-module-view`
  once Magisk writes its `ready` marker, and hides that prefix from `getcwd`,
  `readlink` and socket names. Each process checks the marker at start and on
  exec, so init's later services see a view published after it started.
- The native image imports `init.magisk.rc` again. Without an installed core,
  `setup.sh` reports `absent` and releases zygote unchanged.
- The Magisk fork detects the loader through its `getpid` probe rather than
  `POCKET_DIRECT_ROOT`. Daemon peers are checked against the kernel UID from
  `/proc/self/status`; logical credentials come from the loader's
  `/.pocket-identity/<pid>` record, matched against the process start time.
- The loader execs only ELF files with a reserved interpreter slot.
  `build_magisk_pocket.py` reserves one in `magisk` and `busybox`; the Manager
  binds `/data/adb/magisk/{magisk,busybox}` to the instance loader on each
  prepare. The Termux libraries carry their own RUNPATH, because Bionic does
  not inherit it and this backend has no global library path.
- Two loader fixes were required: a zero-length `sendmsg`/`sendto` address is
  now treated as none, as the kernel does (Rust's std passes one when sending
  descriptors), and `/dev/tmp`, the installer's scratch directory, is
  guest-private rather than the host `/dev`.

On the Pi-hosted Cuttlefish VM (`docs/magisk-native-validation.json`):

- PASS: boot without a core, `pocket.magisk.status=absent`; with the core,
  `ready`, and post-fs-data, late-start and boot-complete stages run.
- PASS: `magisk su -c 'toybox id -u'` returns 0.
- PASS: the actual `magisk --install-module` installs a module; after restart its
  `system/etc` file is readable and listed, `realpath` shows `/system/...`, both
  module scripts ran as logical root, and the base image is unchanged.
- PASS: disabling the module removes the view and the file on the next boot.
- PASS (0.52): Zygisk with Pocket's NeoZygisk provider. The native-loader
  zygote previously lacked the `pocketInstallZygisk` call
  (`tools/build_init_loader.py` now builds it with `-DPOCKET_ZYGISK`, as
  `build_boot_zygote.py` does). The provider gate also accepts the loader's
  getpid probe, not only `POCKET_DIRECT_ROOT`. Logged:
  `NEO_POCKET_READY methods=6`, `POCKET_ZYGISK_INSTALL result=0`. A Zygisk
  fixture module that ships a `sepolicy.rule` got `onLoad`, `preServer
  target=1000` and `postServer uid=1000` in system_server, and `onLoad` in
  each forked app, including the Magisk app.
- UNVERIFIED: physical phone.

### Magisk app (0.50)

magiskd trusts the Magisk app only if its certificate matches `stub.apk` in
`MAGISKTMP`; without it, a release daemon treats every app as mismatched.
The optional runtime now carries the `assets/stub.apk` of the official v30.7 app
(digest pinned in `build_magisk_pocket.py`), and `setup.sh` copies it to
`/sbin` before post-fs-data. The guest has no `pm`, so the app was imported
from the outer VM through the Manager's Install APK picker.

Getting the official app past its splash screen needed these runtime fixes:

- Zygote unsets `LD_PRELOAD` after mapping the Binder/HIDL shims, so apps no
  longer pass them on to `su` and shells. Each exec previously loaded 86
  platform libraries first (about 430 ms), and libsu's 2 s timeout killed `su`.
- A boot with CE storage locked no longer destroys user 0's data. `CeStorage`
  mirrors the xattrs of the CE roots and answers `getxattr` from the mirror
  while locked. Previously `UserDataPreparer` got `EACCES` and PackageManager
  removed `/data/user_de/0`, so magiskd could not resolve the app's UID.
- The image now links the toybox applets (`id`, `ls`, …) that libsu runs.
- `/system/bin/app_process64` restores the shims and starts the guest
  app_process for the app's root service.
- The loader runs unprepared ELFs, such as the app's static
  `libbusybox.so`, in which libsu execs its root shell (see `exec-loader.md`).

On the Pi VM (Manager 0.50), Home shows Magisk installed 30.7 (30700) with
Ramdisk Yes, and the Superuser and Modules tabs work.

The optional runtime also installs the boot image tools a release install keeps
in `/data/adb/magisk`: `magiskboot`, `magiskinit`, `magiskpolicy`, `init-ld` and
`boot_patch.sh`, taken from the pinned official app. `util_functions.sh` now
quotes `MAGISK_VER` as upstream does. In the guest, `magiskboot` and
`magiskpolicy` run, and the app's `env_check 30.7 30700` passes the file and
version checks.

Its last check wants the `preinit` block device, where a phone keeps module
`sepolicy.rule` files. Pocket emulates that partition: `setup.sh` links
`/sbin/.magisk/preinit` to the persistent `/data/adb/pocket-preinit` and creates
the node `/sbin/.magisk/device/preinit` as a regular file, which the loader's
`stat` reports as a block device of the backing filesystem. The installer's
`copy_preinit_files` writes there. The guest runs with SELinux disabled, so
stored rules restrict nothing, and magiskd loads modules that ship a
`sepolicy.rule`, logging that the rules are ignored. Only modules with a `riru`
directory are skipped (`collect_modules` in the Pocket patch). Pocket's
NeoZygisk adapter module ships no rules.

The native loader's DenyList view (0.54) resolves inherited directory descriptors
and relative/proc aliases before applying its per-process policy.
Starting guest programs through the owner console while the Manager prepares
the image can fail with `ETXTBSY`, because prepare rewrites `system/bin` in place.

### Per-process DenyList (0.54, NeoZygisk Pocket provider 0.2)

Pocket has no kernel module mounts to undo. The provider reads Magisk's existing
`denylist` table and `/data/system/packages.list` before app specialization,
reports `PROCESS_ON_DENYLIST` to modules and selects the loader's base view in
the forked child after pre-specialization callbacks. Selection follows Neo's
normal behavior: Magisk's separate **Enforce DenyList** switch may remain off.
Enable guest Zygisk and restart Pocket after installing/upgrading the provider;
restart affected apps after changing their DenyList selection.

Selected processes resolve system/vendor files from the base image. The loader
returns ENOENT for the Magisk/su entry points, `/sbin/.magisk`, `/data/adb` and
direct access to `/.pocket-module-view`. Normalized paths, inherited directory
fds, proc aliases and symlink targets use the same policy; directory reads filter
the hidden names. A private monotonic loader control is accepted only before
guest UID 0 is dropped. Its state survives fork and exec without changing the
zygote parent's view. The Zygisk `FORCE_DENYLIST_UNMOUNT` option selects this
userspace view as well.

Maps/smaps use the same guest system/vendor path names as readlink, removing the
module-view backing prefix without removing mappings or altering kernel fields.
Already-loaded module/provider code can remain visible. This supplies a guest
path view, not kernel namespaces, host UID isolation or hardware attestation.
Companions and kernel namespace operations remain unsupported.

Pi 0.54/provider 0.2 validation selected the real Attest Probe main process
with Magisk enforcement disabled. Public flags changed from 0 to
`PROCESS_ON_DENYLIST` (2); `root_su_paths` and `magisk_paths` changed from FAIL
to PASS. The selected run reported 46 PASS and 2 FAIL (test keys and absent
GMS), with no module-view prefix in proc maps. The first run's contacts timeout
did not recur in the warm selected run; contacts code/timeouts were unchanged.
The harness restored the app's original selection and enforcement setting,
removed its temporary fixture module, and verified owner Magisk access. Guest
Zygisk, initially disabled, was enabled to activate the provider. Encrypted
storage, codec regression and password UI unlock also passed. See
[denylist-validation.json](denylist-validation.json).


### Persistent Attest configuration and slow module-view startup

Attest Probe is now persistently selected in the Pi guest's DenyList:

```sh
/sbin/magisk --denylist add dev.kreijstal.attestprobe dev.kreijstal.attestprobe
```

NeoZygisk provider 0.2 is active with guest Zygisk enabled; Magisk's separate
mount enforcement remains off. Selection applies to the next Attest Probe
process; this validation also restarted the guest for the startup fix below.
Unlike the original comparison harness, this configuration keeps its selection.

A later cold boot revealed why selection alone could still fail: Magisk's
post-fs-data client timed out after 120 seconds while the private module tree was
still being copied. Boot continued, and zygote logged `POCKET_ZYGISK_LOAD_FAILED`
because the provider library was not yet available. `runtime/magisk/stage.sh`
now waits up to 300 seconds. The guest reboot then logged
`POCKET_ZYGISK_INSTALL result=0`, and Magisk status became `ready`.

The live patch preserves the installed script's exact length, including its
final newline, because image preparation checks the cached asset stamp and file
size. Normal packaged updates carry the new script directly.

On the running audio 0.62 build with that startup fix, the unchanged real probe
reported `root_su_paths=PASS` and `magisk_paths=PASS`. Total: 46 PASS, 2 FAIL,
3 SKIP, 8 INFO. The remaining failures are test-keys and the Contacts timeout.
Owner-console Magisk access and the existing password UI unlock still pass.

A source-packaged candidate, 0.62.1 (versionCode 62), preserves audio 0.62's host
DEX and all native payloads. Only the stage script and version manifest differ.
See [neozygisk-config-validation.json](neozygisk-config-validation.json) for
commands, checksums and the distinction between the live fix and packaged APK.
