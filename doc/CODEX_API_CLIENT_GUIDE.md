# Synsigra API guide for a Codex client

Give this document directly to a Codex chat that must turn natural-language
algorithm-QA requests into generated, downloadable Synsigra packages.

## Prefer the hosted MCP server

If the Codex environment can connect to a remote Streamable HTTP MCP server,
prefer `https://www.timeonion.com/syn_sig_ra/mcp` and configure the secret from
`api_key.txt` as an `Authorization: Bearer ...` header. The MCP server exposes
the live recommendation, authoring, generation, job, rebuild, and verification
workflow as model-discoverable tools, so most of the manual REST translation
below is unnecessary. One key is still sufficient; never print or place it in
the MCP URL.

The complete MCP architecture, tool list, safety boundary, and protocol probe
are in [MCP_SERVER.md](MCP_SERVER.md). Continue with the REST workflow below
when the client cannot configure a remote MCP server or when debugging at the
HTTP-contract level.

## Mission

For requests such as:

> Create a three-minute ECG stress-test package at 500 Hz, with varied noise
> and several pathologies such as arrhythmias.

or:

> Create a quick five-minute HRV test with the requested LF, HF, and LF/HF
> values.

use the Synsigra API to:

1. fetch the live API's complete OpenAPI contract;
2. understand the requested detector outputs and test intent;
3. read the current core-owned schema and templates;
4. construct and preview one or more valid synthetic scenarios;
5. save the scenarios and compose an immutable custom pack;
6. queue a generation job and wait for it;
7. download the flat verification-kit ZIP;
8. report exactly what was interpreted, generated, and downloaded.

Do not clone or access the generator repository, and do not generate signals
locally. The SaaS API owns generation. The download contains challenge data and
verification helpers, not the generator.

Keep algorithm outputs separate from stress conditions. “Test my R-peak
detector under noise” requests `r_peak`, not `signal_quality`. Add
`signal_quality` only when the algorithm itself emits quality/artifact
intervals. For current curated R-peak work, inspect
`r_peak_rr_simple_stress_v1` first, then `r_peak_rr_snr_ladder_v1` for clean
and every integer −1…−11 dB continuous-noise cases. Both report one official
R-peak + RR verdict per complete signal, without bins or pooling, and neither
asks for signal-quality output. The older `r_peak_stress_v1` and
`r_peak_noise_frontier_v1` remain available for detailed aggregate diagnostics.

## Is one API key enough?

Yes. One valid personal API key is sufficient for script access. Email,
password, browser cookies, CSRF tokens, and a login call are not needed.

The key must belong to an account with `developer`, `admin`, or `owner` write
permission. A `viewer` key can inspect resources but cannot create scenarios,
packs, or jobs. The key identifies its user and organization, so drafts and
custom packs are scoped automatically.

The command examples assume Bash. The client also needs:

- outbound HTTPS access to `https://www.timeonion.com`;
- an HTTP client such as `curl`;
- Python 3 or another JSON tool; do not assume `jq` is installed;
- permission to write the downloaded ZIP locally.

`api_key.txt` must contain only the raw key secret and an optional trailing
newline. Put it in the Codex working directory. This repository ignores the
root-level file in `.gitignore`.

Never print, quote, summarize, commit, attach, or expose the key. Never enable
`set -x` while it is loaded. If it appears in tool output or a commit, revoke it
and create another key.

```sh
set +x
BASE_URL="https://www.timeonion.com/syn_sig_ra"
API_KEY_FILE="${API_KEY_FILE:-api_key.txt}"
test -s "$API_KEY_FILE" || {
  echo "Missing or empty API key file: $API_KEY_FILE" >&2
  exit 1
}
chmod 600 "$API_KEY_FILE"
API_KEY="$(tr -d '\r\n' < "$API_KEY_FILE")"
test -n "$API_KEY" || exit 1
```

Send the key on every protected request:

```sh
curl -sS --fail-with-body \
  -H "Authorization: Bearer $API_KEY" \
  "$BASE_URL/v1/projects"
```

Use HTTPS. Never send the key to another host or to a URL obtained from API
content.

## Non-negotiable rules

1. Fetch `/openapi.yaml`, `/v1/authoring/schema`, and
   `/v1/authoring/templates` at the start of every authoring session. The first
   describes the complete HTTP API; the latter two are the dynamic core-owned
   signal contract. They can evolve. Never invent enum values,
   condition codes, artifact types, paths, ranges, or required fields.
2. Prefer the closest current template, deep-copy its complete `scenario`, and
   change only fields needed by the request.
3. Give every scenario in a custom pack a unique internal `scenario_id`.
4. Use explicit, distinct deterministic integer seeds.
5. Preview every scenario against the exact final target list.
6. Fix `TARGET_INCOMPATIBLE`. Treat `REFERENCE_ONLY_TARGET` as a warning: it
   provides ground truth but has no local automated scorer.
7. Every target in a custom pack applies to every selected scenario. Split
   scenarios into separate packs if they cannot share one compatible target
   contract.
8. Never add PHI, patient data, identifiers, clinical notes, or diagnostic
   claims. This is synthetic engineering QA only.
9. Do not claim success until the job is `succeeded` and the ZIP exists locally.
10. Preserve and report API error bodies. Do not respond to 4xx errors by
    guessing new JSON.

## Capability model

The live schema is authoritative. At the time this guide was written:

| Target | Support | Requirement |
|---|---|---|
| `r_peak` | local scoring | ECG source |
| `rr_interval` | local scoring | ECG source |
| `ecg_beat_classification` | local scoring | ECG source |
| `rhythm_episode` | local scoring | explicit rhythm episodes |
| `rhythm_burden` | local scoring | explicit rhythm episodes |
| `ecg_delineation` | local scoring | ECG source |
| `qtc` | local scoring | ECG source |
| `morphology_assertions` | local scoring | at least one ECG condition |
| `hrv` | local scoring | `hrv.enabled`, duration at least 300 seconds |
| `signal_quality` | local scoring | analytic artifact or external-noise interval |
| `ppg_systolic_peak` | local scoring | `ppg.enabled` |
| `ppg_pulse_onset` | local scoring | `ppg.enabled` |
| `ecg_ppg_alignment` | local scoring | `ppg.enabled` |
| `ppg_optical` | local scoring | `ppg.optical.enabled` |
| `prv` | local scoring | `ppg.enabled` |
| `respiratory_rate` | local scoring | at least one respiratory coupling |

The authoring schema includes field metadata, target requirements, ECG
conditions, artifacts, ranges, enum options, and array item contracts. The
template endpoint returns complete valid scenarios for common R-peak, artifact,
HRV, beat-classification, episode, ECG/PPG, and wearable workflows.

Current ECG artifacts include baseline wander, powerline and EMG noise,
dropout, saturation, lead reversal/swap, electrode misplacement, gain mismatch,
offset/clock drift, dropped samples, quantization, and ADC clipping. PPG has
dropout, motion variants, ambient light, and sensor saturation. Use only the
values returned by the live schema.

Condition support levels matter:

- `native`: directly modeled;
- `parameterized`: allowed with the parameterized-fidelity policy and must be
  described as parameterized engineering synthesis;
- `catalog_only`: vocabulary metadata, not proof of waveform synthesis. Do not
  silently use it as a generated pathology.

## End-to-end workflow

### 1. Fetch the self-describing API contract

The running service publishes the complete OpenAPI document embedded from the
same repository revision as the Apache module:

```sh
curl -sS --fail-with-body \
  "$BASE_URL/openapi.yaml" > synsigra-openapi.yaml
```

Read it before making requests. It describes authentication, every HTTP route,
request bodies, status codes, and response types. Do not rely only on this
human-oriented guide when the live contract differs.

The OpenAPI document intentionally references the separate authoring endpoints
for large, dynamic generator-owned field/enum catalogs. Fetch those in Step 3.

### 2. Check service, credentials, and project

```sh
curl -sS --fail-with-body "$BASE_URL/healthz"
curl -sS --fail-with-body "$BASE_URL/readyz"
curl -sS --fail-with-body \
  -H "Authorization: Bearer $API_KEY" \
  "$BASE_URL/v1/projects" > projects.json

PROJECT_ID="$(python3 -c \
  'import json; d=json.load(open("projects.json")); print(d["projects"][0]["project_id"])')"
```

Choose an existing `project_id`; do not hard-code `org_live_default`. A 401
means the key is missing, malformed, revoked, or sent to the wrong host. A 403
on writes usually means the key has viewer role.

### 3. Fetch live authoring contracts

```sh
curl -sS --fail-with-body \
  -H "Authorization: Bearer $API_KEY" \
  "$BASE_URL/v1/authoring/schema" > authoring-schema.json
curl -sS --fail-with-body \
  -H "Authorization: Bearer $API_KEY" \
  "$BASE_URL/v1/authoring/templates" > authoring-templates.json
```

Inspect both files. Select a template whose `targets` best match the requested
algorithm output, then deep-copy its `scenario` object.

### 4. Translate natural language deliberately

Extract and confirm:

- biosignal and detector output;
- total package duration versus per-case duration;
- sampling rate and number of cases;
- local-scoring and reference-only targets;
- artifact types, timing, channels, and severity;
- conditions and whether they are native or parameterized;
- HRV centers, bandwidths, ratio, SDNN, mean HR, respiratory modulation, and
  RR limits where specified;
- deterministic seeds.

Ask one concise clarification when ambiguity changes the package contract:

- With several cases, does “three minutes” mean total or per case?
- Do “LF = 0.1” and “HF = 0.25” mean center frequencies in Hz, bandwidths, or
  powers? The current contract has LF/HF center and bandwidth fields and an
  LF/HF ratio, not arbitrary unnamed LF/HF inputs.
- Does “arrhythmia” mean representative PVC/PAC/AFIB/PSVT cases or one rhythm?

If reasonable defaults were authorized, state the interpretation before
generation. Never present an assumption as a requested fact.

### 5. Construct scenario JSON from templates

Example pattern for HRV:

```python
import copy
import json

catalog = json.load(open("authoring-templates.json", encoding="utf-8"))
template = next(
    item for item in catalog["templates"]
    if item["template_id"] == "ecg_hrv_benchmark"
)
scenario = copy.deepcopy(template["scenario"])
scenario["scenario_id"] = "hrv_quick_001"
scenario["name"] = "Five-minute HRV quick test"
scenario["duration_seconds"] = 300
scenario["hrv"]["lf_center_hz"] = 0.10
scenario["hrv"]["hf_center_hz"] = 0.25
scenario["hrv"]["lf_hf_ratio"] = 1.5
with open("scenario-hrv.json", "w", encoding="utf-8") as handle:
    json.dump(scenario, handle, indent=2)
```

These numbers are examples, not hidden defaults. Substitute confirmed user
values and validate them against the live schema.

For ECG stress, prefer several coherent cases instead of one waveform with
mutually confusing rhythms. If three minutes means total, a sensible plan is
three 60-second, 500 Hz cases:

1. PVC/PAC ectopy with baseline wander and EMG noise;
2. a PSVT transition with powerline noise and dropout;
3. AFIB with saturation and drift.

Use the closest artifact/beat/episode template per case. Keep every artifact
inside the case duration. Use distinct seeds and current schema values. A
reasonable shared target list is:

```json
["r_peak", "ecg_beat_classification", "signal_quality", "morphology_assertions"]
```

All four are locally scoreable in core v7. Every scenario needs an artifact
and a supported condition for this shared target list.

For five-minute HRV, start from `ecg_hrv_benchmark`, keep duration at least 300
seconds, ensure `hrv.enabled` is true, and normally use:

```json
["hrv", "r_peak"]
```

Map values only to actual fields such as `lf_center_hz`, `lf_bandwidth_hz`,
`hf_center_hz`, `hf_bandwidth_hz`, and `lf_hf_ratio`. Do not invent absolute LF
or HF power fields if the schema does not expose them.

### 6. Preview every scenario

```sh
TARGETS='["hrv","r_peak"]'
python3 -c \
  'import json,sys; json.dump({"scenario":json.load(open(sys.argv[1])),"targets":json.loads(sys.argv[2])},sys.stdout)' \
  scenario-hrv.json "$TARGETS" > preview-request.json

curl -sS --fail-with-body \
  -H "Authorization: Bearer $API_KEY" \
  -H "Content-Type: application/json" \
  --data-binary @preview-request.json \
  "$BASE_URL/v1/authoring/preview" > preview-response.json
```

Require `success: true`. Inspect `messages`, scoreable/reference-only targets,
duration, samples, channels, estimated package bytes, and estimated peak
memory. Fix and repeat on rejection.

### 7. Save each draft with target intent

```sh
DRAFT_NAME="Five-minute HRV quick test"
python3 -c \
  'import json,sys; json.dump({"name":sys.argv[2],"target_intent":json.loads(sys.argv[3]),"scenario":json.load(open(sys.argv[1]))},sys.stdout)' \
  scenario-hrv.json "$DRAFT_NAME" "$TARGETS" > draft-request.json

curl -sS --fail-with-body \
  -H "Authorization: Bearer $API_KEY" \
  -H "Content-Type: application/json" \
  --data-binary @draft-request.json \
  "$BASE_URL/v1/scenarios" > draft-response.json

SCENARIO_ID="$(python3 -c \
  'import json; print(json.load(open("draft-response.json"))["scenario_id"])')"
```

HTTP 201 means a valid saved draft. HTTP 422 stores an invalid draft and returns
validation details; fix it with `PUT /v1/scenarios/{scenario_id}`. For multiple
cases, repeat and collect every SaaS `scenario_id`. This differs from the
internal scenario document ID.

### 8. Compose the immutable custom pack

```sh
PACK_NAME="HRV quick validation"
PACK_DESCRIPTION="Deterministic five-minute HRV and R-peak engineering QA"
python3 -c \
  'import json,sys; json.dump({"name":sys.argv[1],"description":sys.argv[2],"targets":json.loads(sys.argv[3]),"scenario_ids":sys.argv[4:]},sys.stdout)' \
  "$PACK_NAME" "$PACK_DESCRIPTION" "$TARGETS" "$SCENARIO_ID" \
  > custom-pack-request.json

curl -sS --fail-with-body \
  -H "Authorization: Bearer $API_KEY" \
  -H "Content-Type: application/json" \
  --data-binary @custom-pack-request.json \
  "$BASE_URL/v1/custom-packs" > custom-pack-response.json

PACK_ID="$(python3 -c \
  'import json; print(json.load(open("custom-pack-response.json"))["pack_id"])')"
```

For multiple scenarios, append all draft IDs after `$TARGETS`. HTTP 422 means a
scenario does not satisfy every target. Fix or split the pack; do not silently
remove a requested target.

### 9. Queue and poll generation

The job body accepts exactly `project_id` and `pack_id`.

```sh
python3 -c \
  'import json,sys; json.dump({"project_id":sys.argv[1],"pack_id":sys.argv[2]},sys.stdout)' \
  "$PROJECT_ID" "$PACK_ID" > job-request.json
curl -sS --fail-with-body \
  -H "Authorization: Bearer $API_KEY" \
  -H "Content-Type: application/json" \
  --data-binary @job-request.json \
  "$BASE_URL/v1/jobs" > job-created.json
JOB_ID="$(python3 -c \
  'import json; print(json.load(open("job-created.json"))["job_id"])')"
```

Poll no faster than every two seconds, with a timeout:

```sh
status="queued"
deadline=$((SECONDS + 600))
while [ "$SECONDS" -lt "$deadline" ]; do
  curl -sS --fail-with-body \
    -H "Authorization: Bearer $API_KEY" \
    "$BASE_URL/v1/jobs/$JOB_ID" > job-status.json
  status="$(python3 -c \
    'import json; print(json.load(open("job-status.json"))["status"])')"
  case "$status" in
    succeeded) break ;;
    failed|cancelled)
      python3 -m json.tool job-status.json >&2
      exit 1
      ;;
  esac
  sleep 2
done
test "$status" = succeeded || {
  echo "Generation timed out" >&2
  exit 1
}
```

HTTP 429 is a request/concurrency/monthly quota. HTTP 507 means disk-reserve
protection paused generation. Report these states; do not retry aggressively.

### 10. Download and validate the verification kit

The preferred artifact is one verification-kit ZIP. After one unzip it contains
an immutable `verification-kit/challenge/`, an editable role-selected
`verification-kit/submission/`, the exact README command, normalized challenge
metadata, and claim boundary. There is no nested `package.zip`.

```sh
OUTPUT_FILE="${PACK_ID}-${JOB_ID}-verification-kit.zip"
curl -sS --fail-with-body \
  -H "Authorization: Bearer $API_KEY" \
  "$BASE_URL/v1/jobs/$JOB_ID/verification-kit.zip" \
  -o "$OUTPUT_FILE"
test -s "$OUTPUT_FILE"
python3 -c \
  'import sys,zipfile; z=zipfile.ZipFile(sys.argv[1]); n=set(z.namelist()); assert "verification-kit/challenge/manifest.json" in n; assert "verification-kit/submission/submission.json" in n; assert not any(x.endswith("package.zip") for x in n); print(len(n), "entries")' \
  "$OUTPUT_FILE"
sha256sum "$OUTPUT_FILE"
```

Do not delete the local ZIP unless the user requested temporary output.
Server-side artifacts are cached for seven days. For a succeeded job with
`artifact_status: expired`, call `POST /v1/jobs/{old_job_id}/rebuild`, poll the
new `job_id`, and download it. Exact historical inputs and generator version
are used; the latest generator is never silently substituted.

## Natural-language behavior examples

### ECG stress request

Good behavior:

1. Clarify total versus per-case duration and which arrhythmias matter. If
   defaults are authorized, disclose three 60-second cases and representative
   PVC/PAC, PSVT, and AFIB coverage.
2. Choose the locally scored R-peak, beat-classification, signal-quality, and
   morphology targets that match the requested algorithm surface.
3. Fetch live catalogs, distribute compatible artifacts across coherent cases,
   set 500 Hz and distinct seeds, then preview all cases.
4. Surface any core compatibility warning without calling it a generation
   error.
5. Save, compose, generate, download, and report the case plan and ZIP path.

Bad behavior includes inventing artifact names, forcing every rhythm into one
case, interpreting three minutes per case without disclosure, or continuing
after incompatible preview.

### HRV quick request

Good behavior:

1. Clarify whether LF/HF inputs mean centers, bandwidths, or powers when units
   are missing.
2. Explain if the live schema cannot express a requested quantity directly.
3. Start from `ecg_hrv_benchmark`, use at least 300 seconds, enable HRV, and use
   `hrv` plus `r_peak` targets.
4. Set exact supported fields, preview, and generate one compact case.

“Quick” means a one-case workflow; it cannot shorten an HRV-scored scenario
below the core's five-minute requirement.

## Final response to the human

Report concisely:

- interpretations and defaults;
- pack ID and job ID;
- case count, duration semantics, sample rate, targets, major artifacts, and
  conditions;
- locally scored targets and any explicit compatibility/reference warning;
- downloaded path and SHA-256;
- preview warnings or a precise failure.

Example:

```text
Created and downloaded the ECG stress package.

- Interpretation: 3 minutes total, split into three 60-second cases at 500 Hz
- Local scoring: R-peak, ECG beat classification, signal quality, morphology assertions
- Pack: custom_pack_...
- Job: job_...
- File: ./custom_pack_...-job_...-verification-kit.zip
- SHA-256: ...

I used representative PVC/PAC, PSVT, and AFIB cases and distributed baseline,
powerline, EMG, dropout, saturation, and drift artifacts across them.
```

## Endpoint checklist

| Method | Path | Use |
|---|---|---|
| `GET` | `/openapi.yaml` | Complete live HTTP API contract |
| `GET` | `/healthz`, `/readyz` | Service checks |
| `GET` | `/v1/projects` | Choose a project |
| `GET` | `/v1/packs` | Inspect ready-made curated packs |
| `GET` | `/v1/authoring/schema` | Current fields, catalogs, ranges, targets |
| `GET` | `/v1/authoring/templates` | Complete valid starting scenarios |
| `POST` | `/v1/authoring/preview` | Authoritative scenario/target preflight |
| `POST` | `/v1/scenarios` | Save draft and target intent |
| `PUT` | `/v1/scenarios/{id}` | Fix/replace a draft |
| `POST` | `/v1/custom-packs` | Snapshot compatible drafts into a pack |
| `POST` | `/v1/jobs` | Queue generation |
| `GET` | `/v1/jobs/{id}` | Poll status |
| `POST` | `/v1/jobs/{id}/rebuild` | Recreate an expired artifact exactly |
| `GET` | `/v1/jobs/{id}/verification-kit.zip` | Preferred flat ZIP download |

Treat the live `/openapi.yaml` response as the primary complete HTTP contract.
This repository's `doc/openapi.yaml` is its build-time source.
