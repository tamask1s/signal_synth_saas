#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$repo_dir/build/e2e"}
signal_synth_root=${SIGNAL_SYNTH_ROOT:-"$repo_dir/../signal_synth"}
signal_synth_build_dir=${SIGNAL_SYNTH_BUILD_DIR:-"$repo_dir/build/signal_synth_live"}
timestamp=$(date -u +%Y%m%d%H%M%S)
[ "$#" -eq 0 ] || {
  echo "usage: $0" >&2
  echo "database resets use scripts/reset_prebeta_state.sh" >&2
  exit 2
}

if [ "${SYN_SIG_RA_SKIP_BUILD:-0}" != 1 ]; then
  cmake -S "$signal_synth_root" -B "$signal_synth_build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
    -DSIGNAL_SYNTH_BUILD_TESTS=OFF \
    -DSIGNAL_SYNTH_BUILD_CLI=ON
  cmake --build "$signal_synth_build_dir" --target signal_synth_cli -j2
  SIGNAL_SYNTH_CLI="$signal_synth_build_dir/signal-synth" \
    "$repo_dir/scripts/build_release.sh"
fi

sudo systemctl stop syn_sig_ra_worker.service
sudo systemctl stop apache22
sudo install -d -m 0755 /opt/signal_synth/bin
if [ -f /opt/signal_synth/bin/signal-synth ]; then
  sudo cp /opt/signal_synth/bin/signal-synth \
    "/opt/signal_synth/bin/signal-synth.before-$timestamp"
fi
sudo install -s -m 0755 "$signal_synth_build_dir/signal-synth" \
  /opt/signal_synth/bin/signal-synth
sudo cp /usr/local/apache2/modules/mod_syn_sig_ra.so \
  "/usr/local/apache2/modules/mod_syn_sig_ra.so.before-$timestamp"
sudo install -s -m 0755 "$build_dir/mod_syn_sig_ra.so" \
  /usr/local/apache2/modules/mod_syn_sig_ra.so
sudo install -s -m 0755 "$build_dir/syn_sig_ra_admin" \
  /usr/local/bin/syn_sig_ra_admin
sudo install -s -m 0755 "$build_dir/syn_sig_ra_worker" \
  /usr/local/bin/syn_sig_ra_worker
sudo install -d -m 0755 /opt/signal_synth_saas/packs
sudo tar -C /opt/signal_synth_saas -czf \
  "/opt/signal_synth_saas/packs.before-$timestamp.tar.gz" packs
sudo find /opt/signal_synth_saas/packs -maxdepth 1 -type f \
  \( -name '*.json' -o -name '*.product' -o -name '*.catalog' \) -delete
sudo rm -rf /opt/signal_synth_saas/packs/scenarios
sudo install -m 0644 "$repo_dir"/packs/*.json "$repo_dir"/packs/*.product "$repo_dir"/packs/*.catalog \
  /opt/signal_synth_saas/packs/
sudo cp -R "$repo_dir"/packs/scenarios /opt/signal_synth_saas/packs/
sudo find /opt/signal_synth_saas/packs/scenarios -type d -exec chmod 0755 {} +
sudo find /opt/signal_synth_saas/packs/scenarios -type f -exec chmod 0644 {} +
sudo install -d -m 0755 /opt/signal_synth_saas/downloads/verifier
sudo find /opt/signal_synth_saas/downloads/verifier -maxdepth 1 -type f -delete
sudo install -m 0644 "$repo_dir"/downloads/verifier/* \
  /opt/signal_synth_saas/downloads/verifier/
sudo install -m 0644 "$repo_dir/ops/apache/synsigra-apache22.logrotate" \
  /etc/logrotate.d/synsigra-apache22

landing_work=$(mktemp -d /tmp/synsigra-landing-deploy.XXXXXX)
trap 'rm -rf "$landing_work"' EXIT HUP INT TERM
unzip -q "$repo_dir/doc/synsigra_main_landing_package_v10.zip" \
  -d "$landing_work"
sudo rsync -a \
  "$landing_work/synsigra_main_landing_package/main/" \
  /usr/local/apache2/htdocs/frontend/
rm -rf "$landing_work"
trap - EXIT HUP INT TERM

sudo install -d -o apache -g nogroup -m 0750 \
  /var/lib/syn_sig_ra/custom_packs \
  /var/lib/syn_sig_ra/derived-artifacts \
  /var/lib/syn_sig_ra/recipes \
  /var/lib/syn_sig_ra/generator_releases

sudo /usr/local/apache2/bin/httpd -t
sudo systemctl start apache22
sudo systemctl start syn_sig_ra_worker.service

nginx_enabled=/etc/nginx/sites-enabled/timeonion.conf
nginx_target=$nginx_enabled
if [ -L "$nginx_enabled" ]; then
  nginx_target=$(readlink -f "$nginx_enabled")
fi
if [ -f "$nginx_target" ]; then
  sudo cp "$nginx_target" "$nginx_target.before-$timestamp"
fi
sudo install -m 0644 "$repo_dir/ops/nginx/timeonion.conf" "$nginx_target"
if ! sudo nginx -t; then
  if [ -f "$nginx_target.before-$timestamp" ]; then
    sudo cp "$nginx_target.before-$timestamp" "$nginx_target"
    sudo nginx -t
  fi
  echo "nginx configuration rejected; previous configuration restored" >&2
  exit 1
fi
sudo systemctl reload nginx.service
"$repo_dir/scripts/verify_live.sh"
