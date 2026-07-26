#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE="$(cd "${ROOT}/../signal_synth" && pwd)"
BUILD="${CORE}/build"

python3 "${CORE}/scripts/generate_r_peak_noise_frontier.py"
python3 "${CORE}/scripts/generate_simple_r_peak_packs.py"
python3 "${CORE}/scripts/generate_simple_hrv_pack.py"
cmake \
  -S "${CORE}" \
  -B "${BUILD}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSIGNAL_SYNTH_BUILD_TESTS=ON \
  -DSIGNAL_SYNTH_BUILD_CLI=ON
cmake --build "${BUILD}" --parallel 2
python3 "${CORE}/scripts/export_curated_pack_metadata.py" \
  --cli "${BUILD}/signal-synth" \
  --catalog "${CORE}/examples/catalog/verification_packs_v1.json" \
  --source-root "${CORE}" \
  --out "${CORE}/examples/catalog/curated_pack_metadata_v1.json"
(
  cd "${BUILD}"
  ctest --output-on-failure
)
python3 "${CORE}/scripts/generate_r_peak_noise_frontier.py" --check
python3 "${CORE}/scripts/generate_simple_r_peak_packs.py" --check
python3 "${CORE}/scripts/generate_simple_hrv_pack.py" --check
git -C "${CORE}" diff --check
