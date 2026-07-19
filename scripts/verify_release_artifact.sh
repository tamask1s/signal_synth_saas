#!/bin/sh
set -eu

[ "$#" -eq 1 ] || {
  echo "usage: $0 <release.tar.gz>" >&2
  exit 2
}
artifact=$1
[ -f "$artifact" ] || {
  echo "release artifact does not exist: $artifact" >&2
  exit 1
}

artifact_dir=$(CDPATH= cd -- "$(dirname -- "$artifact")" && pwd)
artifact_name=$(basename "$artifact")
if [ -f "$artifact.sha256" ]; then
  (cd "$artifact_dir" && sha256sum -c "$artifact_name.sha256")
fi

work=$(mktemp -d /tmp/synsigra-release-verify.XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM
tar -xzf "$artifact" -C "$work"
set -- "$work"/*
[ "$#" -eq 1 ] && [ -d "$1" ] || {
  echo "release artifact must contain exactly one top-level directory" >&2
  exit 1
}
release_root=$1
release_id=$(basename "$release_root")
for file in \
  manifest.json SHA256SUMS \
  bin/mod_syn_sig_ra.so bin/syn_sig_ra_worker bin/syn_sig_ra_admin \
  bin/challenge_artifact.py \
  bin/signal-synth ops/nginx/timeonion.conf \
  downloads/verifier/synsigra-wheel.whl \
  downloads/verifier/metadata.json \
  ops/systemd/syn_sig_ra_worker.service \
  ops/apache/synsigra-apache.logrotate; do
  [ -f "$release_root/$file" ] || {
    echo "release payload is missing $file" >&2
    exit 1
  }
done
(cd "$release_root" && sha256sum -c --quiet SHA256SUMS)
python3 - "$release_root/manifest.json" "$release_id" <<'PY'
import json
import pathlib
import sys

manifest = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
assert manifest["schema_version"] == 1
assert manifest["release_id"] == sys.argv[2]
assert len(manifest["source"]["signal_synth_saas_commit"]) == 40
assert len(manifest["source"]["signal_synth_commit"]) == 40
assert manifest["runtime"]["apache_module_abi"] in {"2.2", "2.4"}
assert manifest["runtime"]["build_type"] == "Release"
assert len(manifest["payload_sha256"]) == 64
PY
printf 'release_id=%s\napache_abi=%s\n' "$release_id" \
  "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["runtime"]["apache_module_abi"])' "$release_root/manifest.json")"
