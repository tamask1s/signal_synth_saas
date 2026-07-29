# Synsigra SaaS developer reference

This document describes the current implementation and the shortest safe
development workflows. Historical handoffs and issue plans are not runtime
contracts.

## Sources of truth

Use these in order:

1. `../signal_synth/signal-synth contract` — generator and verifier contracts.
2. `../signal_synth/examples/catalog/curated_pack_metadata_v1.json` — exported
   curated release set.
3. `CMakeLists.txt` — exact accepted core commit.
4. `packs/curated_pack_metadata_v1.catalog` — exact imported release snapshot.
5. `doc/openapi.yaml` — customer HTTP contract.
6. The tests and live readiness response — executable acceptance evidence.

Do not copy a core commit or catalog hash into a new hand-written registry.
The adoption workflow updates every intentional pin and rejects stale ones.

## Product and trust boundary

Synsigra is synthetic biosignal engineering QA software. It is not diagnostic
software, clinical evidence, certification, or a PHI/patient-data service.

The server:

- authenticates users and API clients;
- stores organizations, projects, authoring drafts, jobs and immutable
  generation provenance;
- invokes the trusted sibling `signal-synth` CLI in a worker;
- serves generated challenge files, the bounded signal viewer and verifier
  downloads.

The server does not receive or execute a customer's proprietary algorithm or
completed submission. Verification happens in the customer's environment with
the generator-free `synsigra` wheel.

## Runtime architecture

```text
nginx :80/:443
  ├── static marketing site
  ├── /tamaskis
  └── /syn_sig_ra/*
        └── Apache 2.2 + mod_syn_sig_ra
              ├── browser UI and sessions
              ├── HTTP API and Streamable HTTP MCP
              ├── SQLite metadata
              ├── immutable artifact delivery
              └── systemd worker
                    └── pinned signal-synth CLI
```

The HTTP module never performs generation inside a request. It validates and
queues work; the worker claims jobs and launches the exact pinned generator.

Main code ownership:

| Area | Location |
|---|---|
| HTTP routing, UI shell and API serialization | `src/api/route.cpp` |
| Pack release parsing and public summaries | `src/catalog/pack_catalog.cpp` |
| Worker and challenge assembly | `src/worker/` |
| SQLite persistence | `src/storage/metadata_store.cpp` |
| Authentication and accounts | `src/auth/`, `src/email/` |
| Artifact streaming/rehydration | `src/artifact/` |
| Signal-window API | `src/viewer/`, `web/viewer/` |
| MCP tools | `src/api/mcp_server.cpp` |
| Release and operations | `scripts/`, `ops/` |
| Authoritative generation/scoring | `../signal_synth` |

## Curated release invariants

A curated release is one immutable unit:

- one full core commit;
- one catalog version and canonical source hash;
- 22 unique pack definitions and 142 unique-in-pack cases;
- exact scenario and approved noise-asset bytes;
- the verifier version and complete integration-contract tuple;
- for each evidence pack, exactly Level 1, Level 2 and Level 3 protocols.

Level 2 is the reviewed default. Level 1/3 alter only acceptance limits. They
must preserve signals, case-target matrix, truth rules, scoring contract and
verdict scope. Minimum gates must be monotonic
`Level 1 <= Level 2 <= Level 3`; maximum-error gates must be monotonic in the
opposite direction. Every complete protocol is selected before generation and
locked by SHA-256.

The old catalog discovery fields named
`recommended_profile`/`supported_threshold_profiles` are intentionally absent.
The verifier's `--mode diagnostic --profile ...` option remains a local expert
control, but it is not package-authoritative evidence.

## One-command checks

Run commands from `signal_synth_saas`.

Fast, read-only cross-repository consistency check:

```sh
scripts/task1_tipusp_dolgok.py audit
```

It checks core/SaaS pins, catalog identity, every imported pack/scenario/noise
file, all evidence-profile hashes and monotonic gates, and Python/shell/browser
script syntax.

Complete pack audit:

```sh
scripts/task1_tipusp_dolgok.py audit-full
```

It performs the fast checks, validates every imported pack with the pinned
CLI, renders all 22 core packs, then audits observable truth, measurement
statuses, notices and internal report links.

Normal SaaS quality gate:

```sh
scripts/task1_tipusp_dolgok.py quality
```

This builds release-mode binaries, runs unit/brand/module tests and the full
isolated Apache integration flow. `scripts/build_release.sh` always runs the
fast cross-repository audit first.

Full core gate:

```sh
scripts/task1_tipusp_dolgok.py core-quality
```

The core repository also provides a much faster default targeted cycle:

```sh
scripts/task2_release_dolgok.py core-refresh
```

Use the full gate before a core commit; use the targeted gate while iterating.

## Changing curated packs

1. Edit generator, scenario, pack or Level 2 protocol sources in
   `../signal_synth`.
2. Regenerate owned evidence sources when applicable:

   ```sh
   scripts/task1_tipusp_dolgok.py core-generate
   ```

3. Iterate with `scripts/task2_release_dolgok.py core-refresh`.
4. Finish with `scripts/task1_tipusp_dolgok.py core-quality`.
5. Commit and push the clean core:

   ```sh
   scripts/task2_release_dolgok.py commit-core --message "..."
   scripts/task2_release_dolgok.py push-core
   ```

6. From a clean SaaS worktree, import the pushed core:

   ```sh
   scripts/task2_release_dolgok.py adopt-core
   ```

The adoption is transactional. It builds the core at its exact HEAD, validates
the embedded identity, imports into staging, updates all tracked pins, and only
then replaces `packs/`. It restores the original checkout on failure.

For a fully automated already-reviewed release, `promote-core` combines
adoption, checks, commit, push and deployment. Do not use it while unrelated
local changes exist.

## Changing the SaaS

After editing:

```sh
scripts/task1_tipusp_dolgok.py audit
scripts/task1_tipusp_dolgok.py quality
scripts/commit_checked.sh - "Message" path/to/file ...
scripts/task2_release_dolgok.py push
scripts/task2_release_dolgok.py deploy
scripts/task1_tipusp_dolgok.py live-verify
```

The commit wrapper runs the release build before staging only the listed
files. The deploy wrapper builds an immutable release archive and keeps a
rollback snapshot. Push is the only inherently network-mutating step.

## Job and artifact lifecycle

1. The API validates the project, pack/custom snapshot, evidence profile,
   quota and disk reserve.
2. SQLite records an immutable queued request and selected catalog/profile
   identity.
3. The worker claims the job and creates a fresh private work directory.
4. `signal-synth pack challenge` renders the challenge.
5. The worker verifies the complete receipt, core contract, manifest,
   fingerprints, selected protocol SHA and package integrity.
6. Files are atomically published and the job becomes `succeeded`.

Customer-facing normal download is one flat `verification-kit.zip`. Generated
artifacts are cached for seven days. After expiry, an authorized download
causes exact-version rehydration; the preserved generator release and immutable
request make that transparent to the user.

Queued jobs can be cancelled. Failed/cancelled jobs can be retried. Running
process termination, deadlines and progress phases remain tracked in GitHub
issue #65.

The current pre-beta database schema is intentionally a clean baseline rather
than a migration chain. A destructive reset is allowed only through
`scripts/reset_prebeta_state.sh --confirm-destroy-prebeta-state`; it recreates
the schema, bootstrap identity and smoke evidence, then verifies the live
release.

## Security rules

- Never accept a user filesystem path for a curated pack or artifact.
- Reject symlinks, traversal, absolute paths and unknown catalog fields.
- Keep the generator, approved noise assets and verifier wheel read-only to the
  worker.
- Keep API keys, session tokens, password hashes and SMTP credentials out of
  logs, command arguments, archives and account exports.
- Browser writes require the signed session and CSRF protections; API/MCP
  clients use one bearer API key.
- Enforce organization ownership on every project, job and artifact lookup.
- Do not weaken package-authored evidence after observing results.
- Do not send external-noise source bytes when redistribution is disallowed.

See `doc/SECURITY_BASELINE.md`, `doc/SECRET_ROTATION.md` and
`doc/PRIVACY_NO_PHI_NOTICE.md` for operational details.

## Test layers

| Layer | Purpose |
|---|---|
| Core C++/Python tests | Generation, truth, scoring, contracts and reports |
| `audit` | Cross-repo identity and all-pack static invariants |
| SaaS unit tests | Routing, auth, catalog, worker, storage and viewer |
| Brand/JS/module tests | User-visible naming, browser syntax and Apache ABI |
| Apache E2E | Real HTTP, sessions, jobs, worker, artifacts, verifier and MCP |
| `audit-full` | Every curated pack rendered and truth-audited |
| `stress_live_packs.py` | Optional all-pack production orchestration test |
| `live-verify` | Deployed release, services and public endpoints |

The optional live stress script soft-deletes its jobs by default, but immutable
files remain until retention cleanup. Prefer `audit-full` for routine
all-pack work and reserve live stress for worker/storage/runtime changes.

## Documentation ownership

- User manual: `README.md`
- Customer API: `doc/API_GUIDE.md`, `doc/openapi.yaml`
- Pack behavior: `doc/PACK_CATALOG.md`
- Operations/deploy: `doc/OPERATIONS.md`, `doc/VPS_DEPLOYMENT.md`
- MCP clients: `doc/MCP_SERVER.md`, `doc/CODEX_API_CLIENT_GUIDE.md`
- Current core handoff:
  `../signal_synth/doc/synsigra_architecture_docs/saas/001_SAAS_IMPLEMENTATION_HANDOFF.md`

When a contract changes, update the executable contract and tests first,
regenerate the catalog, then update only these current documents. Dated handoff
files are historical and must identify themselves as such.
