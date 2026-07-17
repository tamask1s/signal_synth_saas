#!/bin/sh
# Shared runtime snapshot helpers. Source this file; do not execute it directly.

synsigra_nginx_target() {
  enabled=/etc/nginx/sites-enabled/timeonion.conf
  if [ -L "$enabled" ]; then
    readlink -f "$enabled"
  else
    printf '%s\n' "$enabled"
  fi
}

synsigra_capture_live_snapshot() {
  snapshot_name=$1
  case "$snapshot_name" in
    ''|*[!A-Za-z0-9._-]*)
      echo "invalid runtime snapshot name" >&2
      return 1
      ;;
  esac
  snapshot_root=/opt/signal_synth_saas/rollback
  snapshot="$snapshot_root/$snapshot_name"
  sudo test ! -e "$snapshot" || {
    echo "runtime snapshot already exists: $snapshot" >&2
    return 1
  }
  work=$(mktemp -d /tmp/synsigra-runtime-snapshot.XXXXXX) || return 1
  mkdir -p "$work/bin" "$work/ops" || return 1
  sudo install -m 0755 /usr/local/apache2/modules/mod_syn_sig_ra.so \
    "$work/bin/mod_syn_sig_ra.so" || return 1
  sudo install -m 0755 /usr/local/bin/syn_sig_ra_worker \
    "$work/bin/syn_sig_ra_worker" || return 1
  sudo install -m 0755 /usr/local/bin/syn_sig_ra_admin \
    "$work/bin/syn_sig_ra_admin" || return 1
  sudo install -m 0755 /opt/signal_synth/bin/signal-synth \
    "$work/bin/signal-synth" || return 1
  sudo tar -C /opt/signal_synth_saas -czf "$work/packs.tar.gz" packs || return 1
  sudo tar -C /opt/signal_synth_saas -czf "$work/verifier.tar.gz" \
    downloads/verifier || return 1
  sudo tar -C /usr/local/apache2/htdocs -czf "$work/frontend.tar.gz" \
    frontend || return 1
  nginx_target=$(synsigra_nginx_target) || return 1
  sudo install -m 0644 "$nginx_target" "$work/ops/nginx.conf" || return 1
  sudo install -m 0644 /etc/logrotate.d/synsigra-apache22 \
    "$work/ops/apache.logrotate" || return 1
  if [ -L /opt/signal_synth_saas/current-release ]; then
    readlink -f /opt/signal_synth_saas/current-release > \
      "$work/release-pointer"
  else
    : > "$work/release-pointer"
  fi
  sudo chmod -R a+rX "$work"
  (cd "$work" && find . -type f ! -name SHA256SUMS -print0 | \
    LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS) || return 1
  sudo install -d -m 0755 "$snapshot_root" || return 1
  sudo chown -R root:root "$work" || return 1
  sudo chmod 0755 "$work" || return 1
  sudo mv "$work" "$snapshot" || return 1
  printf '%s\n' "$snapshot"
}

synsigra_restore_live_snapshot() {
  requested=$1
  snapshot=$(readlink -f "$requested") || return 1
  case "$snapshot" in
    /opt/signal_synth_saas/rollback/*) ;;
    *)
      echo "runtime snapshot must be under /opt/signal_synth_saas/rollback" >&2
      return 1
      ;;
  esac
  [ -f "$snapshot/SHA256SUMS" ] || {
    echo "runtime snapshot is incomplete: $snapshot" >&2
    return 1
  }
  (cd "$snapshot" && sha256sum -c --quiet SHA256SUMS) || return 1

  sudo systemctl stop syn_sig_ra_worker.service || return 1
  sudo systemctl stop apache22 || return 1
  sudo install -s -m 0755 "$snapshot/bin/signal-synth" \
    /opt/signal_synth/bin/signal-synth || return 1
  sudo install -s -m 0755 "$snapshot/bin/mod_syn_sig_ra.so" \
    /usr/local/apache2/modules/mod_syn_sig_ra.so || return 1
  sudo install -s -m 0755 "$snapshot/bin/syn_sig_ra_admin" \
    /usr/local/bin/syn_sig_ra_admin || return 1
  sudo install -s -m 0755 "$snapshot/bin/syn_sig_ra_worker" \
    /usr/local/bin/syn_sig_ra_worker || return 1

  sudo rm -rf /opt/signal_synth_saas/packs \
    /opt/signal_synth_saas/downloads/verifier \
    /usr/local/apache2/htdocs/frontend || return 1
  sudo tar -C /opt/signal_synth_saas -xzf "$snapshot/packs.tar.gz" || return 1
  sudo tar -C /opt/signal_synth_saas -xzf "$snapshot/verifier.tar.gz" || return 1
  sudo tar -C /usr/local/apache2/htdocs -xzf \
    "$snapshot/frontend.tar.gz" || return 1

  nginx_target=$(synsigra_nginx_target) || return 1
  sudo install -m 0644 "$snapshot/ops/nginx.conf" "$nginx_target" || return 1
  sudo install -m 0644 "$snapshot/ops/apache.logrotate" \
    /etc/logrotate.d/synsigra-apache22 || return 1
  sudo /usr/local/apache2/bin/httpd -t || return 1
  sudo nginx -t || return 1
  sudo systemctl start apache22 || return 1
  sudo systemctl start syn_sig_ra_worker.service || return 1
  sudo systemctl reload nginx.service || return 1

  release_pointer=$(cat "$snapshot/release-pointer")
  if [ -n "$release_pointer" ] && [ -f "$release_pointer" ]; then
    sudo ln -sfn "$release_pointer" /opt/signal_synth_saas/current-release || \
      return 1
  else
    sudo rm -f /opt/signal_synth_saas/current-release || return 1
  fi
}
