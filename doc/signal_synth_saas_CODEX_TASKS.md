# Codex Task Plan for signal_synth_saas

Use these as larger Codex work packages. Each task should become a GitHub issue or equivalent task before implementation. Keep the task URL as the first token in the first line of every commit message.

## Task 1 — Repository scaffold and project rules

Create the initial `signal_synth_saas` repository scaffold.

Scope:

- add `.gitignore`;
- add initial `CMakeLists.txt`;
- add placeholder directories: `apache/`, `include/`, `src/`, `packs/`, `test/`, `scripts/`;
- document that `../signal_synth` is used directly and is not a submodule.

Acceptance criteria:

- repository configures with CMake;
- no generated build artifacts are tracked;
- task-first workflow is documented.

Suggested Codex prompt:

```text
Implement the initial signal_synth_saas repository scaffold. Use ../signal_synth as a sibling dependency, not a submodule. Add README.md and WOW.md matching the project boundary and route prefix /syn_sig_ra. Add a minimal CMake project and placeholder source/test directories. Do not implement runtime behavior yet.
```

## Task 2 — Apache module skeleton and health route

Create a loadable Apache 2 module named `mod_syn_sig_ra`.

Scope:

- implement Apache module registration;
- route only requests under `/syn_sig_ra`;
- add `GET /syn_sig_ra/healthz`;
- return JSON with service name, status, and build metadata;
- add example Apache config.

Acceptance criteria:

- module builds as a shared object;
- Apache can load it;
- `GET /syn_sig_ra/healthz` returns HTTP 200 JSON;
- non-owned paths are declined by the module.

Suggested Codex prompt:

```text
Implement a minimal Apache 2 module mod_syn_sig_ra with a /syn_sig_ra/healthz JSON endpoint. Add build support using Apache/APR headers and an example apache/syn_sig_ra.conf.example. Ensure the handler declines paths outside /syn_sig_ra. Include unit or smoke tests where practical.
```

## Task 3 — Configuration directives and local data layout

Add runtime configuration for the module.

Scope:

- define config directives for data root, signal-synth CLI path, pack root, and public base path;
- validate configured paths at startup or first use;
- create a documented local data layout under `/var/lib/syn_sig_ra`;
- add safe defaults for development.

Acceptance criteria:

- config values are accessible from request handlers;
- invalid required config produces a clear server-side error;
- README deployment section matches implemented directive names.

Suggested Codex prompt:

```text
Add Apache configuration directives for SynSigRaDataRoot, SynSigRaSignalSynthCli, SynSigRaPackRoot, and SynSigRaPublicBasePath. Wire them into the module config and update the example Apache config and README. Do not implement jobs yet.
```

## Task 4 — API-key authentication and metadata schema

Add minimal authenticated SaaS identity.

Scope:

- implement `Authorization: Bearer <api-key>` parsing;
- store API key hashes, organization/user IDs, jobs, packages, and audit events in SQLite;
- add migration/init logic for local development;
- keep `/healthz` public, protect v1 job/artifact routes.

Acceptance criteria:

- missing or invalid API key returns 401;
- valid key identifies an owner scope;
- schema creation is deterministic;
- no plaintext API keys are stored.

Suggested Codex prompt:

```text
Implement API-key authentication and a minimal SQLite metadata store for signal_synth_saas. Protect /syn_sig_ra/v1 job and artifact routes, keep /syn_sig_ra/healthz public, and store only API key hashes. Add schema initialization and tests for auth success/failure.
```

## Task 5 — Pack catalog API

Expose local built-in packs.

Scope:

- read pack definitions from configured `packs/` root;
- reject path traversal;
- expose `GET /syn_sig_ra/v1/packs`;
- expose `GET /syn_sig_ra/v1/packs/{pack_id}`;
- include pack ID, display name, description, and fingerprint if available.

Acceptance criteria:

- valid pack IDs resolve to files under pack root;
- invalid IDs return 404 or 400 safely;
- no absolute paths are accepted from clients.

Suggested Codex prompt:

```text
Implement the v1 pack catalog backed by the configured local packs directory. Add GET /syn_sig_ra/v1/packs and GET /syn_sig_ra/v1/packs/{pack_id}. Validate pack IDs strictly and prevent path traversal. Add tests.
```

## Task 6 — Job creation and status API

Add the core asynchronous API contract.

Scope:

- implement `POST /syn_sig_ra/v1/jobs` accepting a known `pack_id`;
- create job rows with `queued` status;
- implement `GET /syn_sig_ra/v1/jobs/{job_id}`;
- scope all job access to the authenticated owner;
- define stable JSON response shapes.

Acceptance criteria:

- job creation returns a job ID and queued status;
- status reads return queued/running/succeeded/failed;
- users cannot read jobs owned by another owner;
- unsupported request fields are rejected or ignored according to documented policy.

Suggested Codex prompt:

```text
Implement job creation and status APIs for challenge package generation. POST /syn_sig_ra/v1/jobs should accept a built-in pack_id, create a queued job in SQLite, and return JSON. GET /syn_sig_ra/v1/jobs/{job_id} should return scoped status. Do not run signal-synth yet.
```

## Task 7 — Worker wrapper around signal-synth CLI

Implement the first real generator integration.

Scope:

- add a worker function or worker binary that claims queued jobs;
- invoke `../signal_synth` build output via configured `signal-synth` path;
- run `signal-synth pack challenge <pack.json> --out <new-directory>`;
- ensure output directory does not already exist;
- parse successful stdout;
- map stable CLI errors from stderr;
- store package fingerprint and generator metadata.

Acceptance criteria:

- a queued job can transition to running then succeeded;
- CLI failure transitions job to failed with a stable error object;
- stdout parser validates required fields;
- no request handler blocks on long render work.

Suggested Codex prompt:

```text
Implement a worker wrapper for queued jobs. It must invoke the configured signal-synth CLI with `pack challenge <pack.json> --out <new-directory>`, using a newly allocated output directory. Parse the documented machine-readable stdout fields and persist package metadata. Persist stable error codes on failure. Keep the HTTP request path asynchronous.
```

## Task 8 — Immutable artifact store and downloads

Persist generated packages as immutable objects.

Scope:

- copy or move successful output to local package storage;
- create `package.zip` or another documented archive format;
- preserve manifest paths inside the archive;
- expose manifest and archive download endpoints;
- enforce owner authorization.

Acceptance criteria:

- succeeded jobs return manifest and archive URLs;
- manifest download returns the generated `manifest.json`;
- archive download returns the full package;
- package content is not modified after success.

Suggested Codex prompt:

```text
Implement local immutable artifact storage for succeeded jobs. Store manifest.json and a package archive under the configured data root. Add authenticated download endpoints under /syn_sig_ra/v1/artifacts/{package_id}/manifest.json and /syn_sig_ra/v1/artifacts/{package_id}/package.zip. Preserve generated manifest-relative paths inside the archive.
```

## Task 9 — End-to-end integration test

Add a smoke test that proves the service contract.

Scope:

- build or locate `../signal_synth` CLI;
- start Apache with the module in a test configuration, or test the module through an integration harness;
- create a job from an example pack;
- run the worker until completion;
- download manifest and package archive;
- verify package layout and fingerprints where practical.

Acceptance criteria:

- one command runs the integration test locally;
- failure output is actionable;
- test does not require production paths.

Suggested Codex prompt:

```text
Add an end-to-end integration test for signal_synth_saas. It should create a challenge job from a known pack, execute the worker, wait for success, download manifest.json and the package archive, and verify the expected package structure. Use ../signal_synth as the sibling generator repository.
```

## Task 10 — VPS deployment documentation

Document deployment on the Apache VPS.

Scope:

- package build prerequisites;
- Apache module install command;
- example config;
- data directory permissions;
- worker execution mode;
- log locations;
- rollback procedure.

Acceptance criteria:

- a fresh VPS can follow the document to deploy the module;
- secrets are not committed;
- deployment keeps all routes under `/syn_sig_ra/...`.

Suggested Codex prompt:

```text
Write VPS deployment documentation for signal_synth_saas as an Apache 2 module. Include build dependencies, module installation, Apache configuration, data directory permissions, worker execution, log inspection, and rollback. Ensure all public routes remain under /syn_sig_ra.
```
