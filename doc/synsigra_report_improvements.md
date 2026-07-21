# Synsigra Verifier Report Generator — Feature Gaps & Improvement Proposals

## Context

The `synsigra-verify` CLI generates HTML reports (`verification_report.html` and per-case `comparison_report.html`) as verification evidence artifacts. In their current form, these reports are **not audit-friendly** — they require manual post-processing to be presentable to a notified body, FDA reviewer, or internal QA auditor.

An auditor (e.g., notified body, FDA reviewer) expects the following from a verification evidence report:

1. **Clear verdict** next to every evaluated criterion
2. **Traceability** from the top-level summary down to individual measurement results
3. **No unexplained pass/fail** — every number must be accompanied by its threshold and rationale
4. **Navigable structure** — main report → per-case details → back to main

This document details the specific gaps observed and proposes concrete improvements.

### Packs evaluated

The observations in this document are based on reports generated from the following packs. Issues marked **(generic)** apply to all packs; issues marked with a specific pack name apply only to that pack's report structure.

| Pack ID | Description | Targets | Fs |
|---------|-------------|---------|-----|
| `r_peak_rr_noise_v1` | Curated R-peak & RR Noise Verification Pack | r_peak, rr_interval, signal_quality | 500 Hz |
| `hrv_robustness_v2` | Curated HRV Robustness Pack | r_peak, hrv, signal_quality | 100 Hz |
| `custom_pack_94737aefcfe52ad359355a5e1052e991` | Custom HRV 500Hz benchmark | r_peak, hrv | 500 Hz |

Most issues are **generic** (they affect all packs equally). Pack-specific observations are noted inline where relevant.

---

## 1. Main verification_report.html Structure **(generic)**

### Current state

- Flat table with raw JSON field names as labels (e.g., `rr_interval/mean_absolute_error`)
- "Report" column contains plain text file paths, not clickable links
- No human-readable description of what each metric measures
- Status shown as `diagnostic_failed` even when the failure is a single marginal check
- No summary verdict box (pass/fail) with visual hierarchy

### Proposed structure

```
1. Header
   - Pack name, version, fingerprint
   - Algorithm name, version (from submission.json)
   - Date, verifier version, scoring contract

2. Summary verdict (green/red box)
   - "ALL ACCEPTANCE CRITERIA PASS" or "N CRITERIA NOT MET"
   - One-sentence description

3. Acceptance criteria table
   Columns: Metric | Description | Required | Actual | Verdict
   - Every applicable policy check = one row
   - "Description": human-readable (e.g., "Mean timing error of RR intervals")
   - "Required": operator + value (e.g., "≤ 25ms")
   - "Actual": measured value with unit
   - "Verdict": PASS/FAIL badge (green/red)

4. Per-case results table
   Columns: Case | Target | Primary score | Detail
   - "Detail" = clickable link to comparison_report.html
   - Optional: per-case pass/fail if case-level thresholds exist

5. Notes/Exclusions section (if applicable)
   - Neutral gray box (not alarming red)
   - Explains any excluded targets or custom thresholds
   - References specific checks

6. Footer
   - Pack fingerprint, generator git commit, provenance chain
```

---

## 2. Per-case comparison_report.html **(generic)**

### Current state

- No back-link to the main report (user cannot navigate back)
- R-peak table shows F1/Se/PPV but no acceptance threshold alongside
- Measurement (RR/HRV) table shows only aggregate numbers, no per-metric truth vs actual
- Notice text is too long and styled with alarming red color
- No acceptance context (what global check does this case contribute to?)

### Proposed structure

```
1. Navigation: "← Back to main report" link

2. Header
   - Case ID, scenario ID, target
   - Sampling rate, duration, channel count

3. Notice (gray, short)
   "Synthetic engineering QA evidence; not diagnosis."

4. R-peak target: existing bin table (total/clean/artifact) BUT with:
   - New column: "Required" threshold for each bin
   - Verdict badge per row

5. Measurement target (rr_interval / hrv):
   - Window-scope metrics table:
     Columns: Metric | Truth value | Measured value | Absolute error | Tolerance | Verdict
   - Beat-scope summary:
     "N/M RR intervals matched within tolerance. MAE = X ms."
   - Not a raw dump of matched_count/extra_count without context

6. Acceptance context box
   - Which global policy check(s) this case contributes to
   - The aggregated value and how this case affects it

7. Footer: file references (comparison.json, comparison.csv)
```

---

## 3. Notice Text and Styling **(generic)**

### Current

- Text: `"Synthetic engineering QA evidence; not diagnosis, patient monitoring, clinical validation certification, or standalone conformity assessment."`
- Style: `border-left: 4px solid #b42318; background: #fef3f2` (red, alarming)

### Proposed

- Text: `"Synthetic engineering QA evidence; not diagnosis."` (shorter = clearer)
- Style: `border-left: 4px solid #6b7280; background: #f9fafb` (gray, neutral)
- Make it configurable: CLI flag `--notice-text "..."` or a field in submission.json

---

## 4. Policy Check Table: Human-Readable Descriptions **(generic)**

The current policy check names are machine field paths. A mapping to human descriptions is needed:

| Machine name | Proposed display name |
|---|---|
| `r_peak/total/f1_score` | R-peak detection F1 score (overall) |
| `r_peak/clean/f1_score` | R-peak detection F1 score (clean signal) |
| `r_peak/artifact/f1_score` | R-peak detection F1 score (artifact regions) |
| `r_peak/total/mean_absolute_error_seconds` | R-peak mean timing error |
| `rr_interval/overall/prediction_match_fraction` | Fraction of RR predictions matched to truth |
| `rr_interval/overall/truth_match_fraction` | Fraction of truth RR intervals matched |
| `rr_interval/overall/tolerance_pass_fraction` | Fraction of matched RR values within tolerance |
| `rr_interval/rr_interval/mean_absolute_error` | RR interval mean absolute error (aggregated) |
| `rr_interval/rr_interval/p95_absolute_error` | RR interval 95th percentile error |
| `hrv/sdnn_seconds/tolerance_pass_fraction` | SDNN accuracy (within defined tolerance) |
| `hrv/rmssd_seconds/tolerance_pass_fraction` | RMSSD accuracy (within defined tolerance) |
| `hrv/pnn50_percent/tolerance_pass_fraction` | pNN50 accuracy (within defined tolerance) |
| `hrv/lf_power_seconds2/tolerance_pass_fraction` | LF power accuracy |
| `hrv/hf_power_seconds2/tolerance_pass_fraction` | HF power accuracy |
| `hrv/lf_hf_ratio/tolerance_pass_fraction` | LF/HF ratio accuracy |
| `signal_quality/overall/time_f1_score` | Signal quality interval detection F1 |
| `signal_quality/overall/temporal_iou` | Signal quality temporal overlap (IoU) |

**Implementation suggestion:** Add a `display_name` field to each entry in `verification_protocol.json` or `scoring_manifest.json`. The verifier renders `display_name` in the HTML if present, falls back to the machine path otherwise.

---

## 5. Navigation and Linking **(generic)**

### Current

- The main report "Report" column shows file paths as plain text (e.g., `verification/clean_70_r_peak/comparison.json`)
- Points to `.json` not `.html`
- No back-navigation from per-case to main

### Proposed

- Main report: clickable `<a href="...">` links to `comparison_report.html`
- Per-case: `← Back to main report` link at top
- If multiple main reports exist (evidence + diagnostic), clarify which is authoritative
- Consider: breadcrumb navigation (`Main > Case: clean_70 > Target: r_peak`)

---

## 6. Custom Acceptance Threshold Support

### Current limitation **(observed with `r_peak_rr_noise_v1` — MAE threshold 25ms too strict for extreme noise case)**

Pack-embedded thresholds are immutable. If a user's acceptance criteria differ (e.g., MAE ≤ 30ms instead of pack default 25ms), there is no mechanism to express this — the user must manually post-edit the HTML.

### Proposed solutions (pick one or combine)

1. **CLI flag:** `--threshold rr_interval.mean_absolute_error.max=0.03`
2. **Override file:** `acceptance_overrides.json` alongside submission.json:
   ```json
   {
     "overrides": [
       {
         "target": "rr_interval",
         "metric": "mean_absolute_error",
         "operator": "max",
         "threshold": 0.03,
         "rationale": "30ms appropriate for HRV application; only external_extreme exceeds 25ms"
       }
     ]
   }
   ```
3. **Report annotation:** The HTML should show both pack default and custom threshold, with clear labeling:
   ```
   Required: ≤ 30ms (custom; pack default: 25ms)
   ```

---

## 7. Evidence Mode: Target Exclusion Support

### Current limitation **(affects `r_peak_rr_noise_v1` and `hrv_robustness_v2` — both include `signal_quality` target)**

In evidence mode, ALL targets defined in the pack must be submitted. If a target is not implemented by the algorithm (e.g., `signal_quality`), the user must either:
- Submit empty files (which produces 0% scores and FAIL)
- Use diagnostic mode (which loses "evidence eligible" status)

### Proposed solution

Allow the submission to declare excluded targets with rationale:

```json
{
  "excluded_targets": [
    {
      "target": "signal_quality",
      "reason": "Signal quality classification is not implemented by the algorithm under test. The algorithm scope is R-peak detection and RR interval derivation only."
    }
  ]
}
```

The evidence report should:
- Show excluded targets in a separate section (not as FAIL)
- Clearly state they are excluded by the submitter
- Still mark the evidence as "eligible" for the non-excluded targets
- Include the exclusion rationale in the report

---

## 8. Measurement Comparison Report Detail Level

### Current (measurement target comparison_report.html) **(affects `hrv_robustness_v2`, `r_peak_rr_noise_v1` rr_interval target, custom HRV packs)**

Shows a single summary row:
```
| Measurement | Scope | Method | Preprocessing | Window | Truth | Predictions | Numeric pairs | Pass fraction | Missing | Extra | MAE |
| rr_interval | beat  |        |               |        | 22    | 22          | 22            | 1.0           | 0       | 0     | 0.001 |
```

This does not tell an auditor:
- What the truth values were
- What the measured values were
- What tolerance was applied to each
- Which specific measurements passed/failed

### Proposed

**For window-scope metrics (SDNN, RMSSD, etc.):**

```
| Metric | Truth | Measured | Abs error | Tolerance (abs) | Tolerance (rel) | Verdict |
| mean_rr_seconds | 1.000 s | 0.998 s | 0.002 s | 0.01 s | 2% | PASS |
| sdnn_seconds | 0.050 s | 0.046 s | 0.004 s | 0.01 s | 10% | PASS |
| rmssd_seconds | 0.047 s | 0.047 s | 0.000 s | 0.01 s | 10% | PASS |
```

**For beat-scope metrics (RR intervals):**

Summary:
```
299 RR intervals evaluated.
Matched: 299/299 (100%)
Within tolerance: 299/299 (100%)
MAE: 0.97 ms
p95 error: 1.14 ms
```

Optional expandable/downloadable detail table.

**Data source:** The `measurement_truth.json` contains `absolute_tolerance` and `relative_tolerance_percent` for every measurement. The `comparison.json` contains per-match errors. These should be rendered, not hidden.

---

## 9. Summary PDF Export (Nice to Have)

Auditors frequently request PDF evidence packages. Options:

1. `--format pdf` CLI flag (requires wkhtmltopdf or similar)
2. Print-friendly CSS (`@media print`) that produces clean PDF from browser Print dialog
3. At minimum: ensure the HTML renders well when printed (no cut tables, no broken layouts)

---

## 10. Status Text Semantics

### Current

- `diagnostic_failed` — sounds alarming even if only 1/14 checks fail marginally
- `evidence_failed` — same

### Proposed

- Separate "scoring status" from "policy status":
  - `scoring: complete (16/16 case-targets scored successfully)`
  - `policy: 13/14 checks pass, 1 marginal failure`
- Or use graduated language:
  - `pass` / `marginal_fail` / `fail`
  - Where `marginal_fail` means <5% deviation from threshold

---

## Priority Matrix

| # | Feature | Effort | Impact | Priority |
|---|---------|--------|--------|----------|
| 1 | Notice text/style config | Low | High (cosmetic, first impression) | P0 |
| 2 | Links in main report (href to .html) | Low | High (navigation) | P0 |
| 3 | Per-case back link | Low | Medium (navigation) | P0 |
| 4 | Acceptance table "Description" column | Medium | High (auditability) | P1 |
| 5 | Per-case acceptance context | Medium | High (traceability) | P1 |
| 6 | Target exclusion in evidence mode | Medium | High (usability) | P1 |
| 7 | Custom threshold support | Medium | Medium (flexibility) | P2 |
| 8 | Measurement detail report (truth vs actual per metric) | High | High (HRV auditability) | P1 |
| 9 | Status text semantics (marginal vs hard fail) | Low | Medium (clarity) | P2 |
| 10 | PDF export | Medium | Medium (auditor convenience) | P3 |

---

## Summary

The core scoring engine works correctly — it produces accurate numeric results. The gap is in **presentation and navigability**. An auditor receiving the current reports must:

1. Manually interpret machine field names
2. Cross-reference multiple JSON files to understand what was tested
3. Cannot navigate between summary and detail views
4. Sees alarming red styling on informational notices
5. Cannot exclude non-applicable targets without losing evidence eligibility
6. Cannot adjust thresholds without post-editing HTML

Addressing items P0-P1 would make the reports audit-ready without manual intervention.
