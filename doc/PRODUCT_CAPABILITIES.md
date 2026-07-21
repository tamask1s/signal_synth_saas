# Synsigra product capabilities

This is the product-level capability summary for the exact core v7 release. A
small curated pack is one validated slice, not the platform limit. The live
`/v1/authoring/schema`, `/v1/authoring/templates`, `/v1/packs`, and `/readyz`
responses remain authoritative.

## Current authoring surface

The pinned core reports:

- authoring metadata `synsigra_authoring_v18`;
- templates `synsigra_templates_v5`;
- scenario schemas 2 through 9 (latest 9);
- 142 configurable authoring fields in 18 groups;
- 71 condition definitions with explicit fidelity support;
- 20 analytic artifact families;
- 16 locally scoreable verification targets;
- approved external-noise assets with checksum, license, channel/rate, and
  redistribution policy.

Unknown fields, invalid ranges, and unsupported cross-field combinations are
rejected. Catalog presence alone does not imply native pathology synthesis.

## Configurable signal space

- Time, sampling rate, deterministic seeds, compact/full output, and controlled
  randomization envelopes.
- ECG heart rate, RR variation, ectopy, rhythm episodes, pacing/non-capture,
  repolarization/QT adaptation, 12-lead morphology, extended P/QRS/T/U
  components, fusion beats, conditions, and fidelity policy.
- HRV mean HR/SDNN, VLF/LF/HF center/bandwidth/power relationships, LF/HF
  ratio, respiratory modulation, and RR bounds.
- PPG transit timing, shape, dicrotic component, jitter/modulation, beat-linked
  amplitude, weak/missing pulses, perfusion, red/infrared optical channels,
  sensor effects, calibration, and oxygenation episodes.
- Wearable ECG/PPG/accelerometer stream rates, independent device clocks,
  offset/drift/jitter, packet loss, placement, and resampling truth.
- Respiration, activity, cardiorespiratory coupling, PRV, respiratory rate, and
  ECG/PPG alignment.
- ECG/PPG artifact intervals including baseline, powerline, EMG, dropout,
  saturation, motion, ambient light, lead/device faults, clock drift, dropped
  samples, quantization, and clipping.
- Approved external noise mixed by asset/channel/offset/SNR/taper/lead, with
  source bytes withheld from rendered-output-only challenges and explicit
  release truth.

Duration may extend to 24 hours and sample rate up to 1 MHz where sample-count,
memory, artifact, and SaaS resource bounds permit. Common presets include 100,
125, 200, 250, 360, 500, and 1000 Hz.

## Verification and evidence

The 16 targets cover point events (`r_peak`, PPG peaks/onsets, ECG beat
classes, lead-specific delineation), intervals (`rhythm_episode`,
`signal_quality`), HRV metrics, and measurements (`rr_interval`, `qtc`,
`morphology_assertions`, `ecg_ppg_alignment`, `ppg_optical`, `prv`,
`respiratory_rate`, `rhythm_burden`).

Stable customer output families are point-event JSON/CSV, interval-event
JSON/CSV, and uniform measurement-value JSON/CSV v2. HRV uses that same
measurement-v2 family rather than a dedicated HRV payload. Package v3 binds
all files by role, media type, size, and SHA-256. It includes the applicable
waveforms, ground truth, summaries, provenance, warnings, claim boundary,
submission templates, and optional pre-specified verification protocol.

The pure-Python verifier 0.11.0 validates archive/path safety, manifest and
role shape, identity, integrity, submission schema, per-target scoring, and
protocol policy. Evidence mode is package-authoritative: it requires a
protocol-v2 package and runs its full matrix with its embedded numeric policy.
Explicit diagnostic mode may filter or use a custom policy, but its reports are
always non-evidence. It is generator-free. Customer algorithms and output can
remain local.

## Product boundary

Synsigra produces synthetic engineering evidence. It does not execute customer
algorithms, accept patient data, make diagnostic claims, replace representative
real-world testing, or provide medical-device authorization/certification.
