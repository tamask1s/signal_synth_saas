#!/bin/sh
set -eu

base=${SYN_SIG_RA_BASE_URL:-https://www.timeonion.com/syn_sig_ra}
public_origin=${SYN_SIG_RA_PUBLIC_ORIGIN:-https://www.timeonion.com}
key=$(sudo cat /root/syn_sig_ra_api_key)

curl -fsS "$base/healthz"
printf '\n'
ready=$(curl -fsS "$base/readyz")
printf '%s' "$ready" | python3 -c \
  'import json,sys; x=json.load(sys.stdin); c=x["accepted_core"]; assert x["status"]=="ready"; assert c["integration_contract"]=="synsigra_core_integration_v1"; assert c["git_commit"]=="ef2c1d9cd00a07c62617619aa939a6996052867e"; assert c["build_identity"]=="signal_synth/"+c["git_commit"]; assert c["challenge_package"]=="synsigra_challenge_package_v1"; assert c["scoring_manifest"]=="synsigra_scoring_manifest_v1"'
printf '%s' "$ready"
printf '\n'
legal=$(curl -fsS "$base/v1/legal")
printf '%s' "$legal" | python3 -c \
  'import json,sys; x=json.load(sys.stdin); assert x["terms_version"]=="private-beta-2026-07-17-r4"; assert x["operator_name"]=="Kis Tamás"; assert x["operator_address"]=="2040 Budaörs, Tátra u. 6, Hungary"; assert x["support_email"]=="synsigra@gmail.com"; assert x["support_url"]=="mailto:synsigra@gmail.com"'
landing=$(curl -fsS "$public_origin/")
case "$landing" in
  *"Synsigra%20technical%20demo"*"Kis Tamás"*"synsigra@gmail.com"*) ;;
  *) echo "landing operator/demo contact is missing" >&2; exit 1 ;;
esac
curl -fsS -H "Authorization: Bearer $key" "$base/v1/projects"
printf '\n'
if [ "${SYN_SIG_RA_BASELINE_ONLY:-0}" = 1 ]; then
  sudo systemctl is-active apache22
  sudo systemctl is-active nginx.service
  sudo systemctl is-active syn_sig_ra_worker.service
  printf 'status=runtime-baseline-verified\n'
  exit 0
fi
curl -fsS -H "Authorization: Bearer $key" "$base/v1/usage"
printf '\n'
curl -fsS -H "Authorization: Bearer $key" "$base/v1/downloads/verifier"
printf '\n'
curl -fsS -H "Authorization: Bearer $key" "$base/v1/metrics"
printf '\n'
printf 'check=audit\n'
audit=$(curl -fsS -H "Authorization: Bearer $key" \
  "$base/v1/audit-events?limit=10")
printf '%s' "$audit" | python3 -c \
  'import json,sys; x=json.load(sys.stdin); assert x["count"]>=1; assert all("api_key" not in e.get("details",{}) for e in x["audit_events"])'
printf 'check=viewer-assets\n'
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
case "$viewer_app" in *"signalCache.find"*prefetchedRequest*setEmptyState*) ;; *) exit 1 ;; esac
printf 'check=openapi\n'
openapi=$(curl -fsS "$base/openapi.yaml")
case "$openapi" in
  *"/v1/jobs/{job_id}/viewer/window:"*"/v1/account/export:"*) ;;
  *) exit 1 ;;
esac
viewer_auth_status=$(curl -sS -o /dev/null -w '%{http_code}' \
  "$base/v1/jobs/job_verify_probe/viewer")
[ "$viewer_auth_status" = "401" ] || {
  echo "signal viewer API auth check returned $viewer_auth_status" >&2
  exit 1
}
printf 'check=jobs-and-viewer\n'
jobs=$(curl -fsS -H "Authorization: Bearer $key" \
  "$base/v1/jobs?limit=100&offset=0")
printf '%s' "$jobs" | python3 -c \
  'import json,sys; jobs=json.load(sys.stdin)["jobs"]; done=[j for j in jobs if j.get("status")=="succeeded"]; assert done; assert all(j.get("integration_contract")=="synsigra_core_integration_v1" and j.get("generator_git_commit")=="ef2c1d9cd00a07c62617619aa939a6996052867e" and j.get("generator_build_identity")=="signal_synth/"+j["generator_git_commit"] and j.get("generator_binary_sha256","").startswith("sha256:") and len(j["generator_binary_sha256"])==71 for j in done)'
viewer_job=$(printf '%s' "$jobs" | python3 -c \
  'import json,sys; jobs=json.load(sys.stdin)["jobs"]; print(next((j["job_id"] for j in jobs if j.get("status")=="succeeded" and j.get("package_id") and j.get("artifact_status")!="expired"), ""))')
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
printf 'check=artifact-delivery\n'
artifact_pair=$(printf '%s' "$jobs" | python3 -c \
  'import json,sys; jobs=json.load(sys.stdin)["jobs"]; j=next((j for j in jobs if j.get("status")=="succeeded" and j.get("package_id") and j.get("artifact_status")!="expired"), {}); print(j.get("job_id",""), j.get("package_id",""))')
set -- $artifact_pair
artifact_job=${1:-}
artifact_package=${2:-}
if [ -n "$artifact_job" ] && [ -n "$artifact_package" ]; then
  artifact_headers=$(mktemp /tmp/synsigra-artifact-head.XXXXXX)
  artifact_range=$(mktemp /tmp/synsigra-artifact-range.XXXXXX)
  trap 'rm -f "$artifact_headers" "$artifact_range"' EXIT HUP INT TERM
  curl -fsSI --max-time 180 -H "Authorization: Bearer $key" \
    "$base/v1/jobs/$artifact_job/verification-kit.zip" >"$artifact_headers"
  grep -qi '^Accept-Ranges: bytes' "$artifact_headers"
  grep -qi '^ETag: "sha256-[0-9a-f]\{64\}"' "$artifact_headers"
  grep -qi '^X-Checksum-SHA256: [0-9a-f]\{64\}' "$artifact_headers"
  grep -qi '^X-Artifact-Expires-At:' "$artifact_headers"
  range_status=$(curl -sS --max-time 60 -o "$artifact_range" -w '%{http_code}' \
    -H "Authorization: Bearer $key" -H 'Range: bytes=0-127' \
    "$base/v1/artifacts/$artifact_package/package.zip")
  [ "$range_status" = "206" ] || {
    echo "artifact byte-range check returned $range_status" >&2
    exit 1
  }
  [ "$(wc -c < "$artifact_range")" -eq 128 ] || {
    echo "artifact byte-range check returned the wrong length" >&2
    exit 1
  }
  rm -f "$artifact_headers" "$artifact_range"
  trap - EXIT HUP INT TERM
fi
printf 'check=services\n'
sudo systemctl is-active apache22
sudo systemctl is-active nginx.service
sudo systemctl is-active syn_sig_ra_worker.service
printf 'status=live-release-verified\n'
