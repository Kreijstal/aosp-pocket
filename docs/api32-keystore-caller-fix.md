# Android 12L keystore caller-context fix

Status: functional keystore fix verified by the unchanged Attest Probe 1.12 on
VM73. Seventy focused host cases pass. Android 12L remains experimental and
**not lifecycle-qualified**; the existing CE162 failure and deployment gate remain.

## Change

The keystore SID adapter previously revalidated every caller by opening
`/proc/<pid>/stat`. On pf-phone2 this lookup becomes inaccessible after a caller
sets `PR_SET_DUMPABLE=0`; `AIBinder_getCallingSid` returned null and stock
keystore2 rejected ordinary key generation with SYSTEM_ERROR. The earlier Binder
UID delivery fix did not remove this separate dependency. The captured failure
and Attest Probe 1.12 regression are described in
[the investigation](api32-keystore-nondumpable-investigation.md).

The API32 Binder dispatch adapter now exposes its current guest caller through
a scoped, thread-local API after the existing kernel-sender checks. Every
nested dispatch pushes a frame, including non-guest calls; leaving dispatch
restores the preceding frame. The API checks the current IPCThreadState UID/PID
against the frame, so clearCallingIdentity cannot retain authorization as the
original caller. Keystore uses this API rather than reading a new proc record.
Missing context, a missing adapter, or mismatched identity fails closed.
Existing UID roles, APP namespace ownership, isolated-UID exclusions and
privileged-operation denials remain in place. These remain guest-role metadata
within one host app UID, not a new host isolation boundary.

The downloadable image changes only `libpocket_binder_api32.so` and
`libpocket_keystore_api32.so`. All 2,650 other payloads were retained and verified
against image 0.78.169. The normal Manager DEX and all five host native libraries
are byte-identical to 0.78.172; its Android 9/10 catalogs are unchanged. The normal
APK contains no test activity or bundled Attest APK.

## Focused checks

Commands executed:

```sh
make -f runtime/Makefile check-binder-dispatch
make -f runtime/Makefile check-keystore-policy
python3 tools/build_api32_native.py --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c --binder
python3 tools/build_api32_keystore.py --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c --policy-only --out out/keystore-caller-fix/policy
python3 -m py_compile tools/update_api32_adapters.py tools/build_api32_keystore.py
```

Results:

- 20 ASan/UBSan dispatch cases PASS: nesting, host/rejected dispatches, changed
  identity, restoration, invalid arguments and thread isolation.
- 45 ASan/UBSan cases against the actual policy implementation PASS: system
  operations, ordinary-app key access, denied maintenance/device-ID operations,
  isolated UIDs, Wi-Fi/locksettings separation, nested rejected context and
  mismatched identity. Test-only IPC/property stubs replace the platform; no
  daemon or stored keys are involved.
- 5 missing-adapter cases PASS: absence of the scoped caller API supplies no SID
  and grants no permission even when a guest frame is present.
- Both ARM64 library builds and Python syntax validation PASS.

## Live keystore verification

`out/keystore-caller-fix/pi-keystore-pass-helper-failure.json` preserves a fresh
nonce-bound runtime report from ordinary guest app UID 10114, running the
unchanged Attest Probe 1.12 APK (SHA-256
`8a28d2e6cc154cdc8e95ebb7aa1f33ddb6bb71f471053c12adb5a3c3cf7edfe6`).
The downloaded image SHA-256 is
`cd1db8f04dd9c7309b65696f61aef39d9e623a2e3a0566deef3e4d38f3c165ce`.

Both raw Binder UID checks PASS. `runtime_keystore_nondumpable` PASSes AES-GCM,
RSA-PKCS1, RSA-OAEP and EC generation/use with observed dumpability 1, 0 and 1.
All 12 temporary-key deletions are verified. The private service restores its
original state; the main process's dumpability remains unchanged. That run also
stops its guest with owner exit 0.

The enclosing exploratory fixture still reports FAIL because a separate native
policy helper lacked its relocated ELF interpreter. That outer result has not
been relabeled as a passing complete trial. The current permission checks above
exercise the actual C++ policy directly; a passing live native-HAL helper run is
not claimed.

The experimental integration fixture and its modified builder are preserved
under `out/keystore-caller-fix/experimental-fixture/`, outside the production
source. They are not shipped and are not a passing CI/release gate. Its archived
source and build receipts record the actual tests rather than leaving an
unstable test mode in the ordinary Manager builder.

## Other preserved failures and environment

The first new-profile attempt stopped before boot because the VM's 8 GB data
disk lacked space for another full image (`pi-first-result.json`). The next
attempt updated the existing stopped VM73 profile and retained its data.
Pathname installation hit a guest SELinux check against the host app-data label
(`pi-install-path-failure.json`); stdin installation avoided that check without
changing policy. Cold PackageManager reconciliation exceeded the fixture's
first 30-second install budget (`pi-stdin-install-timeout.json`); only test
provisioning was allowed 120 seconds. Attest's 25-second watchdog was unchanged.

A later attempt lost Manager PID 18473 to an outer-host LOW_MEMORY kill before
any verdict (`pi-host-exits.txt`, `pi-oom-observation.json`). VM73 was restarted
with 2560 MB rather than 1536 MB, keeping its disk and installed Manager.
No Android OOM policy, SELinux policy or process limit was relaxed. Only one
QEMU Android VM was running on the Pi (`pi-running-vms.json`); VM72 was untouched.

Subsequent exploratory runs also captured a terminated reinstall with owner
exit 137/leftover-process cleanup (`pi-reinstall-terminated.json`) and a missing
fresh runtime report (`pi-latest-runtime-fixture-failure.json`). These failures
remain evidence of incomplete integration/lifecycle validation. No full Attest
report, CE162 resolution, or complete lifecycle pass is claimed. The successful
individual keystore report above does not qualify the build.

The normal Manager candidate is restored on VM73 after testing, retaining app
UID 10157 and profile data. pf-phone2 and actual Revolut startup were not
retested; the user selected Pi validation while the phone tunnel was offline.

## Image and Manager candidates

```sh
python3 tools/update_api32_adapters.py --base-image out/binder-update-169/image/pocket-api32-arm64-0.78.169.zip --base-catalog out/binder-update-169/image/pocket-images.json --binder-library out/api32/native/libpocket_binder_api32.so --keystore-policy out/keystore-caller-fix/policy/libpocket_keystore_api32.so --out out/keystore-caller-fix/image --version 0.78.173 --url https://github.com/Kreijstal/aosp-pocket/releases/download/pocket-0.78.173/pocket-api32-arm64-0.78.173.zip
python3 tools/build_api29_manager.py --base-apk out/termux-screencap-172/manager/aosp-pocket-manager-0.78.172.apk --host-native-dir out/binder-update-169/host-native --external-native-dir out/api32/native-clean-42/external --catalog out/binder-update-169/phone-catalog-before.json --api32-catalog out/keystore-caller-fix/image/pocket-images.json --package-name dev.kreijstal.aosppocket.api32managerimage --version 0.78.173 --version-code 78173 --out out/keystore-caller-fix/manager
```

Complete input/output archive validation PASS; only the paired adapter payloads
changed. Receipts, failed trials, the positive keystore report, APKs and image
catalogs remain under `out/keystore-caller-fix/`.

The normal Manager 0.78.173 candidate SHA-256 is
`8a5568bb71ac2b5c086f748e02def0737bf5d1d0e9c5e58fc0ca356d9bec5523`.
It and its image remain unpublished; the configured download URL is reserved
for a future authorized release. VM73 already has the matching cached image.
Existing release-qualification requirements remain in force.
