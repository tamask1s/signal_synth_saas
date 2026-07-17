#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$repo_dir/build/e2e"}

"$repo_dir/scripts/build_verifier_downloads.sh" "$repo_dir/downloads/verifier"

cmake -S "$repo_dir" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSIGNAL_SYNTH_ROOT="$repo_dir/../signal_synth" \
  -DAPXS_EXECUTABLE=/usr/local/apache2/bin/apxs \
  -DSYN_SIG_RA_ENABLE_INTEGRATION_TESTS=ON \
  -DBUILD_TESTING=ON
cmake --build "$build_dir" -j"${BUILD_JOBS:-1}"
(cd "$build_dir" && ctest -E integration_e2e_smoke --output-on-failure)

if [ "${RUN_E2E:-0}" = 1 ]; then
  (cd "$build_dir" && \
    APACHE_HTTPD=/usr/local/apache2/bin/httpd \
    SIGNAL_SYNTH_CLI="${SIGNAL_SYNTH_CLI:-/opt/signal_synth/bin/signal-synth}" \
    ctest -R integration_e2e_smoke --output-on-failure)
fi
