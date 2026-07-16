#!/bin/sh
set -eu

if [ "$#" -lt 3 ]; then
  echo "usage: $0 <issue-number|-> <message> <file> [file ...]" >&2
  exit 2
fi

issue=$1
message=$2
shift 2
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

cd "$repo_dir"
git diff --check
"$repo_dir/scripts/build_release.sh"
git add -- "$@"
git diff --cached --check
if [ "$issue" = "-" ]; then
  git commit -m "$message"
else
  git commit -m \
    "https://github.com/tamask1s/signal_synth_saas/issues/$issue $message"
fi
