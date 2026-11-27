# Pocket attestation backend validation

The Manager projects `local-software` into the guest marker and boot environment.
Selecting `host-passthrough` or `embedded-simulator` logs that the requested
backend is unavailable and falls back to stock software at startup. Older
preferences containing `teesimulator` are read as `embedded-simulator`.
**Reset guest keystore** remains a manual action and makes apps recreate keys.

## Stock baseline

The expected `payload_classifier_verdict` for stock software, including both
fallbacks, comes from `passthrough/validate/expected-verdict.json` in the design
checkout. Its `route` field corresponds to `predictedRoute` in the probe report.

| Field | Expected value |
| --- | --- |
| `verifiedBootState` | `2` |
| `pinStatusEnum` | `2` |
| `w24` | `false` |
| `predictedCode` | `0x314` |
| `predictedRoute` | `blocked` |

Any mismatch falsifies the baseline. A report with `attestationAvailable=false`
or `classifierStructSentinel=true` is inconclusive because it lacks an observed
chain. A fallback is confirmed by a single startup warning that names the
requested backend and says it is unavailable, together with `local-software` in
both the `0600` marker and `POCKET_ATTESTATION_BACKEND`.

To inspect a previously captured stock probe report offline, run:

```sh
jq -e '.results[] | select(.id == "payload_classifier_verdict") |
  .attestationAvailable == true and .verifiedBootState == 2 and
  .pinStatusEnum == 2 and .w24 == false and
  .predictedCode == "0x314" and .predictedRoute == "blocked"' report.json
```

`tools/build_manager_local.sh` compiles the Manager Java code and verifies the
APK signature. The local APK contains the manifest and dex and omits native
runtime libraries and guest images. Guest startup and probe collection require
a full Manager build and were outside the offline validation.
