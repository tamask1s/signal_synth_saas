# Synsigra Report Improvements — Round 2

Pack tested: `r_peak_rr_noise_v1` v1.3, verifier v0.13.0.

---

## 1. Aggregate policy checks have no per-case breakdown

**Problem:** The acceptance criterion table shows e.g. `Artifact F1 = 96.91%` but there is no way to see which cases contribute, what their individual artifact-bin counts are, or why this number differs from the per-case total F1 scores shown elsewhere.

**Ask:** Add a breakdown row or expandable section per aggregate criterion showing:
- Which cases contribute to this bin
- Per-case TP/FP/FN within the bin
- Which case(s) pull the aggregate down

---

## 2. Per-case table: "Primary result" has no threshold or verdict

**Problem:** The case table shows `clean_70 → 100.00%` and `external_extreme RR → 55.00%` but:
- No indication what the acceptance threshold is for each row
- No PASS/FAIL badge per case
- An auditor cannot tell if 55% is acceptable or catastrophic without navigating away

**Ask:** Add a "Required" and "Verdict" column to the per-case table, or at minimum a color code (green/amber/red) with a legend.

---

## 3. Detail page: "Within tolerance" percentage without stating the tolerance

**Problem:** The detail page for `external_extreme` RR shows `Within tolerance: 55.00%` but nowhere does it state what the tolerance IS (e.g., ±10ms? ±5%? absolute or relative?).

**Ask:** Show the tolerance definition in the detail page header, e.g.:
> "Tolerance: ±10ms absolute OR ±2% relative (whichever is larger)"

This should come from the `measurement_truth.json` fields (`absolute_tolerance`, `relative_tolerance_percent`).

---

## 4. Terminology: "Predictions" in a detection test

**Problem:** The RR interval detail page uses terms like "Predictions matched" and "prediction_match_fraction". For an auditor evaluating a peak *detector*, "prediction" is confusing — it sounds like a machine learning classifier, not a measurement device.

**Ask:** Use measurement-domain language:
- "Predictions matched" → "Submitted measurements matched to reference"
- "prediction_match_fraction" → "measurement_match_fraction" (or at minimum explain in a glossary)
- "Truth matched" → "Reference values covered"

---

## 5. Detail page: P95, status agreement, etc. not explained

**Problem:** The detail page shows metrics like:
- "P95 absolute error" — no explanation that this is the 95th percentile of |measured - reference|
- "Status agreement" — unclear whether this means the `valid/undefined/absent` status field matches
- No unit shown alongside the value

**Ask:** Either:
- Add a one-line description per metric row (like the acceptance table already does for aggregate criteria), OR
- Add a "Glossary" section at the bottom of every detail page defining each metric shown

---

## 6. No link from aggregate criterion to contributing case details

**Problem:** The aggregate criterion (e.g., `AC-001 Artifact F1 ≥ 60%`) links nowhere. The per-case details link to individual pages, but there's no connection between "which criterion applies to which cases."

**Ask:** Each acceptance criterion row should link to (or list) the contributing case detail pages. Conversely, each detail page already shows the acceptance context — but the main page should also provide the reverse link.

---

## Summary

| # | Issue | Impact | Effort |
|---|-------|--------|--------|
| 1 | No per-case breakdown of aggregate criteria | Auditor cannot trace numbers | Medium |
| 2 | No threshold/verdict in per-case table | Unclear if 55% is OK | Low |
| 3 | Tolerance value not shown in detail page | Cannot assess fairness of scoring | Low |
| 4 | "Prediction" terminology confusing | Misleads non-ML auditors | Low |
| 5 | Metrics unexplained in detail pages | Auditor guesses meaning | Medium |
| 6 | No bidirectional links criteria ↔ cases | Navigation dead ends | Low |
