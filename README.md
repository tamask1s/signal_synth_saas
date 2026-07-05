# signal_synth_saas

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

Returns available built-in challenge packs.

### Create challenge job

```http
POST /syn_sig_ra/v1/jobs
Authorization: Bearer <api-key>
Content-Type: application/json

{
  "pack_id": "example_pack",
  "export_formats": ["wfdb", "edf", "bdf"],
  "report_format": "html"
}
```

Initial response:

```json
{
  "job_id": "job_...",
  "status": "queued"
}
```

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
```

Succeeded jobs return package metadata, manifest URL, archive URL, package fingerprint, generator version, and scenario fingerprints where available.

Failed jobs return a stable error code and safe message.

### Download artifacts

```http
GET /syn_sig_ra/v1/artifacts/{package_id}/manifest.json
GET /syn_sig_ra/v1/artifacts/{package_id}/package.zip
```

Artifacts are immutable and scoped to the authenticated organization/user.

## Local development

The Apache module build requires CMake, a C++11 compiler, Apache development
headers, APR development headers, and `apxs`. On Debian/Ubuntu these are
provided by packages such as `cmake`, `g++`, `apache2-dev`, and `libapr1-dev`.

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
See `var.example/README.md` for directory creation and permission guidance.

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
