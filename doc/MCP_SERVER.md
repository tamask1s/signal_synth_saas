# Synsigra MCP server

Synsigra exposes a hosted Model Context Protocol server for AI-assisted
algorithm QA:

```text
https://www.timeonion.com/syn_sig_ra/mcp
```

It uses stateless Streamable HTTP. One personal API key is the only credential:

```http
Authorization: Bearer YOUR_API_KEY
```

The key identifies the user, organization, and role. Browser cookies, email,
password, and a separate MCP token are neither needed nor accepted at this
endpoint. Create the key on the Account page and store it outside source
control. Never put it in a URL or prompt.

## Architecture

The MCP implementation is deliberately an adapter, not a second product
backend:

```text
MCP client
  -> Streamable HTTP + JSON-RPC validation
  -> goal/target interpretation and pack ranking
  -> existing authenticated REST application routes
  -> existing metadata store, pack catalog, queue, worker and signal_synth core
```

All modifying tools delegate to the same REST routes used by the UI and API.
Consequently roles, per-key request limits, organization job quotas, storage
guards, core integration checks, immutable recipes, audit behavior, job state
rules, and error bodies remain identical. The MCP layer does not generate a
signal, write a job directly, or maintain a second catalog.

The server is stateless and returns one JSON response per POST. It deliberately
returns `405` for GET because it does not need an independent SSE stream or
server-initiated requests. It negotiates MCP `2025-11-25`, `2025-06-18`, and
`2025-03-26`. Every POST must advertise both `application/json` and
`text/event-stream` in `Accept`.

If an `Origin` header is present, it must match the configured public service
origin. This prevents DNS-rebinding/cross-origin use. Every connection requires
a personal Bearer API key. The server has no access to a user's proprietary
algorithm binary or completed output unless the user independently chooses to
send it somewhere; the documented workflow keeps both local.

## Tools

The server currently exposes:

- `synsigra_recommend_packs`: infer or accept explicit verification targets,
  rank curated packs, show scoreable/reference-only/missing coverage, check
  requested duration and sampling rate, and select curated versus custom flow;
- `synsigra_list_packs` and `synsigra_get_pack`: discover and inspect the live
  immutable curated catalog;
- `synsigra_list_projects`: obtain a real project ID instead of guessing one;
- `synsigra_create_job`, `synsigra_get_job`, and `synsigra_list_jobs`: queue and
  monitor normal generation;
- `synsigra_rebuild_expired_job`: recreate an expired artifact from its
  preserved recipe and exact historical generator;
- `synsigra_get_authoring_contract`, `synsigra_get_curated_scenario`,
  `synsigra_preview_scenario`, `synsigra_save_scenario`, and
  `synsigra_create_custom_pack`: fulfill exact duration, sampling, physiology,
  artifact, or target requirements through the live core-owned contract;
- `synsigra_get_verification_guide`: return job status, authenticated download
  destinations, local submission steps, the package-authoritative verifier
  mode and exact command, report entry points, exit codes, audit archive
  checklist, evidence boundary, and expired-artifact action.

Creating jobs/rebuilds, saving scenarios, and composing custom packs are marked
non-read-only and non-idempotent. MCP hosts can therefore keep a human approval
step around quota-consuming or persistent changes. Read tools are marked
read-only and idempotent.

Two prompt templates are also published:

- `design_algorithm_qa_pack(goal)`;
- `verify_algorithm_outputs(job_id)`.

## Expected model workflow

For a request such as “prove that my peak detector and RR/HR/HRV calculations
are correct under noise”:

1. Call `synsigra_recommend_packs` with the complete goal and explicit targets
   when they are known.
2. Explain which requested outputs are locally scoreable, reference-only, or
   missing. Never present reference-only ground truth as a passed score.
3. If the best curated pack satisfies the requirements, inspect it with
   `synsigra_get_pack`. Otherwise fetch the live authoring contract, clone the
   closest complete scenario, change only requested fields, and preview every
   scenario against the final target list.
4. Ask for human approval of the selected pack and project.
5. Call `synsigra_create_job` and poll `synsigra_get_job` with backoff until a
   terminal state.
6. Call `synsigra_get_verification_guide`.
7. Download the kit over HTTPS with the same Bearer header. Run the proprietary
   algorithm locally against `challenge/`, populate the declared files under
   `submission/`, and run `synsigra-verify` locally.

For a succeeded job, the guide is intentionally compact: it returns a
`job_summary` and small `challenge` summary instead of repeating the full job
and submission schemas. Use `synsigra_get_job` when those details are needed.
The guide supplies direct authenticated URLs for the single verification kit
and the matching generator-free Python wheel. Its `verification_command` is
authoritative: diagnostic packages include `--mode diagnostic`; evidence
packages do not. Run it from the extracted kit directory, then open
`verification-results/index.html`. The canonical machine-readable result is
`verification-results/evidence.json`; supporting pages and payloads are under
`verification-results/details/`.

Do not submit PHI, patient signals, identifiers, clinical notes, or diagnostic
claims. Synsigra provides synthetic engineering QA and does not establish
clinical, regulatory, or production fitness.

## Minimal protocol probe

```sh
BASE=https://www.timeonion.com/syn_sig_ra
curl -fsS "$BASE/mcp" \
  -H "Authorization: Bearer $SYNSIGRA_API_KEY" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  --data-binary '{
    "jsonrpc":"2.0",
    "id":1,
    "method":"initialize",
    "params":{
      "protocolVersion":"2025-11-25",
      "capabilities":{},
      "clientInfo":{"name":"my-client","version":"1.0"}
    }
  }'
```

After initialization, send `notifications/initialized`, then `tools/list` or a
tool call. Include `MCP-Protocol-Version: 2025-11-25` on subsequent requests.

The human setup page is
<https://www.timeonion.com/syn_sig_ra/mcp-setup>. The low-level REST contract
remains available at <https://www.timeonion.com/syn_sig_ra/openapi.yaml>.
