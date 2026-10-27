# microG compatibility in the Android 9 guest

Pocket 0.55 adds Android 9 package-query compatibility for original microG
Services (`com.google.android.gms`) and Companion (`com.android.vending`).
It does not change the guest API level (28).

The source-built zygote invokes `MicrogSupport.install()` in system_server after
ART post-fork initialization, before SystemServer publishes PackageManager.
The package-service Binder facade runs in the same process as the original
PackageManager and delegates with the original caller identity. System-server
local service lookups retain the original concrete PackageManagerService Binder
for platform services that cast it directly. Internal package
objects, installation/update verification, signature permissions and
`checkSignatures` continue to use the actual APK signer. No Zygisk provider is
needed for the compatibility layer.

Android also supplies a cached package Binder to applications during
`attachApplication`. A second publication facade forwards ActivityManager calls
unchanged and, at this normal first-binding lifecycle point, replaces only the
package entry in AMS's regular and isolated app-service caches. It uses AMS's
existing cache initialization under its own lock, preserving other cached
services. This covers cached app clients as well as direct service-manager
queries, without app hooks or changing caller identities.

Only the two package names above, with exactly one current signer matching
official microG certificate SHA-256
`9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165`,
receive Google's public release certificate in read queries. There is no general
signature-spoofing permission or caller-supplied fake-signature metadata.
The Google certificate's SHA-1 is
`38918a453d07199354f8b19af05ec6562ced5788`.

The facade covers legacy `GET_SIGNATURES`, `GET_SIGNING_CERTIFICATES` (including
signing history), versioned package queries, installed/permission-filtered
package lists, and package/UID `hasSigningCertificate` with raw X.509 or SHA-256
input. It clones returned package information and never mutates PMS caches.
Unexpected projection failures preserve the original reply; original permission
denials retain their exception type. Successful projection is checked with an
ordinary app, rather than inferred from package presence.

The guest image includes microG power/data-save exemptions and narrowly scoped
privileged-permission allowlists for optional system installation. Android still
requires the APK to request each permission. Runtime permissions remain under
the usual Android permission controls. microG APKs are separate from the base
Pocket image; official APKs can be installed normally, or included under
`system/priv-app` for system location-provider integration. Avoid installing
original Google Play Services alongside microG.
When including a system APK that has compressed native libraries, extract its
original matching ABI libraries into the package's `lib/arm64/` directory too.
Normal installation through Pocket's APK installer handles native extraction.

For validation, official GmsCore/Companion v0.3.17.252432 and optional GSF Proxy
v0.1.0 were downloaded:

| Artifact | SHA-256 |
| --- | --- |
| `com.google.android.gms-252432034.apk` | `e5e6cba078bdec812b5450599350e5c1a12c8b7438da5bf77123af170f8c1a35` |
| `com.android.vending-84022634.apk` | `9b8616011cc8f026bcbbe094df850794abb9d6d9f23214d7a8f8c4d1033ea012` |
| Optional `GsfProxy-0.1.0.apk` | `86891b174301f06a1c84187b545a0a2a57044c6b768f3e84e865908743349692` |

All APK signatures were independently checked with `apksigner verify
--print-certs`. They are not re-signed. F-Droid's advertised Google signature is
not their actual install-time signer.

Build and validate the client using:

```sh
python3 tools/build_microg_runtime.py --ndk /path/to/android-ndk-r27c
# After changing only the Java compatibility layer:
python3 tools/build_microg_runtime.py --java-only
python3 tools/build_microg_probe.py
# On the dedicated Pi after copying tools/probe artifacts and unlocking guest CE:
python3 test_microg_on_pi.py --probe microg-api-probe.apk \
  --owner microg-owner.jar --require-zygisk-disabled
```

The ordinary client exercises both signature API generations, package lists,
certificate digests/UIDs, wrong-certificate rejection, unchanged self signatures,
cross-user and permission-grant denial, Google AccountManager discovery, and a
real `IGmsServiceBroker.getService` request for the common service. The owner
helper installs through PackageInstaller and opens/dumps microG's real Self-Check
screen. It does not alter microG's self-check code.

Pocket 0.55 passed all 41 ordinary-app API assertions on the dedicated Pi VM,
both with Zygisk disabled and with the original enabled setting restored. The
original microG Self-Check UI confirms signature support, both package
signatures, GSF presence and battery exemption. Seven location/storage/account/
phone/SMS permission rows remain ungranted under normal Android controls; these
are app configuration choices, not missing signature APIs.

The codec probe enumerated six real software codecs and passed PCM, both G.711
variants, MP3 and four-frame H.264 encode/decode checks. Cold locked CE storage,
wrong-password rejection, correct-password data recovery and UI password unlock
also passed. The lock test explicitly requests the real secure credential
bouncer rather than depending on the lock-screen swipe animation.

Local native runtime, Java/D8, APK packaging and original APK signature checks
passed. The verified APK is `out/manager-0.55/aosp-pocket-manager-0.55.apk`,
SHA-256 `e500256e08b7e28e9e606c03929c1225779110c04d4f054a7cf4313d8079dd21`.
The APK and the three original microG downloads are also in the Pi's Downloads.
Attest Probe now reports `gms_present=PASS`; this is separately backed by the
real GMS broker/common-service RPC checks above, not merely package presence.
The final unlisted Attest Probe run has four remaining failures: visible su
paths, visible Magisk paths, test-keys and the contacts query exceeding its
two-second deadline. An initial cold codec enumeration also exceeded the probe's
one-second deadline; its warm enumeration passed. These probe failures were
not suppressed or relabeled. The separate real codec round-trip probe passed.

Google account login, push delivery, online location and Play Integrity verdicts
have not been validated. microG support does not certify the VM or provide
hardware-backed Google attestation.

Upstream references:
[signature support](https://github.com/microg/GmsCore/wiki/Signature-Spoofing),
[installation](https://github.com/microg/GmsCore/wiki/Installation),
[official release](https://github.com/microg/GmsCore/releases/tag/v0.3.17.252432).
