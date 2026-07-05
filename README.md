# SynSigRa SaaS user manual

SynSigRa creates deterministic synthetic biosignal challenge packages for
engineering verification. The service can be used from the browser or through
its HTTP API.

Live browser UI:

```text
http://www.timeonion.com/syn_sig_ra/
```

This private-beta deployment currently uses HTTP, not HTTPS. Do not transmit
production credentials over an untrusted network. Submit synthetic engineering
data only: never enter patient data, names, identifiers, clinical notes, PHI,
or other personal data.

## Quick start in the browser

1. Open the UI and paste the private-beta API key. It remains in the current
   tab's `sessionStorage`; it is not placed in the URL.
2. Check the Service card. `health` means Apache responds; `ready` means the
   database, generator, pack catalog, and artifact store are available.
3. Select the Default project or create another project if your role permits.
4. Select a curated or custom pack.
5. Create the job. The list updates automatically without reloading the page.
6. Wait for `succeeded`, then download `manifest.json` and `package.zip`.

## Everything available in the UI

### Service status

The header shows:

- service liveness and build version;
- database readiness;
- generator readiness;
- pack catalog readiness;
- artifact storage readiness;
- free server disk space.

### API key and roles

Keys belong to a user in an organization. Supported roles are:

- `owner`: read/write jobs and drafts, create projects, view metrics;
- `admin`: the same product operations as owner;
- `developer`: read/write jobs, scenarios, and custom packs;
- `viewer`: read-only access to resources owned by that identity.

### Projects

Jobs belong to projects. The UI lists available projects and lets owners/admins
create a project. Cross-organization resources deliberately return 404.

### Curated packs

The catalog shows:

- stable pack ID and semantic version;
- release status and release date;
- purpose, targets, and scenarios;
- authoritative pack fingerprint;
- compatible generator contract/version;
- deprecation message, when applicable;
- release changelog.

Curated pack files are immutable product releases.

### Scenario drafts

The JSON editor can create, validate, update, list, and delete user-owned
scenario drafts.

- Valid drafts are canonicalized and receive a SHA-256 document fingerprint.
- Invalid drafts are still saved and display actionable validation code, JSON
  path, and message.
- Another user, including one in the same organization, cannot read the draft.
- Viewer keys cannot modify drafts.

The scenario JSON contract is validated directly by the sibling
`signal_synth` implementation.

### Custom pack composer

Select one or more valid drafts, enter a name, description, and comma-separated
targets, then create the custom pack.

Composition snapshots the canonical scenario documents. Editing or deleting a
source draft later does not change the pack. Removing a custom pack hides it
from new-job selection, but retained job/package snapshots remain reproducible.

### Jobs

The UI supports:

- creating jobs from curated or custom packs;
- automatic status polling without full-page refresh;
- loading older jobs;
- queued-job cancellation;
- retrying failed or cancelled jobs as new records;
- soft-deleting non-running jobs;
- downloading manifests and ZIP packages;
- displaying queued, running, succeeded, failed, cancelled, and
  artifact-expired states.

Running jobs cannot currently be force-cancelled. This returns HTTP 409 because
terminating the external generator is not yet considered safe.

### Usage and operational metrics

All users can see:

- requests in the last minute;
- active jobs;
- monthly jobs and configured limits;
- monthly package count and stored bytes.

Owners/admins additionally see queue depth, running workers, monthly failures,
quota rejections, and the worker's last heartbeat.

Current private-beta limits:

- 120 requests/minute per API key;
- 2 concurrent queued/running jobs per organization;
- 100 jobs/month per organization.

### Documentation

The UI links to this manual and the
[OpenAPI reference](doc/openapi.yaml).

## Job and artifact lifecycle

Job states:

```text
queued → running → succeeded
                 ↘ failed
queued → cancelled
```

A retry creates a new queued job and preserves the original. Deletion is a
soft delete. Package files are immutable while retained.

Default artifact retention is 90 days. A soft-deleted job becomes immediately
eligible for cleanup. After expiry, job timestamps, pack/package fingerprints,
and generator identity remain, while downloads return 404 and the UI reports
`artifact_status: expired`.

## Package contents

`package.zip` is a ZIP-compatible challenge package. Important top-level files:

- `manifest.json`: package identity, file roles, case IDs, hashes, scenario and
  generator identities;
- `pack.json`: pack metadata and case ordering;
- `summary.json` / `summary.csv`: pack-level generation summary;
- `index.html`: human-readable package overview;
- `cases/<case-id>/scenario.json`: canonical scenario;
- `cases/<case-id>/waveform.csv`: waveform samples;
- `cases/<case-id>/annotations.json`: deterministic ground truth;
- `cases/<case-id>/ground_truth_metrics.json`: reference metrics;
- `cases/<case-id>/report.html`: case report;
- WFDB (`.hea`, `.dat`, `.atr`), EDF and BDF exports where requested.

Treat `manifest.json`, document fingerprints, package fingerprint, and
generator build identity as the verification evidence chain.

## Verify algorithm output against a downloaded package

Scoring is local. The SaaS never needs your proprietary detector executable or
its output.

### 1. Build the authoritative CLI and expose the Python package

```sh
git clone https://github.com/tamask1s/signal_synth.git
cmake -S signal_synth -B signal_synth/build \
  -DSIGNAL_SYNTH_BUILD_TESTS=OFF
cmake --build signal_synth/build

export SIGNAL_SYNTH_CLI="$PWD/signal_synth/build/signal-synth"
export PYTHONPATH="$PWD/signal_synth/python"
```

### 2. Run your algorithm

For each case, run the algorithm on the format it supports:

- `waveform.csv`;
- WFDB files;
- EDF;
- BDF.

Write one detection file per case using the case ID as filename:

```text
detections/
├── clean_70.csv
├── slow_45.csv
├── fast_120.csv
└── baseline_powerline.csv
```

Minimal R-peak CSV:

```csv
time_seconds
0.82
1.68
2.54
```

Optional columns include `sample_index`, `channel`, `label`, and `confidence`.
JSON detection documents are also supported by `signal_synth`.

### 3. Score one case with the upstream example script

The loader accepts `package.zip` directly; extraction is optional:

```sh
python signal_synth/examples/python/score_challenge.py \
  package.zip \
  clean_70 \
  detections/clean_70.csv \
  verification-clean-70
```

Outputs include `comparison.json`, `comparison.csv`, and
`comparison_report.html`, with sensitivity, positive predictive value, F1,
timing error, false positives, and false negatives.

### 4. Score all detector-compatible cases

This repository includes an orchestration script that still delegates every
score to the authoritative `signal-synth` CLI:

```sh
python scripts/verify_downloaded_package.py \
  package.zip \
  detections \
  verification-results
```

Score selected cases only:

```sh
python scripts/verify_downloaded_package.py \
  package.zip detections verification-results \
  --case clean_70 --case fast_120
```

The result directory contains one authoritative comparison report per case and
`verification_summary.json`.

### 5. Direct CLI verification

```sh
unzip package.zip -d challenge

"$SIGNAL_SYNTH_CLI" compare rpeaks \
  challenge/cases/clean_70/scenario.json \
  detections/clean_70.csv \
  --out verification-clean-70
```

Other supported commands include `compare ppg-peaks`,
`compare beat-classes`, and `hrv score`. Target support depends on the selected
scenario/pack. Signal-quality-only targets currently provide generated
reference artifacts but no event-detector score.

## HTTP API

Base URL:

```text
http://www.timeonion.com/syn_sig_ra
```

Authenticated requests use:

```http
Authorization: Bearer <api-key>
```

| Method | Path | Purpose | Authentication |
|---|---|---|---|
| GET | `/healthz` | Liveness/build | No |
| GET | `/readyz` | Component readiness/disk | No |
| GET | `/v1/packs` | Curated pack list | No |
| GET | `/v1/packs/{pack_id}` | Curated pack detail | No |
| GET | `/v1/projects` | Project list and caller role | Yes |
| POST | `/v1/projects` | Create project | Owner/admin |
| GET | `/v1/scenarios` | List owned drafts | Yes |
| POST | `/v1/scenarios` | Validate/create draft | Developer+ |
| GET | `/v1/scenarios/{id}` | Draft detail | Owner identity |
| PUT | `/v1/scenarios/{id}` | Replace/revalidate draft | Developer+ |
| DELETE | `/v1/scenarios/{id}` | Delete draft | Developer+ |
| GET | `/v1/custom-packs` | List owned custom packs | Yes |
| POST | `/v1/custom-packs` | Compose immutable pack | Developer+ |
| GET | `/v1/custom-packs/{id}` | Custom pack detail | Owner identity |
| DELETE | `/v1/custom-packs/{id}` | Hide custom pack | Developer+ |
| GET | `/v1/jobs?limit=25&offset=0` | Paginated job list | Yes |
| POST | `/v1/jobs` | Queue curated/custom pack | Developer+ |
| GET | `/v1/jobs/{id}` | Job status/detail | Organization |
| DELETE | `/v1/jobs/{id}` | Soft-delete non-running job | Developer+ |
| POST | `/v1/jobs/{id}/cancel` | Cancel queued job | Developer+ |
| POST | `/v1/jobs/{id}/retry` | Retry failed/cancelled job | Developer+ |
| GET | `/v1/artifacts/{package_id}/manifest.json` | Download manifest | Organization |
| GET | `/v1/artifacts/{package_id}/package.zip` | Download ZIP | Organization |
| GET | `/v1/usage` | Usage and limits | Yes |
| GET | `/v1/metrics` | Operational metrics | Owner/admin |

Create a job:

```sh
read -r -s SYN_SIG_RA_API_KEY
BASE=http://www.timeonion.com/syn_sig_ra

curl -fsS \
  -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"project_id":"org_live_default","pack_id":"r_peak_stress_v1"}' \
  "$BASE/v1/jobs"
```

See [doc/API_GUIDE.md](doc/API_GUIDE.md) for curl onboarding and stable error
codes, and [doc/openapi.yaml](doc/openapi.yaml) for the machine-readable
contract.

## Security and limitations

- HTTPS is not enabled yet.
- API keys are stored server-side only as SHA-256 hashes.
- Browser keys live only in the current tab session.
- This is synthetic engineering tooling, not clinical validation evidence.
- Do not upload PHI or personal data.
- The service does not execute customer detector code.
- Built-in and composed pack snapshots are immutable.

## Further documentation

- [Customer API guide](doc/API_GUIDE.md)
- [Scenario drafts](doc/SCENARIO_DRAFTS.md)
- [Custom packs](doc/CUSTOM_PACKS.md)
- [Pack release policy](doc/PACK_CATALOG.md)
- [Retention and backup](doc/RETENTION_BACKUP.md)
- [Operations and observability](doc/OPERATIONS.md)
- [Developer/build/deployment reference](doc/DEVELOPER_REFERENCE.md)
- [Product plan](doc/SAAS_PRODUCT_PLAN.md)
