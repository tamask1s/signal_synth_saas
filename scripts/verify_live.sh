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
curl -fsS -H "Authorization: Bearer $key" "$base/v1/downloads/verifier"
printf '\n'
curl -fsS -H "Authorization: Bearer $key" "$base/v1/metrics"
printf '\n'
viewer_html=$(curl -fsS "$base/viewer")
case "$viewer_html" in
  *"Synsigra Lab"*"$base"*) ;;
  *"Synsigra Lab"*) ;;
  *) echo "signal viewer page is missing" >&2; exit 1 ;;
esac
viewer_js=$(curl -fsS "$base/viewer/signal-viewer.js")
case "$viewer_js" in *decodeSignalWindow*visibleBucketRange*) ;; *) exit 1 ;; esac
case "$viewer_js" in *"Math.max(centerY - halfHeight"*) exit 1 ;; *) ;; esac
viewer_app=$(curl -fsS "$base/viewer/app.js")
case "$viewer_app" in *cacheSatisfiesViewport*prefetchedRequest*) ;; *) exit 1 ;; esac
openapi=$(curl -fsS "$base/openapi.yaml")
case "$openapi" in *"/v1/jobs/{job_id}/viewer/window:"*) ;; *) exit 1 ;; esac
viewer_auth_status=$(curl -sS -o /dev/null -w '%{http_code}' \
  "$base/v1/jobs/job_verify_probe/viewer")
[ "$viewer_auth_status" = "401" ] || {
  echo "signal viewer API auth check returned $viewer_auth_status" >&2
  exit 1
}
jobs=$(curl -fsS -H "Authorization: Bearer $key" \
  "$base/v1/jobs?limit=100&offset=0")
viewer_job=$(printf '%s' "$jobs" | python3 -c \
  'import json,sys; jobs=json.load(sys.stdin)["jobs"]; print(next((j["job_id"] for j in jobs if j.get("status")=="succeeded" and j.get("package_id")), ""))')
if [ -n "$viewer_job" ]; then
  viewer_metadata=$(curl -fsS -H "Authorization: Bearer $key" \
    "$base/v1/jobs/$viewer_job/viewer")
  set -- $(printf '%s' "$viewer_metadata" | python3 -c \
    'import json,sys; c=json.load(sys.stdin)["cases"][0]; print(c["case_id"], min(c["sample_count"],5000))')
  viewer_case=$1
  viewer_count=$2
  viewer_window=$(mktemp /tmp/synsigra-viewer-verify.XXXXXX)
  viewer_timing=$(curl -fsS -H "Authorization: Bearer $key" \
    "$base/v1/jobs/$viewer_job/viewer/window?case_id=$viewer_case&start_sample=0&sample_count=$viewer_count&points=1024&channels=0" \
    -o "$viewer_window" \
    -w 'connect=%{time_connect}s ttfb=%{time_starttransfer}s total=%{time_total}s bytes=%{size_download}')
  printf 'viewer_window_%s\n' "$viewer_timing"
  python3 -c \
    'import pathlib,sys; data=pathlib.Path(sys.argv[1]).read_bytes(); assert len(data)>=84 and data[:8]==b"SYNSIGV1"' \
    "$viewer_window"
  rm -f "$viewer_window"
fi
sudo systemctl is-active apache22
sudo systemctl is-active nginx.service
sudo systemctl is-active syn_sig_ra_worker.service
