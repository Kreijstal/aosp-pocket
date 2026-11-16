# Authenticated guest screenshots — Manager 0.78.171

Status: offline build and protocol checks pass. Physical-phone capture remains
untested; the phone was disconnected before a screenshot could be obtained.
Android 12L remains **UNQUALIFIED**, with the separate CE162 release gate open.

The paired console previously exposed execution and lifecycle operations, but
no screenshot operation. The phone image also lacks `screencap`. An attempted
capture through the guest SurfaceControl API returned no screenshot. Neither
an activity marked resumed nor that failed capture establishes what the user
actually sees.

Operation 6 now returns the Manager's existing guest display snapshot as PNG,
using the existing loopback listener and mutual HMAC authentication. Copying
the stable front buffer runs through the normal service callback; PNG encoding
runs on the admin worker. Revoking administration closes the connection and
invalidates the capture generation. A missing frame returns an error. This
operation does not start another guest process or request a host screenshot.

The updated client command is:

```sh
python pocketctl.py screenshot screen.png
```

It stages the file privately, verifies the PNG signature, and replaces the
destination only after a successful remote exit. Failed capture leaves an
existing screenshot intact. The APK builder now packages the current client,
so **Save Termux client** includes the new command.

## Offline validation

```sh
mkdir -p out/admin-tests
javac --release 8 -d out/admin-tests runtime/app/src/dev/kreijstal/aosppocket/runtime/AdminServer.java runtime/tests/AdminServerFixture.java
python3 runtime/tests/test_admin.py
```

**PASS: 12 host protocol checks**, including rejection before unauthenticated
capture, a real PNG larger than one output chunk, PNG chunk checksums, private
CLI output, failed-capture preservation and existing disconnect/revocation
checks. The fixture uses the production Java server and Python client; its
PNG comes from a host-only test image, not the phone.

```sh
python3 tools/build_api29_manager.py --base-apk out/product-identity-170/manager/aosp-pocket-manager-0.78.170.apk --host-native-dir out/binder-update-169/host-native --external-native-dir out/api32/native-clean-42/external --catalog out/binder-update-169/phone-catalog-before.json --api32-catalog out/binder-update-169/image/pocket-images.json --package-name dev.kreijstal.aosppocket.api32managerimage --version 0.78.171 --version-code 78171 --out out/admin-screenshot-171/manager
java -cp ../helloworldandroid/build/apksigner.jar com.android.apksigner.ApkSignerTool verify out/admin-screenshot-171/manager/aosp-pocket-manager-0.78.171.apk
```

**PASS**: APK compilation and signing verification. Payload checks confirm the
current client is bundled and the image catalog, product-identity profile and
five host libraries are unchanged from 0.78.170. No image download is needed.

APK SHA-256:
`d8229a4d347519f6808182ca19851ca5f965c0a87f5ad9b5b302a9a2c6596ede`.

After reconnecting: install 0.78.171, enable Termux administration, update the
saved client and capture the actual error screen. Do not count the host test
image as a physical-phone screenshot or infer Revolut success from a resumed
activity record.

## Termux `screencap` command — 0.78.172

The Termux client now exposes the same authenticated PNG capture as:

```sh
python pocketctl.py screencap screen.png
```

`screenshot` remains a compatible alias. Both use the existing paired token
and Manager 0.78.171+ endpoint, retain private file creation, and preserve the
destination after failed capture. This is a Termux command; no guest command
or image payload is added. Manager 0.78.172 bundles the updated client for
**Save Termux client**. A separately updated client also works with 0.78.171.

`python3 runtime/tests/test_admin.py`: **PASS, 12 checks**, including a real
PNG through both command names. The 0.78.172 Manager build and APK signature
verification pass. APK payload inspection confirms Java, native libraries and
the image catalog are byte-identical to 0.78.171; only the client and version
metadata change. Phone capture remains pending reconnection.

APK SHA-256:
`011a18f479982f76873dcbe8867ceb8f5082ebe369e3627852e56982979d1b73`.
