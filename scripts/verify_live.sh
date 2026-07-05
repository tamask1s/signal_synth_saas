#!/bin/sh
set -eu

base=${SYN_SIG_RA_BASE_URL:-https://www.timeonion.com/syn_sig_ra}
key=$(sudo cat /root/syn_sig_ra_api_key)

curl -fsS "$base/healthz"
printf '\n'
curl -fsS "$base/readyz"
printf '\n'
curl -fsS -H "Authorization: Bearer $key" "$base/v1/projects"
printf '\n'
curl -fsS -H "Authorization: Bearer $key" "$base/v1/usage"
printf '\n'
curl -fsS -H "Authorization: Bearer $key" "$base/v1/metrics"
printf '\n'
sudo systemctl is-active apache22
sudo systemctl is-active nginx.service
sudo systemctl is-active syn_sig_ra_worker.service
