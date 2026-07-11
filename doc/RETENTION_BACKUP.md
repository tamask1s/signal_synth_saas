# Backup, restore, and artifact retention

## Retention policy

Private-beta artifact cache retention is 14 days. A soft-deleted job's package is
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

Published packages are immutable while cached. Cleanup never modifies their
contents. Current plans share the same 14-day window; plan-specific windows can
be introduced when billing plans exist.

New packages store only the root manifest and ZIP; the temporary extracted
render tree is removed after the ZIP is created. Existing safe duplicates can
be removed independently of retention:

```sh
scripts/compact_artifacts.sh --dry-run
scripts/compact_artifacts.sh --apply
```

For current jobs, the worker snapshots the selected pack and referenced
scenarios under `recipes/`, and archives the exact executable under
`generator_releases/<sha256>/signal-synth`. An expired job can request a new
exact rebuild. The worker verifies both the executable hash and resulting
package fingerprint; unavailable historical inputs produce an explicit error.

## Backup policy

Artifacts are backed up rather than assumed reproducible. Exact regeneration
would require retaining the same pack document, every referenced scenario, and
the exact generator binary identified by `generator_build_identity`.

Create a consistent backup:

```sh
scripts/backup_live.sh
```

The script briefly stops the worker, creates an online SQLite backup, archives
the immutable package, recipe, generator-release and custom-pack snapshot trees, writes SHA-256 checksums,
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
