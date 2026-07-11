#!/bin/sh
set -eu

mode=${1:-}
case "$mode" in
  --dry-run|--apply)
    ;;
  *)
    echo "usage: $0 <--dry-run|--apply>" >&2
    exit 2
    ;;
esac

apply=
[ "$mode" = "--apply" ] && apply=--apply
sudo -u apache /usr/local/bin/syn_sig_ra_admin \
  compact-artifacts /var/lib/syn_sig_ra $apply
