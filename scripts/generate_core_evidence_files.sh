#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE="$(cd "${ROOT}/../signal_synth" && pwd)"

python3 "${CORE}/scripts/generate_r_peak_noise_frontier.py"
python3 "${CORE}/scripts/generate_simple_r_peak_packs.py"
python3 "${CORE}/scripts/generate_simple_hrv_pack.py"
