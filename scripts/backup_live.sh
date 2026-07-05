#!/bin/sh
set -eu

backup_root=${BACKUP_ROOT:-/var/backups/syn_sig_ra}
timestamp=$(date -u +%Y%m%d%H%M%S)
destination="$backup_root/$timestamp"

sudo install -d -m 0700 "$destination"
sudo systemctl stop syn_sig_ra_worker.service
trap 'sudo systemctl start syn_sig_ra_worker.service' EXIT HUP INT TERM
sudo /usr/local/bin/syn_sig_ra_admin backup-db \
  /var/lib/syn_sig_ra/db.sqlite3 "$destination/db.sqlite3"
sudo tar -C /var/lib/syn_sig_ra -czf "$destination/packages.tar.gz" packages
sudo sha256sum "$destination/db.sqlite3" "$destination/packages.tar.gz" |
  sudo tee "$destination/SHA256SUMS" >/dev/null
sudo systemctl start syn_sig_ra_worker.service
trap - EXIT HUP INT TERM
printf 'backup=%s\n' "$destination"
