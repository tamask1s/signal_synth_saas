# Synsigra Truth Policy Issues — R-peak Scoring Fairness

## Resolution (implemented 2026-07-23)

The valid findings are fixed in core and the generator-free verifier. The
result is intentionally stricter and more deterministic than either proposed
heuristic:

- `annotations.json` v2 marks event scoreability under the shared
  `synsigra_observable_event_truth_v1` contract.
- ECG truth is excluded only when its full QRS support does not fit in the
  record, or when a complete all-lead ECG dropout leaves at most 5% of the
  known generated signal. The decision is derived from scenario/render
  parameters, never from a post-hoc local-amplitude measurement.
- A prediction near excluded truth is retained in the report for traceability
  but is not counted as an FP. Excluded truth is not counted as an FN.
- RR truth remains one row per consecutive constructed QRS pair. A row whose
  previous or current endpoint is unscoreable is explicitly
  `not_evaluable`; no interval is silently bridged or deleted.
- Severe additive and external noise remains scoreable. In particular, the
  `external_extreme` -12 dB case is still a real stress test; its 30 ms RR MAE
  acceptance limit remains isolated to that stratum, while the global and
  standard-case limit remains 25 ms.

For the reported pack this excludes beats 45–47 in `analytic_extreme` and the
physically truncated final QRS in `external_extreme`. It does not forgive the
noise-region FP/FN observations.

The policy is implemented in shared core code and consumed by annotations,
RR measurement truth, native event/classification scoring and Python local
verification. It therefore applies consistently to every pack, not only
`r_peak_rr_noise_v1`. A full release audit rendered all 18 curated packs (101
cases, 7,916 QRS events and 9,981 measured PPG fiducials); 14 QRS events were
explicitly excluded for one of the two allowed reasons, and no inconsistent
truth, report notice or internal HTML link remained.

During the same audit, broken challenge-index links were fixed from
`<case>/report.html` to `cases/<case>/report.html`, and all native scoring HTML
reports were aligned with the single neutral-gray evidence notice.

## Summary

We identified **5 false negatives and 5 false positives** in the `r_peak_rr_noise_v1` pack (v1.2) verification results that are not algorithm defects but rather truth policy / scenario design issues. The HRV custom pack has zero errors (clean signal, no edge cases).

All issues are in two cases: `analytic_extreme` and `external_extreme`.

---

## Issue 1: Undetectable beats scored as FN (`analytic_extreme`, beats 45-47)

### Observation

Beats 45, 46, 47 (t=35.67s, 36.41s, 37.17s) are scored as false negatives, but the signal at these positions has:

- R-peak amplitude: **0.018 mV** (vs normal ~0.9 mV)
- Local peak-to-peak: **0.023 mV**
- Attenuation vs clean beats: **~50x = 34 dB**

The "peaks" technically exist as local maxima, but their amplitude is at the quantization noise floor (23 µV). No peak detector can reliably distinguish these from noise.

### Evidence

| Beat | Time (s) | Amplitude (mV) | Local p-p (mV) | Note |
|------|----------|----------------|-----------------|------|
| 44 | 34.864 | 0.907 | 1.161 | Normal — detected ✓ |
| **45** | **35.675** | **0.018** | **0.023** | **34 dB attenuation — FN** |
| **46** | **36.407** | **0.018** | **0.023** | **34 dB attenuation — FN** |
| **47** | **37.166** | **0.018** | **0.023** | **34 dB attenuation — FN** |
| 48 | 37.964 | 0.800 | 1.052 | Recovering — detected ✓ |
| 49 | 38.755 | 0.906 | 1.160 | Normal — detected ✓ |

### Proposed fix

These beats should be excluded from R-peak scoring because the signal is not observable. Two options:

**Option A (preferred):** Add an `observability_threshold` to the truth policy. Beats where the local signal amplitude is below a configurable fraction of the median beat amplitude (e.g., <5%) should be marked as `not_scoreable` and excluded from TP/FN counting.

**Option B:** Mark the corresponding time interval as an artifact in the scenario's artifact layer. The existing `exclusion_policy` already handles artifact-bin beats differently (lower F1 threshold). If these beats fall in an "artifact" bin, the F1 threshold is 60% rather than 90%.

---

## Issue 2: End-of-record beat scored as FN (`external_extreme`, beat 21)

### Observation

Beat 21 (t=17.986s) has **normal amplitude** (0.907 mV) but is only **7 samples (14ms) from the end of the record** (18.000s). The peak detector requires post-peak context to confirm a detection (it needs to see the signal descend after a peak). With only 14ms of signal remaining, this is impossible.

### Evidence

- Record length: 9000 samples = 18.000s
- Beat 21 at sample 8993 (t=17.986s)
- Remaining samples after peak: 7 (14ms)
- Beat amplitude: 0.907 mV (perfectly normal)
- The detector's last detection is at t=17.118s — it simply runs out of signal

### Proposed fix

**Option A (preferred):** Extend the `first_beat` exclusion policy to also cover the **last beat**. The current truth_policy says: *"first_beat: Excluded because no preceding in-record R peak is observable."* The same logic applies to the last beat: *"last_beat: Excluded because insufficient post-peak context exists for reliable detection."*

A concrete rule: exclude any truth beat within `N` samples of the record boundary (e.g., N = 50 = 100ms at 500Hz).

**Option B:** Extend the scenario duration by 1-2 seconds beyond the last annotated beat, giving the detector sufficient trailing context.

---

## Issue 3: FP and FN inside severe noise region (`external_extreme`, 7-10s)

### Observation

In the 7.00-10.00s artifact interval (severity=1.0, external noise at -12dB SNR):
- **1 FN:** Beat 8 (t=7.20s) — signal is present but buried in noise (p-p = 0.93 mV with noise p-p = 2.12 mV, SNR < 0 dB)
- **5 FP:** Detections at 7.33, 7.84, 8.10, 8.30, 8.57, 8.90, 9.17s — noise peaks that look like QRS

The detector is confused by the noise (correctly so — the noise is stronger than the signal). These are scored in the "artifact" bin which has a 60% F1 threshold, so they don't cause a policy failure. But they inflate the aggregate RR MAE.

### Proposed consideration

This is **acceptable behavior** for a stress test — the pack is designed to push the detector to failure. However, for the RR MAE strata calculation, these artifact-region FPs generate large spurious RR intervals that dominate the aggregate error.

Consider: should RR intervals that involve an FP (matched to noise, not to a real beat) be excluded from the RR MAE calculation? Currently the `truth_policy` says *"artifact_overlap: Retained for RR scoring"* — this may be too harsh for -12dB conditions.

---

## Summary table

| Case | Issue | Beats affected | Root cause | Suggested action |
|------|-------|----------------|------------|------------------|
| analytic_extreme | Undetectable beats scored as FN | 45, 46, 47 | 34 dB attenuation = signal at noise floor | Add observability exclusion |
| external_extreme | End-of-record beat scored as FN | 21 | 14ms from record end, no post-peak context | Exclude last beat or extend record |
| external_extreme | FN + FP in -12dB noise | beat 8 + 5 FPs | -12dB SNR, noise > signal | Already handled by artifact bin; consider RR MAE exclusion |

---

## Impact on current results

With these fixes, the expected results would improve from:
- **analytic_extreme R-peak:** F1 from 96.97% → **100%** (3 FN removed)
- **external_extreme R-peak:** F1 from 85.11% → **~90%** (1 FN removed, 5 FP unchanged as they are noise-induced)
- **Aggregate R-peak F1:** from 98.00% → **~99%**
- **RR MAE:** would decrease as the extreme outlier intervals are removed

None of these issues affect the clean, rate-range, or moderate-noise cases (all 100% F1).
