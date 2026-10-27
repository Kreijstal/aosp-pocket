# Android 9 native crash investigation

Observed on Samsung SM-G780G with Pocket 0.27, Android 9 guest and Revolut
10.135 on 2026-10-21. Addresses below are offsets in the unpacked anonymous
executable mapping `[anon:1313]`, not offsets in the APK's shared library.
Raw app-memory captures remain in ignored local `out/native-trace/` files.

## Confirmed immediate cause

A ptrace capture records SIGSEGV / SEGV_MAPERR at address zero:

- `+0x130a8` calls the internal serialization/encoding routine `+0x73118`.
- Its return value becomes `x20` at `+0x130b0`.
- `+0x13550` passes `x20` as the string argument to `+0x6e6cc`.
- `+0x6e6d0` loads a byte through that null pointer and faults.

The encoder's global input length (mapping base + `0x90b38`) is 4,095 bytes.
A subsequent hardware execute breakpoint confirms that length at encoder
entry `+0x73118`. At the failure branch `+0x73290`, registers show:

| Value | Observed |
| --- | ---: |
| Input length / x20 | 4095 |
| Header length / x8 | 5 |
| Encoded payload allowance / x21 | 4143 |
| Required output space (x8 + x21) | 4148 |
| Output limit | 4095 |

The bounds check at `+0x73264` compares the required space with `0xfff`;
`+0x73268` branches to `+0x73290`, which sets the return pointer to zero.
The crypto call at `+0x73288` is skipped on this path. The caller later uses
the null return as a string without handling the error.

This identifies the immediate crash chain. It does **not** establish why the
input buffer becomes full, whether this is the app's normal failure-reporting
path, which VM incompatibility initially triggered it, or a security verdict.
It does not substantiate the earlier hypothesis of a platform string API
returning null. Attaching a debugger can affect protected software; the fault
location also matches the earlier development crash recorder.

The hardware breakpoints observed execution only: no APK changes, instruction
patches, register replacements, or skipped decisions were used.

## Separate guest termination issue

Host ADB logs from Artix establish that guest exit 137 was caused by the host
ActivityManager's phantom-process trimming. At 14:26:42 it explicitly killed
`pocket-init-api28`, `magiskd`, `zygote64`, and other guest services with reason
`Trimming phantom processes`. This is separate from the app's SIGSEGV.

For this development phone, the existing Artix ADB connection was used to run:

```sh
adb shell settings put global settings_enable_monitor_phantom_procs false
adb shell settings get global settings_enable_monitor_phantom_procs
```

The readback was `false`. The setting had previously been absent (`null`).
Restore its prior state with:

```sh
adb shell settings delete global settings_enable_monitor_phantom_procs
```

No ADB daemon restart was performed. The setting is host-wide, not confined
to Pocket. The subsequent capture completed; long-term stability is unverified.

## Focused validation

The local diagnostic tracer `out/native-trace/trace-encoder.c` was rebuilt with:

```sh
/home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang -Wall -Wextra -Werror -O2 -static -fno-emulated-tls -Wl,-z,max-page-size=16384 out/native-trace/trace-encoder.c -o out/native-trace/pocket-native-trace-encoder
```

Result: exit 0. Uploaded using rsync, then installed through the existing
paired Pocket administration console. Running the diagnostic capture through
`python ~/.config/pocketctl/magisk-test/run_trace.py` on the phone returned
`trace exit 0`. It captured both hardware breakpoint stops and the subsequent
null-address SIGSEGV, then detached while delivering the original signal.

Private evidence: `out/native-trace/revolut-length-capture.txt`,
`out/native-trace/revolut-encoder-capture.txt`, and
`out/native-trace/host-logcat.txt`. Next investigation: identify which producer
fills the serialization buffer, without assuming each recorded event means
an unsupported API or an intentional security response.

## Reproduction inside Pocket on the Raspberry Pi VM

The Pi's running ARM64 KVM LineageOS VM hosts the Pocket application. Pocket
was updated in place from 0.18 to 0.27, and its Android 9 guest was restarted.
Revolut was launched inside that nested Pocket guest, not directly in the
outer LineageOS VM. The diagnostic tracer ran through Pocket's paired owner
console under the application's real host UID 10177.

The initially installed Revolut 10.152 (version code 1030152101) terminated
with a protected Java exception followed by SIGKILL. It did not reproduce the
Samsung fault. Its APKs were preserved under
`/home/pi/pocket-revolut-10.152-backup` before installing the matching 10.135
APK set (version code 1013509766). The diagnostic PackageInstaller session
used replace-existing and allow-downgrade flags and retained app data.

With 10.135, `python3 -u /home/pi/pi_capture.py` returned `TRACE exit 0` and
captured both hardware execute breakpoint stops and the same subsequent
SIGSEGV. Input length was 4095 at encoder entry; the overflow branch had
header length 5 and payload allowance 4143, requiring 4148 bytes. The null
read again occurred at `+0x6e6d0`. The encoder-entry, bounds-check, caller, and
null-load instruction bytes all match the Samsung capture.

This provides a second environment for investigating the buffer producer;
it does not yet identify that producer or establish a fix. Pocket 0.27 and
Revolut 10.135 remain installed in the Pi test instance. There were no
production code changes for this reproduction. The local Python diagnostic
helpers passed `python -m py_compile`; the temporary Java installer compiled
with javac and D8 for API 28 and reported `MATCHED_INSTALL_PASS` on the guest.

Structured result: `docs/api28-revolut-pi-validation.json`. Private capture:
`out/native-trace/pi-revolut-encoder-capture.txt`.

### Buffer producer identified

`python3 -u /home/pi/pi_capture_producer.py` completed with `TRACE exit 0`.
Hardware execute breakpoints on the single-byte append helper `+0x72ee8`,
string append helper `+0x72f5c`, and encoder entry captured the producers on
the main thread. Each append breakpoint was disabled for a single instruction
step and restored; instruction bytes and register values were not replaced.

The dominant producer invokes the byte helper at `+0x4eb10` (return address
`+0x4eb14`) and the string helper at `+0x4eb18` (return address `+0x4eb1c`).
It made 46 string-append calls, starting with buffer length 25 and continuing
after the buffer reached 4095. The captured serialized buffer contains entries
such as `mkdirat->.../libpocket_direct_boot.so`,
`settimeofday->.../libpocket_direct_boot.so`,
`geteuid->.../libpocket_direct_boot.so`, and
`fstat64->.../libpocket_direct_boot.so`. These are Pocket's redirected function
implementations. Static instructions immediately before the append calls
inspect AArch64 branch/trampoline instruction patterns and optionally resolve
their targets. The evidence supports a function-redirection reporting path
filling the serialization buffer, followed by the already verified overflow
and null dereference. This is different from a missing platform string API.

The producer trace again captured the same overflow branch and null fault.
It does not determine which redirect or other event first triggers reporting,
prove every reported function is rejected individually, or establish a fix.
The tracer watches only the main thread; other-thread writes are not covered.
Debugger attachment can affect protected software. Raw buffer contents remain
private under ignored `out/native-trace/pi-buffer-report.bin` and
`out/native-trace/pi-revolut-producer-capture.txt`.

The outer Pi VM's root shell was independently checked: kernel UID/GID 0,
SELinux context `u:r:magisk:s0`, with SELinux Enforcing. Pocket continues to
run under its host application UID. Actual outer-VM root and Pocket's guest
identity emulation are distinct.
