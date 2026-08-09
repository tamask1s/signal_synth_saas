#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:---check}"

case "$MODE" in
  --check|--apply) ;;
  *) echo "usage: $0 [--check|--apply]" >&2; exit 2 ;;
esac

cmake --build "$ROOT/build" --target syn_sig_ra_scenario_schema -j2 >/dev/null
exec "$ROOT/build/syn_sig_ra_scenario_schema" "$MODE" "$ROOT/packs/scenarios"
