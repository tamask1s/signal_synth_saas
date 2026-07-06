# SynSigRa SaaS

SynSigRa SaaS is the hosted orchestration layer for deterministic synthetic biosignal challenge-package generation. It lets an algorithm developer or QA team create reproducible ECG/PPG/HRV test packages, download waveform and ground-truth artifacts, run proprietary algorithms locally, and verify the algorithm output against packaged reference data.

The service is intentionally thin: signal generation and authoritative scoring contracts remain in the sibling [`signal_synth`](https://github.com/tamask1s/signal_synth) project. The SaaS layer handles browser access, projects, curated/custom packs, job orchestration, artifact retention, usage limits, and operational visibility.

Live beta UI:

```text
https://www.timeonion.com/syn_sig_ra/
```

API base URL:

```text
https://www.timeonion.com/syn_sig_ra
```

> Synthetic engineering data only. Do not enter patient data, personal identifiers, clinical notes, PHI, or other personal data. SynSigRa SaaS is engineering QA tooling, not a medical diagnostic device, patient monitor, certified validator, or clinical validation system.

## Current status

- HTTPS is enabled. Public HTTP requests are redirected to the canonical HTTPS origin.
- Browser access uses server-side sessions through `Secure`, `HttpOnly`, `SameSite=Lax` cookies.
- The browser UI does not ask users to paste API keys.
- API keys are created from the account panel for scripts and CI; each secret is shown once and only a SHA-256 hash is retained server-side.
- Registration is currently open, but e-mail ownership verification and password recovery are not implemented until a transactional mail provider is configured.
- The current curated beta catalog is imported from `signal_synth/examples/catalog/curated_pack_metadata_v1.json` and contains ECG, HRV, PPG, signal-quality and wearable stress packs, not only the R-peak smoke pack.
- Generated packages are immutable while retained. Default artifact retention is 90 days.

## Product boundary

SynSigRa SaaS is for offline-first algorithm verification:

1. Select or compose a deterministic synthetic challenge pack.
2. Generate a package in the SaaS worker.
3. Download `manifest.json` and `package.zip`.
4. Run your algorithm locally against the package files.
5. Verify local detector outputs against packaged ground truth using the SynSigRa local verifier.
6. Archive the package, manifest, detector build/configuration, detections, and verification reports together.

The SaaS does **not** execute customer detector code and does **not** receive proprietary algorithm output unless the user explicitly sends it elsewhere outside this product workflow.

Non-goals:

- clinical decision support;
- patient record storage;
- PHI or clinical note handling;
- regulated medical-device validation claims;
- server-side execution of user algorithms;
- a generic ECG/PPG datastore.

## Quick start: browser

1. Open `https://www.timeonion.com/syn_sig_ra/`.
2. Create an account with an e-mail address, display name, and a password of at least 12 characters, or sign in with an existing account.
3. Check the service status card:
   - `health` means the Apache application responds;
   - `ready` means the database, generator, pack catalog, and artifact store are available.
4. Select the `Default` project, or create another project if your role permits.
5. Select a curated pack or a caller-owned custom pack.
6. Create a job.
7. Wait for `succeeded`.
8. Download `manifest.json` and `package.zip`.
9. Use the completed-job verification panel to copy the exact `synsigra-verify` command for that package.

## Recommended verification workflow

### 1. Generate and preserve the package

For each experiment, preserve at least:

- job ID;
- pack ID and semantic version;
- pack fingerprint;
- package ID;
- package fingerprint;
- generator version;
- generator build identity;
- `manifest.json`;
- `package.zip`.

A fingerprint change means the test input changed. Do not compare results as though they came from the same fixture.

### 2. Run your detector locally

Run the algorithm against whichever generated format it supports:

- `waveform.csv`;
- WFDB files;
- EDF/BDF files;
- case-level scenario or annotation files, where appropriate.

The UI and `/v1/packs` show which targets are locally scoreable and which are reference-only before you create a job. For a completed curated job, the job card shows a first-run recipe with the package filename, recommended threshold profile, output directory, and accepted detection folder shape.

Detection files live under a local `detections/` directory. The verifier accepts recommended names from the package plus fallback names:

```text
detections/
├── <case_id>_<target>.csv
├── <case_id>_<target>.json
└── <case_id>.csv              # accepted when the case has one scoreable target
```

Minimal R-peak CSV:

```csv
time_seconds
0.82
1.68
2.54
```

Optional columns include:

```text
sample_index, channel, label, confidence
```

JSON detection documents are also supported by the sibling `signal_synth` verifier.

### 3. Verify locally with the SynSigRa SDK

Install the local verifier from the beta checkout or wheel. During local development next to this repo:

```sh
python -m pip install ../signal_synth
```

If you downloaded `pkg_123-package.zip` from the UI and generated detections under `detections/`, run:

```sh
synsigra-verify "pkg_123-package.zip" detections/ "verification-pkg_123" \
  --profile stress \
  --force
```

The completed-job panel fills in the package ID and recommended profile for the selected pack. Useful filters:

```sh
synsigra-verify "pkg_123-package.zip" detections/ "verification-pkg_123" \
  --case clean_70 \
  --target r_peak \
  --profile stress \
  --force
```

The verifier checks package integrity, scores compatible case/target pairs, applies the selected threshold profile, and writes:

- `verification_summary.json`;
- `verification_summary.csv`;
- `verification_report.html`;
- per-case/per-target details under `verification/`.

CI exit codes:

- `0`: verification passed;
- `1`: package/input/scoring/threshold-policy failure;
- `2`: invalid CLI usage.

Reference-only packs or targets intentionally do not have a local scoring policy. Use their artifacts for manual inspection, contract checks, or later template-based work.

### 4. Fallback: direct authoritative CLI scoring

Build the sibling generator CLI:

```sh
git clone https://github.com/tamask1s/signal_synth.git
cmake -S signal_synth -B signal_synth/build \
  -DSIGNAL_SYNTH_BUILD_CLI=ON \
  -DSIGNAL_SYNTH_BUILD_TESTS=OFF
cmake --build signal_synth/build

export SIGNAL_SYNTH_CLI="$PWD/signal_synth/build/signal-synth"
```

Score a single case directly:

```sh
unzip package.zip -d challenge

"$SIGNAL_SYNTH_CLI" compare rpeaks \
  challenge/cases/clean_70/scenario.json \
  detections/clean_70.csv \
  --out verification-clean-70
```

Other supported scoring commands depend on the selected scenario/pack and may include:

```text
compare rpeaks
compare ppg-peaks
compare beat-classes
hrv score
```

Signal-quality-only targets may provide generated reference artifacts without event-detector scoring.

## Browser UI capabilities

### Service status

The UI reports:

- service liveness and build version;
- database readiness;
- generator readiness;
- pack catalog readiness;
- artifact-store readiness;
- free server disk space.

### Accounts and access

Supported organization roles:

| Role | Capabilities |
|---|---|
| `owner` | Read/write jobs and drafts, create projects, manage product operations, view owner/admin metrics. |
| `admin` | Same product operations as owner. |
| `developer` | Read/write jobs, scenarios, and custom packs. |
| `viewer` | Read-only access to resources available to that identity. |

Notes:

- Accounts and API keys belong to a user in an organization.
- Cross-organization resources deliberately return `404`.
- Sign out invalidates the server-side browser session.
- API keys can be listed and revoked from the UI.

### Projects

Jobs belong to projects. The UI lists available projects and lets owners/admins create additional projects.

### Curated packs

The curated catalog shows:

- stable pack ID;
- semantic version;
- release status;
- release date;
- purpose;
- scoreable targets versus reference-only targets;
- detector output schemas;
- recommended verifier profile and supported threshold profiles;
- scenario/case count;
- estimated duration, sampling rates, channel count, and package size;
- difficulty/stress tags;
- recommended-for and not-recommended-for guidance;
- scenarios with per-case duration/rate/channel metadata;
- authoritative pack fingerprint;
- compatible generator contract/version;
- changelog;
- deprecation message, if applicable.

Built-in curated pack files are immutable product releases. A changed curated release requires a semantic version update, changelog, reviewed fingerprint, and generator compatibility declaration.

The committed `packs/*.json` and `packs/*.product` files are generated from the sibling `signal_synth` release-set artifact with:

```sh
python3 scripts/import_curated_release_set.py \
  --metadata ../signal_synth/examples/catalog/curated_pack_metadata_v1.json \
  --source-root ../signal_synth \
  --out packs \
  --clean \
  --signal-synth-cli /opt/signal_synth/bin/signal-synth
```

The import also stores `packs/curated_pack_metadata_v1.catalog`, which the SaaS API uses for discovery metadata. The `.product` sidecars retain the fingerprint of the SaaS-imported pack JSON after path normalization.

### Scenario drafts

The scenario editor supports creating, validating, updating, listing, and deleting user-owned drafts.

Key properties:

- The clean ECG example provides a deterministic starting point.
- Local JSON formatting catches syntax errors before submission.
- Valid drafts are canonicalized and receive a SHA-256 document fingerprint.
- Invalid drafts are saved as editable drafts and return actionable validation entries with code, JSON path, and message.
- Drafts are scoped to the exact owner identity.
- Viewer-role credentials cannot modify drafts.
- The scenario contract is validated by the sibling `signal_synth` implementation.

Do not place PHI, personal data, patient identifiers, or clinical free text in draft names or scenario fields.

### Custom pack composer

Custom packs are immutable snapshots assembled from valid scenario drafts.

The composer accepts:

- pack name;
- description;
- target list;
- selected valid scenario drafts.

Composition copies canonical scenario documents into a snapshot. Later edits or deletion of source drafts do not change the pack. Deleting a custom pack hides it from new-job selection, while retained job/package snapshots remain reproducible.

### Jobs

The UI supports:

- creating jobs from curated or custom packs;
- automatic status polling;
- loading older jobs;
- queued-job cancellation;
- retrying failed or cancelled jobs as new records;
- soft-deleting non-running jobs;
- downloading `manifest.json` and `package.zip`;
- viewing project, lifecycle timestamps, generator identity, build identity, and package fingerprint;
- linking from completed jobs to the local verification workflow;
- displaying `queued`, `running`, `succeeded`, `failed`, `cancelled`, and artifact-expired states.

Running jobs cannot currently be force-cancelled. The API returns `409` because terminating the external generator is not yet considered safe.

### Usage and metrics

All users can see:

- requests in the last minute;
- active jobs;
- monthly jobs and configured limits;
- monthly package count;
- stored bytes.

Owners/admins additionally see:

- queue depth;
- running workers;
- monthly failures;
- quota rejections;
- last worker heartbeat.

Current private-beta limits:

| Limit | Value |
|---|---:|
| Requests per minute per API key | 120 |
| Concurrent queued/running jobs per organization | 2 |
| Jobs per month per organization | 100 |

## Job and artifact lifecycle

Job states:

```text
queued → running → succeeded
                 ↘ failed
queued → cancelled
```

A retry creates a new queued job and preserves the original. Deletion is a soft delete.

Default artifact retention is 90 days. A soft-deleted job becomes immediately eligible for cleanup. After expiry:

- job metadata remains;
- lifecycle timestamps remain;
- pack/package fingerprints remain;
- generator identity remains;
- downloads return `404`;
- the UI/API reports `artifact_status: expired`.

## Package contents

`package.zip` is a ZIP-compatible challenge package. Important top-level files include:

| Path | Purpose |
|---|---|
| `manifest.json` | Package identity, file roles, case IDs, hashes, scenario identities, generator identities. |
| `pack.json` | Pack metadata and case ordering. |
| `summary.json` / `summary.csv` | Pack-level generation summary. |
| `index.html` | Human-readable package overview. |
| `cases/<case-id>/scenario.json` | Canonical scenario. |
| `cases/<case-id>/waveform.csv` | Waveform samples. |
| `cases/<case-id>/annotations.json` | Deterministic ground truth. |
| `cases/<case-id>/ground_truth_metrics.json` | Reference metrics. |
| `cases/<case-id>/report.html` | Case report. |
| WFDB / EDF / BDF files | Interchange formats for compatible tools. |

Treat `manifest.json`, document fingerprints, package fingerprint, and generator build identity as the evidence chain.

Challenge jobs always produce the service's complete export/report set. The job API intentionally has no per-request format switches.

## HTTP API

Base URL:

```text
https://www.timeonion.com/syn_sig_ra
```

Browser calls use the secure session cookie. Scripts and CI use bearer API keys:

```http
Authorization: Bearer <api-key>
```

| Method | Path | Purpose | Auth |
|---|---|---|---|
| `GET` | `/healthz` | Liveness/build | Public |
| `GET` | `/readyz` | Component readiness/disk | Public |
| `GET` | `/v1/packs` | Curated pack list | Public |
| `GET` | `/v1/packs/{pack_id}` | Curated pack detail | Public |
| `POST` | `/v1/auth/register` | Create account and browser session | Public |
| `POST` | `/v1/auth/login` | Start browser session | Public |
| `GET` | `/v1/auth/me` | Current account | Session |
| `POST` | `/v1/auth/logout` | End browser session | Session |
| `GET` | `/v1/projects` | Project list and caller role | Authenticated |
| `POST` | `/v1/projects` | Create project | Owner/admin |
| `GET` | `/v1/api-keys` | List personal API keys | Authenticated |
| `POST` | `/v1/api-keys` | Create one-time API key secret | Authenticated |
| `DELETE` | `/v1/api-keys/{id}` | Revoke personal API key | Authenticated |
| `GET` | `/v1/scenarios` | List owned drafts | Authenticated |
| `POST` | `/v1/scenarios` | Validate/create draft | Developer+ |
| `GET` | `/v1/scenarios/{id}` | Draft detail | Owner identity |
| `PUT` | `/v1/scenarios/{id}` | Replace/revalidate draft | Developer+ |
| `DELETE` | `/v1/scenarios/{id}` | Delete draft | Developer+ |
| `GET` | `/v1/custom-packs` | List owned custom packs | Authenticated |
| `POST` | `/v1/custom-packs` | Compose immutable pack | Developer+ |
| `GET` | `/v1/custom-packs/{id}` | Custom pack detail | Owner identity |
| `DELETE` | `/v1/custom-packs/{id}` | Hide custom pack | Developer+ |
| `GET` | `/v1/jobs?limit=25&offset=0` | Paginated job list | Authenticated |
| `POST` | `/v1/jobs` | Queue curated/custom pack | Developer+ |
| `GET` | `/v1/jobs/{id}` | Job status/detail | Organization |
| `DELETE` | `/v1/jobs/{id}` | Soft-delete non-running job | Developer+ |
| `POST` | `/v1/jobs/{id}/cancel` | Cancel queued job | Developer+ |
| `POST` | `/v1/jobs/{id}/retry` | Retry failed/cancelled job | Developer+ |
| `GET` | `/v1/artifacts/{package_id}/manifest.json` | Download manifest | Organization |
| `GET` | `/v1/artifacts/{package_id}/package.zip` | Download ZIP | Organization |
| `GET` | `/v1/usage` | Usage and limits | Authenticated |
| `GET` | `/v1/metrics` | Operational metrics | Owner/admin |

Create a job from a script:

```sh
read -r -s SYN_SIG_RA_API_KEY
BASE=https://www.timeonion.com/syn_sig_ra

curl -fsS \
  -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"project_id":"org_live_default","pack_id":"r_peak_stress_v1"}' \
  "$BASE/v1/jobs"
```

The create-job body accepts exactly `project_id` and `pack_id`; unsupported fields are rejected.

## Operations summary

The production shape is:

```text
nginx : public HTTP/HTTPS edge, TLS, canonical redirects
Apache module : localhost backend under /syn_sig_ra
SQLite : metadata database
filesystem data root : immutable packages and snapshots
worker service : queued generation jobs
signal_synth CLI : authoritative generator
```

Operational endpoints:

| Endpoint | Use |
|---|---|
| `/healthz` | Confirms the Apache module can answer. |
| `/readyz` | Confirms DB, generator, catalog, and artifact store are usable. |
| `/v1/usage` | Customer-facing usage counters and limits. |
| `/v1/metrics` | Owner/admin operational counters and worker heartbeat. |

Operational scripts documented in `doc/` cover build, deploy, live verification, retention cleanup, backup, and restore drill workflows.

## Repository layout

```text
apache/                 Apache integration files
include/syn_sig_ra/      Public/internal C++ headers
src/                    SaaS module, API, auth, jobs, catalog, worker logic
test/                   Unit/integration tests
packs/                  Built-in curated pack definitions and product sidecars
scripts/                Build/deploy/verify/backup/retention helper scripts
ops/                    nginx, letsencrypt, and service configuration assets
doc/                    API, operations, tenancy, custom-pack, deployment docs
var.example/            Example runtime state layout
```

## Build and deployment references

Start here for implementation and operations details:

- [`doc/DEVELOPER_REFERENCE.md`](doc/DEVELOPER_REFERENCE.md)
- [`doc/VPS_DEPLOYMENT.md`](doc/VPS_DEPLOYMENT.md)
- [`doc/OPERATIONS.md`](doc/OPERATIONS.md)
- [`doc/RETENTION_BACKUP.md`](doc/RETENTION_BACKUP.md)
- [`doc/openapi.yaml`](doc/openapi.yaml)

The current deployment uses nginx as the public TLS edge and a custom Apache backend bound to localhost. The sibling `signal_synth` CLI is installed separately and invoked by the SaaS worker.

## Security and limitations

- HTTPS terminates at nginx.
- The Apache application backend listens only on localhost.
- Browser authentication uses revocable, expiring server-side sessions.
- API key secrets are displayed once and stored only as SHA-256 hashes.
- Passwords are stored using salted PBKDF2-HMAC-SHA256.
- E-mail verification and password recovery are not implemented yet.
- The product accepts synthetic engineering scenarios only.
- Do not upload PHI, personal data, patient identifiers, or clinical free text.
- The service does not execute customer detector code.
- Built-in and composed pack snapshots are immutable.
- Generated reports are engineering verification evidence, not clinical validation evidence.

## Further documentation

- [Customer API guide](doc/API_GUIDE.md)
- [OpenAPI reference](doc/openapi.yaml)
- [Scenario drafts](doc/SCENARIO_DRAFTS.md)
- [Custom packs](doc/CUSTOM_PACKS.md)
- [Pack catalog release policy](doc/PACK_CATALOG.md)
- [Tenancy and authorization](doc/TENANCY.md)
- [Retention and backup](doc/RETENTION_BACKUP.md)
- [Operations and observability](doc/OPERATIONS.md)
- [Developer reference](doc/DEVELOPER_REFERENCE.md)
- [VPS deployment runbook](doc/VPS_DEPLOYMENT.md)
- [SaaS product plan](doc/SAAS_PRODUCT_PLAN.md)
- [Codex handoff after core release-set export](doc/SAAS_CODEX_HANDOFF_2026_07_06.md)
