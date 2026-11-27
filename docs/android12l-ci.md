# Android 12L lifecycle CI gate

**Current result: FAIL. The build remains UNQUALIFIED / RELEASE BLOCKED.**

The GitHub Actions workflow [Android 12L lifecycle](../.github/workflows/android12l-lifecycle.yml)
runs on pushes, pull requests and manual dispatch. Its job is named
`CE162 full lifecycle gate`. It runs the checker tests, then executes:

```sh
python3 tools/check_android12l_lifecycle.py
```

That command currently exits **1** because Binder cleanup is unresolved and
CE162 run `182e3ad9cec0419cb30677049f69a95e` failed after two strict cycles,
with a SIGSEGV in the capture for cycle2. Missing replacement evidence is a
failure, never a skipped or successful check. The workflow does not use
`continue-on-error` or convert the expected failure into a green result.

The gate reads [ci/android12l-lifecycle.json](../ci/android12l-lifecycle.json)
and the committed [historical evidence manifest](android12l-deployment-evidence.json).
It also replays the native-fatal detector against the
[actual CE162 crash excerpt](../ci/fixtures/ce162-native-crash.json): 37 verbatim
`DEBUG:` lines extracted from the failing cycle2 capture. The fixture records
the original report/cycle hashes and is itself hash-pinned by the CI input.
Extraction verified those hashes locally; the original full capture is preserved
unchanged. CI checks the excerpt's hash, crash identity and detector result.

It works in a clean checkout without the ignored `out/` directory, Pi access,
SSH credentials, guest images or an Android toolchain. This is an **evidence
gate**; the full runtime test must still be performed on the Pi or another
validated test host. This workflow does not run that VM test or change repository
branch-protection settings.

## Evidence required to clear this lifecycle gate

Keep the CE162 historical FAIL and its report hash intact. New successful
evidence belongs to a new run and new files. Update the CI input only after
the Binder fix and complete runtime validation have actually succeeded:

- `binderCleanup.resolved` records the reviewed resolution. `fixCommit` must
  identify an existing commit in this checkout's history that changes runtime
  code. A status checkbox alone cannot pass the gate.
- `binderCleanup.regression` references a JSON report with `path` and `sha256`.
  It must record `validation: PASS`, `affectedProcess: android.process.acore`,
  `binderProxyCleanupObserved: true`, an empty `nativeCrashes` list and its
  actual `nativeLog`. Its image and Manager hashes must match the full run.
  Client-only GC or a cold recovery report is insufficient.
- `fullLifecycle.report`, `fullLifecycle.verification` and `fullLifecycle.budget`
  each reference a JSON file using repository-relative `path` and `sha256`.
  Supply these files in the CI checkout; the workflow does not download evidence
  or credentials. The verification receipt must come from the matching reviewed
  independent full-lifecycle verifier, rather than a hand-edited status summary.
- The full report and independent receipt must agree on run, image, Manager,
  report hash and six cycle hashes. The report's `recoveredSourcePins` must match
  the checkout, including coverage of the runtime source/configuration file
  extensions checked by the gate. Stale passing evidence fails.
- Place all six raw cycle captures and the seventh cold capture beside the full
  report. Preserve their exact `ce-manager-cycle-<runId>-<index>.json` names and
  hashes, even when the original report records their former absolute Pi paths.
- Every raw cycle is inspected again for native fatals, Java fatals, ANRs,
  external-volume cleanup, kernel child ownership, service accounting and warm
  observer teardown. A `check.strictPassed` summary cannot hide a raw crash.
- Require all four credential phases, both actual host-input keyguard unlocks,
  the offline outer Manager restart, the preserved original inventory/key/data
  checks and a seventh cold authless boot without credentials. Process accounting
  must include every actual helper/child, remain within default32 and end at0;
  host policies must remain unchanged and Enforcing.

The checker verifies the supplied evidence's consistency. Causal fix review and
authentic runtime measurement remain necessary; synthetic checker fixtures are
not guest validation. Passing this one gate does not qualify a build or clear
the other [deployment checklist](android12l-deployment-checklist.md) items.
It never rewrites the historical evidence or changes qualification/support flags.

## Focused checker tests

```sh
python3 -m unittest discover -s tools/tests -p 'test_android12l_lifecycle_gate.py'
```

The positive control uses explicitly synthetic files in a temporary repository.
The crash rejection case injects the actual recorded excerpt into that otherwise
passing synthetic lifecycle, so a relabelled PASS still fails raw inspection.
Other negative cases include CE162's `DEBUG:signal 0 (SIGSEGV)` form, false
PASS summaries, missing captures, incomplete credential/UI checks, stale sources,
unobserved affected-process cleanup and excess process counts. The default
command is also tested in an isolated checkout without `out/`, including under
Python optimization, and must exit1 for the preserved CE162 failure.

The workflow commands were also executed in a fresh local Git clone with the CI
changes overlaid, no `out/` directory and no ignored Android artifacts. All21
tests passed, then the gate exited1 and reported the recorded SIGSEGV. This
verifies **recorded failure detection**, not a newly triggered Android crash or a
GitHub-hosted workflow run. A fresh live crash reproduction remains unverified.
