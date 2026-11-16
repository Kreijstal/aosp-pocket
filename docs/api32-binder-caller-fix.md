# Native Binder caller identity fix — 0.78.169 test build

**UNQUALIFIED.** The CE162 Binder cleanup crash and the full Android 12L
lifecycle release gate remain unresolved. This patch fixes a separate caller
identity error; it does not qualify Android 12L or establish that Revolut starts.
Original CE162 reports, frozen sources and recovery evidence remain unchanged.

## Cause and change

An ordinary native Binder client lacks the guest UID/PID prefix used by our
libbinder adapter. The server's fallback identity lookup opens `/proc/PID/stat`
to validate the PID start time. On pf-phone2 this fails with ENOENT after the
client becomes nondumpable. PackageManager therefore receives the host app's
identity and returns UID -1 for a package in the guest.

The current loader now adds the existing guest credential prefix to recognized
unprefixed native Parcel requests. Already-prefixed requests and non-Parcel
protocols retain their framing. Transaction data and object offsets are staged
without changing client buffers. Binder/FD/SG objects, partial writes, host
scope and error cleanup are covered by the focused regression. PID reuse checks
and client dumpability behavior remain intact.

## Build and evidence

- `make -f runtime/Makefile check-binder-transport`: **PASS, 31 cases**, with
  AddressSanitizer and UndefinedBehaviorSanitizer.
- `python3 tools/update_api32_loader.py --base-image
  out/api32/codec136-image/pocket-api32-arm64-0.78.136.zip --base-catalog
  out/api32/codec136-image/pocket-images.json --ndk
  /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c
  --out out/binder-update-169/image --version 0.78.169 --url
  https://github.com/Kreijstal/aosp-pocket/releases/download/pocket-0.78.169/pocket-api32-arm64-0.78.169.zip`:
  **PASS**. The current loader is source-built and ELF-validated. All incoming
  and outgoing payloads are verified; 2,651 payloads are retained, and only
  `system/bin/pocket-loader` changes. The base archive is preserved.
- `python3 tools/build_host_native.py --ndk
  /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c
  --out out/binder-update-169/host-native --jobs 8`: **PASS** against current
  source pins, with existing dependency caches. These newly built artifacts
  do not inherit the previous native163 runtime qualification.
- `python3 tools/build_api29_manager.py --base-apk
  out/download-image/release/aosp-pocket-manager-0.76.apk --host-native-dir
  out/binder-update-169/host-native --external-native-dir
  out/api32/native-clean-42/external --catalog
  out/binder-update-169/phone-catalog-before.json --api32-catalog
  out/binder-update-169/image/pocket-images.json --package-name
  dev.kreijstal.aosppocket.api32managerimage --version 0.78.169 --version-code
  78169 --out out/binder-update-169/manager`: **PASS**. Normal Manager classes,
  no probe activities, no bundled guest image. Source and native hash guards
  remain enabled. The application ID and signing certificate match pf-phone2.
- `ApkSignerTool verify --print-certs`: **PASS** for the installed and new APK;
  certificate SHA-256
  `b04ea6cd081f4b5afb30b25f5492ccd044c821fd1e17d18c72067f1b9fcf6916`.
- Live per-process current-loader check on pf-phone2: **PASS**. The unprefixed
  native package UID query returns guest UID10109 both with dumpability 1 and
  with dumpability 0. The old loader returned -1 with dumpability 0. This test
  does not replace the installed system loader or restart the guest. Receipt:
  `out/binder-update-169/phone-current-loader-validation.json`.

| Artifact | SHA-256 |
| --- | --- |
| Manager APK | `35c27abfea1d3e2ac4d808ff835fbd073b7b07e7b40acd8579ba6109130f766d` |
| Downloadable image | `c99dc59c961bf5aba16d317fdf8b55d066f05442e619b23e20caa98028092ae3` |

Build receipts and logs are retained under `out/binder-update-169/`. Delivery
and public release status are separate from the successful local build checks.
The full updated Manager/image combination, post-install Attest Probe and
Revolut startup still require validation. No support or qualification flag is
changed by this fix.
