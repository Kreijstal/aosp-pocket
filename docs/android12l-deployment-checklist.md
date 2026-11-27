# Android 12L deployment checklist

**Status: UNQUALIFIED — RELEASE BLOCKED.**

The full credential-lifecycle run failed. Binder cleanup remains unresolved.
No release gate below is satisfied by this document, and no qualification or
support flag is changed. This checklist does not authorize deployment or publication.

The [CI lifecycle evidence gate](android12l-ci.md) currently fails on CE162.
It requires a Binder fix record and complete matching replacement evidence;
a scoped recovery PASS cannot clear it.

## Candidate and recorded result

This record concerns the ARM64 API32 image **0.78.136** and the test Manager
package `dev.kreijstal.aosppocket.api32managerimage`. The Manager is a probe build,
not a production release artifact.

| Artifact | SHA-256 |
| --- | --- |
| Downloadable image | `86cc3e1d1f09563cdc5464a1cd17744186d4e5f1cd4b960a037ae4c960025d65` |
| Test Manager APK | `9483c38d6daa5fe808ade871deb0551f9015930bd948fe45346447ecb84f66c1` |
| Failed full-lifecycle report | `4fb9487164bd360e85da9f2f11057dd5e72b6efa1649f2d422cd00da2f9da948` |

The [evidence manifest](android12l-deployment-evidence.json) records hashes and
sizes for 114 existing evidence/artifact files and binds the 807-file source
snapshot through the original candidate's source pins. Paths in that manifest
are relative to the repository root.

| Evidence | Recorded outcome | Release implication |
| --- | --- | --- |
| [Full run CE162](../out/api32/recovered162-result/ce-manager-report.json), run `182e3ad9cec0419cb30677049f69a95e`; [failure audit](../out/api32/recovered162-result/failure-audit.json) | **FAIL**. Strict cycles 0 and 1 passed; captured cycle 2 failed native-crash validation. The six-cycle lifecycle, offline outer restart, both physical UI unlock checks and seventh cold proof were not completed as one full run. | Mandatory lifecycle gate remains open. |
| [Cold recovery165](../out/api32/own-authless165-result/independent-check.json), run `9705668a84e04565984518c5000ab45f` | Scoped PASS: original password-free state, original 143 retained entries, UID10113 witness key/data, five private canaries and public fixture survived a cold restart without a password channel. | Recovery/data preservation only; does not fix Binder or supersede CE162. |
| [Exact tombstone167](../out/api32/acore167-result/independent-check.json) and [raw tombstone](../out/api32/acore167-result/tombstone_07.txt) | Read-only collection PASS. The stale or overwritten Binder reference points into a SQLite index allocation; the responsible allocation/free or writer is not established. | Diagnostic evidence; no fix is qualified. |
| [Native repeat163](../out/api32/native163-repro/independent-check.json) | Four retained host libraries reproduced byte-for-byte from their original source/build root with caches retained. | Does not resolve the current whole-source-lock packaging guard or qualify the differing native158 libraries. |

CE162's failing capture contains a SIGSEGV in `android.process.acore`, logical
UID10036, thread `ReferenceQueueD`, at `RefBase::decStrong+16` called by
`BinderProxy_destroy+60`. Password removal was recorded in that phase, but the
native crash still makes the run fail. Its 347 process samples peaked at31
under the unchanged default limit32. Cold recovery165 had106 samples, peak30;
all native guests/helpers stopped and the exact Manager base was restored.
These sampled counts establish no hard process bound.

## Preserve evidence before further testing or deployment

- [ ] Make a durable archive of the manifest's files, the referenced source
  snapshot and the signed artifacts; independently recheck every size/hash
  after transfer. Keep the original paths or retain a documented path mapping.
- [ ] Preserve CE162's raw report, all three cycle captures, console/native
  logs, process-budget samples, AMS accounting, observer/producer exits,
  candidate pins, source snapshot and independent failure audit together.
- [ ] Preserve recovery165, the exact tombstone167, failed diagnostic166 and
  earlier failures/recovery records. New runs get new IDs and output paths;
  never overwrite, delete or relabel a failed report as PASS.
- [ ] Preserve the original 143 retained entries, UIDs, certificates, kept-data
  state, witness UID10113 non-exportable key/data, five private canaries and
  existing provider-owned public fixture. No profile reset, uninstall,
  data-clear, disk/overlay replacement or fixture reseeding may manufacture a pass.
- [ ] Keep protected recovery credentials private, in place and mode0600.
  Do not print, copy into evidence archives or publish them. This manifest
  contains no protected credential file. Credential-file removal requires its
  own successful restoration proof; failure evidence must still be retained.

## Resolve Binder cleanup — mandatory release gate

- [ ] Identify and fix the ownership/lifetime or memory-corruption cause of
  the stale Binder reference. Record the causal evidence, patch and review.
  Ignoring SIGSEGV, skipping finalization, disabling Contacts or filtering the
  crash out of validation does not resolve this gate.
- [ ] Add a focused regression that exercises the affected Binder lifetime,
  including the actual `android.process.acore` cleanup path. Contacts queries
  with client-side GC alone cannot establish that acore's cleanup is fixed.
- [ ] Run the narrow rebuild and regression for the fix; retain exact commands,
  outputs and artifact hashes. Bind the new source snapshot, image, Manager,
  native libraries and signing certificate to the subsequent runtime tests.
- [ ] Demonstrate clean Contacts/Binder operation and teardown with no native
  crash, Java fatal or ANR. Retain the strict crash and cleanup checks unchanged.

## Pass the complete lifecycle on the fixed candidate

- [ ] Use a fresh full-run ID and matching collector/independent verifier.
  Reproduce all **six strict cycles** without resets or substituting recovery
  results for missing cycles. Retain every raw capture and acknowledgement.
- [ ] Verify wrong-password rejection, correct unlock, password change,
  password removal, authless boot and password restoration across the required
  encrypted phases, with mounted storage and app readiness checked each time.
- [ ] Verify **both physical keyguard unlocks through actual host input**,
  including the offline outer Manager restart. Retain the readiness, awake/UI
  state, input and unlock observations; credential CLI calls are not UI proof.
- [ ] Complete the **seventh cold boot** in the restored original password-free
  state, with no password channel or credential request. Verify the original
  inventory, UIDs, certificates, keys/data and private/public canaries again.
- [ ] Preserve the original private canary's public-alias denial. Reuse the
  existing proper public fixture by verifying its bytes without overwriting it.
- [ ] Require clean service, observer, external-volume, native-owner and kernel
  descendant teardown for every cycle. No leaked process or native fatal may
  be waived because functional checks passed.
- [ ] Count every actual same-UID child/helper, including warm observers, across
  startup, workload and teardown. Retain independent accounting under default32
  with Enforcing and unchanged host policies, RAM and disk layout. Do not
  subtract helpers or relax deadlines/limits to obtain a pass.
- [ ] Obtain a new independent **full-lifecycle PASS** tied to the fixed
  artifacts. CE162 must continue to read FAIL in the preserved history.

## Finish candidate validation and delivery

- [ ] Bind AV, all nine namespace groups, ordinary app/Keystore/storage and
  genuine Signal results to the final candidate. Rerun affected checks after
  changes; earlier scoped AV137/namespace144/Signal139 passes do not establish
  an untested combination of image, classes and native libraries.
- [ ] Complete fresh plaintext-to-encrypted migration in a distinct sandbox,
  Android9/10 regressions on the final backend, and physical-phone/media checks.
  Preserve the existing normal profile throughout; unavailable tests remain open.
- [ ] Resolve native source provenance and the packaging guard without bypassing
  hash checks. If changed native158 binaries are selected, qualify those exact
  binaries; the retained libraries' results cannot be transferred to them.
- [ ] Build and validate the production Manager separately from the probe APK.
  Verify signing/update continuity, app data preservation and a tested rollback
  path that does not depend on resetting guest data.
- [ ] Verify HTTPS catalog/image delivery on demand, no bundled guest images in
  the production APK, archive/manifest/payload integrity, download recovery and
  cached offline restart. Finish matching source, provenance and license delivery.
- [ ] Review the complete candidate evidence before considering any release.
  Until all mandatory gates pass, retain **UNQUALIFIED / RELEASE BLOCKED** and
  keep `full_guest_boot_verified` and `pocket_manager_api32_supported` false.
  This checklist does not mark the build qualified, even if individual scoped
  checks pass.

See [Android 12L bring-up](android12l-port.md) and the current summary in
[source/image validation](api32-source-image-validation.json) for the wider
implementation history and remaining gates.
