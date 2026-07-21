#!/bin/bash
set -Eeuo pipefail

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$#" -le 1 ] || {
  echo "usage: $0 [runtime-snapshot]" >&2
  exit 2
}
# shellcheck source=scripts/release_runtime.sh
. "$repo_dir/scripts/release_runtime.sh"

target=${1:-}
if [ -z "$target" ]; then
  [ -L /opt/signal_synth_saas/last-rollback ] || {
    echo "no successful release rollback snapshot is registered" >&2
    exit 1
  }
  target=$(readlink -f /opt/signal_synth_saas/last-rollback)
fi

timestamp=$(date -u +%Y%m%d%H%M%S)
forward=$(synsigra_capture_live_snapshot "pre-manual-rollback-${timestamp}-$$")
restore_forward_on_error() {
  status=$?
  trap - ERR
  echo "rollback failed; restoring the state captured before rollback" >&2
  synsigra_restore_live_snapshot "$forward" || true
  exit "$status"
}
trap restore_forward_on_error ERR
synsigra_restore_live_snapshot "$target"
SYN_SIG_RA_RUNTIME_ONLY=1 "$repo_dir/scripts/verify_live.sh"
sudo ln -sfn "$forward" /opt/signal_synth_saas/last-rollback
"$repo_dir/scripts/prune_release_history.sh" --apply
trap - ERR
printf 'rollback_status=succeeded\nrestored_snapshot=%s\nroll_forward_snapshot=%s\n' \
  "$(readlink -f "$target")" "$forward"
