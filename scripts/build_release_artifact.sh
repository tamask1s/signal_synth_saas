#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$repo_dir/build/e2e"}
signal_synth_root=${SIGNAL_SYNTH_ROOT:-"$repo_dir/../signal_synth"}
signal_synth_build_dir=${SIGNAL_SYNTH_BUILD_DIR:-"$repo_dir/build/signal_synth_live"}
signal_synth_cli=${SIGNAL_SYNTH_CLI:-"$signal_synth_build_dir/signal-synth"}
apache_httpd=${APACHE_HTTPD:-/usr/local/apache2/bin/httpd}
output_dir=${1:-"$repo_dir/build/releases"}

[ "$#" -le 1 ] || {
  echo "usage: $0 [output-directory]" >&2
  exit 2
}

if [ "${SYN_SIG_RA_SKIP_BUILD:-0}" != 1 ]; then
  cmake -S "$signal_synth_root" -B "$signal_synth_build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
    -DSIGNAL_SYNTH_BUILD_TESTS=OFF \
    -DSIGNAL_SYNTH_BUILD_CLI=ON
  cmake --build "$signal_synth_build_dir" --target signal_synth_cli \
    -j"${BUILD_JOBS:-2}"
  SIGNAL_SYNTH_CLI="$signal_synth_cli" "$repo_dir/scripts/build_release.sh"
fi

for file in \
  "$build_dir/mod_syn_sig_ra.so" \
  "$build_dir/syn_sig_ra_admin" \
  "$build_dir/syn_sig_ra_worker" \
  "$signal_synth_cli" \
  "$repo_dir/doc/synsigra_main_landing_package_v10.zip"; do
  [ -f "$file" ] || {
    echo "release input is missing: $file" >&2
    exit 1
  }
done

saas_commit=$(git -C "$repo_dir" rev-parse HEAD)
core_commit=$(git -C "$signal_synth_root" rev-parse HEAD)
expected_core=$(sed -n \
  's/^[[:space:]]*"\([0-9a-f]\{40\}\)"[[:space:]]*$/\1/p' \
  "$repo_dir/CMakeLists.txt" | head -1)
[ -n "$expected_core" ] && [ "$core_commit" = "$expected_core" ] || {
  echo "release core commit does not match the pinned producer" >&2
  exit 1
}
[ -z "$(git -C "$signal_synth_root" status --porcelain)" ] || {
  echo "the pinned signal_synth checkout must be clean" >&2
  exit 1
}

apache_version=$($apache_httpd -v | sed -n \
  's/^Server version: Apache\/\([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' | head -1)
[ -n "$apache_version" ] || {
  echo "unable to determine Apache ABI from $apache_httpd" >&2
  exit 1
}

work=$(mktemp -d /tmp/synsigra-release.XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM
payload="$work/payload"
mkdir -p \
  "$payload/bin" \
  "$payload/packs" \
  "$payload/downloads/verifier" \
  "$payload/ops/nginx" \
  "$payload/ops/apache" \
  "$payload/ops/systemd" \
  "$payload/frontend"

install -s -m 0755 "$build_dir/mod_syn_sig_ra.so" \
  "$payload/bin/mod_syn_sig_ra.so"
install -s -m 0755 "$build_dir/syn_sig_ra_admin" \
  "$payload/bin/syn_sig_ra_admin"
install -s -m 0755 "$build_dir/syn_sig_ra_worker" \
  "$payload/bin/syn_sig_ra_worker"
install -s -m 0755 "$signal_synth_cli" "$payload/bin/signal-synth"
install -m 0755 "$repo_dir/scripts/challenge_artifact.py" \
  "$payload/bin/challenge_artifact.py"
cp -R "$repo_dir/packs/." "$payload/packs/"
cp -R "$repo_dir/downloads/verifier/." "$payload/downloads/verifier/"
install -m 0644 "$repo_dir/ops/nginx/timeonion.conf" \
  "$payload/ops/nginx/timeonion.conf"
install -m 0644 "$repo_dir/ops/apache/synsigra-apache22.logrotate" \
  "$payload/ops/apache/synsigra-apache.logrotate"
install -m 0644 "$repo_dir/ops/systemd/syn_sig_ra_worker.service" \
  "$payload/ops/systemd/syn_sig_ra_worker.service"
unzip -q "$repo_dir/doc/synsigra_main_landing_package_v10.zip" \
  -d "$work/landing"
cp -R "$work/landing/synsigra_main_landing_package/main/." \
  "$payload/frontend/"

(cd "$payload" && find . -type f -print0 | LC_ALL=C sort -z | \
  xargs -0 sha256sum > RELEASE_PAYLOAD_SHA256SUMS)
payload_sha256=$(sha256sum "$payload/RELEASE_PAYLOAD_SHA256SUMS" | \
  cut -d ' ' -f 1)
architecture=$(uname -m)
release_id="synsigra-${saas_commit%${saas_commit#????????????}}-core-${core_commit%${core_commit#????????????}}-apache${apache_version}-${architecture}-${payload_sha256%${payload_sha256#????????????}}"
release_root="$work/$release_id"
mv "$payload" "$release_root"

if [ -n "$(git -C "$repo_dir" status --porcelain --untracked-files=no)" ]; then
  working_tree=dirty
else
  working_tree=clean
fi
source_epoch=${SOURCE_DATE_EPOCH:-$(git -C "$repo_dir" show -s --format=%ct HEAD)}
python3 - "$release_root/manifest.json" "$release_id" "$saas_commit" \
  "$core_commit" "$apache_version" "$architecture" "$payload_sha256" \
  "$working_tree" "$source_epoch" <<'PY'
import datetime
import json
import pathlib
import sys

path, release_id, saas, core, apache, architecture, payload, tree, epoch = sys.argv[1:]
manifest = {
    "schema_version": 1,
    "release_id": release_id,
    "source": {
        "signal_synth_saas_commit": saas,
        "signal_synth_commit": core,
        "working_tree": tree,
    },
    "runtime": {
        "apache_module_abi": apache,
        "architecture": architecture,
        "build_type": "Release",
        "release_flags": "-O2 -DNDEBUG; stripped payload binaries",
    },
    "payload_sha256": payload,
    "source_date": datetime.datetime.fromtimestamp(
        int(epoch), datetime.timezone.utc
    ).isoformat().replace("+00:00", "Z"),
}
pathlib.Path(path).write_text(
    json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
PY
(cd "$release_root" && find . -type f ! -name SHA256SUMS -print0 | \
  LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS)

mkdir -p "$output_dir"
artifact="$output_dir/$release_id.tar.gz"
temporary_artifact="$work/$release_id.tar.gz"
tar --sort=name --mtime="@$source_epoch" --owner=0 --group=0 --numeric-owner \
  -C "$work" -cf - "$release_id" | gzip -n > "$temporary_artifact"
if [ -f "$artifact" ]; then
  cmp -s "$temporary_artifact" "$artifact" || {
    echo "refusing to replace a different immutable release: $artifact" >&2
    exit 1
  }
else
  install -m 0644 "$temporary_artifact" "$artifact"
fi
(cd "$output_dir" && sha256sum "$(basename "$artifact")" > \
  "$(basename "$artifact").sha256")

printf 'release_id=%s\nrelease_artifact=%s\nrelease_checksum=%s.sha256\n' \
  "$release_id" "$artifact" "$artifact"
