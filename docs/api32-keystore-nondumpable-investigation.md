# Android 12L: Revolut startup key generation failure

Status: root cause reproduced on pf-phone2, 2026-11-16. **Not fixed or release-qualified.**
Manager 0.78.170, guest image 0.78.169, Revolut 10.135.
The existing Android 12L lifecycle/CE162 release gate remains unresolved.

## Observed failure

The actual Pocket screen shows Revolut's generic “We're sorry, something has
gone wrong. Please try later” error. A resumed LoginActivity does not demonstrate
successful startup.

Revolut's private Crashlytics diagnostic events record repeated
`java.security.ProviderException: Failed to generate key pair`, caused by
`android.security.KeyStoreException: System error`. The stack passes through
`AndroidKeyStoreKeyPairGeneratorSpi.generateKeyPairHelper`, Revolut's cipher
certificate generator, and its token-loading path. Its diagnostic log records
`CertificateException$CertificateFailure` for the default token-encryption
certificate. Static inspection shows an ordinary RSA/ECB/OAEP request using
SHA-1, without an attestation challenge or StrongBox request on this path.

## Controlled reproduction

A temporary Java diagnostic process creates only fresh, randomly named test
keys and deletes its own entries in `finally`. It does not enumerate, read,
replace, or delete existing application keys.

The same process and UID (39998), against the running phone's guest keystore,
produced:

| PR_GET_DUMPABLE | AES | RSA/PKCS1 | RSA/OAEP | EC |
| --- | --- | --- | --- | --- |
| 1, baseline | PASS | PASS | PASS | PASS |
| 0 | FAIL | FAIL | FAIL | FAIL |
| 1, restored | PASS | PASS | PASS | PASS |

All four failures have `KeyStoreException: System error`; the RSA failure matches
Revolut's exception and framework stack. Separate runs under UID 10108 also
passed with dumpability enabled and failed with it disabled. Those runs likewise
used only unique diagnostic aliases and removed their own entries.

The compile commands were:

```sh
javac --release 8 -cp out/api32/tools/android-system32.jar -d out/revolut-rootcause-current/key-probe/classes out/revolut-rootcause-current/key-probe/PocketKeyProbe.java
jar cf out/revolut-rootcause-current/key-probe/classes.jar -C out/revolut-rootcause-current/key-probe/classes .
java -cp ../s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar com.android.tools.r8.D8 --lib out/api32/tools/android-system32.jar --min-api 32 --output out/revolut-rootcause-current/key-probe/dex out/revolut-rootcause-current/key-probe/classes.jar
jar cf out/revolut-rootcause-current/key-probe/probe.jar -C out/revolut-rootcause-current/key-probe/dex classes.dex
```

Compilation and DEX conversion succeeded. After uploading that JAR to
`/tmp/pocket-key-probe.jar`, this command ran inside the existing guest:

```sh
. /system/etc/pocket-runtime-env.sh
export LD_PRELOAD=/system/lib64/libpocket_binder_api32.so
/system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /tmp/pocket-key-probe.jar /system/bin PocketKeyProbe
```

The diagnostic exits zero after recording observations, including the expected
failures. This is a successful reproduction, **not a passing keystore test**.

## Source-level cause

`runtime/api32/keystore_policy.cpp:22` performs a second caller verification
through `pocket_binder_identity()`. That helper, in
`runtime/api28/binder_identity.h`, first opens `/proc/<caller-pid>/stat` and checks
the process start time against its guest identity record.

On this host, the non-dumpable caller's proc entry is inaccessible. The current
Revolut process is recorded by ActivityManager as PID 5942, UID 10108, but reads
of `/proc/5942/stat` and `/proc/5942/status` return ENOENT through the owner
console. Its guest identity record still exists. The controlled dumpability
toggle reproduces the keystore failure without running Revolut.

The failed lookup makes `caller()` return false, so Pocket's
`AIBinder_getCallingSid()` returns null. Stock 12L keystore2's
`check_key_permission()` in `system-security/keystore2/src/utils.rs` requires a
calling SID and maps its absence to `Error::sys()` before the key permission
check. This explains the generic Java `System error` instead of a hardware
attestation or key algorithm error.

The installed policy library matches the locally built library byte for byte:

```text
libpocket_keystore_api32.so
SHA-256 7c8b0c096b27914e5b6fe6cad42b52d8bbfcbf57edd62bd09d5d1320d730b2d1
```

The loader's earlier Binder UID fix does not remove this independent proc-based
keystore policy check. A PackageManager UID probe can therefore pass while
keystore key generation still fails for the same non-dumpable process.

## Correct repair boundary

Keystore's caller/SID adapter should consume the current guest Binder dispatch
context already validated against the kernel sender, with correct nesting and
restoration, instead of depending on a fresh proc lookup after dispatch.
Reject calls outside that context and retain guest UID role checks, APP namespace
ownership, isolated-UID restrictions, and privileged-operation denials. Do not
accept stale identity files as a fallback or force applications to be dumpable.

Validate key generation and use with dumpability 1, 0, and restored 1, plus the
existing role/namespace denial checks. Then check actual Revolut startup and its
diagnostics. No such repair or post-repair Revolut success is claimed here.

The separately confirmed Samsung StorageManagerService/AppFuse watchdogs remain
an open issue. The evidence above does not establish that those watchdogs caused
this particular popup. NTP synchronization errors are also present; they do not
explain the controlled local key-generation failure.

## Preserved private evidence

Local directory: `out/revolut-rootcause-current/f437ae5dd742/`.
It contains the diagnostic exception events, process metadata, policy-library
identity, and key-probe captures. The probe source and JAR are in
`out/revolut-rootcause-current/key-probe/`. A hash manifest is saved with those
captures. Raw logs and APK inspection outputs remain under ignored `out/`;
they are not release assets. Existing screenshots, watchdog records, tombstones,
and CE162 evidence were preserved.

No Manager/image update, guest restart, application data reset, or Revolut APK
modification was performed during this investigation.

The subsequent repair and its separate validation are tracked in
[the caller-context fix](api32-keystore-caller-fix.md); the failed observations
above remain the pre-fix evidence.
