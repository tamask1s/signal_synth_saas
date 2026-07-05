# Backup, restore, and artifact retention

## Retention policy

Private-beta artifact retention is 90 days. A soft-deleted job's package is
eligible immediately. Run a dry-run before applying cleanup:

```sh
scripts/cleanup_retention.sh --dry-run
scripts/cleanup_retention.sh --apply
```

Cleanup first hides the package in metadata, then removes only the exact
`<data-root>/packages/<package_id>` tree. It rejects symlinks and unexpected
storage paths. The succeeded job, pack/package fingerprints, generator build
identity, timestamps, and safe error metadata remain. Its API representation
uses `artifact_status: expired`; artifact downloads return 404.

Published packages are immutable while retained. Cleanup never modifies their
contents. Current plans share the same 90-day window; plan-specific windows can
be introduced when billing plans exist.

## Backup policy

Artifacts are backed up rather than assumed reproducible. Exact regeneration
would require retaining the same pack document, every referenced scenario, and
the exact generator binary identified by `generator_build_identity`.

Create a consistent backup:

```sh
scripts/backup_live.sh
```

The script briefly stops the worker, creates an online SQLite backup, archives
the immutable package and custom-pack snapshot trees, writes SHA-256 checksums,
and restarts the worker.
The default destination is `/var/backups/syn_sig_ra/<UTC timestamp>`.

Backups contain API-key hashes and customer metadata. Keep them root-readable,
off-host copy them through an encrypted operator-controlled channel, and apply
the same deletion policy as production data.

## Restore drill

Validate checksums, extract packages to an isolated temporary path, and open
the copied metadata without touching production:

```sh
scripts/restore_drill.sh /var/backups/syn_sig_ra/YYYYMMDDhhmmss
```

Expected output includes `status=restore-drill-succeeded`.

An actual production restore is deliberately manual: stop Apache and the
worker, preserve the current data root, restore both `db.sqlite3` and
`packages/` from the same backup, restore `apache:nogroup` ownership, run
`httpd -t`, then start Apache/worker and run `scripts/verify_live.sh`. Never
combine a database from one backup with packages from another.
