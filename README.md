# Synsigra user manual

Synsigra creates deterministic synthetic ECG, PPG, HRV, wearable, optical,
cardiorespiratory, noise, rhythm, morphology, and timing challenges for
algorithm development and QA. It is useful for repeatable regression tests,
edge-case coverage, stress testing, and synthetic AI training-data workflows.

The hosted product is at <https://www.timeonion.com/syn_sig_ra>. It generates
signals on the server, but verification runs locally: proprietary algorithms,
source code, and completed algorithm output do not need to leave the user's
environment.

Synsigra is engineering QA software. It is not diagnostic software, a
patient-data service, clinical evidence, certification, or a replacement for
validation on representative real-world data. Do not submit PHI or real patient
signals.

## What the product does

The normal workflow is:

1. Register with an email address, verify ownership, and sign in.
2. Choose one of 18 curated challenge packs or author a custom pack.
3. Generate a deterministic challenge in an organization project.
4. Inspect its waveforms and ground truth in Synsigra Lab.
5. Download one verification-kit ZIP.
6. Run the proprietary algorithm locally and write its output into the kit's
   manifest-declared `submission/` files.
7. Install the generator-free verifier wheel and create local verification
   results with the package's pre-specified profile.

Generated jobs preserve the pack version, catalog identity, pack and package
fingerprints, exact generator commit/binary hash, challenge contracts, and
integrity result. Curated and custom packs use the same post-render validation,
download, and verification workflow.

## UI guide

The product navigation is deliberately task-oriented:

- **Start** explains the next useful action and current workspace state.
- **Choose pack** starts with the algorithm goal, then allows intent,
  difficulty, modality, and scoring filters. Pack cards show compatible target
  families, output schemas, size estimates, verification protocol, and external
  noise policy before generation.
- **Generate** confirms the selected pack and project and opens the accepted job
  immediately.
- **Jobs** shows queued, running, succeeded, failed, cancelled, expired, and
  deleted work. A job can be cancelled while queued, retried after a failed or
  cancelled run, deleted when not running, and exactly rebuilt after retained
  files expire.
- **Verify** provides the generator-free verifier download and a job-specific,
  copyable local recipe.
- **Scenarios** is a guided, target-first authoring flow. It loads live core
  field metadata and templates, displays only relevant controls first, previews
  compatibility, and retains an advanced JSON editor for the complete surface.
- **Custom packs** composes validated owned scenario snapshots. Later draft
  edits or deletion cannot mutate a queued or completed custom-pack job.
- **Lab** displays retained WFDB signals without downloading the whole file. It
  fetches only the visible binary viewport, caches a bounded number of client
  viewports, supports horizontal/time and vertical/amplitude/spacing zoom,
  stacked or overlaid channels, panning, channel selection, and bounded ground-
  truth overlays. Available overlays include R peaks, beat classes,
  lead-specific ECG delineation fiducials, PPG peaks/onsets, rhythm/artifact
  intervals, perfusion events, and expected missing pulses.
- **Account** manages the profile, password, personal API keys, account export,
  and permanent workspace deletion.
- **MCP assistant** connects a compatible AI client directly to the live pack,
  authoring, generation, job-status, rebuild, and local-verification workflow.
  It turns an engineering goal into explicit scoreable/reference-only target
  coverage before any job is created.
- **Docs** serves the live OpenAPI document, rendered API reference, quickstart,
  and troubleshooting guide.

The browser uses the signed-in session. It does not ask a human user to paste an
API key.

## Accounts and API keys

Registration is open. It requires the current private-beta terms, a verified
email address, and a password of at least 12 characters. Email verification
starts a browser session. Sign-in, sign-out, resend-verification, forgot-password,
and single-use password-reset flows are available. A password change invalidates
older sessions and returns one replacement session to the initiating browser.

Personal API-key secrets are shown once. Keys can be listed without their
secret, rotated atomically, and revoked. Owner/admin users can export
organization audit events. The Account export includes owned metadata but never
password material, API-key hashes/secrets, session tokens, or email-action
tokens.

One personal API key is sufficient for an automated client. No email, password,
CSRF token, or login request is needed. The key identifies its user,
organization, and role. Use:

```http
Authorization: Bearer YOUR_ONE_TIME_SECRET
```

Store the secret outside source control and send it only to
`https://www.timeonion.com/syn_sig_ra`. A complete natural-language/Codex client
workflow is in [doc/CODEX_API_CLIENT_GUIDE.md](doc/CODEX_API_CLIENT_GUIDE.md).

## MCP assistant integration

Synsigra also exposes a stateless Streamable HTTP MCP server:

```text
https://www.timeonion.com/syn_sig_ra/mcp
```

Use one personal API key as an HTTP header:

```http
Authorization: Bearer YOUR_ONE_TIME_SECRET
```

No email, password, cookie, or second credential is needed. The server
negotiates MCP `2025-11-25`, `2025-06-18`, or `2025-03-26`; it does not expose
an unauthenticated SSE stream or stateful session. A client must send both
`application/json` and `text/event-stream` in `Accept`, as required by the
Streamable HTTP transport.

The MCP server exposes model-discoverable tools to:

- infer verification targets from a plain-language engineering goal and rank
  current curated packs;
- show scoreable, reference-only, and missing coverage without overstating a
  pack's claims;
- inspect packs, projects, jobs, and exact generator/package identities;
- fetch the live core-owned authoring schema and templates, clone curated
  cases, preview/save scenarios, and compose custom packs when duration,
  sampling rate, physiology, noise, or target requirements are not met by a
  curated pack;
- create a generation job only after selection/approval and rebuild an expired
  job with its exact preserved generator;
- return an exact download and local `synsigra-verify` runbook. Proprietary
  algorithms and their completed outputs stay local.

The modifying tools are annotated as non-read-only and non-idempotent so MCP
hosts can ask for human confirmation. The same organization roles, request/job
quotas, storage checks, core contracts, and API errors apply as in the web UI.
Setup and example prompts are available at
<https://www.timeonion.com/syn_sig_ra/mcp-setup>.

## Verification targets

The current core exposes 16 locally scoreable target families:

| Target | Algorithm output or measurement | Main prerequisite |
|---|---|---|
| `r_peak` | ECG R-peak point events | ECG source |
| `rr_interval` | RR measurements | ECG source |
| `ppg_systolic_peak` | PPG peak point events | PPG enabled |
| `ppg_pulse_onset` | PPG onset point events | PPG enabled |
| `ecg_beat_classification` | Beat point events/classes | ECG source |
| `rhythm_episode` | Rhythm intervals | Explicit rhythm episodes |
| `rhythm_burden` | Episode count/duration/fraction | Explicit rhythm episodes |
| `signal_quality` | Quality/artifact intervals | Analytic or external noise |
| `ecg_delineation` | Lead-specific ECG fiducials | ECG source |
| `qtc` | QTc measurement values | ECG source |
| `hrv` | Time, nonlinear, VLF/LF/HF metrics | HRV enabled; at least 300 s |
| `morphology_assertions` | Morphology measurements/assertions | ECG conditions |
| `ecg_ppg_alignment` | ECG-to-PPG timing measurements | PPG enabled |
| `ppg_optical` | Red/IR/SpO2 optical measurements | Optical PPG enabled |
| `prv` | Pulse-rate-variability measurements | PPG enabled |
| `respiratory_rate` | Respiratory-rate measurements | Respiratory coupling |

Stable submission format families are:

- `point_events_json_v1` and `point_events_csv_v1`;
- `interval_events_json_v1` and `interval_events_csv_v1`;
- `measurement_values_json_v2` and `measurement_values_csv_v2`.

RR, QT/QTc, HRV, morphology, alignment, optical, PRV, respiration, and rhythm
burden all use the one measurement-v2 family. Its 14-column CSV form and JSON
form preserve window bounds, method ID, preprocessing-policy ID, scope,
channel/lead, formula and beat/time anchor as part of measurement identity.

Never guess a filename or format. The succeeded job's `challenge.submission_outputs`
and the kit's `submission/submission.json` are authoritative.

## Curated packs

The immutable catalog 3.0 release contains 18 packs:

| Pack | Focus |
|---|---|
| `r_peak_rr_noise_v1` | R peaks, RR values, signal quality, verification protocol, analytic/external noise |
| `ecg_qtc_verification_v1` | R peaks, delineation, QTc formula/rate cases |
| `ecg_extended_morphology_v1` | Delineation, extended morphology, beat classification |
| `advanced_rhythm_burden_v1` | Rhythm intervals and burden measurements |
| `r_peak_stress_v1` | R-peak and artifact stress regression |
| `hrv_robustness_v2` | HRV v2 metrics and contamination robustness |
| `ecg_beat_classification_v1` | Normal, PAC, PVC, paced beat classes |
| `ecg_rhythm_v1` | Rhythm transitions, ectopy, pacing, episodes |
| `signal_quality_v1` | ECG/PPG quality intervals and PPG peaks |
| `ecg_morphology_stress_v1` | Morphology assertions under signal-quality stress |
| `ppg_alignment_v1` | PPG peaks, ECG/PPG timing, quality |
| `combined_worst_case_v1` | Mixed ECG, PPG, HRV, and quality stress |
| `wearable_timebase_v2` | Multi-device wearable clocks and ECG/PPG alignment |
| `ppg_benchmark_v1` | PPG peak and onset benchmarks |
| `ppg_optical_v2` | Red/infrared optical PPG and oxygenation cases |
| `ecg_delineation_v2` | Lead-specific P/QRS/T/U fiducials |
| `cardiorespiratory_v1` | PRV and respiratory-rate coupling |
| `ecg_hybrid_noise_v1` | Combined analytic and approved external-noise stress |

The catalog API returns the complete target, case, profile, protocol, role,
modality, duration, rate, channel, estimated-size, changelog, fingerprint, and
external-noise metadata. A small starter pack is a deliberate validation slice,
not the platform's generation limit.

## Custom authoring

The UI and API obtain authoring fields and templates directly from the live
core. The current contract is `synsigra_authoring_v18`, with 142 fields, 71
condition definitions, 20 artifact families, 16 targets, 18 logical groups,
scenario schemas 2 through 9, and template contract `synsigra_templates_v5`.

It covers, among other controls:

- duration, sampling rate, deterministic seeds, compact/full output;
- heart rate, RR variability, rhythm episodes, pacing, QT adaptation;
- ECG conditions, 12-lead morphology, extended components, and fusion beats;
- HRV mean/SDNN, VLF/LF/HF centers, bandwidths, powers, ratio, respiratory
  modulation, and RR limits;
- PPG timing, shape, weak/missing pulses, perfusion, optical red/IR channels,
  oxygenation episodes, and sensor effects;
- wearable stream rates, clock offset/drift/jitter, packet loss, and device
  placement;
- cardiorespiratory coupling, activity, respiration, randomization envelopes;
- analytic ECG/PPG artifacts and licensed, checksum-pinned external noise.

Unknown fields and unsupported combinations are rejected. A condition catalog
entry is not by itself proof of native synthesis; the core's fidelity metadata
and validation response are authoritative.

For automation, fetch these before authoring instead of hard-coding fields:

```sh
BASE=https://www.timeonion.com/syn_sig_ra
curl -fsS "$BASE/openapi.yaml" -o synsigra-openapi.yaml
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  "$BASE/v1/authoring/schema" -o authoring-schema.json
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  "$BASE/v1/authoring/templates" -o authoring-templates.json
```

Preview every final scenario/target combination before saving it. Every target
selected for a custom pack applies to every selected scenario.

## Download and local verification

The job page exposes one recommended customer artifact:

```text
verification-kit.zip
└── verification-kit/
    ├── challenge/                 immutable challenge package v3
    ├── submission/                editable role-selected working template
    ├── README.txt                 exact command and package profile
    ├── ENGINEERING_CLAIM_BOUNDARY.txt
    └── challenge-metadata.json    normalized contracts, roles and integrity
```

There is no redundant nested `package.zip`. The generator and its source are
not distributed. The raw `manifest.json` and `package.zip` endpoints remain
available for API consumers that need lower-level immutable artifacts, but they
are not additional steps in the normal UI workflow.

Download and use a kit:

```sh
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  -o verification-kit.zip \
  "$BASE/v1/jobs/JOB_ID/verification-kit.zip"
unzip verification-kit.zip
cd verification-kit
```

Install the separately downloadable, pure-Python, generator-free verifier:

```sh
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  -o synsigra-wheel.whl "$BASE/v1/downloads/verifier/synsigra-wheel.whl"
python -m pip install synsigra-wheel.whl
synsigra-verify --help
```

Edit the algorithm name/version and replace example rows under `submission/`.
Keep target names, paths, units, and formats unchanged. Then copy the exact
command from the job and kit README. A protocol-v2 kit uses the complete
package-authoritative evidence matrix and embedded numeric policy:

```sh
synsigra-verify challenge submission verification-results --force
```

Do not add a profile, case, or target override to an evidence run. If a
package has no protocol v2, the kit instead uses explicit
`--mode diagnostic`; that run is useful for exploration but is never
evidence-eligible.

The verifier first enforces ZIP/path safety, manifest roles, listed files,
sizes, hashes, package/scoring/submission identity, and submission schema. It
then creates per-target results and a top-level evidence summary. Exit status is
`0` for a passing verification, `1` for integrity/input/scoring/threshold
failure, and `2` for invalid CLI use.

Keep the immutable challenge, completed submission, exact algorithm build and
configuration, verification results, and generator/package identities together
as reproducible engineering evidence.

## Retention and exact rebuilds

Generated server artifacts are retained for seven days. Job metadata,
immutable recipe, package fingerprint, and exact producer identity remain. If a
user downloads an expired successful job, the UI can queue a rebuild using the
preserved SHA-256-addressed generator release. A newer generator is never
silently substituted, and a fingerprint mismatch fails the rebuild.

Verification kits and package ZIPs support `HEAD`, strong SHA-256 ETags,
checksums, expiry headers, byte ranges, and browser resume. The Lab API reads
only bounded visible data, so retained multi-gigabyte waveform sources do not
have to be transferred to the browser.

## API map

The exact machine-readable contract is always the live
[`/openapi.yaml`](https://www.timeonion.com/syn_sig_ra/openapi.yaml). The main
routes are:

| Area | Routes |
|---|---|
| Public/service | `GET /healthz`, `/readyz`, `/openapi.yaml`, `/v1/legal`, `/v1/packs`, `/v1/packs/{id}` |
| Registration/session | `POST /v1/auth/register`, `/verify-email`, `/resend-verification`, `/login`, `/logout`, `/password-reset/request`, `/password-reset/complete`; `GET /v1/auth/me` |
| Account | `GET/PATCH/DELETE /v1/account`, `POST /v1/account/password`, `GET /v1/account/export` |
| API keys/audit | `GET/POST /v1/api-keys`, `DELETE /v1/api-keys/{id}`, `POST /v1/api-keys/{id}/rotate`, `GET /v1/audit-events` |
| Discovery/authoring | `GET /v1/authoring/schema`, `/templates`, `/curated-scenarios/{pack}/{case}`; `POST /v1/authoring/preview` |
| Drafts/custom packs | `GET/POST /v1/scenarios`, `GET/PUT/DELETE /v1/scenarios/{id}`, `GET/POST /v1/custom-packs`, `GET/DELETE /v1/custom-packs/{id}` |
| Projects/jobs | `GET /v1/projects`, `GET/POST /v1/jobs`, `GET/DELETE /v1/jobs/{id}`, `POST /v1/jobs/{id}/cancel`, `/retry`, `/rebuild` |
| Downloads | `GET/HEAD /v1/jobs/{id}/verification-kit.zip`, `GET/HEAD /v1/artifacts/{package}/{manifest.json|package.zip}`, `GET /v1/downloads/verifier[/{filename}]` |
| Lab | `GET /v1/jobs/{id}/viewer`, `/viewer/window`, `/viewer/overlays` |
| Usage/operations | `GET /v1/usage`, `GET /v1/metrics` (owner/admin) |

Browser sessions and bearer keys are organization-scoped. Cross-organization
resource access intentionally returns 404. Write access requires an owner,
admin, or developer role; selected account/audit operations require owner/admin.
JSON write endpoints require `application/json` and reject extra/duplicate
fields. Rate, concurrency, monthly-job, output-size, disk-reserve, CPU, memory,
file, wall-time, and no-network worker bounds are enforced.

## Frozen release contract

This release intentionally has no old core compatibility layer. It requires the
clean sibling checkout `../signal_synth` at commit
`13fd76d3f57bf5b55ae0ccf18ebd06f06329a819` and the exact tuple:

- generator `0.10.0-dev`, C++ facade `1.5.0`;
- integration `synsigra_core_integration_v7`, pack schema `2`;
- challenge `synsigra_challenge_package_v3`;
- scoring `synsigra_scoring_manifest_v3`;
- verification protocol `synsigra_verification_protocol_v2`;
- submission `synsigra_submission_v1` and formats
  `synsigra_submission_formats_v2`;
- measurements `synsigra_measurement_values_v2`,
  `synsigra_measurement_truth_v2`, and `synsigra_measurement_score_v2`;
- local verification report `synsigra_local_verification_v2`;
- authoring `synsigra_authoring_v18`, templates `synsigra_templates_v5`;
- verifier `0.10.0`, external-noise truth
  `synsigra_external_noise_truth_v1`;
- curated catalog `3.0` with 18 packs and source hash
  `sha256:2ab03e48ed533636d2abb5bc5a6f90590f1d9abbb4ed8664ed9efd0dac06892e`.

Configuration, startup, readiness, worker post-render validation, and release
verification fail closed if these identities diverge. `/readyz` publishes the
complete canonical core contract as `accepted_core.contract_document`.

## Further documentation

- [Customer API guide](doc/API_GUIDE.md)
- [Codex/natural-language API client guide](doc/CODEX_API_CLIENT_GUIDE.md)
- [MCP server architecture and client workflow](doc/MCP_SERVER.md)
- [Custom pack model](doc/CUSTOM_PACKS.md)
- [Operations](doc/OPERATIONS.md)
- [Security baseline](doc/SECURITY_BASELINE.md)
- [Private-beta terms](doc/PRIVATE_BETA_TERMS.md)
- [Privacy and no-PHI notice](doc/PRIVACY_NO_PHI_NOTICE.md)
- [Support policy](doc/PRIVATE_BETA_SUPPORT.md)
- [Developer reference](doc/DEVELOPER_REFERENCE.md)

Private-beta support: `synsigra@gmail.com`.
Operator: Kis Tamás, 2040 Budaörs, Tátra u. 6, Hungary
