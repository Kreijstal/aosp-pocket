# Experimental Magisk userspace support

The Magisk fork is at <https://github.com/Kreijstal/Magisk/tree/pocket-api28>,
based on upstream Magisk 30.7, commit
`e8a58776f1d7bdf852072ad0baa6eceb9a1e4aac`.

Pocket runs its actual native daemon, CLI, MagiskSU and module installer inside
the Android 9 guest. No Samsung boot partition, host kernel, SELinux policy or
host root installation is changed. Logical guest root remains confined to the
manager app's actual kernel UID and SELinux domain; it is not an additional
security boundary against guest code issuing raw syscalls.

## Reproducible build

```sh
make -f runtime/Makefile magisk-pocket
make -f runtime/Makefile magisk-pocket-manager
```

The first prepares guest payloads; the second also packages a complete numeric
0.23 Manager APK. Both passed on 2026-10-21. The full APK passed v3 signature
verification. ONDK r29.5 builds the pinned Magisk source and submodules. Versioned
Termux ARM64 BusyBox, libandroid-selinux and PCRE2 packages provide dynamically
linked shell tools that can use Pocket's path translation. The stock static
Magisk BusyBox cannot do so. Dependency license notices are bundled in the guest.
The official, unchanged Magisk 30.7 manager APK is included as a guest system app.

`runtime/magisk/magisk-pocket.patch` contains the corresponding source changes;
`tools/build_magisk_pocket.py` fetches, builds and prepares the exact assets.
No administration token, attestation signing identity or private configuration
is bundled or committed.

## Boot and recovery

After data preparation, the guest starts the Magisk preparation service, a
foreground init-supervised daemon and post-fs-data handling before zygote.
Late-start scripts follow, with the boot-complete callback after Android boots.
Module state remains under `/data/adb/modules` and `/data/adb/modules_update`.
The actual manager and module protocols are retained.

Guest owner-console recovery remains available while Android is stopped:

```sh
python pocketctl.py exec -- /system/bin/toybox touch /data/adb/pocket-magisk.disabled
```

Restart the guest to boot without Magisk. Remove that file to re-enable it.
This preserves modules and their data. Diagnostics are in `/logs/magisk.log`.

This userspace integration does not implement Magic Mount, kernel modules,
mount namespace changes, SELinux policy loading, Riru or Zygisk. Module
collection skips modules requiring system overlays, sepolicy rules or injection.
Complete Specter compatibility has not been demonstrated.

## Physical-device validation

On the unrooted Samsung SM-G780G, using the paired `pf-phone2` owner console:

- PASS: actual daemon version handshake and privileged database query.
- PASS: `magisk su -c '/system/bin/toybox id -u'` returns logical guest UID 0.
- PASS: an ordinary guest UID is denied the privileged database API.
- PASS: official Magisk module installer extracts the offline fixture ZIP into
  `modules_update`; the next boot upgrades it and executes both module scripts.
- PASS: real FIFO read/write and `/proc/self/cwd`/`exe` traversal regression tests;
  the actual kernel UID stays unprivileged.
- UNRESOLVED: the fixture's `resetprop -n` write is not visible to the ordinary
  guest `getprop` client, so the full restart/property assertion currently fails.
- PENDING: complete Android UI boot and Magisk manager UI with the 0.23 APK.

The focused device test is `tools/test_magisk_pocket.py`, run in phone Termux
beside the generated payload tar, fixture ZIP and native test clients. It uses
a small actual AOSP init profile, the real Magisk daemon and no network or
attestation keys. The current failure is retained as a failing assertion;
it is not reported as a successful full integration test.

The direct runtime fixes keep host `/proc` magic-link traversal in the host
tree and translate dynamically called FIFO-creation functions into the guest
filesystem. FIFO creation still uses the real kernel operation; character and
block devices retain the host's actual permission checks.
