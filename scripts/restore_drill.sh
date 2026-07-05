#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <backup-directory>" >&2
  exit 2
fi

backup=$1
work=$(mktemp -d /tmp/syn_sig_ra_restore.XXXXXX)
trap 'sudo rm -rf "$work"' EXIT HUP INT TERM

sudo sha256sum -c "$backup/SHA256SUMS"
sudo cp "$backup/db.sqlite3" "$work/db.sqlite3"
sudo tar -C "$work" -xzf "$backup/packages.tar.gz"
sudo /usr/local/bin/syn_sig_ra_admin list-api-keys "$work/db.sqlite3" >/dev/null
sudo test -d "$work/packages"
sudo test -d "$work/custom_packs"
printf 'status=restore-drill-succeeded\nbackup=%s\n' "$backup"
