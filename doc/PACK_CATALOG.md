# Pack catalog release policy

Built-in packs are curated, immutable product releases. Each
`packs/<pack_id>.json` has a required `packs/<pack_id>.product` sidecar. The
authoritative `signal_synth` parser validates the pack and computes its
fingerprint; the SaaS then requires the sidecar's ID, semantic version, and
expected fingerprint to match exactly.

The sidecars and SaaS-local pack JSON files are imported from the sibling
`signal_synth/examples/catalog/curated_pack_metadata_v1.json` release-set
artifact:

```sh
python3 scripts/import_curated_release_set.py --metadata ../signal_synth/examples/catalog/curated_pack_metadata_v1.json --source-root ../signal_synth --out packs --clean
```

This prevents a scenario or pack edit from silently changing a deployed
release. A changed pack requires:

1. a semantic version increment;
2. a changelog entry;
3. an updated expected fingerprint after review;
4. an explicit generator compatibility declaration.

Major compatibility lines use stable IDs such as `r_peak_stress_v1`. Breaking
scenario/target changes require a new major pack ID (`..._v2`) so existing API
clients can continue requesting the old release. Additive, reviewed changes
may increment the semantic minor version, but published package artifacts and
their stored fingerprints remain immutable.

## Generator compatibility

`integration_contract` is the single supported SaaS/core boundary. This release
accepts `synsigra_core_integration_v1`; generator version ranges and legacy CLI
aliases are deliberately not compatibility surfaces.
New jobs return `pack_generator_incompatible` rather than invoking an
unapproved generator. Completed jobs retain the exact generator build hash.

## Deprecation

Release status is `beta`, `stable` or `deprecated`. Beta packs are visible and
usable in the private-beta product catalog, but must not be described as
clinically validated. Deprecated releases remain visible for reproducibility
and require a human-readable migration message. They are not silently removed.
Physical artifacts follow the retention policy, while job, pack fingerprint,
package fingerprint, and generator identity metadata remain.

## Current curated beta set

- `r_peak_stress_v1` 1.0: R-peak smoke, rate-stress, and
  baseline/powerline signal-quality scenarios.
- `hrv_v1` 1.0: HRV metric and RR-tachogram benchmark cases.
- `ecg_beat_classification_v1` 1.0: normal, PAC, PVC and paced beat labels.
- `ecg_rhythm_v1` 1.0: rhythm, transition, ectopy and pacing stress cases.
- `signal_quality_v1` 1.0: signal-quality intervals plus a PPG peak case.
- `ecg_morphology_stress_v1` 1.0: reference-only morphology and condition assertions.
- `ppg_alignment_v1` 1.0: PPG peak scoring and ECG/PPG timing references.
- `combined_worst_case_v1` 1.0: mixed ECG/PPG/HRV/signal-quality stress.
- `wearable_stress_v1` 1.0: wearable ECG/PPG/HRV long-duration stress.
- `ppg_benchmark_v1` 1.0: PPG peak and onset benchmark cases.
