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
  sudo -u apache test -f "$package_root/package.zip" || continue
  if sudo -u apache test -d "$package_root/viewer/cases"; then
    case_count=$(sudo -u apache find "$package_root/viewer/cases" -mindepth 1 -maxdepth 1 -type d | wc -l)
    overlay_count=$(sudo -u apache find "$package_root/viewer/cases" -mindepth 2 -maxdepth 2 -type f -name overlays.sqlite3 | wc -l)
    [ "$case_count" -eq 0 ] || [ "$case_count" -ne "$overlay_count" ] || continue
  fi
  candidates=$((candidates + 1))
  package_id=${package_root##*/}
  signal_bytes=$(sudo -u apache unzip -Z -l "$package_root/package.zip" | \
    awk '$4 ~ /^[0-9]+$/ && $NF ~ /\/synsigra[.]dat$/ { total += $4 } END { print total + 0 }')
  signal_bytes_total=$((signal_bytes_total + signal_bytes))
  printf 'candidate=%s signal_bytes=%s\n' "$package_id" "$signal_bytes"
  [ "$mode" = "--apply" ] || continue

  temporary=$(sudo -u apache mktemp -d "$data_root/work/viewer-backfill.XXXXXX")
  if ! sudo -u apache unzip -qq "$package_root/package.zip" \
      '*/synsigra.hea' '*/synsigra.dat' '*/annotations.json' -d "$temporary"; then
    sudo -u apache rm -rf "$temporary"
    echo "error=viewer-backfill-unzip-failed package_id=$package_id" >&2
    exit 1
  fi
  sudo -u apache chmod 0750 "$package_root"
  next_viewer="$package_root/viewer.next"
  if sudo -u apache test -d "$next_viewer"; then
    sudo -u apache find "$next_viewer" -type d -exec chmod 0750 {} +
  fi
  sudo -u apache rm -rf "$next_viewer"
  if ! sudo -u apache "$admin" prepare-viewer-source \
      "$temporary" "$next_viewer"; then
    sudo -u apache chmod 0550 "$package_root"
    sudo -u apache rm -rf "$temporary"
    echo "error=viewer-backfill-prepare-failed package_id=$package_id" >&2
    exit 1
  fi
  sudo -u apache rm -rf "$temporary"
  if sudo -u apache test -d "$next_viewer"; then
    previous_viewer="$package_root/viewer.previous"
    if sudo -u apache test -d "$previous_viewer"; then
      sudo -u apache find "$previous_viewer" -type d -exec chmod 0750 {} +
      sudo -u apache rm -rf "$previous_viewer"
    fi
    had_previous=0
    if sudo -u apache test -d "$package_root/viewer"; then
      if ! sudo -u apache mv "$package_root/viewer" "$previous_viewer"; then
        sudo -u apache chmod 0550 "$package_root"
        echo "error=viewer-backfill-stage-old-failed package_id=$package_id" >&2
        exit 1
      fi
      had_previous=1
    fi
    if ! sudo -u apache mv "$next_viewer" "$package_root/viewer"; then
      if [ "$had_previous" -eq 1 ]; then
        sudo -u apache mv "$previous_viewer" "$package_root/viewer"
      fi
      sudo -u apache chmod 0550 "$package_root"
      echo "error=viewer-backfill-activate-failed package_id=$package_id" >&2
      exit 1
    fi
    sudo -u apache find "$package_root/viewer" -type f -exec chmod 0440 {} +
    sudo -u apache find "$package_root/viewer" -type d -exec chmod 0550 {} +
    if sudo -u apache test -d "$previous_viewer"; then
      sudo -u apache find "$previous_viewer" -type d -exec chmod 0750 {} +
      sudo -u apache rm -rf "$previous_viewer"
    fi
    prepared=$((prepared + 1))
  else
    skipped=$((skipped + 1))
  fi
  sudo -u apache chmod 0550 "$package_root"
done

printf 'status=viewer-backfill-%s\nscanned=%s\ncandidates=%s\nsignal_bytes=%s\nprepared=%s\nskipped=%s\n' \
  "${mode#--}" "$scanned" "$candidates" "$signal_bytes_total" \
  "$prepared" "$skipped"
