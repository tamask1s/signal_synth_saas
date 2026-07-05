#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$repo_dir/build/e2e"}
timestamp=$(date -u +%Y%m%d%H%M%S)
reset_db=0
[ "${1:-}" = "--reset-db" ] && reset_db=1

"$repo_dir/scripts/build_release.sh"

sudo systemctl stop syn_sig_ra_worker.service
sudo systemctl stop apache22
sudo cp /usr/local/apache2/modules/mod_syn_sig_ra.so \
  "/usr/local/apache2/modules/mod_syn_sig_ra.so.before-$timestamp"
sudo install -m 0755 "$build_dir/mod_syn_sig_ra.so" \
  /usr/local/apache2/modules/mod_syn_sig_ra.so
sudo install -m 0755 "$build_dir/syn_sig_ra_admin" \
  /usr/local/bin/syn_sig_ra_admin
sudo install -m 0755 "$build_dir/syn_sig_ra_worker" \
  /usr/local/bin/syn_sig_ra_worker

if [ "$reset_db" = 1 ]; then
  sudo cp /var/lib/syn_sig_ra/db.sqlite3 \
    "/var/lib/syn_sig_ra/db.sqlite3.before-$timestamp"
  sudo rm -f /var/lib/syn_sig_ra/db.sqlite3 \
    /var/lib/syn_sig_ra/db.sqlite3-shm \
    /var/lib/syn_sig_ra/db.sqlite3-wal
  sudo -u apache /usr/local/bin/syn_sig_ra_admin \
    init-db /var/lib/syn_sig_ra/db.sqlite3
  sudo cat /root/syn_sig_ra_api_key |
    sudo -u apache /usr/local/bin/syn_sig_ra_admin create-api-key \
      /var/lib/syn_sig_ra/db.sqlite3 \
      org_live user_live key_live_20260705 "live integration" owner
fi

sudo /usr/local/apache2/bin/httpd -t
sudo systemctl start apache22
sudo systemctl start syn_sig_ra_worker.service
"$repo_dir/scripts/verify_live.sh"
