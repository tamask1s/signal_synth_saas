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

The complete machine-readable reference is
[`openapi.yaml`](openapi.yaml). A dependency-free Python implementation of
the same flow is [`scripts/customer_smoke.py`](../scripts/customer_smoke.py).

## Stable error codes

| HTTP | Code | Meaning |
|---:|---|---|
| 400 | `invalid_job_request`, `unsupported_field` | Request JSON is invalid |
| 401 | `unauthorized` | Bearer key is missing, unknown, or revoked |
| 403 | `forbidden` | The role cannot perform the operation |
| 404 | `project_not_found`, `pack_not_found`, `job_not_found`, `artifact_not_found` | Resource is absent or belongs to another tenant |
| 409 | `job_cancel_invalid_state`, `job_retry_invalid_state` | Lifecycle action is invalid |
| 409 | `pack_generator_incompatible` | Pack is not approved for this generator |
| 415 | `unsupported_media_type` | JSON endpoint requires `application/json` |
| 429 | `request_rate_limit`, `concurrent_job_limit`, `monthly_job_limit` | Retry after reducing request/job usage |
| 503 | `metadata_unavailable` | Temporary storage/readiness failure |

Cross-tenant resources intentionally return 404. Clients must not infer
resource existence from that response.
