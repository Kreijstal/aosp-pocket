# Contacts latency and encrypted storage

Pocket 0.68 is installed on the dedicated Pi Cuttlefish VM. With the Contacts
provider already running, as requested, ordinary client queries pass the unchanged
2,000 ms budget. Total times were **1,621, 376 and 205 ms**, including provider
acquisition, query and cursor handling. The provider was not restarted for this
acceptance test. Query execution alone took 1,219, 294 and 189 ms. Installed
version, DEX and native library digests matched before and after the test.

The unchanged Attest Probe 1.10 reports **47 PASS / 1 FAIL / 3 SKIP / 8 INFO**.
`contacts_readable`, codec enumeration, su paths and Magisk paths pass. The only
failure is `build_tags_test_keys`, because this guest is a userdebug/test-keys
image. No probe deadline or reported check result was changed.

## Storage changes

The original measurement spent 14,824 ms in the first query. Thread dumps showed
encrypted-storage commits holding filesystem monitors, while unrelated requests
waited. The storage implementation now:

* Keeps a bounded, authenticated 2 MiB plaintext cache in unlocked memory and
  wipes it on eviction or storage close.
* Reuses unchanged ciphertext chunks and compares namespace chunks before
  backend writes, avoiding rereads of unchanged metadata after cache eviction.
* Buffers ordinary writes and metadata, with a checkpoint 250 ms after the
  previous checkpoint finishes. Explicit fsync remains a durability boundary.
* Captures immutable namespace/content snapshots under filesystem monitors,
  then performs encryption and disk flushes outside those monitors. Later writes
  remain pending; collection preserves committed and live references.
* Handles native FUSE fsync requests on a dedicated worker. The receive loop can
  continue processing reads, writes and attributes while a flush is pending.
  Replies retain their request IDs and fsync succeeds only after the Java
  durable commit completes. Exceptions produce EIO. Shutdown joins both workers
  before releasing callbacks and encryption keys.

These changes preserve the encryption format and existing password. Shutdown
drains mapped writes and commits buffered data. Ordinary writes that have not
reached a completed checkpoint or explicit fsync can be lost on a sudden crash.

## Validation

`python3 tools/test_ce_fuse_concurrency.py` passes using production JNI code and
real FUSE protocol sockets. It checks reads/writes during a blocked fsync, no
early durability acknowledgement, sync errors, and shutdown with a full queue.
The old serial implementation fails the read-progress regression.

The isolated Pi fixture also passes through Android's actual AppFuse bridge:
another inode can be read before a deliberately blocked fsync completes. Its
SQLite WAL/FULL, cross-process writer exclusion, shared/executable mappings,
truncation, shutdown persistence and credential removal/restoration checks pass.
The focused fixture build and run commands are:

```sh
python3 tools/build_ce_fixture.py --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c --base-apk out/camera-0.66/aosp-pocket-manager-0.66.apk
ssh pi@juanraspberry 'python3 ~/microg-test/test_ce_fixture_on_pi.py --apk ~/microg-test/ce-fixture.apk'
```

The combined build preserves the camera 0.67 Java classes and other payloads:

```sh
python3 tools/build_contacts_update.py --base-apk out/camera-0.67/aosp-pocket-manager-0.67.apk --version 0.68 --host-library libpocket_ce.so=out/ce-fixture/libpocket_ce.so
```

APK signing and the class/payload audit pass. Cold locked storage, wrong-password
rejection, normal UI password unlock and the existing encrypted-data canary all
pass after installation. The APK is
`out/contacts-0.68/aosp-pocket-manager-0.68.apk`, SHA-256
`99b571d070cc357308eb50479661e3ad5ac5679c64e3f49be2e562b5319c8256`.

Complete commands, hashes and measured reports are recorded in
[contacts-storage-validation.json](contacts-storage-validation.json).

## Scope and remaining cold-start work

A separate stress test deliberately killed `android.process.acore` before the
query. On 0.68 that measured 1,701 ms acquisition plus 5,526 ms query, or 7,235 ms
total; repeats were 82 and 72 ms. This still fails a strict two-second cold-start
target. A diagnostic-only stack sample caught the checkpoint worker scanning
`collectUnreferenced` while the fsync worker waited for the commit lease. That
scan remains a possible source of cold-start stalls. The stack-sampled timing
is excluded from acceptance measurements.

The user subsequently specified assuming an already-running Contacts provider.
The final acceptance test follows that scope; it does not establish that cold
provider initialization completes within two seconds.

The first full Attest launcher attempted a reinstall and raced Android's package
replacement callback, which killed the activity before it wrote a report. The
successful run launches the installed probe with `RuntimeProbeRunner --device`
and no APK argument, removes the old report, and waits for all 59 fresh checks.

No physical phone was modified. Camera/audio tests can use the installed combined
build; replacing the shared APK or restarting the guest still needs coordination.
