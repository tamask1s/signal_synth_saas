#!/bin/sh
set -eu

build_dir=${1:-build}

exec "${build_dir}/syn_sig_ra_worker" run-once \
  "${build_dir}/var/db.sqlite3" \
  "../signal_synth/build/signal-synth" \
  "packs" \
  "${build_dir}/var" \
  "packs/noise_assets" \
  "scripts/challenge_artifact.py" \
  "downloads/verifier/synsigra-wheel.whl"
