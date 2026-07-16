#!/bin/sh
set -eu

mode=${1:---dry-run}
case "$mode" in
  --dry-run|--apply) ;;
  *) echo "usage: $0 [--dry-run|--apply]" >&2; exit 2 ;;
esac

data_root=${SYN_SIG_RA_DATA_ROOT:-/var/lib/syn_sig_ra}
admin=${SYN_SIG_RA_ADMIN:-/usr/local/bin/syn_sig_ra_admin}
candidates=0
prepared=0
skipped=0
scanned=0
signal_bytes_total=0

package_roots=$(sudo -u apache "$admin" list-active-package-paths \
  "$data_root/db.sqlite3")
for package_root in $package_roots; do
  case "$package_root" in
    "$data_root"/packages/pkg_*) ;;
    *) echo "error=unsafe-package-storage-key" >&2; exit 1 ;;
  esac
  scanned=$((scanned + 1))
  sudo -u apache test ! -e "$package_root/viewer" || continue
  sudo -u apache test -f "$package_root/package.zip" || continue
  candidates=$((candidates + 1))
  package_id=${package_root##*/}
  signal_bytes=$(sudo -u apache unzip -Z -l "$package_root/package.zip" | \
    awk '$4 ~ /^[0-9]+$/ && $NF ~ /\/synsigra[.]dat$/ { total += $4 } END { print total + 0 }')
  signal_bytes_total=$((signal_bytes_total + signal_bytes))
  printf 'candidate=%s signal_bytes=%s\n' "$package_id" "$signal_bytes"
  [ "$mode" = "--apply" ] || continue

  temporary=$(sudo -u apache mktemp -d "$data_root/work/viewer-backfill.XXXXXX")
  if ! sudo -u apache unzip -qq "$package_root/package.zip" \
      '*/synsigra.hea' '*/synsigra.dat' -d "$temporary"; then
    sudo -u apache rm -rf "$temporary"
    echo "error=viewer-backfill-unzip-failed package_id=$package_id" >&2
    exit 1
  fi
  sudo -u apache chmod 0750 "$package_root"
  if ! sudo -u apache "$admin" prepare-viewer-source \
      "$temporary" "$package_root/viewer"; then
    sudo -u apache chmod 0550 "$package_root"
    sudo -u apache rm -rf "$temporary"
    echo "error=viewer-backfill-prepare-failed package_id=$package_id" >&2
    exit 1
  fi
  sudo -u apache rm -rf "$temporary"
  if sudo -u apache test -d "$package_root/viewer"; then
    sudo -u apache find "$package_root/viewer" -type f -exec chmod 0440 {} +
    sudo -u apache find "$package_root/viewer" -type d -exec chmod 0550 {} +
    prepared=$((prepared + 1))
  else
    skipped=$((skipped + 1))
  fi
  sudo -u apache chmod 0550 "$package_root"
done

printf 'status=viewer-backfill-%s\nscanned=%s\ncandidates=%s\nsignal_bytes=%s\nprepared=%s\nskipped=%s\n' \
  "${mode#--}" "$scanned" "$candidates" "$signal_bytes_total" \
  "$prepared" "$skipped"
