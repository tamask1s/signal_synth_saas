#!/bin/bash
# One-time infrastructure change; never called by a service deployment.
set -Eeuo pipefail
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
target=$(readlink -f /etc/nginx/sites-enabled/timeonion.conf)
directory=/etc/nginx/timeonion-services
sudo install -d -o root -g root -m 0755 "$directory"
if [ ! -e "$directory/.deploy.lock" ]; then
  sudo install -o root -g sudo -m 0660 /dev/null "$directory/.deploy.lock"
fi
exec 9>"$directory/.deploy.lock"
flock -x 9
if grep -Fq 'include /etc/nginx/timeonion-services/*.conf;' "$target"; then
  test -f "$directory/synsigra.conf"
  sudo nginx -t
  echo 'shared_nginx=already-configured'
  exit 0
fi
# Refuse to replace a vhost changed by another service or administrator.
expected=1bac2f0f86faa8b9b876269d5ee1e9cd6100c8a7d0ede9b1ea29450951818d80
actual=$(sha256sum "$target" | cut -d ' ' -f 1)
[ "$actual" = "$expected" ] || {
  echo 'Shared vhost differs from the inspected baseline; review it before setup.' >&2
  exit 1
}
test ! -e "$directory/synsigra.conf"
backup=/etc/nginx/timeonion-before-service-split.conf.bak
test ! -e "$backup"
sudo nginx -t
sudo cp -p "$target" "$backup"
restore() {
  status=$?
  trap - ERR INT TERM
  sudo cp -p "$backup" "$target"
  sudo nginx -t && sudo systemctl reload nginx.service
  exit "$status"
}
trap restore ERR INT TERM
sudo install -m 0644 "$repo_dir/ops/nginx/synsigra.conf" "$directory/synsigra.conf"
sudo install -m 0644 "$repo_dir/ops/nginx/timeonion.conf" "$target"
sudo nginx -t
sudo systemctl reload nginx.service
trap - ERR INT TERM
echo 'shared_nginx=configured'
