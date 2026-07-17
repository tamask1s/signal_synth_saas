# Operations and observability

## Endpoints

- `GET /syn_sig_ra/healthz` proves the Apache module can answer.
- `GET /syn_sig_ra/readyz` returns HTTP 200 only when the current metadata DB
  opens, the linked core and external CLI report the exact same supported
  producer identity, the pack catalog is non-empty, and the artifact data root
  is writable. It reports the accepted contract/identity and free disk bytes.
- `GET /syn_sig_ra/v1/metrics` is restricted to owner/admin API keys and
  exposes queue depth, running jobs, monthly failures, quota rejections,
  package usage, and the last worker heartbeat.
- `GET /syn_sig_ra/v1/usage` exposes customer-facing counters and limits.
- `GET /syn_sig_ra/v1/audit-events` gives owner/admin users a bounded,
  organization-scoped JSON or CSV security audit export.

No endpoint or log line includes API-key values, request bodies, generated
signal content, or other PHI-like payloads.

The current threat model, incident checklist, and rotation procedures are in
[`SECURITY_BASELINE.md`](SECURITY_BASELINE.md) and
[`SECRET_ROTATION.md`](SECRET_ROTATION.md).

## Structured logs

Apache emits one key-value request event at info level:

```text
event=http_request method=GET path=/syn_sig_ra/v1/jobs status=200 duration_ms=4
```

The worker writes job completion, heartbeat failure, and fatal worker events
as key-value lines to journald. Inspect them with:

```sh
sudo tail -n 200 /usr/local/apache2/logs/error_log
sudo journalctl -u syn_sig_ra_worker.service -n 200 --no-pager
```

## Alert thresholds

Alert when any condition persists for two checks:

- `/readyz` is not HTTP 200;
- worker heartbeat is more than 30 seconds old;
- queued jobs are non-zero while running jobs remain zero for 60 seconds;
- free disk is below 2 GiB or 10 percent;
- monthly failures exceed 10 percent of monthly jobs;
- quota rejections increase unexpectedly.

The current VPS has no external pager integration. `scripts/verify_live.sh`
is the canonical active check and exits non-zero on endpoint or service
failure; it can be called by cron/systemd and wired to the chosen alert
transport later.

## Repeatable workflow

```sh
scripts/build_release.sh
RUN_E2E=1 scripts/build_release.sh
scripts/build_release_artifact.sh
scripts/deploy_live.sh
scripts/verify_live.sh
scripts/rollback_live.sh
scripts/commit_checked.sh 17 "Add observability" file1 file2
git push origin master
```

`build_release_artifact.sh` is the single packaging boundary. It puts the
stripped module, worker/admin tools, pinned generator, curated packs, verifier,
landing site, and deployment configuration into an immutable
`build/releases/synsigra-*.tar.gz` plus an external SHA-256 file. Its manifest
records the exact SaaS/core commits, payload digest, architecture, Release
flags, working-tree state, and Apache module ABI.

The production VPS builds against Apache 2.2. GitHub CI deliberately builds and
executes the same source against Apache 2.4 as a portability check and uploads
that separately labelled artifact for 14 days. Apache 2.4 is not a second live
server. `deploy_release.sh` compares the artifact ABI with the running backend
before any service is stopped, so the CI artifact cannot be installed on the
2.2 VPS accidentally.

`deploy_live.sh` builds one artifact and delegates installation to
`deploy_release.sh`. Before changing runtime files, deployment captures a
checksummed snapshot under `/opt/signal_synth_saas/rollback/`. Apache and nginx
configuration checks and the complete `verify_live.sh` smoke gate must pass.
Any failure restores that snapshot automatically and reruns the live check.
Successful artifacts are registered under `/opt/signal_synth_saas/releases/`;
`current-release`, `previous-release`, and `last-rollback` are symlinks, not
mutable copies.

Explicit rollback restores the last successful pre-deploy snapshot, verifies
the live product, and retains a roll-forward snapshot:

```sh
scripts/rollback_live.sh
scripts/rollback_live.sh /opt/signal_synth_saas/rollback/SNAPSHOT_NAME
```

Normal deploy preserves state and refuses an unsupported database schema.
There are no in-place compatibility migrations. The explicitly destructive,
idempotent pre-beta reset is separate:

```sh
scripts/reset_prebeta_state.sh --confirm-destroy-prebeta-state
scripts/reset_prebeta_state.sh --confirm-destroy-prebeta-state \
  --backup /var/backups/syn_sig_ra/pre-v1-reset
```

It stops services, optionally archives the old state, removes the database and
derived artifacts, bootstraps the real operator (`Kis Tamás`,
`synsigra@gmail.com`) from a secret supplied on stdin, rotates the live key,
deploys, proves the old key returns 401, and generates/downloads a fresh smoke
package. Config, TLS, mail credentials, logs, source packs, verifier downloads,
and repositories are not removed. Push remains a separate operation.
