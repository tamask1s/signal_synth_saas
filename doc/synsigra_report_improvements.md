# Synsigra verification evidence report

Status: implemented on 2026-07-21 in Python verifier `0.11.0` with the clean
`synsigra_local_verification_v3` contract.

This document records the final audit-reporting design. It replaces the
earlier gap list; the old output names and layouts are intentionally not
supported.

## Minimal result structure

```text
verification-results/
├── index.html
├── evidence.json
└── details/
    └── <case-target>.html
```

There is one canonical machine-readable record, one human entry point and one
human detail view per evaluated case-target. The verifier no longer emits a
parallel CSV summary, a second summary JSON, per-case JSON copies or per-case
CSV files. For `N` case-targets the result contains exactly `N + 2` files.

## Human entry point

`index.html` contains:

- a prominent PASS, FAIL or INCOMPLETE verdict;
- package, generator, algorithm, verifier and protocol provenance;
- evidence/diagnostic mode and evidence-eligibility state;
- package integrity and complete matrix coverage;
- every applicable and non-applicable acceptance criterion with a stable
  `AC-NNN` ID, human name, description, unit, required value, actual value,
  signed margin and verdict;
- one row per case-target with its primary metric and a link to its detail
  page;
- the HRV processing-stage trace when applicable;
- a link to `evidence.json`.

`SCORED` means that a valid comparison was produced. It is deliberately not a
case-level PASS. Only the packaged aggregate acceptance policy determines the
overall PASS/FAIL verdict.

## Case-target detail pages

Every page under `details/` links to `../index.html` at the top and bottom and
links to the canonical evidence JSON. It identifies the case, scenario,
target, algorithm output, fingerprints, duration, sampling rate and relevant
aggregate acceptance criteria.

Target-specific views include:

- event detection strata, TP/FP/FN, sensitivity, PPV, F1 and timing error;
- classification summary and per-class metrics;
- interval overlap, boundary error and per-label metrics;
- delineation accuracy and per-wave metrics;
- measurement truth, algorithm value, absolute error, absolute and relative
  tolerance inputs, effective tolerance, tolerance rationale and verdict.

The complete raw comparison remains embedded once in `evidence.json`.

## Notice and presentation

Every generated result HTML and every human-readable HTML shipped inside the
challenge contains exactly this notice once:

> Synthetic engineering QA evidence; not diagnosis, nor clinical evidence

The notice uses a neutral gray treatment. Reports are self-contained,
responsive and print-friendly; no external CSS, JavaScript or generated PDF is
required.

## Evidence policy decisions

Packaged evidence is immutable:

- evidence mode evaluates the complete protocol case-target matrix;
- evidence mode uses only the acceptance policy shipped in the challenge;
- caller-selected thresholds, target exclusions, case filters and target
  filters are rejected in evidence mode;
- custom thresholds and scoped exploratory runs use explicit diagnostic mode
  and are always marked non-evidence;
- a differently scoped evidence claim requires a separately authored pack and
  protocol, not an after-the-fact report override;
- there is no configurable notice text and no “marginal pass” state.

These constraints preserve audit meaning: a user cannot silently change the
claim after seeing the result.

## Verification kit

The generator-free customer kit is also minimal:

```text
verification-kit/
├── README.txt
├── challenge/
└── submission/
```

Provenance and the engineering claim boundary occur once inside the immutable,
manifest-protected `challenge/` tree. Normalized trusted metadata is returned
by the job API and is not duplicated inside the ZIP.

## Automated acceptance

Core tests enforce:

- exact file count and names;
- all index-to-detail and detail-to-index links resolve;
- every result HTML contains the exact notice once;
- all ten supported target families render;
- measurement evidence retains truth, prediction, error and tolerance data;
- source-tree and installed-wheel verification produce the same v3 contract.
- challenge index, scenario and characterization HTML use the same single,
  neutral-gray notice and contain no legacy red disclaimer panel.
