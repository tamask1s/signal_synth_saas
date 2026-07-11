# Synsigra product capabilities

This document is the source-traceable capability summary for the Synsigra SaaS. It describes the current Signal Synth authoring contract used by this repository, rather than the contents of any one starter pack.

> A basic pack is one deliberately small validation slice. It is not the platform limit.

## Current authoring surface

The capability inventory is generated from the bundled core with:

```sh
build/signal_synth_live/signal-synth authoring schema
```

At the time of this update, the schema reports:

- authoring metadata version `synsigra_authoring_v6` and scenario schema v4;
- 74 configurable authoring fields;
- 71 condition catalog entries with explicit native, parameterized, or catalog-only support;
- 20 artifact families;
- 8 verification target types; and
- 12 logical field groups.

Unsupported condition or parameter combinations are rejected explicitly. Catalog presence alone does not imply native synthesis support.

## What can be varied

### Time, sampling, and reproducibility

- Duration from 0.01 seconds to 86,400 seconds (24 hours), subject to sample-count and package limits.
- Sample rates from 100 Hz to 1 MHz, with common presets at 100, 125, 200, 250, 360, 500, and 1,000 Hz.
- Deterministic unsigned 64-bit seeds for repeatable generation.
- Controlled randomization envelopes for ECG heart rate and RR timing, PPG timing and amplitude, HRV parameters, and activity.

### ECG physiology and morphology

- Heart rate from 10 to 400 bpm, RR variability, and ectopic cadence.
- Parameterized 12-lead morphology and condition severity.
- Rhythm, conduction, atrial and ventricular timeline, AV-pattern, Q-wave territory, episode, pacing, noncapture, and fidelity controls where supported by the selected condition.
- Native and parameterized support is declared per catalog entry; unsupported combinations fail instead of silently producing a misleading trace.

### HRV, respiration, and activity modulation

- Mean heart rate, SDNN, LF/HF ratio, LF/HF center frequencies and bandwidths.
- Respiratory frequency and amplitude, RR bounds, and physiology-linked respiratory modulation.
- Activity-driven modulation for time-varying scenarios.

### PPG timing and morphology

- Pulse transit delay, rise and decay, amplitude and baseline.
- Dicrotic timing, width, and amplitude.
- Beat-to-beat timing jitter, low-frequency amplitude modulation, morphology variation, and PAC/PVC/paced amplitude scaling.
- Perfusion episodes, weak or missing pulses, clock drift, and pulse-transit-time variation.

### Noise, artifacts, and device failure

The authoring contract exposes 20 artifact families:

- ECG baseline wander, powerline interference, EMG noise, dropout, and saturation;
- lead reversal, lead swap, electrode misplacement, gain mismatch, and offset drift;
- clock drift, dropped samples, quantization, ADC clipping, and per-lead targeting;
- PPG dropout, periodic/burst/broadband motion, ambient light, and sensor saturation.

Artifacts can be combined with physiology and timing changes to test behavior beyond clean, stationary traces.

## Verification targets and evidence

Scenarios can target:

- R-peak detection;
- PPG systolic peaks and pulse onsets;
- ECG beat classification;
- HRV;
- signal quality;
- morphology assertions;
- ECG–PPG alignment.

Generated packages include the applicable waveform data, annotations, metrics, warnings, provenance, fingerprints, claim boundaries, and human-readable reports. Depending on scenario settings and supported output paths, the engine can emit CSV, WFDB, EDF+, BDF+, and compact package representations.

## Product boundary

Synsigra is an engineering verification tool. Synthetic output is not patient data, clinical evidence, a medical-device authorization, or a substitute for validation on representative real-world data. The SaaS exposes a curated, validated subset of the underlying engine contract and records the exact generator build used for each job.

When this document and the live authoring schema differ, the live schema and per-pack provenance are authoritative.
