# Experimental selected-syscall profiles

The ARM64 adapter provides process-local, opt-in path translation for `openat`.
Pocket 0.30 removes the experimental translator from its core. Users install
`pocket-syscall-profiles-0.1.zip` through an optional Zygisk provider. The module
uses the public Zygisk API; its policy and syscall handler live in the module.
No provider, module, or profile is bundled in the Manager APK. Processes without
a profile retain the existing backend. Existing preload wrappers remain in use;
this does not make their redirects or host paths universally invisible.

## Current behavior

`pocket_syscall_profile_install()` selects `POCKET_SYSCALL_OFF` (no additional
filter), `POCKET_SYSCALL_OPENAT_PREFIX`, or an explicitly chained variant.
The caller supplies one
absolute guest prefix and its absolute backing prefix. Select a profile once
in a single-threaded child before running app code or creating worker threads.
The module selects an exact guest process name in `preAppSpecialize`, before
app worker threads start. It excludes system_server and guest UIDs below 10000.
An invalid explicitly selected profile stops that child rather than silently
launching it with an unintended backend. There is no manager UI for profiles.

The prefix profile installs an additional seccomp filter with
`PR_SET_NO_NEW_PRIVS`. It traps `openat` calls from both libc and inline AArch64
`svc` instructions. The SIGSYS handler translates matching paths and executes
the operation through an instruction-address-specific syscall gate. Other
syscall numbers pass through the added filter, except exec calls as described below. Existing host filters and
SELinux restrictions remain in effect.

Every intercepted `openat` incurs handler work, including calls outside the
configured prefix. BPF cannot examine the pathname string itself. The handler
uses `/proc/self/mem` and `pread64` to safely copy the pathname, without malloc
or shared locks. It opens self/mem anew on each trap, so forked children read
their own memory. Host access to self/mem is required; unsupported hosts have
not been tested. No performance improvement has been measured.

The standalone non-chaining mode refuses an existing SIGSYS handler with EBUSY.
App integration explicitly uses chaining: its own traps carry a marker, and
unrelated SIGSYS events are forwarded to the prior runtime handler. Signal masks
and alternate-stack/restart/nodefer flags are preserved. Ignored or one-shot
prior handlers are unsupported. With no prior handler, unexpected traps terminate
the process. The
profile is inherited by subsequently created threads and forked children.
Changing profiles requires a fresh process. The profile rejects execve/execveat
with EOPNOTSUPP because the filter survives exec but the handler does not. Host
filters can impose stricter restrictions.

## Selecting an app process

Use the paired Pocket owner console to create this guest directory:

```sh
python pocketctl.py exec /system/bin/toybox mkdir -p /data/adb/pocket-syscall-profiles
```

A file named `/data/adb/pocket-syscall-profiles/com.example.app.profile`
selects the exact process `com.example.app`. A secondary process needs its own
file, such as `com.example.app:worker.profile`. Each file has exactly three
newline-terminated lines:

```text
openat-prefix-v1
/data
/data
```

The second line is the guest prefix to translate; the third is the backing
prefix **in the guest namespace**, not an unrestricted host path. The existing
resolver maps that backing path into Pocket's image and the loader verifies
that it is inside the image before enabling interception. The backing directory
must be resolvable when the process starts. This example supplies direct openat
calls with access to the same guest `/data` tree as existing libc wrappers.

Upload the profile:

```sh
python pocketctl.py push example.profile /data/adb/pocket-syscall-profiles/com.example.app.profile
```

Install the ZIP with the guest's optional Magisk runtime (`magisk --install-module
/data/local/tmp/pocket-syscall-profiles-0.1.zip`), enable Zygisk, and restart the
guest to load it. No profile is enabled by installation alone.

Start a fresh app process to apply a profile; stopping and starting
Pocket's Android 9 guest also works. Remove the profile and restart the process
to return to the default backend. Disabling the module and restarting the
guest also restores the default backend while preserving profiles. Already-installed filters cannot be disabled
in place. No app profiles are bundled or automatically enabled for other apps.

## Limits

This is a path-translation prototype, **not a filesystem security boundary**.
Unmatched and relative paths pass through. Lexical `..` escapes under the
translated prefix are rejected, but symlink containment is not implemented.
The syscall gate is available inside the process and is not an isolation
mechanism. Do not use this adapter to run untrusted applications without
independent containment.

The profile covers only `openat`. It does not virtualize `openat2`, metadata,
directory enumeration, `readlink`, `/proc` contents, identities, Binder, or
other interfaces. It does not make host paths universally invisible or establish
that an application accepts the environment. It does not replace Pocket's
current preload wrappers. ARM32 and x86_64 app interception are unsupported.

## Build and focused validation

```sh
make -f runtime/Makefile syscall-profile-probe
rsync --timeout=20 out/syscall-profile/syscall-profile-probe tools/test_syscall_profile_on_pi.py tools/pocketctl.py pi@juanraspberry:/home/pi/
ssh pi@juanraspberry 'python3 -u /home/pi/test_syscall_profile_on_pi.py'
```

The Pi harness requires a running, paired Android 9 Pocket guest and its
existing private owner-console config at `~/.config/pocket-pi-debug.json`.
It verifies the Cuttlefish host is virtual and uses Pocket's authenticated
console. It neither restarts ADB nor uses host root for the test. Optional
arguments select another paired config, probe, fixture parent, or report path.

The probe statically links NDK Bionic so the existing preload wrappers cannot
implement the test's virtual path instead of the new syscall adapter. Each
case forks a separate child to select its profile. The fixture backing paths
reside in Pocket's existing host-app-private writable directory.

On 2026-10-21 all eight cases passed inside Pocket on the Pi's LineageOS
ARM64 KVM VM, with actual kernel UID 10177:

| Case | Verified behavior |
| --- | --- |
| off | Real file opens; virtual alias remains absent |
| libc-openat | Libc openat accesses the real fixture via its virtual alias |
| direct-svc-openat | Inline SVC accesses the same fixture via the alias |
| pass-through | Relative dirfd, unmatched absolute paths and getpid work |
| errors-and-flags | ENOENT, EFAULT, ENOTDIR, O_NOFOLLOW, prefix boundaries, lexical escape rejection, profile immutability and clean exec rejection |
| thread-and-fork-inheritance | Four concurrent threads and a forked child use the inherited profile |
| existing-sigsys-handler | Installation refuses to replace a prior handler |
| chained-sigsys-handler | An unrelated SIGSYS reaches the prior handler; selected openat still translates |

The native probe returned 0 with `SYSCALL_PROFILE_RESULT failures=0
kernel_uid=10177`. Compilation used `-Wall -Wextra -Werror` and passed. The
harness passed `python -m py_compile tools/test_syscall_profile_on_pi.py`.
Structured device output is in `syscall-profile-validation.json` beside this
document.

Source: `runtime/modules/syscall-profiles/` and `runtime/tests/syscall_profile.c`.

## App integration build and test

```sh
python tools/build_syscall_profiles.py
# One-time migration from the experimental core integration:
python tools/build_syscall_profiles.py --base-apk out/manager-0.29/aosp-pocket-manager-0.29.apk --version 0.30
```

The default command builds the optional module ZIP and JNI fixture APK without
rebuilding Pocket. The migration command additionally packages a clean Manager
0.30, removes the retired translator asset, and verifies that the core zygote
contains no translator startup hook. `--test-only` rebuilds just the fixture and its
control helper. Its manifest declares two separate processes and the builder
checks their encoded Android resource IDs; `PocketManifest` now explicitly
supports the platform `process` attribute (0x01010011).

The guarded Pi harness `tools/test_syscall_app_on_pi.py` stages a profile only
for `dev.kreijstal.pocketsyscalltest:adapted`. The APK starts a worker thread
that opens the same real fixture through libc and through an inline SVC, writes
its result, then launches a second activity in the unselected `:baseline`
process. Both processes use the same guest UID and ordinary host app UID.
The baseline must succeed through the existing libc wrapper and fail through
the direct syscall; the selected process must succeed through both routes.

On 2026-10-21 this comparison passed in Pocket 0.29: the selected process
reported `libcOk=true`, `directOk=true`, and errno 0. The unselected process
reported `libcOk=true`, `directOk=false`, and direct errno 2 (ENOENT). Both
reported guest UID 10019 and actual kernel UID 10177. Structured results are
in `syscall-app-validation.json`. The tests caught and corrected a missing
manifest process resource ID and the need to chain the existing runtime's
SIGSYS handler before the final successful run.

Copy the fixture APK, control executable, harness and pocketctl.py to the Pi.
With an already paired console, run:

```sh
ssh pi@juanraspberry 'python3 -u /home/pi/test_syscall_app_on_pi.py --install-manager /home/pi/aosp-pocket-manager-0.30.apk'
```

`--install-manager` stops only the Pocket guest, updates the host APK through
the outer VM's ADB, and re-enables the existing owner endpoint. That setup uses
outer-VM root to start the host's administration service, while the app probe
itself runs under the ordinary host app UID. Subsequent tests can omit the
installation option. By default the dedicated fixture is reinstalled and its two old reports removed.
After a guest restart, `--launch-existing` instead starts the already installed
fixture and still removes both old reports. This avoids a package reinstall when
the outer VM is below Android's free-space threshold; use it only after stopping
the old fixture processes (a guest restart does that).

## Provider compatibility

The user-installed Pocket Magisk fork provides the existing cooperative Zygisk
entry point. It remains separate from the core APK. The optional module targets
ARM64 Pocket Android 9 and uses Pocket's existing path resolver; it is not a
general standalone Android sandbox.

The NeoZygisk fork is published at https://github.com/Kreijstal/NeoZygisk. Its
daemon was executed under Pocket's actual UID 10177: the version command worked,
but startup failed opening `/proc/1/ns/mnt` with EACCES before loading modules.
It needs a cooperative provider adapter to work inside rootless Pocket. See the
fork's `docs/pocket-compatibility.md`. No kernel namespace operation is faked.

The 0.29 app results above are historical validation of the translator. Current
module lifecycle results are recorded separately for absent, enabled, and
disabled states. The `--expect-disabled` harness flag requires both processes
to retain baseline direct-syscall behavior, even when the profile file exists.

## Optional provider startup ordering

Full UI validation exposed a provider startup race: upstream Magisk's
`--post-fs-data` command waits at most 40 seconds and then returns success even
when the daemon is still building Pocket's merged private tree. The guest then
started zygote before `libpocket_zygisk.so` existed. The Pocket Magisk fork now
waits up to 110 seconds and returns failure on timeout. The existing integration
script keeps its 120-second outer deadline and continues guest boot without
promising module hooks when preparation fails. Ordinary Android retains its
upstream timeout. The provider fix is commit `a93ef54` on
https://github.com/Kreijstal/Magisk/tree/pocket-api28. The fix belongs to the optional provider; the Manager APK
does not gain a module-specific startup hook.

On the Pi, Pocket 0.30 passed the module-absent and module-disabled real APK
checks with the adapted profile still present. Both processes returned
`libcOk=true`, `directOk=false`, direct errno 2, guest UID 10019 and actual kernel
UID 10177. With the module loaded, the selected process instead returned
`directOk=true`, errno 0; the unselected process stayed unchanged. Reports:
`syscall-module-absent.json`, `syscall-module-enabled.json`, and
`syscall-module-disabled.json`. The first enabled report was collected after
restarting zygote; the subsequent cold-boot report validates the updated provider
startup path.

Focused lifecycle commands (after the corresponding restart):

```sh
python3 /home/pi/test_syscall_app_on_pi.py --expect-disabled
python3 /home/pi/test_syscall_app_on_pi.py --launch-existing
python3 /home/pi/test_syscall_app_on_pi.py --launch-existing --expect-disabled
```

The final cold boot with the updated optional provider also passed.
`syscall-module-enabled-coldboot.json` records the fresh two-process result
after re-enabling the module. The Pi instance was left running with the module
enabled. No app profiles other than the dedicated fixture were enabled.
