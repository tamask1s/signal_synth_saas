# signal_synth_saas — developer reference

`signal_synth_saas` is the first hosted orchestration layer for Synsigra / `signal_synth` challenge-package generation.

The service is intentionally thin. The authoritative signal generation remains in the sibling `signal_synth` repository, and the SaaS layer invokes the existing CLI worker contract instead of reimplementing generation logic.

```text
../signal_synth       # existing C++ generator repository
./signal_synth_saas   # Apache 2 module and SaaS orchestration repository
```

For the first implementation, `signal_synth_saas` uses `../signal_synth` directly. Do not add `signal_synth` as a submodule or vendored dependency yet.

## Product boundary

Synsigra SaaS is a B2B / developer-tool platform for synthetic biosignal ground-truth QA packages.

The initial product flow is:

1. A user selects or requests a scenario pack.
2. The SaaS service generates a challenge package.
3. The user downloads the package or retrieves its artifacts through the API.
4. The user runs their own algorithm locally.
5. The user scores locally with the Synsigra Python package against packaged ground truth.

The first SaaS increment must not implement:

- medical diagnostic workflow;
- patient record or PHI storage;
- clinical validation or certification claims;
- server-side user-algorithm upload or execution;
- generic ECG datastore behavior.

## Deployment shape

The SaaS HTTP API is implemented as an Apache 2 module.

Default URL prefix:

```text
/syn_sig_ra/...
```

Recommended v1 API namespace:

```text
/syn_sig_ra/v1/...
```

The Apache module is the HTTP entry point. Render work should be isolated from request handling. The first implementation may use a simple local worker loop and filesystem-backed queue; it should still preserve the API/job boundary so the storage and worker implementations can later be replaced.

## Initial architecture

```text
Apache 2
  └── mod_syn_sig_ra
        ├── API routing under /syn_sig_ra
        ├── API-key authentication
        ├── job creation and status reads
        ├── local metadata store
        ├── local immutable artifact store
        └── worker wrapper
              └── ../signal_synth build output: signal-synth
```

Recommended repository layout:

```text
signal_synth_saas/
  README.md
  WOW.md
  CMakeLists.txt
  apache/
    syn_sig_ra.conf.example
  include/
    syn_sig_ra/
  src/
    mod_syn_sig_ra.cpp
    api/
    auth/
    config/
    job/
    storage/
    worker/
  packs/
    example_pack.json
  test/
    integration/
    unit/
  scripts/
    dev_build_signal_synth.sh
    dev_run_worker_once.sh
  var.example/
    README.md
```

## signal_synth integration contract

The first worker boundary is the `signal-synth` CLI command:

```sh
signal-synth pack challenge <pack.json> --out <new-directory>
```

The output directory must not already exist.

Successful stdout is expected to be line-oriented and machine-readable:

```text
status=challenge-rendered
output_directory=<path>
package_id=<pack-id>
scenario_count=<count>
pack_fingerprint=sha256:<64-hex>
package_fingerprint=sha256:<64-hex>
```

Failure behavior:

- non-zero exit code;
- empty stdout;
- stderr begins with `error=<stable-code>`.

The SaaS layer must capture at least:

- request JSON;
- selected pack ID or source pack path;
- pack fingerprint;
- package fingerprint;
- generator version;
- generator git commit or build identity;
- normalized CLI command;
- manifest hash;
- artifact storage key;
- creation and completion timestamps;
- stable error object on failure.

## API v1 draft

All endpoints are rooted under `/syn_sig_ra`.

### Health

```http
GET /syn_sig_ra/healthz
```

Returns HTTP 200 with service status and build metadata:

```json
{
  "service": "signal_synth_saas",
  "status": "ok",
  "build": {
    "version": "0.1.0",
    "git_commit": "<build-commit>"
  }
}
```

Other methods on this route return HTTP 405. Unknown paths below
`/syn_sig_ra` return HTTP 404, while paths outside that prefix are declined so
another Apache handler can process them.

### Pack catalog

```http
GET /syn_sig_ra/v1/packs
GET /syn_sig_ra/v1/packs/{pack_id}
```

Returns available built-in challenge packs without authentication. Catalog
entries are regular `.json` files below `SynSigRaPackRoot`; symlinks, absolute
paths, traversal sequences, unsafe IDs, malformed packs, and filename/pack-ID
mismatches are rejected. Pack metadata and fingerprints are parsed by the
authoritative pack implementation compiled directly from the sibling
`../signal_synth` checkout. Responses include pack version, targets, scenario
count, and scenario IDs/targets, but never expose internal scenario file paths.

### Create challenge job

```http
GET /syn_sig_ra/v1/jobs
DELETE /syn_sig_ra/v1/jobs/{job_id}
POST /syn_sig_ra/v1/jobs
Authorization: Bearer <api-key>
Content-Type: application/json

{
  "project_id": "org_example_default",
  "pack_id": "example_pack"
}
```

Initial response:

```json
{
  "job_id": "job_...",
  "status": "queued"
}
```

`GET /syn_sig_ra/v1/jobs?limit=25&offset=0` returns the authenticated
organization's most recent jobs. `limit` is bounded to 1-100 and responses
include `next_offset` when another page may exist:

```json
{
  "jobs": [
    {
      "job_id": "job_...",
      "status": "queued",
      "pack_id": "r_peak_stress_v1",
      "created_at": "..."
    }
  ],
  "limit": 25,
  "offset": 0,
  "count": 1
}
```

`DELETE /syn_sig_ra/v1/jobs/{job_id}` soft-deletes a non-running job from the
authenticated user's job list. Succeeded job artifacts become inaccessible
through the API after deletion; the physical immutable files are left in local
storage for a later retention/cleanup process. Running jobs currently return
HTTP 409.

`POST /syn_sig_ra/v1/jobs/{job_id}/cancel` cancels queued jobs. Running jobs
return HTTP 409 because terminating the external generator process is not yet
considered safe. `POST /syn_sig_ra/v1/jobs/{job_id}/retry` creates a new queued
job from a failed or cancelled job, preserving the original record.

The current pre-beta retention state is explicit: soft-deleted records and
immutable files remain until the cleanup policy in task #18 is implemented.

### Usage and quota policy

`GET /syn_sig_ra/v1/usage` returns the authenticated key's requests in the
last minute plus organization-wide active jobs, monthly jobs, monthly package
count, actual immutable package bytes, and configured limits. Current private
beta limits are 120 requests/minute per key, 2 concurrent queued/running jobs
per organization, and 100 jobs/month per organization. Rejections return HTTP
429 with stable `request_rate_limit`, `concurrent_job_limit`, or
`monthly_job_limit` codes. Every rejected quota decision is persisted in
`quota_decisions` for operator inspection.

Implemented request policy:

- `pack_id` is required and must name a catalog pack;
- `project_id` is required and must name a project in the caller's organization;
- challenge jobs produce the service's complete fixed export/report set;
- duplicate or unsupported JSON fields are rejected;
- accepted requests return HTTP 202 and are persisted as owner-scoped
  `queued` jobs.

### Read job status

```http
GET /syn_sig_ra/v1/jobs/{job_id}
Authorization: Bearer <api-key>
```

Statuses:

```text
queued
running
succeeded
failed
cancelled
```

Succeeded jobs return package metadata, manifest URL, archive URL, package fingerprint, generator version, and scenario fingerprints where available.

Failed jobs return a stable error code and safe message.
Job IDs are cryptographically random, and status reads deliberately return
HTTP 404 when the job does not belong to the authenticated organization/user.

## Local worker

Render work runs outside Apache request handling. Process one queued job:

```sh
build/syn_sig_ra_worker run-once \
  build/var/db.sqlite3 \
  ../signal_synth/build/signal-synth \
  packs \
  build/var
```

The worker atomically claims the oldest queued job, invokes
`signal-synth pack challenge` without a shell, captures bounded stdout/stderr,
validates every required machine-output field, records a SHA-256 generator
build identity, and transitions the job to `succeeded` or `failed`. Stable CLI
error codes are persisted; raw stderr is not returned through the API.
For a long-running deployment, use the same arguments with `run-loop`; it
polls for jobs and handles `SIGTERM`/`SIGINT` for service shutdown.

### Download artifacts

```http
GET /syn_sig_ra/v1/artifacts/{package_id}/manifest.json
GET /syn_sig_ra/v1/artifacts/{package_id}/package.zip
```

Artifacts are immutable and scoped to the authenticated organization/user.
Successful workers allocate a random `pkg_...` ID and store:

```text
<SynSigRaDataRoot>/packages/<package_id>/
  manifest.json
  package.zip
  extracted/
```

The generated tree is archived with manifest-relative paths intact, then all
package files and directories are made read-only. Downloads are streamed by
Apache after metadata owner authorization; another owner receives HTTP 404.

## Web UI

The module serves a minimal browser UI directly from the Apache module:

```text
/syn_sig_ra
/syn_sig_ra/ui
```

The UI is static HTML/CSS/JavaScript backed by secure server-side sessions.
Users register or sign in with e-mail/password; Bearer API keys are reserved
for scripts and CI and are managed from the account panel. The UI supports
service health, pack browsing, job creation,
recent job status, authenticated manifest/ZIP downloads, and job deletion.
It polls the job list in the background, but only re-renders the list when the
job payload actually changes.

## Local development

The Apache module build requires CMake, a C++11 compiler, Apache development
headers, APR development headers, SQLite, OpenSSL, and `apxs`. On Debian/Ubuntu
these are provided by packages such as `cmake`, `g++`, `apache2-dev`,
`libapr1-dev`, `libsqlite3-dev`, and `libssl-dev`.

Expected sibling checkout:

```sh
cd ..
git clone git@github.com:tamask1s/signal_synth.git
mkdir -p signal_synth_saas
cd signal_synth_saas
```

Build the generator CLI:

```sh
cmake -S ../signal_synth -B ../signal_synth/build -DSIGNAL_SYNTH_BUILD_TESTS=ON -DSIGNAL_SYNTH_BUILD_CLI=ON
cmake --build ../signal_synth/build
cd ../signal_synth/build
ctest --output-on-failure
cd ../../signal_synth_saas
```

Build and test the Apache module:

```sh
cmake -S . -B build -DSIGNAL_SYNTH_ROOT=../signal_synth -DBUILD_TESTING=ON
cmake --build build
cd build
ctest --output-on-failure
cd ..
```

Run the Apache-backed end-to-end smoke test:

```sh
test/integration/e2e_smoke.sh
```

The script uses a disposable database and data root under `/tmp`, creates a
temporary API key, starts Apache on `127.0.0.1`, queues
`r_peak_stress_v1`, runs one worker pass, downloads `manifest.json` and
`package.zip`, and validates the archive layout. On the VPS, or anywhere the
old Apache 2.2 runtime is the target, make the runtime explicit:

```sh
APXS_EXECUTABLE=/usr/local/apache2/bin/apxs \
APACHE_HTTPD=/usr/local/apache2/bin/httpd \
test/integration/e2e_smoke.sh
```

The same smoke test can be enabled in CTest when explicitly requested:

```sh
cmake -S . -B build/e2e \
  -DSIGNAL_SYNTH_ROOT=../signal_synth \
  -DAPXS_EXECUTABLE=/usr/local/apache2/bin/apxs \
  -DSYN_SIG_RA_ENABLE_INTEGRATION_TESTS=ON
cmake --build build/e2e
cd build/e2e
ctest --output-on-failure
cd ../..
```

The build produces `build/mod_syn_sig_ra.so`. The test suite verifies the
health routing contract, prefix ownership, and exported Apache module
registration symbol. A full load/configuration smoke test additionally
requires an installed Apache runtime.

Install locally during development:

```sh
sudo apxs -i -a -n syn_sig_ra build/mod_syn_sig_ra.so
sudo cp apache/syn_sig_ra.conf.example /etc/apache2/conf-available/syn_sig_ra.conf
sudo a2enconf syn_sig_ra
sudo systemctl reload apache2
```

Example Apache configuration:

```apache
LoadModule syn_sig_ra_module /usr/lib/apache2/modules/mod_syn_sig_ra.so

SynSigRaDataRoot /var/lib/syn_sig_ra
SynSigRaSignalSynthCli /opt/signal_synth/build/signal-synth
SynSigRaPackRoot /opt/signal_synth_saas/packs
SynSigRaPublicBasePath /syn_sig_ra

<Location "/syn_sig_ra">
    SetHandler syn_sig_ra
</Location>
```

All filesystem directive values must be absolute. Apache configuration parsing
rejects a missing or non-writable data directory, a missing/non-executable CLI,
and a missing/non-readable pack directory. `SynSigRaPublicBasePath` must be
`/syn_sig_ra` or a normalized descendant and must not end in `/`.

CMake supplies disposable development defaults: runtime state is placed below
the build directory, packs are read from this checkout, and the CLI defaults to
`../signal_synth/build/signal-synth`. Explicit Apache directive values are
validated at configuration time.

## Storage model for v1

Use local storage first:

```text
/var/lib/syn_sig_ra/
  db.sqlite3
  jobs/
  work/
  packages/
    <package_id>/
      manifest.json
      package.zip
      extracted/
```

The storage interface should be abstracted so local filesystem storage can later be replaced with object storage.
See [`var.example/README.md`](../var.example/README.md) for directory creation
and permission guidance.

For the current Apache 2.2 VPS deployment procedure, see
[`VPS_DEPLOYMENT.md`](VPS_DEPLOYMENT.md).

For the next steps toward a real user-ready SaaS, see
[`SAAS_PRODUCT_PLAN.md`](SAAS_PRODUCT_PLAN.md).

For readiness, metrics, alert thresholds, and repeatable build/deploy/verify
commands, see [`OPERATIONS.md`](OPERATIONS.md).

For immutable artifact retention, backups, and restore drills, see
[`RETENTION_BACKUP.md`](RETENTION_BACKUP.md).

For built-in pack semantic versions, compatibility, changelog, and
deprecation rules, see [`PACK_CATALOG.md`](PACK_CATALOG.md).

For the customer onboarding flow, curl examples, stable error codes, and the
OpenAPI reference, see [`API_GUIDE.md`](API_GUIDE.md) and
[`openapi.yaml`](openapi.yaml).

For user-owned editable scenario validation and CRUD, see
[`SCENARIO_DRAFTS.md`](SCENARIO_DRAFTS.md).

For immutable custom packs assembled from scenario drafts, see
[`CUSTOM_PACKS.md`](CUSTOM_PACKS.md).

## Metadata and API keys

The module initializes a versioned SQLite database at
`<SynSigRaDataRoot>/db.sqlite3`. Schema version 8 contains accounts, salted
password credentials, expiring sessions, organizations, memberships, projects,
API-key hashes, jobs, packages, and audit events. API keys must be high-entropy
secrets; only their lowercase SHA-256 hashes are persisted. This pre-beta
schema requires a clean database reset when its version changes.

API keys resolve to an organization membership with an `owner`, `admin`,
`developer`, or `viewer` role. Jobs and packages are project-scoped. See
[`TENANCY.md`](TENANCY.md) for the authorization matrix.

Initialize a development database:

```sh
build/syn_sig_ra_admin init-db build/var/db.sqlite3
```

Create a development API key without placing the plaintext secret in command
arguments:

```sh
read -r -s API_KEY
printf '%s\n' "$API_KEY" |
  build/syn_sig_ra_admin create-api-key \
    build/var/db.sqlite3 org_dev user_dev key_dev "local development" owner
unset API_KEY
```

List API keys without exposing plaintext secrets:

```sh
build/syn_sig_ra_admin list-api-keys build/var/db.sqlite3
build/syn_sig_ra_admin list-api-keys build/var/db.sqlite3 org_dev
```

Revoke an API key:

```sh
build/syn_sig_ra_admin revoke-api-key build/var/db.sqlite3 key_dev
```

`/syn_sig_ra/healthz` remains public. Routes at or below
`/syn_sig_ra/v1/projects`, `/syn_sig_ra/v1/jobs`, and
`/syn_sig_ra/v1/artifacts` require
`Authorization: Bearer <api-key>`. Missing, malformed, inactive, or unknown
keys return the same HTTP 401 response. Metadata failures return a safe HTTP
503 response while their details are written only to the Apache error log.

## Security requirements

- Require API-key auth for job creation and artifact download.
- Scope jobs and artifacts to an organization/user from day one.
- Reject path traversal and absolute pack paths from API input.
- Never expose raw worker stderr to unauthenticated users.
- Treat generated artifacts as immutable.
- Do not accept patient data or user algorithm uploads in v1.

## Test requirements

Minimum test coverage:

- Apache module loads successfully.
- `GET /syn_sig_ra/healthz` works.
- unauthenticated protected requests are rejected.
- a known pack can be queued.
- worker invokes `signal-synth pack challenge` with a non-existing output directory.
- successful stdout is parsed into package metadata.
- failed CLI execution maps to a stable API error.
- manifest and package archive can be downloaded after success.
- downloaded package remains loadable by the Synsigra Python package where available.

## First implementation milestones

1. Repository scaffold, README, WOW, and build skeleton.
2. Apache module skeleton with `/syn_sig_ra/healthz`.
3. Configuration directives and local data directory initialization.
4. API-key auth and minimal SQLite metadata store.
5. Pack catalog from local `packs/` directory.
6. Job create/status API.
7. Worker wrapper around the sibling `../signal_synth` CLI.
8. Immutable local artifact store and manifest/archive download.
9. End-to-end integration test.
10. Deployment notes for the VPS Apache setup.
