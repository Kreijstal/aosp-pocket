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
or provide device-node whiteouts. Modules requiring `sepolicy.rule` or Riru
are skipped. Arbitrary module-issued mount syscalls remain subject to host
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
- PENDING: complete Android UI boot and Magisk manager UI with the 0.25 APK.

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
