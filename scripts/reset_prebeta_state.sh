#!/bin/sh
set -eu

usage() {
  echo "usage: $0 --confirm-destroy-prebeta-state [--backup DIRECTORY]" >&2
  exit 2
}

[ "${1:-}" = "--confirm-destroy-prebeta-state" ] || usage
shift
backup=
if [ "$#" -gt 0 ]; then
  [ "$#" -eq 2 ] && [ "$1" = "--backup" ] || usage
  backup=$2
fi

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$repo_dir/build/e2e"}
core_build=${SIGNAL_SYNTH_BUILD_DIR:-"$repo_dir/build/signal_synth_live"}
data_root=/var/lib/syn_sig_ra
database=$data_root/db.sqlite3
key_file=/root/syn_sig_ra_api_key
base=${SYN_SIG_RA_BASE_URL:-https://www.timeonion.com/syn_sig_ra}
umask 077
new_key=$(mktemp /tmp/synsigra-new-key.XXXXXX)
cleanup() { rm -f "$new_key"; }
trap cleanup EXIT HUP INT TERM
printf 'synsigra_live_%s\n' "$(openssl rand -hex 32)" >"$new_key"
old_key=$(sudo cat "$key_file" 2>/dev/null || true)

cmake -S "$repo_dir/../signal_synth" -B "$core_build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
  -DSIGNAL_SYNTH_BUILD_TESTS=OFF \
  -DSIGNAL_SYNTH_BUILD_CLI=ON
cmake --build "$core_build" --target signal_synth_cli -j2
SIGNAL_SYNTH_CLI="$core_build/signal-synth" \
  "$repo_dir/scripts/build_release.sh"

sudo systemctl stop syn_sig_ra_worker.service
sudo systemctl stop apache22

if [ -n "$backup" ]; then
  sudo install -d -m 0700 "$backup"
  sudo tar -C /var/lib -czf "$backup/syn_sig_ra-prebeta-state.tar.gz" syn_sig_ra
  sudo sha256sum "$backup/syn_sig_ra-prebeta-state.tar.gz" |
    sudo tee "$backup/SHA256SUMS" >/dev/null
fi

sudo find "$data_root" -maxdepth 1 -type f \
  \( -name 'db.sqlite3*' -o -name '*.before-*' \) -delete
for directory in packages work recipes custom_packs generator_releases derived-artifacts; do
  sudo install -d -o apache -g nogroup -m 0750 "$data_root/$directory"
  sudo find "$data_root/$directory" -mindepth 1 -delete
done
if [ -d "$data_root/jobs" ]; then
  sudo find "$data_root/jobs" -mindepth 1 -delete
  sudo rmdir "$data_root/jobs"
fi

sudo install -s -m 0755 "$build_dir/syn_sig_ra_admin" \
  /usr/local/bin/syn_sig_ra_admin
sudo -u apache /usr/local/bin/syn_sig_ra_admin init-db "$database"
cat "$new_key" | sudo -u apache /usr/local/bin/syn_sig_ra_admin \
  bootstrap-owner "$database" org_live user_live synsigra@gmail.com \
  "Kis Tamás" "key_live_$(date -u +%Y%m%d%H%M%S)" "live operator"
sudo chown apache:nogroup "$database"
sudo chmod 0600 "$database"
sudo install -o root -g root -m 0600 "$new_key" "$key_file"

SYN_SIG_RA_SKIP_BUILD=1 "$repo_dir/scripts/deploy_live.sh"

auth_status() {
  secret=$1
  url=$2
  {
    printf 'silent\nshow-error\noutput = "/dev/null"\n'
    printf 'write-out = "%%{http_code}"\n'
    printf 'header = "Authorization: Bearer %s"\n' "$secret"
    printf 'url = "%s"\n' "$url"
  } | curl -K -
}

if [ -n "$old_key" ]; then
  [ "$(auth_status "$old_key" "$base/v1/projects")" = 401 ] || {
    echo "old live API key was not rejected after reset" >&2
    exit 1
  }
fi
new_secret=$(cat "$new_key")
[ "$(auth_status "$new_secret" "$base/v1/projects")" = 200 ] || {
  echo "new operator API key was not accepted" >&2
  exit 1
}

job=$(curl -fsS -K - -H 'Content-Type: application/json' \
  -d '{"project_id":"org_live_default","pack_id":"r_peak_stress_v1"}' \
  "$base/v1/jobs" <<EOF
header = "Authorization: Bearer $new_secret"
EOF
)
job_id=$(printf '%s' "$job" | python3 -c \
  'import json,sys; print(json.load(sys.stdin)["job_id"])')
status=queued
attempt=0
while [ "$status" = queued ] || [ "$status" = running ]; do
  [ "$attempt" -lt 180 ] || {
    echo "fresh-state smoke job timed out" >&2
    exit 1
  }
  sleep 1
  result=$(curl -fsS -K - "$base/v1/jobs/$job_id" <<EOF
header = "Authorization: Bearer $new_secret"
EOF
)
  status=$(printf '%s' "$result" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["status"])')
  attempt=$((attempt + 1))
done
[ "$status" = succeeded ] || {
  printf '%s\n' "$result" >&2
  exit 1
}
kit=$(mktemp /tmp/synsigra-reset-kit.XXXXXX)
curl -fsS -K - -o "$kit" "$base/v1/jobs/$job_id/verification-kit.zip" <<EOF
header = "Authorization: Bearer $new_secret"
EOF
unzip -tq "$kit" >/dev/null
rm -f "$kit"
printf 'status=prebeta-reset-verified\njob_id=%s\n' "$job_id"
