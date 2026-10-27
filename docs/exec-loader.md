# Native interpreter and exec continuity

This is the replacement direction for Pocket's temporary PRoot backend:
native ARM64 execution, a custom ELF interpreter/syscall layer, and unmodified
Bionic executable code. The implementation in `runtime/loader/` is a
native-process backend used by the native Manager builds. The original isolated
fixture runs the real Android 9 init core and zygote, including a forked Java
child. The Manager proc-view integration below also exercises the running
framework and an installed application.

Pocket 0.55 adds [scoped microG package-query compatibility](microg.md) in the
guest framework, including Android 9 signing-certificate APIs and the package
Binder supplied in app service caches. It is independent of Zygisk and keeps
installation and signature-permission checks on the APK's real signer.

## What runs

The kernel loads a small freestanding interpreter from the executable's prepared
`PT_INTERP`. It maps the pinned Android 9 linker, updates `AT_BASE` on the original
initial stack, and transfers control to that linker's entry point. The stock
linker loads stock Bionic and the executable normally. Neither stock linker's
nor Bionic's executable instructions are patched. Only the target executable's
interpreter field and the custom loader's own instance configuration are prepared.

The interpreter has no libc, dynamic dependencies, TLS accesses, preload library,
or tracing process. Its syscall handler therefore continues to work when the
stock linker initializes a different libc/TLS layout.

Additional seccomp filtering sends selected syscalls to an in-process SIGSYS
handler. Calls from the loader's fixed executable range bypass its own filter;
other host filters still apply. Unselected syscalls execute normally. This is
userspace compatibility inside the host app sandbox, not a new isolation or
privilege boundary.

Linux preserves filters across allowed fork/exec operations; see the
[kernel seccomp documentation](https://www.kernel.org/doc/html/latest/userspace-api/seccomp_filter.html).
Exec resets the handler and mappings, so the next prepared executable again
enters the interpreter at its fixed address. It installs the handler before
running the stock linker. A probe outside the bypass range detects the inherited
filter, avoiding another filter installation at every exec. The handler restores
the pre-trap signal mask before exec, retaining the application's blocked signals
without carrying a temporarily blocked SIGSYS into the next process.

Instance configuration lives in the custom interpreter's data segment rather
than environment variables. Empty-environment exec therefore retains the mapping
configuration. Guest credentials use a separate private exec-transport entry;
the interpreter consumes it before the stock linker runs, preserving the
application-visible environment, including an otherwise empty environment.
This transport is not authenticated or an isolation boundary.

## Current syscall coverage

Filesystem translation covers `openat`, `newfstatat`, `fstat`, `faccessat`,
`readlinkat`, `getcwd`, `chdir`, `mkdirat`, `unlinkat`, `renameat`/`renameat2`,
`linkat`, `symlinkat`, `fchmodat`, `fchownat`, `fchown`, `utimensat`, `truncate`
and `mknodat`. Absolute paths use the private backing root. Relative paths
retain kernel directory-FD/cwd semantics; `fchdir` works directly with the real
backing directory FD. Relative symlink targets remain relative; absolute targets
are translated when created. Readlink and getcwd reverse backing paths.

`/proc`, `/sys`, and most `/dev` nodes remain real host paths. Exceptions are
private `/dev/socket`, `/dev/__properties__`, and `/dev/input`; `/dev/hwbinder`
selects the real `/dev/binder` node for the separate Binder/HIDL bridge.
Pathname Unix sockets are translated for bind/connect, and getsockname/getpeername
reverse the pathname. Abstract and IP addresses retain their kernel ABI.
The original Pocket root is never bound into this fixture.

Boot mode starts with logical guest UID/GID 0. The get/set UID/GID and saved-ID
syscalls operate on guest state, inherited across fork and transported across
prepared exec. Guest ownership is recorded by device/inode in `.pocket-owner`
using the existing Pocket metadata format; stat reports those guest owners.
Fresh open/mkdir creations record the creator, and chown changes metadata only.
The actual kernel UID, capabilities, SELinux domain, supplementary groups,
`/proc` credentials and Unix peer credentials remain unchanged. This layer does
not enforce a new guest security boundary. Supplementary-group specialization
is still rejected by the rootless zygote profile.

Pathname copies use kernel-checked writes to a private pipe, bounded at page
boundaries. This returns EFAULT for unreadable/PROT_NONE pointers while allowing
a valid terminator immediately before an inaccessible page. Readlink replies
use a kernel-checked pipe read into the application's buffer, respecting invalid
and read-only destination pointers. There is no `/proc/self/mem` dependency.

`execve` validates that the target ELF selects this interpreter, then executes
the mapped pathname with the original argv/environment. Missing files retain
ENOENT. Scripts return ENOEXEC instead of selecting a host interpreter.
Unprepared AArch64 ELFs, such as the static busybox or the dynamic binaries an
app ships in its native library directory, are never handed to the kernel
either: the handler execs the loader itself as a static program and passes the
target in a private `POCKET_LOADER_EXEC_V1` environment entry, which the loader
removes. The loader then maps the target, rewrites `AT_PHDR`, `AT_PHNUM` and
`AT_ENTRY`, and jumps to the target's entry, or maps the pinned linker for a
target whose `PT_INTERP` is `/system/bin/linker64`. Any other interpreter
returns ENOEXEC. In such a process, `readlink("/proc/self/exe")` and
`execve("/proc/self/exe")` name the target rather than the loader. The source also has a flags-zero `execveat` route; the current live
regression exercises `execve`, not that route. `AT_EMPTY_PATH` is unsupported.

SIGSYS is reserved: replacing its handler returns EBUSY, and signal masks keep
it deliverable, including while other handlers run. Other signal dispositions
remain native. rt_sigprocmask updates the saved signal-frame mask, rather than
losing the requested change at sigreturn. Large pathname/exec scratch buffers
use private mmap allocations, keeping the dispatcher stack frame small enough
for nested traps on small alternate signal stacks. This adds allocation
cost to translated filesystem calls; no performance ratio is claimed.
The internal getpid marker is a filter-presence probe; ordinary getpid calls
remain native.

## Reproducible test

Existing NDK r27c, the staged ARM64 Android 9 reference libraries, and
`out/boot/platform/system/bin/{sh,toybox}` are required. The builder checks ELF
architectures, builds only this experiment under `out/exec-loader/`, and does
not invoke the broad native/LineageOS build.

```sh
python tools/build_exec_loader.py --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c
rsync -a out/exec-loader/loader-test.apk tools/test_exec_loader_on_pi.py pi@juanraspberry:/home/pi/
ssh pi@juanraspberry 'python3 -u /home/pi/test_exec_loader_on_pi.py'
```

The harness requires the known virtual Cuttlefish instance. It installs a
separate non-debuggable fixture package, `dev.kreijstal.aosppocket.loadertest`.
An Activity extracts only its own assets and launches the native program via
ProcessBuilder with a cleared environment. Execution never passes through the
existing Pocket guest or its owner console. Host root is used only to retrieve
the fixture's private report. ADB servers and the Pocket instance are not reset.

The native regression checks:

- Normal untrusted-app SELinux domain, actual app UID, zero effective capabilities,
  seccomp enabled, and TracerPid zero.
- No PRoot or Bionic-patching library in the process mappings.
- All 858,956 file-backed executable Bionic bytes match the original.
- Translated libc and inline-SVC file operations, directory FDs, metadata,
  access checks, readlink truncation and reverse translation.
- Invalid/PROT_NONE path pointers, a valid path at a page boundary, invalid and
  read-only readlink buffers.
- Concurrent threads and fork inheritance.
- A filesystem trap inside a signal handler on a guarded 16 KiB alternate stack,
  with a broad handler mask that must keep SIGSYS deliverable.
- Sixteen consecutive execs with preserved argv and signal masks, alternating
  ordinary and empty environments, and no filter-count growth.
- The actual prepared Android 9 shell and Toybox reading a physical fixture file.

Evidence: [live report](exec-loader-validation.json).

## Android 9 init and zygote test

The optional build uses the pinned AOSP init parser/action/service core and
app_process with the existing reviewed rootless JNI profile. These are rebuilt
from source for ARM64 in `out/exec-loader/init-zygote`; the x86 build output and
working Pocket instance are not replaced. Stock linker and Bionic are packaged
unchanged. No PRoot, Bionic wrapper, preload trampoline or executable-byte patch
is included in this route.

```sh
python tools/build_exec_loader.py --init-zygote --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c
rsync -a out/exec-loader/loader-test.apk tools/test_exec_loader_on_pi.py pi@juanraspberry:/home/pi/
ssh pi@juanraspberry 'python3 -u /home/pi/test_exec_loader_on_pi.py --boot'
ssh pi@juanraspberry 'python3 -u /home/pi/test_exec_loader_on_pi.py'
```

The Activity supplies ANDROID_ROOT and ANDROID_DATA, prepares only its own
private directories, and recreates its volatile property area for each run.
Stdio uses /dev/null and a regular private log, as required by the real zygote
FD whitelist. Init creates the property and zygote sockets, publishes Android 9
properties, and supervises zygote plus a diagnostic Android log receiver.
The log receiver implements only the logdw datagram input, not full logd.

The independent native client queries the actual ZygoteConnection ABI protocol,
requests a Java child at guest UID/GID 39999, checks the child's file report and
thread execution, then queries the parent again. Init terminates and reaps its
services and removes its sockets. Native probes independently verify the normal
untrusted-app domain, actual app UID, zero effective capabilities, zero tracer
PID, and unchanged Bionic executable bytes.

Evidence: [native init/zygote report](init-zygote-validation.json).

## Remaining framework work

This validates init, lazy zygote startup and a real Java-child fork, not
system_server, launcher, installed applications or complete hardware support.
PID 1, kernel root, new capabilities, SELinux transitions, mount namespaces and
supplementary-group specialization are not provided by this rootless profile.
Ownership metadata does not grant host filesystem access or enforce guest DAC.
No hardware-integrity or banking-app compatibility result follows from it.

Broader filesystem calls, complete metadata lifecycle/race handling, script
dispatch and preparation of newly installed executables remain open. Apps' own
SIGSYS handlers remain unsupported. An explicit 8 KiB alternate-stack stress
test exhausted the stack with nested signal frames; 16 KiB is the tested size
for nested traps, and automatic alternate-stack growth is not implemented. The loader uses a fixed ARM64 address
layout and has only been validated on the Pi VM's 4 KiB-page kernel. Symlink
containment and relative-path escapes are not an isolation guarantee. Host
Android restrictions still apply to every actual kernel syscall.

Version 0.34 used the PRoot fallback. The later native Manager uses this
interpreter; its proc-view integration is validated separately below.

## Guest proc path view

The native loader now translates the backing-root prefix in `maps` and `smaps`
to the same guest paths used by file operations. This includes the Manager's
`system-data` backing directory, which appears as `/data`. The executable
pathname is checked against the current instance before another process's
report is translated. Kernel path resolution handles numeric PIDs, task paths,
`thread-self`, relative directory FDs and symlink aliases.

Mapping records are retained, including the loader at
`/system/bin/pocket-loader`. Addresses, permissions, offsets, device/inode
numbers, deleted-file suffixes and `smaps` measurements are unchanged. This
removes accidental host backing paths; it does not make Pocket undetectable.
Existing proc `readlink` translation covers executable, cwd, file-descriptor
and `map_files` targets.

Each mapping report is an immutable per-open snapshot in a sealed memfd. Reads,
small reads, `pread`, `readv`, seeking, duplication, fork and non-CLOEXEC exec
use normal kernel descriptor behavior. Reopen the proc report to capture new
mappings; seeking an existing descriptor does not refresh its snapshot.
The source proc metadata is encoded in the descriptor's name so it remains
available across exec without a process-local fd-number registry. The syscall
layer supplies read-only flags, write errors, proc `stat` metadata and proc link
targets. `fdinfo` for these descriptors reports the corresponding proc inode,
mount ID, read-only flags and current offset. Mapping these proc reports returns
`ENODEV`; opening a report for writing returns `EACCES`. Android can deny
reopening an app's memfd for reading, so a proc-fd reopen regenerates the report
through its original proc pathname, subject to the original kernel permissions.

The snapshots are bounded to 16 MiB and individual input lines to 32 KiB;
overflow fails explicitly. No mapping records are silently dropped. `/status`
retains real kernel credentials, tracer, seccomp and capability evidence.
Mount topology, process visibility and other proc interfaces remain host-backed;
this is a path compatibility view, not PID/mount isolation or complete procfs
emulation. The memfd implementation itself remains observable. Exec-inherited
snapshots stay readable after their source process exits, but reopening their
proc path requires that process to still exist.

Focused validation:

```sh
python3 tools/build_exec_loader.py --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c --out out/proc-view-fixture
scp out/proc-view-fixture/loader-test.apk pi@juanraspberry:/home/pi/proc-view-loader-test.apk
scp tools/test_exec_loader_on_pi.py pi@juanraspberry:/home/pi/test_exec_loader_on_pi.py
ssh pi@juanraspberry python3 -u /home/pi/test_exec_loader_on_pi.py --apk /home/pi/proc-view-loader-test.apk --proc-view
```

The regression checks inline-SVC opens, aliases, `smaps`, preserved mapped-file
addresses/inodes, filenames with spaces and deleted suffixes, descriptor metadata
and `fdinfo`, read-only operations, partial/vector/positioned reads, reopening,
shared offsets, concurrent threads and fork/exec inheritance. The existing
16-exec, signal-stack and unchanged-Bionic checks run in the same fixture.

Evidence: [isolated native regression](proc-view-validation.json) and
[Manager 0.41 / Attest Probe 1.9 integration](proc-view-manager-validation.json).
The Manager APK changes only its version metadata and native loader relative to
0.40; its host DEX and other guest payloads are retained. Guest data was backed
up before the update, and a canary survived the restart. The live test compares
Attest Probe's path `stat` device/inode values with its maps records, verifies
the selected mapping records against the outer kernel, and checks `/data` APK
and native-library mappings against guest file operations. Both outer and guest
app processes retain `TracerPid=0`.

With Attest Probe >=1.9 installed in the outer VM and its update already staged
in the guest:

```sh
scp tools/validate_proc_view_on_pi.py pi@juanraspberry:/home/pi/validate_proc_view_on_pi.py
ssh pi@juanraspberry python3 -u /home/pi/validate_proc_view_on_pi.py --guest-apk /data/local/tmp/attest-probe-1.9.apk
```

This uses the existing paired owner console; administration must be re-enabled
after a Manager restart. It only relaunches Attest Probe and leaves the guest
running. The runtime mapping result remains INFO, including its visible loader;
the extra file-identity fields measure consistency rather than an integrity gate.

## Samsung 4.19 interpreter data mapping (0.42)

On the physical SM-G780G, Manager 0.41 failed before opening its display/input
endpoint, and every owner-console command exited with a segmentation fault.
An isolated Termux reproduction narrowed this to the guest-identity path:
ordinary shell execution worked, but enabling logical guest credentials faulted
when Binder state was first accessed. The same failure was reproducible with
the 0.40 loader.

The writable interpreter segment had 3,080 file bytes and 4,960 memory bytes.
On this older interpreter loader, its BSS extension beyond the first 4 KiB
page was not mapped, because the later probe segment extends above it.
`binder_owner_pid` at `0x5000011150` therefore faulted. Extending the file-backed
data through the zero-initialized state made the same Termux shell and nested
exec succeed immediately.

`loader.ld` now places zero-initialized state in the writable file-backed data
section and pads it to 16 KiB. The final probe segment remains page-complete.
The focused builders run `validate_native_loader.py`, which rejects unbacked
loader BSS and checks the final executable segment and ELF alignment. The guard
rejects the old 0.41 artifact. Manager 0.42 packages this loader correction while
retaining the 0.41 host DEX, stock Bionic/linker and all other guest payloads.

The rootless hardware regression runs ordinary and guest-identity shell
processes and nested exec in an isolated Termux directory. It checks virtual
versus real UID, zero tracer PID and guest-relative mapping paths:

```sh
python3 tools/build_exec_loader.py --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c --out out/samsung-loader-fix
python3 tools/validate_native_loader.py out/samsung-loader-fix/pocket-loader
scp out/samsung-loader-fix/loader-test.apk pf-phone2:pv42-fixture.apk
scp tools/test_loader_on_termux.py pf-phone2:test_loader_on_termux.py
ssh pf-phone2 python3 test_loader_on_termux.py --fixture pv42-fixture.apk
```

Evidence: [Samsung Termux regression](samsung-loader-layout-validation.json) and
[Pi native/proc regression](loader-layout-pi-validation.json). These fixtures
establish the loader correction; full physical Manager startup is a separate
check after installing the update. No phone ADB pairing, host root or guest data
reset is required by the fix.

## Android 9 software media (Manager 0.43)

`tools/build_guest_media.py` builds the pinned Android 9 software PCM, G.711
A-law/mu-law, MP3 and H.264 decoder/encoder implementations for ARM64. The
matching reference image supplied these codecs only as 32-bit binaries.
The six advertised entries are backed by real OMX components; enumeration
queries their capabilities through private `IOmxStore` and `IOmx` services in
`pocket-hw-services`. No host media service or global hardware registry is used.
`MediaCodecList::getInstance()` uses the stock local catalog builder so it does
not wait for an absent `media.player`. The profile also starts the stock AOSP
resource manager implementation and matching media metrics executable.

Android 9's ARM64 framework sends a 96-byte port definition using 32-bit pointer
slots, while the source-built ARM64 software components expect 112 bytes.
`boot_media_port.cpp` converts the numeric audio/video fields at that boundary,
zeros pointers and preserves the other parameter transactions.
`boot_media_fence.cpp` represents an empty fence as a null HIDL handle in buffer
submissions and observer messages. Android 9 otherwise emits an empty FD-array
fixup that the Pi VM's Rust Binder rejects with `EINVAL` (`Fixups oob`), blocking
the Idle/Executing transition and buffer delivery. Real fence descriptors and
nonempty handles retain their original meaning.

The narrow build command is:

```sh
python3 tools/build_guest_media.py \
  --base-apk out/manager-0.42/aosp-pocket-manager-0.42.apk \
  --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c \
  --out-dir out/media-codecs
```

Its `payload.json` is accepted by `build_manager_update.py --guest-file`.
`build_native_manager.py` includes the same media payload in a full build.
`runtime/tests/omx_port_layout.cpp` checks the wire layout against the actual
AOSP OMX headers during compilation. `runtime/tests/MediaCodecProbe.java`
exercises logical app UID 19990, all six catalog entries, exact PCM and both
G.711 outputs, MP3 silence decoding and a four-frame H.264 encode/decode round
trip including EOS and decoded pixels. The MP3 fixture is generated silence.
The Pi harness updates the existing Manager, checks a guest data canary and
runs the probe without resetting user data:

```sh
python3 ~/test_guest_media_on_pi.py \
  --install ~/aosp-pocket-manager-0.43.apk --probe ~/media-codec-probe.jar
```

The harness first requires the dedicated virtual Android target; its outer VM
root is used only for deployment and diagnostics. Pocket's guest codec backend
runs as the Manager's ordinary kernel UID. This profile supports software
`MediaCodec` byte-buffer operation. MediaPlayer playback, AudioFlinger output,
surface input/output, camera and hardware codecs remain separate work.

The final signed 0.43 APK passed this probe on the Pi without an extra diagnostic
preload: six codecs enumerated in 748 ms, exact PCM and both G.711 outputs,
4,608 bytes of decoded MP3 silence, and four encoded/decoded H.264 frames.
Attest Probe 1.10's unchanged codec check passed with six entries and one
encoder. Its complete device report was 44 PASS / 4 FAIL / 3 SKIP / 8 INFO.
The remaining failures are test keys, no secure guest lock, unsupported guest
encryption and absent GMS. The headless test owner granted the requested Android
9 device-check permissions; optional camera/microphone tests were not run.

`docs/guest-media-validation.json` records the commands, output, APK digest and
guest-data preservation checks. The release APK is 578,126,510 bytes with SHA256
`a00090cc5d420519f885e77e6ee83a6c24dab6139ecd1891fd2fedcca8390201`.
Only its version manifest and eleven explicit media payloads differ from 0.42;
the host DEX, native loader, stock linker and Bionic are unchanged. The physical
phone was left unchanged. Installation encountered the VM's storage reserve;
temporarily lowering that reserve allowed the update, and the original setting
was restored. Both guest images and the guest data canary were preserved.

## Guest password validation on the Pi

The existing 0.43 guest supports real software-backed Android password
enrollment. `runtime/tests/GuestLockProbe.java` uses the same `LockPatternUtils`
APIs as Settings, under the authorized owner console's logical system role.
It does not alter LockSettings permission checks or Attest Probe results.
The Binder shell-command control transaction is excluded from ordinary guest
identity adaptation, so invoking the stock locksettings command through the
owner console failed with `Caller must be shell`; regular framework credential
calls succeeded without a runtime change.

`tools/test_guest_lock_on_pi.py` requires the dedicated virtual target and takes
`setup`, `verify` or `unlock`, with `--probe ~/guest-lock-probe.jar`. Setup was
explicitly authorized for this guest. It generates a private temporary password
on the Pi and sends it on stdin; passwords are excluded from validation output
and repository artifacts. The helper preserves an existing credential unless
it can verify the saved test password.

The live test verified correct-password acceptance, incorrect-password
rejection, persistence across a Pocket stop/start, and the actual SystemUI
password unlock. The image lacks virtual keyboard character maps, so the UI
test fills the password field through UI automation and injects Enter while
holding a temporary wake lock. It verifies that Keyguard changes from locked
to unlocked, then releases the wake lock and disconnects automation.

With the credential configured, Attest Probe 1.10 reported `device_secure: PASS` and
45 PASS / 3 FAIL / 3 SKIP / 8 INFO. Test keys, unsupported guest encryption and
absent GMS remain failures. `docs/guest-lock-validation.json` contains the
commands and results without the password. No Manager APK update or physical
phone change was needed for this guest configuration.

## Credential-encrypted storage: initial backend feasibility (0.43)

This section records the initial 0.43 feasibility stage. The integrated 0.44
implementation and live migration are described in the following section.
At this earlier stage the 0.43 guest used the plaintext app-private storage profile in
`runtime/direct/boot_storage.cpp`. At that stage encryption properties were unchanged, existing user data had
not been migrated, and Attest Probe still reported a storage failure.

`runtime/storage/src/dev/kreijstal/aosppocket/storage/EncryptedStore.java`
implements authenticated, random-access encrypted objects using the platform
AES-256-GCM provider, 4 KiB chunks, fresh random nonces, random backing names,
and an encrypted index of logical file names, sizes and chunk references.
Associated data binds each chunk to its store, file, position and object ID.
HKDF-SHA256 separates wrapping, index and content keys. The random master key
is wrapped with a high-entropy Android synthetic-password disk secret; raw
passwords and unstretched credential hashes are outside this API's contract.
The derived key schedule is checked against RFC 5869 test case 1.

Writes stay in memory until `sync()`. A commit writes and fsyncs immutable
ciphertext chunks, fsyncs their directory, writes and fsyncs an encrypted
index, atomically replaces `state`, and fsyncs the store directory. Creation
also syncs the parent directory. Failed or interrupted commits may leave
unreferenced ciphertext; collection runs only after a successful commit.
`close()` discards uncommitted changes, clears owned key/dirty-data arrays,
and releases the single-writer lock. Java crypto providers and temporary
objects can retain internal copies until garbage collection; this is not a
hardware-backed key-erasure guarantee. The owner lock file is reserved to the
backend and must not be independently opened by code in the same process:
POSIX closing rules could release its process lock.

The host behavior tests cover 180 random read/write/resize operations, 18
close/open cycles, sparse and truncated file behavior, committed and discarded
namespace changes, competing owners, wrong secrets, absent plaintext canaries
in backing files, altered keys/index/chunks, swapped chunks, missing chunks,
truncated indexes, and eight process kills during active commits. These test
process death, not power loss or arbitrary disk write reordering.

```sh
python3 tools/test_encrypted_store.py
python3 tools/build_storage_probe.py \
  --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c
scp out/storage-probe/storage-probe.apk tools/test_storage_probe_on_pi.py \
  pi@juanraspberry:/home/pi/
ssh pi@juanraspberry \
  'python3 -u ~/test_storage_probe_on_pi.py --apk ~/storage-probe.apk'
```

The APK is a separate permission-free scratch app, not a Manager update. The
Pi harness requires the dedicated virtual target. Root is used for installation
diagnostics/report collection, never for the backend's file operations. The
test ran with kernel UID 10176, no effective capabilities, and enforcing
`untrusted_app_27` SELinux policy on Android 16. The same backend tests passed
there. A public proxy descriptor backed by encrypted objects supported a
read mapping, a shared writable mapping and `msync`; the changed byte survived
closing and reopening the encrypted backend without a final explicit commit.

The ordinary app API and a separate raw bridge with cached I/O produced the
same compatibility constraints:

| Operation | Pi result | Remaining runtime work |
| --- | --- | --- |
| Read and shared writable `mmap`, `msync` | Passed, including ciphertext-backed persistence | Route CE file descriptors through the backend |
| `ftruncate` | `ENOSYS` (38) | Implement truncation and inode size/cache coherence in the loader adapter |
| POSIX record lock | `EACCES` (13), SELinux `{ lock }` denial | Preserve cross-process byte locks and fd/fork/dup/close semantics using a supported mechanism |
| Executable file mapping | `EACCES` (13), SELinux `{ execute }` denial | Provide and validate a memory-backed executable loading path for CE files |

The failed lock operation means SQLite compatibility has **not** been proved.
The raw bridge test is pinned to Android 16's IStorageManager wire IDs 74/75;
it is a feasibility probe, not a portable production dependency. Reflection
of the hidden mount method was unavailable. The public proxy API was sufficient
for the encrypted mapping test. Android 9 guest version does not imply that
other host Android versions have the same proxy or SELinux behavior.

Integration must implement an encrypted namespace and guest path/fd adapter,
including relative paths, symlinks, directory enumeration, file attributes,
rename/unlink, descriptor passing, locking and truncate/cache behavior. A
plaintext backing file cache or an archive encrypted only when Pocket stops
would not meet this design. Kernel cached plaintext and active keys are
expected while CE storage is unlocked; the adapter must flush mappings before
acknowledging durable operations and enforce the locked state across every
access route.

The credential lifecycle should use Android 9 LockSettings/SyntheticPassword
and the real `vold` key calls, rather than collecting a second password in
Manager. The current guest's synthetic-password/keystore capability must be
validated before attempting credential migration. Legacy credential-derived
hashes must not be treated as high-entropy disk secrets. Keep lock settings,
credential blobs and DE data available during boot; expose CE data such as
`/data/user/0`, `system_ce`, `misc_ce` and user media only after successful
first unlock. Normal screen relocking after first unlock does not itself
evict CE keys. Password changes must preserve access to the same data key;
reboot/user-key eviction must close descriptors, discard relevant caches and
remove keys. Test wrong passwords, password changes, cold boot before unlock,
user stop and restart, as well as real SQLite multi-process locking and crash
recovery, before enabling or reporting guest storage encryption.

Existing data migration remains separate: stage encrypted data, authenticate
and verify the staged copy, make activation crash-safe, and account for every
plaintext copy before marking migration complete. This backend offers no
host-proof isolation, rollback resistance, guaranteed secure deletion of
historical plaintext blocks, or guest hardware/TEE attestation. Android's
[FBE documentation](https://source.android.com/docs/security/features/encryption/file-based)
describes the CE/DE lifecycle and the additional hardware/kernel requirements
of platform FBE; [RFC 5869](https://www.rfc-editor.org/rfc/rfc5869) defines HKDF.
`docs/guest-storage-validation.json` records this stage's commands and results.


## Credential-encrypted storage integration (0.53)

Pocket now routes Android 9 user 0 credential-encrypted storage through the
Manager's encrypted namespace and same-UID AppFuse transport. The production
backend runs in the ordinary Manager process. Host root is used only by the
Pi validation harness for installation and diagnostics. The four protected
roots are `/data/user/0`, `/data/system_ce/0`, `/data/misc_ce/0`, and
`/data/media/0`, including `/data/data` and emulated-storage aliases. Other
credential-encrypted users fail closed. DE storage remains available at boot.

Guest LockSettings verifies the existing password. Its synthetic-password disk
secret travels through the real StorageManager/vold unlock flow to authenticate
the encrypted key envelope; Pocket does not store the password or write the
secret to disk. Initial enrollment uses an owner helper with the credential on
stdin, followed by an offline migration. Migration encrypts the complete CE
namespace, closes/reopens the vault and verifies every regular file's SHA-256,
then commits a durable verified journal before deleting the old plaintext
files. Startup can finish this verified cleanup after interruption. Empty
physical CE roots are sealed with mode 0000. Only completed cleanup activates
the encryption marker and `ro.crypto.state=encrypted`, `ro.crypto.type=file`.
The existing Pi guest was migrated with its existing password unchanged.

Directory descriptors refer to empty opaque directories sealed with mode
0500. The loader resolves CE paths through directory fds and proc magic-link
aliases; unsupported aliases cannot create plaintext in those directories.
File descriptors carry the encrypted transport and an independent ordinary
lock descriptor. Kernel record locking preserves cross-process SQLite locks;
truncate updates the encrypted object and refreshes the cached inode size.
Private executable mappings use a RAM-only sealed memfd. Execution of an
arbitrary CE ELF with `execve` is currently unsupported. Shared writable
mappings are flushed before durable metadata commits and on orderly shutdown.

A normal screen relock retains the CE key after first unlock, as Android FBE
does. A cold Pocket start leaves CE locked until successful guest credential
verification. Password replacement retains the synthetic disk secret. If the
guest explicitly removes its password, an AndroidKeyStore-wrapped default slot
supports the resulting credential-free unlock; restoring credential protection
removes that slot. Removing/restoring credentials was tested in the isolated
fixture; the real Pi password was not removed.

Build and operator commands:

```sh
python3 tools/build_guest_storage.py \
  --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c \
  --base-apk out/manager-0.52-preserved/aosp-pocket-manager-0.52-preserved.apk \
  --version 0.53
python3 tools/pocketctl.py --config /path/to/private-config.json encrypt-storage
```

`encrypt-storage` checks the active guest, prompts for its existing password,
prepares the synthetic disk secret in Manager memory, stops the guest, waits
for verified migration and restarts it. Unlock Android with the same password.
An already-encrypted guest is reported without prompting or remigrating.
Credentials and connection configuration must remain private.

The isolated production-adapter fixture passed migration, locked access,
additional-user rejection, POSIX permissions and fd metadata, directory/proc
aliases, symlinks and hardlinks, truncate/cache coherence, writable `mmap`,
RAM-only executable mapping, SQLite WAL/FULL with a competing writer, reopen,
and credential-free/default-slot removal/restoration. Host tests also passed
RFC 5869, wrong-secret and ciphertext-tamper rejection, exclusive ownership,
and eight process-kill recovery cycles. These tests do not simulate power loss.

This is a userspace encrypted filesystem. The Manager and its host can access
unlocked keys and memory. It supplies no TEE claim, rollback resistance,
secure deletion of historical plaintext blocks, or protection from a compromised
host or malicious guest root. Host compatibility has been exercised only in
the dedicated Android 16 Pi VM; other versions and the physical phone remain
unvalidated. See `docs/guest-storage-validation.json` for recorded results.


The final 0.53 update uses the Pi's preserved 0.52 APK as its base. The ZIP
audit found only five changed entries (manifest, host DEX, owner console,
loader, and credential helper); the other 0.52 files, including the existing
Magisk/NeoZygisk bootstrap, stock Bionic and linker, were preserved. The
Manager update installed successfully in the dedicated Pi VM, and both APKs
are retained for comparison. The host storage reserve was restored afterward.

The controlled Pi 0.53 restart test denied private-file reads before unlock,
rejected a wrong password through LockSettings, then recovered the same data
canary using the existing password. Actual SystemUI password entry passed on
retry after an initial accessibility discovery timeout. The already-encrypted
owner command returned without prompting. The codec regression probe passed
six codec entries and the PCM, G711, MP3 and four-frame H264 round trip.

Attest Probe 1.10 reported 43 PASS, 5 FAIL, 3 SKIP and 8 INFO (59 checks).
Storage encryption passed: its `activating_default` label is a probe enum-label
bug for Android's `ENCRYPTION_STATUS_ACTIVE_PER_USER` (5). Failures were the
existing su and Magisk artifacts, test keys, absent GMS, and a 2000 ms contacts
query timeout. The contacts timeout remains a performance limitation; neither
the probe nor its timeout was changed to make it pass.


## Per-process DenyList view (0.54)

The native loader accepts guest-control selector 3 through its reserved getpid
control only while its logical effective UID is 0. It monotonically selects
the base system/vendor image and hides Magisk entry paths for that process.
Selector 4 reads this state. A fork inherits it; the internal exec identity
transport appends a single flag while accepting the previous V1 record length.
The zygote parent's view is unchanged. `guest_api.h` exposes the activation
helper, and the NeoZygisk Pocket provider invokes the same control after its
pre-specialization callbacks.

The resolver normalizes relative paths and inherited directory/proc aliases,
checks hidden symlink targets and filters directory enumeration. Proc maps
project module-view system/vendor backing names into the guest paths already
used by readlink. Mapping records, addresses, permissions and inode/device
values are retained. Existing code and descriptors can remain; this is a guest
compatibility view rather than a kernel isolation boundary. Operational behavior
and the real app comparison are recorded in `docs/magisk-pocket.md` and
`docs/denylist-validation.json`.
