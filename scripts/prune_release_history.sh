#!/bin/sh
set -eu

mode=${1:---dry-run}
case "$mode" in
  --dry-run|--apply) ;;
  *)
    echo "usage: $0 [--dry-run|--apply]" >&2
    exit 2
    ;;
esac

release_root=/opt/signal_synth_saas/releases
rollback_root=/opt/signal_synth_saas/rollback
keep_releases=3
keep_snapshots=5
work=$(mktemp -d /tmp/synsigra-release-prune.XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM

safe_name() {
  case "$1" in
    ''|.|..|*[!A-Za-z0-9._-]*) return 1 ;;
    *) return 0 ;;
  esac
}

last_rollback=$(readlink -f /opt/signal_synth_saas/last-rollback 2>/dev/null || true)
if sudo test -d "$rollback_root"; then
  sudo find "$rollback_root" -mindepth 1 -maxdepth 1 -type d \
    -printf '%T@ %f\n' | LC_ALL=C sort -nr > "$work/snapshots"
  index=0
  while read -r _ name; do
    [ -n "$name" ] || continue
    safe_name "$name" || {
      echo "refusing unsafe rollback history entry: $name" >&2
      exit 1
    }
    index=$((index + 1))
    path="$rollback_root/$name"
    if [ "$index" -le "$keep_snapshots" ] || [ "$path" = "$last_rollback" ]; then
      printf 'keep_snapshot=%s\n' "$path"
    else
      printf 'remove_snapshot=%s\n' "$path"
      [ "$mode" = --dry-run ] || sudo rm -rf -- "$path"
    fi
  done < "$work/snapshots"
fi

current_release=$(readlink -f /opt/signal_synth_saas/current-release 2>/dev/null || true)
previous_release=$(readlink -f /opt/signal_synth_saas/previous-release 2>/dev/null || true)
if sudo test -d "$release_root"; then
  sudo find "$release_root" -mindepth 1 -maxdepth 1 -type f \
    -name '*.tar.gz' -printf '%T@ %f\n' | LC_ALL=C sort -nr > "$work/releases"
  index=0
  while read -r _ name; do
    [ -n "$name" ] || continue
    safe_name "$name" || {
      echo "refusing unsafe release history entry: $name" >&2
      exit 1
    }
    index=$((index + 1))
    path="$release_root/$name"
    if [ "$index" -le "$keep_releases" ] || [ "$path" = "$current_release" ] ||
        [ "$path" = "$previous_release" ]; then
      printf 'keep_release=%s\n' "$path"
    else
      printf 'remove_release=%s\n' "$path"
      if [ "$mode" = --apply ]; then
        sudo rm -f -- "$path" "$path.sha256"
      fi
    fi
  done < "$work/releases"
fi
