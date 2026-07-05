#!/bin/sh
set -eu

mode=${1:-}
days=${RETENTION_DAYS:-90}
case "$mode" in
  --dry-run)
    sudo -u apache /usr/local/bin/syn_sig_ra_admin cleanup-retention \
      /var/lib/syn_sig_ra/db.sqlite3 /var/lib/syn_sig_ra "$days"
    ;;
  --apply)
    sudo -u apache /usr/local/bin/syn_sig_ra_admin cleanup-retention \
      /var/lib/syn_sig_ra/db.sqlite3 /var/lib/syn_sig_ra "$days" --apply
    ;;
  *)
    echo "usage: $0 <--dry-run|--apply>" >&2
    exit 2
    ;;
esac
