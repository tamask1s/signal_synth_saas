# Curated pack catalog contract

Built-in packs are imported as one immutable catalog 3.1 release snapshot from
the exact sibling `signal_synth` checkout. SaaS does not maintain product
sidecars or reinterpret core analysis.

```sh
SIGNAL_SYNTH_CLI=build/signal_synth_live/signal-synth \
  python3 scripts/import_curated_release_set.py \
    --metadata ../signal_synth/examples/catalog/curated_pack_metadata_v1.json \
    --source-root ../signal_synth --out packs --clean
```

The importer removes files not present in the snapshot, copies the declared
pack/scenario/protocol/approved-noise inputs, and validates every pack through
the pinned CLI. Startup and readiness require:

- catalog version `3.1`;
- exactly 19 unique curated packs;
- catalog source hash
  `sha256:34725e1b879904dd70000a42b422822beb6133e48b628b8a8ae8bc71277bb765`;
- integration `synsigra_core_integration_v7`;
- challenge `synsigra_challenge_package_v3`;
- scoring `synsigra_scoring_manifest_v3`;
- submission `synsigra_submission_v1`;
- verification protocol `synsigra_verification_protocol_v2`;
- verifier `0.14.0`;
- only external-noise assets whose release truth allows redistribution.

Each API pack response exposes catalog/release identity, pack fingerprint,
targets, score types and accepted submission formats, cases, output roles,
verification protocol document/hash, external-noise policy, estimates, intended
use, contraindicated use, difficulty, modality, badges, and changelog. The
challenge copy of a protocol is authoritative for the rendered job. Protocol-v2
packages run package-authoritative evidence mode; protocol-free packs are
explicit diagnostic-only, not weaker evidence packages.

## Current set

1. `r_peak_rr_noise_v1`
2. `ecg_qtc_verification_v1`
3. `ecg_extended_morphology_v1`
4. `advanced_rhythm_burden_v1`
5. `r_peak_stress_v1`
6. `r_peak_noise_frontier_v1`
7. `hrv_robustness_v2`
8. `ecg_beat_classification_v1`
9. `ecg_rhythm_v1`
10. `signal_quality_v1`
11. `ecg_morphology_stress_v1`
12. `ppg_alignment_v1`
13. `combined_worst_case_v1`
14. `wearable_timebase_v2`
15. `ppg_benchmark_v1`
16. `ppg_optical_v2`
17. `ecg_delineation_v2`
18. `cardiorespiratory_v1`
19. `ecg_hybrid_noise_v1`

`r_peak_stress_v1` is the package-authoritative evidence baseline for a
peak-only detector. It requires only R-peak event files. The calibrated
`r_peak_noise_frontier_v1` then evaluates the same detector on paired −7, −8,
−9, and −10 dB tiers with separate per-tier acceptance strata.
`r_peak_rr_noise_v1` intentionally remains a combined pipeline protocol and
therefore requires R-peak, RR, and signal-quality outputs.

There is deliberately no older catalog/pack compatibility path in this
pre-beta release. A catalog change is a new clean deployment baseline. Jobs
persist the selected pack version and, for curated work, the exact catalog
version/hash so the result is auditable after the live catalog changes.
