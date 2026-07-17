#!/bin/bash
set -Eeuo pipefail

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$#" -eq 1 ] || {
  echo "usage: $0 <release.tar.gz>" >&2
  exit 2
}
artifact=$(readlink -f "$1")
"$repo_dir/scripts/verify_release_artifact.sh" "$artifact"

work=$(mktemp -d /tmp/synsigra-release-deploy.XXXXXX)
snapshot=
trap 'rm -rf "$work"' EXIT
tar -xzf "$artifact" -C "$work"
release_root=$(find "$work" -mindepth 1 -maxdepth 1 -type d -print -quit)
release_id=$(basename "$release_root")
release_abi=$(python3 -c \
  'import json,sys; print(json.load(open(sys.argv[1]))["runtime"]["apache_module_abi"])' \
  "$release_root/manifest.json")
live_abi=$(/usr/local/apache2/bin/httpd -v | sed -n \
  's/^Server version: Apache\/\([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' | head -1)
[ "$release_abi" = "$live_abi" ] || {
  echo "release Apache ABI $release_abi does not match live Apache $live_abi" >&2
  exit 1
}

# shellcheck source=scripts/release_runtime.sh
. "$repo_dir/scripts/release_runtime.sh"

rollback_on_error() {
  status=$?
  [ "$status" -ne 0 ] || status=130
  trap - ERR INT TERM
  if [ -n "$snapshot" ]; then
    echo "deployment failed; restoring $snapshot" >&2
    if synsigra_restore_live_snapshot "$snapshot" && \
        "$repo_dir/scripts/verify_live.sh"; then
      echo "automatic rollback succeeded" >&2
    else
      echo "automatic rollback failed; operator intervention is required" >&2
    fi
  fi
  exit "$status"
}
trap rollback_on_error ERR INT TERM

timestamp=$(date -u +%Y%m%d%H%M%S)
snapshot=$(synsigra_capture_live_snapshot "pre-${release_id}-${timestamp}")

sudo systemctl stop syn_sig_ra_worker.service
sudo systemctl stop apache22
sudo install -d -m 0755 /opt/signal_synth/bin /opt/signal_synth_saas
sudo install -s -m 0755 "$release_root/bin/signal-synth" \
  /opt/signal_synth/bin/signal-synth
sudo install -s -m 0755 "$release_root/bin/mod_syn_sig_ra.so" \
  /usr/local/apache2/modules/mod_syn_sig_ra.so
sudo install -s -m 0755 "$release_root/bin/syn_sig_ra_admin" \
  /usr/local/bin/syn_sig_ra_admin
sudo install -s -m 0755 "$release_root/bin/syn_sig_ra_worker" \
  /usr/local/bin/syn_sig_ra_worker

sudo rm -rf /opt/signal_synth_saas/packs \
  /opt/signal_synth_saas/downloads/verifier \
  /usr/local/apache2/htdocs/frontend
sudo install -d -m 0755 /opt/signal_synth_saas/packs \
  /opt/signal_synth_saas/downloads/verifier \
  /usr/local/apache2/htdocs/frontend
sudo cp -R "$release_root/packs/." /opt/signal_synth_saas/packs/
sudo cp -R "$release_root/downloads/verifier/." \
  /opt/signal_synth_saas/downloads/verifier/
sudo cp -R "$release_root/frontend/." /usr/local/apache2/htdocs/frontend/
sudo find /opt/signal_synth_saas/packs /opt/signal_synth_saas/downloads/verifier \
  /usr/local/apache2/htdocs/frontend -type d -exec chmod 0755 {} +
sudo find /opt/signal_synth_saas/packs /opt/signal_synth_saas/downloads/verifier \
  /usr/local/apache2/htdocs/frontend -type f -exec chmod 0644 {} +

sudo install -m 0644 "$release_root/ops/apache/synsigra-apache.logrotate" \
  /etc/logrotate.d/synsigra-apache22
nginx_target=$(synsigra_nginx_target)
sudo install -m 0644 "$release_root/ops/nginx/timeonion.conf" "$nginx_target"
sudo install -d -o apache -g nogroup -m 0750 \
  /var/lib/syn_sig_ra/custom_packs \
  /var/lib/syn_sig_ra/derived-artifacts \
  /var/lib/syn_sig_ra/recipes \
  /var/lib/syn_sig_ra/generator_releases

sudo /usr/local/apache2/bin/httpd -t
sudo nginx -t
sudo systemctl start apache22
sudo systemctl start syn_sig_ra_worker.service
sudo systemctl reload nginx.service
"$repo_dir/scripts/verify_live.sh"

release_store=/opt/signal_synth_saas/releases
registered_artifact="$release_store/$(basename "$artifact")"
sudo install -d -m 0755 "$release_store"
if [ -L /opt/signal_synth_saas/current-release ]; then
  previous=$(readlink -f /opt/signal_synth_saas/current-release)
  sudo ln -sfn "$previous" /opt/signal_synth_saas/previous-release
fi
sudo install -m 0644 "$artifact" "$registered_artifact"
if [ -f "$artifact.sha256" ]; then
  sudo install -m 0644 "$artifact.sha256" "$registered_artifact.sha256"
fi
sudo ln -sfn "$registered_artifact" /opt/signal_synth_saas/current-release
sudo ln -sfn "$snapshot" /opt/signal_synth_saas/last-rollback

snapshot=
trap - ERR INT TERM
printf 'deployed_release=%s\nartifact=%s\nrollback_snapshot=%s\n' \
  "$release_id" "$registered_artifact" \
  "$(readlink -f /opt/signal_synth_saas/last-rollback)"
