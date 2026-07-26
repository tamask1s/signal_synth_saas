#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE="$(cd "${ROOT}/../signal_synth" && pwd)"

[[ "$#" -eq 1 && -n "$1" && "${#1}" -le 160 ]] || {
  echo "usage: $0 <non-empty message up to 160 characters>" >&2
  exit 2
}

git -C "${CORE}" diff --check
git -C "${CORE}" add --all
git -C "${CORE}" diff --cached --quiet && {
  echo "core commit has no staged changes" >&2
  exit 1
}
git -C "${CORE}" commit -m "$1"
