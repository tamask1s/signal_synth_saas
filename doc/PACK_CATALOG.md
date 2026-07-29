# Curated pack catalog contract

Built-in packs are imported as one immutable catalog 3.6 release snapshot from
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

- catalog version `3.6`;
- a positive declared pack count that exactly matches the unique pack array;
- catalog source hash
  `sha256:21f3baee51b6e386962b54a9f30f4223b555f03e055e35cb3259c9c6f7cb9136`;
- integration `synsigra_core_integration_v8`;
- challenge `synsigra_challenge_package_v3`;
- scoring `synsigra_scoring_manifest_v3`;
- submission `synsigra_submission_v1`;
- verification protocol `synsigra_verification_protocol_v3`;
- evidence-level policy `synsigra_evidence_profile_policy_v1`;
- verifier `0.17.0`;
- only external-noise assets whose release truth allows redistribution.

Each API pack response exposes catalog/release identity, pack fingerprint,
targets, score types and accepted submission formats, cases, output roles,
three complete verification-level documents and hashes, external-noise policy,
estimates, intended use, contraindicated use, difficulty, modality, badges, and
changelog. Level 1 Foundation, Level 2 Advanced (default), and Level 3 Frontier
share the pack recipe and differ only in deterministic acceptance gates. The
level selected before generation is copied into the immutable challenge and is
authoritative for that job. Protocol-v3 packages run package-authoritative
evidence mode; protocol-free packs are explicit diagnostic-only, not weaker
evidence packages.

## Current set

1. `r_peak_rr_noise_v1`
2. `ecg_qtc_verification_v1`
3. `ecg_extended_morphology_v1`
4. `advanced_rhythm_burden_v1`
5. `r_peak_rr_simple_stress_v1`
6. `r_peak_rr_snr_ladder_v1`
7. `r_peak_stress_v1`
8. `r_peak_noise_frontier_v1`
9. `hrv_simple_cases_v1`
10. `hrv_robustness_v2`
11. `ecg_beat_classification_v1`
12. `ecg_rhythm_v1`
13. `signal_quality_v1`
14. `ecg_morphology_stress_v1`
15. `ppg_alignment_v1`
16. `combined_worst_case_v1`
17. `wearable_timebase_v2`
18. `ppg_benchmark_v1`
19. `ppg_optical_v2`
20. `ecg_delineation_v2`
21. `cardiorespiratory_v1`
22. `ecg_hybrid_noise_v1`

`r_peak_rr_simple_stress_v1` is the recommended first evidence run for an
R-peak detector with directly derived beat-to-beat RR output. Its eight cases
cover clean rate limits, structured and moderate noise, strongly variable
rhythm, genuine non-conducted pauses, and combined stress.
`r_peak_rr_snr_ladder_v1` follows with clean and continuous-noise cases at
−0.2 and −0.5 dB plus every integer level from −1 through −11 dB. Both make
each complete signal an independent official verdict and have no pooled
acceptance profile.
Its RR tolerance-pass, mean absolute error, and median absolute error gates are
specified per noise level. The displayed RR 95th-percentile error is diagnostic
only because peak splits and merges intentionally produce a heavy error tail.
`r_peak_stress_v1` and `r_peak_noise_frontier_v1` remain as detailed legacy
alternatives. `r_peak_rr_noise_v1` is the combined pipeline protocol for
algorithms that additionally emit signal-quality intervals.

`hrv_simple_cases_v1` is the focused HRV-only starting point. Its ten complete
cases are independent official verdicts and require no R-peak or
signal-quality submission. Every case checks RR reconstruction and all 15
declared HRV summary measurements. `hrv_robustness_v2` remains the combined
pipeline alternative for algorithms that also claim R-peak and signal-quality
outputs.

There is deliberately no older catalog/pack compatibility path in this
pre-beta release. A catalog change is a new clean deployment baseline. Jobs
persist the selected pack version, evidence-level protocol SHA-256 and, for
curated work, the exact catalog version/hash so the result remains auditable
after the live catalog changes.
