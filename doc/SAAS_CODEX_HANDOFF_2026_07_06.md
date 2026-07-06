# SaaS Codex Handoff After Core Release-set Export

Date: 2026-07-06

Core repo: `../signal_synth`

SaaS repo: `../signal_synth_saas`

This handoff is for a separate Codex session continuing SaaS implementation.
The core `signal_synth` work should continue in the core repository; do not
move generator logic into the SaaS repository.

## Current State

The core release-set bridge is complete:

- `signal_synth` commit:
  `5542b5e298c1cacb408c78111fa5dc9ad3c7d537`
- `signal_synth_saas` commit:
  `eeb2780e8c23a14c86cebbdba89cd4f51bdf6ff8`
- implementation issue:
  <https://github.com/tamask1s/signal_synth/issues/70>

The SaaS repository now contains imported beta pack files for all ten curated
packs:

- `r_peak_stress_v1`
- `hrv_v1`
- `ecg_beat_classification_v1`
- `ecg_rhythm_v1`
- `signal_quality_v1`
- `ecg_morphology_stress_v1`
- `ppg_alignment_v1`
- `combined_worst_case_v1`
- `wearable_stress_v1`
- `ppg_benchmark_v1`

The import source is:

```sh
../signal_synth/examples/catalog/curated_pack_metadata_v1.json
```

The SaaS import command is:

```sh
python3 scripts/import_curated_release_set.py --metadata ../signal_synth/examples/catalog/curated_pack_metadata_v1.json --source-root ../signal_synth --out packs --clean
```

The SaaS `PackCatalog` currently loads the imported `packs/*.json` and
`packs/*.product` files. It accepts `beta`, `stable`, and `deprecated` release
statuses.

## Important Boundary

Do not reimplement generator, pack validation, package rendering, or scoring
contracts in SaaS. The authoritative boundary remains:

```sh
signal-synth pack validate <pack.json>
signal-synth pack analyze <pack.json>
signal-synth pack challenge <pack.json> --out <new-directory>
```

For product metadata, prefer the core release-set artifact:

```text
examples/catalog/curated_pack_metadata_v1.json
```

It contains more information than the current SaaS `PackSummary`:

- scoreable targets;
- reference-only targets;
- detector output schemas;
- recommended verifier profile;
- expected package contents;
- estimated duration, sampling rate, channel count, package size;
- recommended-for and not-recommended-for copy;
- local verifier smoke-test references.

## What Is Not Done Yet

The SaaS import commit makes the release set listable, but it does not yet make
the UI/API fully use the rich metadata. In particular:

- `/v1/packs` and `/v1/packs/{pack_id}` still expose a compact pack summary;
- UI cards do not yet show scoreable vs reference-only target status;
- there is no target filter;
- there is no package-size/duration/channel display from the release metadata;
- the completed-job view still needs a first-run verification recipe;
- detector-output templates are not generated yet.

These are covered by existing SaaS issues. Do not create duplicate issues for
them unless the scope changes materially.

## Open Issue Review

No new issue was needed during this handoff review. Existing open issues cover
the remaining SaaS work:

- <https://github.com/tamask1s/signal_synth_saas/issues/33>
  Expand curated pack catalog and discovery metadata.
- <https://github.com/tamask1s/signal_synth_saas/issues/31>
  Add first-run guided verification workflow and completed-job recipe panel.
- <https://github.com/tamask1s/signal_synth_saas/issues/32>
  Generate detector-output templates per completed job.
- <https://github.com/tamask1s/signal_synth_saas/issues/35>
  Publish rendered OpenAPI, one-page quickstart, and troubleshooting guide.
- <https://github.com/tamask1s/signal_synth_saas/issues/36>
  Add API usability hardening: idempotency, request IDs, artifact metadata.
- <https://github.com/tamask1s/signal_synth_saas/issues/37>
  Refresh SaaS product plan and remove roadmap/docs drift.
- <https://github.com/tamask1s/signal_synth_saas/issues/34>
  Add template/form-assisted scenario authoring with preview.
- <https://github.com/tamask1s/signal_synth_saas/issues/38>
  Add privacy-preserving beta activation telemetry.
- <https://github.com/tamask1s/signal_synth_saas/issues/22>
  Release, CI/CD, and deployment automation.
- <https://github.com/tamask1s/signal_synth_saas/issues/23>
  Security baseline: threat model, secret rotation, audit export.
- <https://github.com/tamask1s/signal_synth_saas/issues/24>
  Commercial and legal readiness for private beta.
- <https://github.com/tamask1s/signal_synth_saas/issues/30>
  Transactional email verification and password recovery.

## Recommended SaaS Priority Order

1. Finish #33 first.
   The release-set files are imported, but the SaaS still needs to consume and
   expose the rich metadata. Users must be able to see whether a pack scores
   their detector output before creating a job.

2. Then implement #31.
   A completed job should show exact local verifier commands, package path,
   detection folder shape, output folder, threshold profile, and pass/fail
   semantics.

3. Then implement #32.
   Generate detector-output templates per completed job and per scoreable
   case/target. This removes the largest first-run ambiguity.

4. Then do #35 and the relevant parts of #37 together.
   The docs should describe the current release-set catalog, auth model, local
   verifier workflow, troubleshooting, and non-clinical product boundary.

5. Then do #36.
   Add idempotency, request IDs, artifact metadata/HEAD support, stable error
   examples, and job/manifest lookup improvements for API users.

6. Before external beta users, complete #22, #23, #24 and #30.
   These are release, security, commercial/legal, and account lifecycle
   blockers. They are less useful than #31/#32 for local product iteration, but
   mandatory before less-trusted users receive access.

7. Do #34 after the default curated-pack verification path is excellent.
   Template/form-assisted authoring is valuable, but custom authoring should
   not distract from the default challenge-package workflow.

8. Do #38 after the beta path is usable.
   Telemetry is useful for iteration, but it must stay privacy-preserving and
   should not collect detector outputs, PHI, scenario free text, or proprietary
   customer data.

## Suggested First Task For The Next SaaS Codex Session

Start with #33.

Recommended implementation shape:

1. Load `curated_pack_metadata_v1.json` or a SaaS-local imported copy alongside
   the existing `packs/*.json` files.
2. Extend `PackSummary` and `pack_summary_json` with:
   `scoring_mode`, `scoreable_targets`, `reference_only_targets`,
   `detector_output_schemas`, `recommended_profile`, `recommended_for`,
   `not_recommended_for`, duration, sampling rates, channel range and estimated
   package size.
3. Keep `packs/*.product` as the immutable deployability/fingerprint sidecar.
   Do not duplicate all product copy into `.product`.
4. Add target/difficulty filters in the UI.
5. Update `/v1/packs` and `/v1/packs/{pack_id}` API responses.
6. Add unit tests that prove the catalog exposes all ten beta packs and clearly
   distinguishes scoreable and reference-only targets.

Avoid making the SaaS catalog infer scoreability from target names. Use the
core metadata or `pack analyze` output.

## Verification Notes

During #70:

- core `Verification` workflow passed:
  <https://github.com/tamask1s/signal_synth/actions/runs/28821564340>
- all ten release-set packs rendered locally with `pack challenge`;
- the SaaS import script reproduced the committed `packs/` directory exactly
  except for `.gitkeep`;
- static SaaS pack/path validation passed.

Full SaaS CMake verification was not run in this environment because this host
has CMake 3.10.2, while the SaaS repo requires CMake 3.16+, and `jansson.h`
was not installed.

## Working Rules

- Keep issue URLs as the first token in the first line of commit messages.
- Do not put generator C++ code into SaaS.
- Do not claim clinical validation, diagnosis, patient monitoring, or medical
  certification.
- Do not collect PHI, patient identifiers, clinical notes, proprietary detector
  outputs, or customer algorithm code in normal SaaS workflows.
- Prefer improving the curated-pack verification path before building broad
  custom scenario authoring.
