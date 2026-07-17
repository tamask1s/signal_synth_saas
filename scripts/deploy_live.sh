#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$#" -eq 0 ] || {
  echo "usage: $0" >&2
  echo "database resets use scripts/reset_prebeta_state.sh" >&2
  exit 2
}

result=$("$repo_dir/scripts/build_release_artifact.sh")
printf '%s\n' "$result"
artifact=$(printf '%s\n' "$result" | sed -n 's/^release_artifact=//p')
[ -n "$artifact" ] || {
  echo "release builder did not return an artifact" >&2
  exit 1
}
exec "$repo_dir/scripts/deploy_release.sh" "$artifact"
