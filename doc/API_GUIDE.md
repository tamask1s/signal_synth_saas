# Customer API guide

Base URL:

```text
https://www.timeonion.com/syn_sig_ra
```

Plain HTTP redirects to HTTPS.

Browser users register and sign in from the web UI; they do not paste API keys
into the browser. Create a named personal API key from the account panel for
the curl examples below. Its secret is shown once and can be revoked there.

## Data policy

Submit synthetic engineering scenarios only. Do not include patient data,
names, identifiers, free-text clinical notes, or other PHI/personal data in
API requests, project names, labels, or future scenario drafts. Generated
signals are engineering test artifacts, not clinical evidence.

## Account lifecycle

The browser Account page is the preferred interface for personal settings.
The same session-authenticated API supports `PATCH /v1/account` for display
name changes, `POST /v1/account/password` for a current-password-confirmed
password change, and `GET /v1/account/export` for a portable JSON export.
Password changes invalidate every older browser session; the initiating browser
receives one replacement session.

`DELETE /v1/account` requires the current password and exact confirmation
`DELETE MY ACCOUNT`. It is available only to an owner whose workspace has no
other members or running jobs. It permanently removes the owned workspace and
server artifacts, retaining only an anonymous deletion receipt. Downloaded
copies are outside the server and cannot be revoked. The export intentionally
omits password material, API-key secrets/hashes, session tokens, and e-mail
action tokens. See live `/openapi.yaml` for exact schemas and status codes.

## First job with curl

Store the private-beta key without putting it in shell history:

```sh
read -r -s SYN_SIG_RA_API_KEY
export SYN_SIG_RA_API_KEY
BASE=https://www.timeonion.com/syn_sig_ra
```

List projects and packs:

```sh
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  "$BASE/v1/projects"
curl -fsS "$BASE/v1/packs"
```

Create a job using a returned `project_id` and `pack_id`:

```sh
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"project_id":"org_live_default","pack_id":"r_peak_stress_v1"}' \
  "$BASE/v1/jobs"
```

Poll the returned job:

```sh
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  "$BASE/v1/jobs/job_REPLACE_ME"
```

When status is `succeeded`, download both immutable artifacts:

```sh
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  -o manifest.json "$BASE/v1/artifacts/pkg_REPLACE_ME/manifest.json"
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  -o package.zip "$BASE/v1/artifacts/pkg_REPLACE_ME/package.zip"
unzip -t package.zip
```

Download detector-output templates for the completed curated job:

```sh
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  -o detection-templates.zip \
  "$BASE/v1/jobs/job_REPLACE_ME/detection-templates.zip"
unzip detection-templates.zip
```

The ZIP contains `README.md` plus one template file per locally scoreable
case/target, for example `detections/clean_70_r_peak.csv`. Replace example
rows with your algorithm output. Reference-only targets are intentionally not
included.

Run the local verifier:

```sh
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  -o synsigra-wheel.whl \
  "$BASE/v1/downloads/verifier/synsigra-wheel.whl"
python -m pip install synsigra-wheel.whl
synsigra-verify package.zip detections/ verification-results/ \
  --profile stress \
  --force
```

The verifier download contains only the Python package and helper scripts. It
does not include the generator binary or source tree.

## Scenario authoring helpers

Authenticated clients can build form-assisted scenario editors from core-owned
metadata:

```sh
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  "$BASE/v1/authoring/schema"
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  "$BASE/v1/authoring/templates"
```

Preview a draft before saving or composing a custom pack:

```sh
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"scenario":{...},"targets":["r_peak"]}' \
  "$BASE/v1/authoring/preview"
```

Clone a curated case into a draft editor:

```sh
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  "$BASE/v1/authoring/curated-scenarios/r_peak_stress_v1/clean_70"
```

The complete machine-readable reference is
[`openapi.yaml`](openapi.yaml). The live app also serves a rendered API
reference at `https://www.timeonion.com/syn_sig_ra/docs/api`, a one-page
quickstart at `/docs/quickstart`, and troubleshooting at
`/docs/troubleshooting`. A dependency-free Python implementation of the same
flow is [`scripts/customer_smoke.py`](../scripts/customer_smoke.py).

## Stable error codes

| HTTP | Code | Meaning |
|---:|---|---|
| 400 | `invalid_job_request`, `unsupported_field` | Request JSON is invalid |
| 401 | `unauthorized` | Bearer key is missing, unknown, or revoked |
| 403 | `forbidden` | The role cannot perform the operation |
| 404 | `project_not_found`, `pack_not_found`, `job_not_found`, `artifact_not_found` | Resource is absent or belongs to another tenant |
| 409 | `job_cancel_invalid_state`, `job_retry_invalid_state` | Lifecycle action is invalid |
| 409 | `job_templates_unavailable`, `custom_pack_templates_unavailable`, `pack_templates_unavailable` | Template ZIP cannot be generated for this job/pack state |
| 409 | `pack_generator_incompatible` | Pack is not approved for this generator |
| 415 | `unsupported_media_type` | JSON endpoint requires `application/json` |
| 429 | `request_rate_limit`, `concurrent_job_limit`, `monthly_job_limit` | Retry after reducing request/job usage |
| 503 | `metadata_unavailable` | Temporary storage/readiness failure |

Cross-tenant resources intentionally return 404. Clients must not infer
resource existence from that response.

## Troubleshooting verification

- If the template ZIP is unavailable, wait for the job to reach `succeeded`
  and use a curated pack with at least one scoreable target.
- If `synsigra-verify` exits `1`, inspect detection filenames, required
  columns, units, selected profile, and the generated per-case report.
- If `synsigra-verify` exits `2`, start from the completed-job command in the
  UI or the template ZIP `README.md`; this is a CLI usage error.
- If an artifact cache expired, request `POST /v1/jobs/{job_id}/rebuild`.
  Synsigra queues a new job using the preserved immutable recipe and exact
  historical generator release, and rejects a package-fingerprint mismatch.
  Historical jobs without those inputs return an explicit `409`; a newer
  generator is never silently substituted.
