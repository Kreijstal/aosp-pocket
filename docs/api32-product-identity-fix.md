# Pocket 0.78.170 product-profile regression fix

Status: **UNQUALIFIED test update**. The CE162 Binder cleanup/full-lifecycle
release gate remains unresolved. No qualification or support flag changes.

The 0.78.169 loader image retained the 0.78.136 base payloads. Compared with
the phone's previous 0.78.20 image, that base lacked the Pocket product identity
block in `system/etc/pocket-profile.prop`. Attest Probe consequently reported
the reference image's emulator device name and `test-keys` build tags.

Manager 0.78.170 packages the exact previous Pocket identity block separately
and reapplies it after image preparation, alongside the existing display and
encryption projections. Image upgrades can therefore replace their own
profile file without discarding Pocket's product identity. The guest remains
API32 and userdebug. The existing 0.78.169 image and fixed loader are retained;
the image catalog and all five host libraries are unchanged. No new image
download, package certificate migration or app-data reset is required.

The restored `release-keys` value is the same compatibility metadata used by
0.78.20. **This change does not re-sign system APKs or establish secure boot,
hardware attestation or production security.** System APK certificates remain
unchanged. Attest Probe itself is unchanged.

The Attest Probe update had also been installed through the owner console
without an installer name. Its attribution was repaired through Android's
PackageManager API as guest shell UID2000, naming `com.android.shell`. This
records the actual command-line installation route; it does not claim a Play
Store installation. The app's UID, certificate and private data are retained.

Validation:

- `python3 tools/build_api29_manager.py --base-apk
  out/binder-update-169/manager/aosp-pocket-manager-0.78.169.apk
  --host-native-dir out/binder-update-169/host-native --external-native-dir
  out/api32/native-clean-42/external --catalog
  out/binder-update-169/phone-catalog-before.json --api32-catalog
  out/binder-update-169/image/pocket-images.json --package-name
  dev.kreijstal.aosppocket.api32managerimage --version 0.78.170 --version-code
  78170 --out out/product-identity-170/manager`: **PASS**.
- APK payload inspection: **PASS**. The profile asset matches source, the
  image catalog is byte-identical to 0.78.169, and all host libraries are
  byte-identical. No test activities or bundled image are added.
- `ApkSignerTool verify
  out/product-identity-170/manager/aosp-pocket-manager-0.78.170.apk`: **PASS**.
- SD-card APK transfer: **PASS**, remote SHA-256 verified. Copies are in the
  physical SD-card root and shared Downloads.

APK SHA-256:
`91c009a561b75dcf5d7c3759f5627b028cb4027425cc27e5da9312783e48e871`.

The original failing report is preserved under
`out/binder-update-169/attest-after-update-e8ba69a22b84/`. Phone receipts are
under `out/product-identity-170/`. Installation approval, guest restart and a
fresh complete Attest Probe report are required before claiming all three
checks pass on the updated phone.
