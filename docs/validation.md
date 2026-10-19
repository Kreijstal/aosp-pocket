# Initial validation — 2026-10-19

## Build

Command:

```sh
make -j2 verify
```

Passed Java compilation, ARM64 native compilation, Toybox source build, APK
packaging and v3 signature verification. The focused package check confirmed
that both native payloads are actual ARM64 PIE executables with Android linker
paths and 16 KiB LOAD alignment, and that the source provenance and license are
packaged. The manifest requests no permissions.

The APK was 57 KiB; the minimal Toybox executable was 46,768 bytes. These are
bootstrap sizes, not estimates for a complete Android guest.

## Android runtime

Command:

```sh
ssh pi@juanraspberry python3 - < tools/validate_on_pi.py
```

Passed on the existing ARM64 Android 16 / LineageOS Cuttlefish instance 72,
running on the Raspberry Pi. The test launched a non-debuggable app normally
through ActivityManager. The existing VM credential lock was not changed.

| Observation | Result |
|---|---|
| Native process UID and EUID match host app | 10168 / 10168 |
| SELinux domain | `untrusted_app` |
| Native bootstrap and five checked AOSP command outputs | Passed |
| `echo` | `aosp-pocket-ready` |
| `uname -m` | `aarch64` |
| `id -u` | `10168` |
| `pwd` | Actual app-private instance root directory |
| `cat data/pocket/boot.txt` | Read actual app-created instance data |
| Public Android shared memory, write/read | Passed |
| Host Binder version query | Protocol 8; no private guest context |
| Mount/PID/IPC/UTS namespace creation | EPERM |
| User namespace creation | EINVAL |
| `PTRACE_TRACEME` in disposable child | Succeeded |
| Anonymous shared mapping | Succeeded |
| Seccomp mode | 2 |
| Native bootstrap elapsed time | 709 ms |
| Guest Android framework boot | Not implemented |
| Independent guest isolation | Not established |

The report is saved as `out/bootstrap-vm.json` locally and
`/home/pi/aosp-pocket-bootstrap.json` on the Pi. It contains actual process
results. Root in the external harness only retrieves this app's private JSON;
the app and its children receive no root privileges.

This is VM validation, not physical SM-G780G validation. The Samsung's Termux
SSH port refused the LAN connection, so no physical installation or runtime
test was performed. Foreground UI appearance and full Android startup are not
covered by this test.
