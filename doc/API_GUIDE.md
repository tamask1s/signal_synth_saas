# Customer API guide

Base URL: `https://www.timeonion.com/syn_sig_ra`.

The complete source of truth is the live
[`/openapi.yaml`](https://www.timeonion.com/syn_sig_ra/openapi.yaml). It is
embedded in the same Apache module revision that handles requests. Fetch it at
the start of an automated integration. Dynamic scenario fields, enumerations,
requirements, and templates are served separately by the authenticated
authoring endpoints.

Submit synthetic engineering scenarios only. Never submit PHI, real patient
signals, proprietary algorithm source, or confidential algorithm output.

## Authentication

Browser users register, verify email, and sign in; the UI uses its secure
session cookie. An API client needs only one personal API key belonging to an
account with the appropriate organization role:

```sh
BASE=https://www.timeonion.com/syn_sig_ra
read -r -s SYN_SIG_RA_API_KEY
export SYN_SIG_RA_API_KEY
AUTH="Authorization: Bearer $SYN_SIG_RA_API_KEY"
```

The secret is displayed once when created. Do not print, commit, or send it to
another origin. A key can be listed, rotated, and revoked from Account or the
`/v1/api-keys` endpoints.

## MCP clients

An AI client can use the same single personal key with Synsigra's stateless
Streamable HTTP MCP endpoint:

```text
https://www.timeonion.com/syn_sig_ra/mcp
Authorization: Bearer YOUR_ONE_TIME_SECRET
```

The MCP adapter delegates to these same API routes, so it cannot bypass roles,
quotas, catalog/core compatibility, storage guards, or job state rules. It adds
goal-based pack ranking, live custom-authoring tools, human-approval annotations
for modifying calls, and job-specific local verification guidance. See the
in-product [MCP setup page](https://www.timeonion.com/syn_sig_ra/mcp-setup).

## Generate and download one curated challenge

Check the exact live core and catalog:

```sh
curl -fsS "$BASE/readyz" > ready.json
curl -fsS "$BASE/v1/packs" > packs.json
curl -fsS -H "$AUTH" "$BASE/v1/projects" > projects.json
```

`ready.json` includes the full canonical core capability document under
`accepted_core.contract_document`. Select returned IDs and queue a job:

For R-peak work, choose by algorithm scope:

- `r_peak_stress_v1`: focused evidence baseline; submit R-peak events and
  beat-to-beat RR measurements, without signal-quality output;
- `r_peak_noise_frontier_v1`: paired −3 through −11 dB R-peak and RR
  robustness ladder, without signal-quality output;
- `r_peak_rr_noise_v1`: combined pipeline evidence that additionally requires
  signal-quality output.

Noise can be a test condition without being an algorithm output. Do not select
`signal_quality` merely because a peak detector is tested under noise.

```sh
curl -fsS -H "$AUTH" -H 'Content-Type: application/json' \
  -d '{"project_id":"PROJECT_ID","pack_id":"r_peak_stress_v1"}' \
  "$BASE/v1/jobs" > job-created.json

curl -fsS -H "$AUTH" "$BASE/v1/jobs/JOB_ID" > job.json
```

Poll until `status` is `succeeded`, `failed`, or `cancelled`. A succeeded job
contains exact pack/catalog/generator identities and normalized trusted
`challenge` metadata, including submission roles and formats.

Download the single recommended customer artifact:

```sh
curl -fsS -H "$AUTH" -o verification-kit.zip \
  "$BASE/v1/jobs/JOB_ID/verification-kit.zip"
unzip verification-kit.zip
cd verification-kit
```

Its layout is:

```text
challenge/                  immutable integrity-protected challenge v3
submission/                 editable role-selected algorithm template
README.txt                  exact package-specific command
```

Provenance and the engineering claim boundary occur once under `challenge/`.
The job response contains the normalized trusted metadata, so the kit does not
duplicate it as a second JSON document.

The raw `manifest.json` and `package.zip` endpoints are available for clients
that explicitly need lower-level immutable artifacts. They are not extra steps
in the normal workflow, and the kit never nests `package.zip` inside another
ZIP.

## Verify locally

Download the pure-Python verifier; it has no generator binary or source:

```sh
curl -fsS -H "$AUTH" -o synsigra-0.14.0-py3-none-any.whl \
  "$BASE/v1/downloads/verifier/synsigra-0.14.0-py3-none-any.whl"
python -m pip install synsigra-0.14.0-py3-none-any.whl
```

Edit the algorithm name/version and replace the example output values under
`submission/`. Preserve the paths, target names, units, and formats declared in
`submission/submission.json`. Then copy the exact command from the kit README.
For a protocol-v2 kit this is the complete package-authoritative evidence run:

```sh
synsigra-verify challenge submission verification-results --force
```

Open `verification-results/index.html`; it links every case-target detail
page. `verification-results/evidence.json` is the single canonical
machine-readable record.

Each acceptance row can be expanded into its contributing cases and raw
counts. The case table shows the relevant gate and an explicitly diagnostic
case comparison; only the pooled criterion is an official verdict.
Measurement detail pages show units, the pairing window and the exact packaged
absolute-or-relative pass tolerance. Criterion and case views link in both
directions, and compact `i` controls explain the displayed metrics.

Do not append a profile, case, or target override to evidence mode. A kit
without protocol v2 instead shows explicit `--mode diagnostic`; diagnostic
reports are always non-evidence. Point, interval, and measurement targets use
their manifest-declared CSV or JSON format; HRV is a measurement-v2 target, not
a separate output family. Never derive paths from a target name. Exit code `0`
means the applicable policy passed, `1` means integrity/input/scoring/threshold
failure, and `2` means invalid CLI usage.

## Scenario and custom-pack authoring

Fetch the live core metadata instead of inventing fields:

```sh
curl -fsS -H "$AUTH" "$BASE/v1/authoring/schema" > authoring-schema.json
curl -fsS -H "$AUTH" "$BASE/v1/authoring/templates" > authoring-templates.json
```

Start from a returned complete template. Preview an exact scenario/target set:

```sh
curl -fsS -H "$AUTH" -H 'Content-Type: application/json' \
  --data-binary @preview-request.json "$BASE/v1/authoring/preview"
```

Save a validated draft with `POST /v1/scenarios`, then compose immutable
snapshots with `POST /v1/custom-packs`. A custom pack uses the same job,
post-render trust checks, verification kit, verifier, and retention behavior as
a curated pack. Every target selected for a custom pack must be compatible with
every selected scenario.

## Viewer API

`GET /v1/jobs/{job_id}/viewer` returns case/channel metadata without waveform
values. `viewer/window` returns a bounded binary viewport for selected channels,
range, and point budget; at wide zoom it uses exact min/max pyramid buckets.
`viewer/overlays` returns only ground-truth items intersecting the requested
range, with dense points clustered under `max_items`.

Clients should cancel obsolete pan requests. WebSockets are unnecessary: each
viewport is independently cacheable and response size is bounded regardless of
the source-file size. See OpenAPI for the 80-byte little-endian `SYNSIGV1`
header.

## Lifecycle and retention

- `POST /v1/jobs/{id}/cancel`: cancel a queued job.
- `POST /v1/jobs/{id}/retry`: create a new job from failed/cancelled input.
- `DELETE /v1/jobs/{id}`: delete a non-running job and server artifacts.
- `POST /v1/jobs/{id}/rebuild`: rehydrate an expired successful artifact with
  the preserved recipe and exact SHA-256-addressed generator release.

Server artifacts expire after seven days. Rebuild never substitutes a newer
generator and fails on fingerprint mismatch. ZIP delivery supports `HEAD`,
strong ETag, SHA-256 checksum, expiry metadata, single byte ranges, and resume.

## Stable response behavior

- `400`: malformed JSON, unknown/extra fields, or invalid request.
- `401`: missing, malformed, unknown, expired, or revoked credential.
- `403`: authenticated role cannot perform the operation.
- `404`: absent or cross-organization resource.
- `409`: invalid lifecycle state or unavailable/inconsistent exact artifact.
- `415`: a JSON endpoint did not receive `application/json`.
- `422`: authoritative scenario/pack compatibility validation failed.
- `429`: request, concurrency, or monthly quota exceeded.
- `503`: temporary metadata/readiness failure.
- `507`: disk reserve blocks generation or derived-artifact creation.

Preserve API error bodies; they contain stable codes and actionable messages.
Cross-tenant 404 deliberately does not reveal whether another tenant owns an
identifier.

The dependency-free example [customer_smoke.py](../scripts/customer_smoke.py)
creates a job and downloads its verification kit. The more detailed
[Codex client guide](CODEX_API_CLIENT_GUIDE.md) covers natural-language ECG/HRV
requests.
