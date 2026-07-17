# Operations and observability

## Endpoints

- `GET /syn_sig_ra/healthz` proves the Apache module can answer.
- `GET /syn_sig_ra/readyz` returns HTTP 200 only when the metadata DB opens,
  the generator is executable, the pack catalog is non-empty, and the artifact
  data root is writable. It also reports free disk bytes.
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
scripts/deploy_live.sh
scripts/deploy_live.sh --reset-db
scripts/verify_live.sh
scripts/commit_checked.sh 17 "Add observability" file1 file2
git push origin master
```

`--reset-db` is intentionally explicit and re-provisions the private-beta
live key after backing up the old database. Push remains a separate operation.
