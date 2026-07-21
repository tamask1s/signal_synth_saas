#!/bin/sh
set -eu

base=${SYN_SIG_RA_BASE_URL:-https://www.timeonion.com/syn_sig_ra}
public_origin=${SYN_SIG_RA_PUBLIC_ORIGIN:-https://www.timeonion.com}

curl -fsS "$base/healthz"
printf '\n'
ready=$(curl -fsS "$base/readyz")
if [ "${SYN_SIG_RA_RUNTIME_ONLY:-0}" = 1 ]; then
  printf '%s' "$ready" | python3 -c \
    'import json,sys; x=json.load(sys.stdin); assert x["status"]=="ready"; assert all(x.get(k) is True for k in ("database","generator","pack_catalog","artifact_store","generation_capacity"))'
  printf '%s\n' "$ready"
  sudo systemctl is-active apache22
  sudo systemctl is-active nginx.service
  sudo systemctl is-active syn_sig_ra_worker.service
  printf 'status=runtime-verified\n'
  exit 0
fi

key=$(sudo cat /root/syn_sig_ra_api_key)
printf '%s' "$ready" | python3 -c \
  'import json,sys; x=json.load(sys.stdin); c=x["accepted_core"]; d=c["contract_document"]; assert x["status"]=="ready"; assert c["integration_contract"]=="synsigra_core_integration_v7"; assert c["git_commit"]=="2531c5c21a1917f9704fa9562d0a32ebacc821da"; assert c["build_identity"]=="signal_synth/"+c["git_commit"]; assert c["cpp_facade"]=="1.5.0" and c["pack_schema_version"]==2; assert c["challenge_package"]=="synsigra_challenge_package_v3"; assert c["scoring_manifest"]=="synsigra_scoring_manifest_v3"; assert c["verification_protocol"]=="synsigra_verification_protocol_v2"; assert c["submission"]=="synsigra_submission_v1"; assert c["submission_formats"]=="synsigra_submission_formats_v2"; assert c["measurement_values"]=="synsigra_measurement_values_v2"; assert c["measurement_truth"]=="synsigra_measurement_truth_v2"; assert c["measurement_scoring"]=="synsigra_measurement_score_v2"; assert c["local_verification"]=="synsigra_local_verification_v3"; assert c["scenario_authoring"]=="synsigra_authoring_v18"; assert c["scenario_templates"]=="synsigra_templates_v5"; assert c["python_verifier"]=="0.11.0"; assert c["external_noise_truth"]=="synsigra_external_noise_truth_v1"; assert d["contract"]==c["integration_contract"] and d["generator"]["git_commit"]==c["git_commit"]'
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
case "$landing" in
  *"AI assistant"*"MCP"*) ;;
  *) echo "landing MCP message is missing" >&2; exit 1 ;;
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
printf 'check=mcp\n'
mcp_setup=$(curl -fsS "$base/mcp-setup")
case "$mcp_setup" in *"MCP assistant"*"$base/mcp"*) ;; *) exit 1 ;; esac
mcp_initialize=$(curl -fsS "$base/mcp" \
  -H "Authorization: Bearer $key" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  --data-binary '{"jsonrpc":"2.0","id":"live-init","method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"live-verify","version":"1"}}}')
printf '%s' "$mcp_initialize" | python3 -c \
  'import json,sys; x=json.load(sys.stdin); r=x["result"]; assert r["protocolVersion"]=="2025-11-25"; assert "tools" in r["capabilities"] and "prompts" in r["capabilities"]; assert r["serverInfo"]["name"]=="synsigra"'
mcp_tools=$(curl -fsS "$base/mcp" \
  -H "Authorization: Bearer $key" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-11-25' \
  --data-binary '{"jsonrpc":"2.0","id":"live-tools","method":"tools/list","params":{}}')
printf '%s' "$mcp_tools" | python3 -c \
  'import json,sys; t={x["name"]:x for x in json.load(sys.stdin)["result"]["tools"]}; required={"synsigra_recommend_packs","synsigra_create_job","synsigra_get_verification_guide","synsigra_get_authoring_contract","synsigra_create_custom_pack"}; assert required<=set(t); assert t["synsigra_create_job"]["annotations"]["readOnlyHint"] is False; assert t["synsigra_recommend_packs"]["annotations"]["readOnlyHint"] is True'
mcp_recommend=$(curl -fsS "$base/mcp" \
  -H "Authorization: Bearer $key" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-11-25' \
  --data-binary '{"jsonrpc":"2.0","id":"live-recommend","method":"tools/call","params":{"name":"synsigra_recommend_packs","arguments":{"goal":"Validate ECG R peaks, RR and HRV LF HF SDNN RMSSD under noise","duration_seconds":300,"sampling_rate_hz":500}}}')
printf '%s' "$mcp_recommend" | python3 -c \
  'import json,sys; r=json.load(sys.stdin)["result"]["structuredContent"]; assert {"r_peak","rr_interval","hrv","signal_quality"}<=set(r["interpreted_targets"]); assert r["candidates"] and r["recommended_workflow"] in {"inspect_top_curated_pack_then_create_job","use_custom_authoring_for_unmet_requirements"}'
verifier=$(curl -fsS -H "Authorization: Bearer $key" "$base/v1/downloads/verifier")
printf '%s' "$verifier" | python3 -c \
  'import json,sys; x=json.load(sys.stdin); assert x["schema_version"]==2 and x["version"]=="0.11.0" and x["generator_included"] is False; assert x["core_git_commit"]=="2531c5c21a1917f9704fa9562d0a32ebacc821da"; assert x["challenge_contract"]=="synsigra_challenge_package_v3" and x["scoring_manifest_contract"]=="synsigra_scoring_manifest_v3"; assert x["submission_contract"]=="synsigra_submission_v1" and x["submission_formats_contract"]=="synsigra_submission_formats_v2"; assert x["measurement_values_contract"]=="synsigra_measurement_values_v2" and x["measurement_truth_contract"]=="synsigra_measurement_truth_v2" and x["measurement_scoring_contract"]=="synsigra_measurement_score_v2" and x["local_verification_contract"]=="synsigra_local_verification_v3"; assert x["verify_example"]=="synsigra-verify challenge submission verification-results --force"'
printf '%s' "$verifier"
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
case "$openapi" in *"/v1/jobs/{job_id}/viewer/window:"*) ;; *) exit 1 ;; esac
case "$openapi" in *"/v1/account/export:"*) ;; *) exit 1 ;; esac
case "$openapi" in *"/mcp:"*"Streamable HTTP MCP"*) ;; *) exit 1 ;; esac
case "$openapi" in *"synsigra_core_integration_v7"*"synsigra_challenge_package_v3"*"ChallengeMetadata:"*) ;; *) exit 1 ;; esac
case "$openapi" in *"detection-templates.zip"*) exit 1 ;; *) ;; esac
viewer_auth_status=$(curl -sS -o /dev/null -w '%{http_code}' \
  "$base/v1/jobs/job_verify_probe/viewer")
[ "$viewer_auth_status" = "401" ] || {
  echo "signal viewer API auth check returned $viewer_auth_status" >&2
  exit 1
}
printf 'check=jobs-and-viewer\n'
packs=$(curl -fsS "$base/v1/packs")
printf '%s' "$packs" | python3 -c \
  'import json,sys; x=json.load(sys.stdin); p=x["packs"]; assert len(p)==18; assert all(v["catalog_version"]=="3.0" and v["catalog_source_sha256"]=="sha256:3a8b53b43dbecdeb834ed3faf0fddb8a859464ff4b822caaaa31830f5a06c88f" and v["integration_contract"]=="synsigra_core_integration_v7" and v["generator_compatibility"]["challenge_package_contract"]=="synsigra_challenge_package_v3" and v["generator_compatibility"]["scoring_manifest_contract"]=="synsigra_scoring_manifest_v3" and v["generator_compatibility"]["submission_contract"]=="synsigra_submission_v1" and v["generator_compatibility"]["verification_protocol_contract"]=="synsigra_verification_protocol_v2" for v in p)'
jobs=$(curl -fsS -H "Authorization: Bearer $key" \
  "$base/v1/jobs?limit=100&offset=0")
printf '%s' "$jobs" | python3 -c \
  'import json,sys; jobs=json.load(sys.stdin)["jobs"]; current=[j for j in jobs if j.get("status")=="succeeded" and j.get("generator_git_commit")=="2531c5c21a1917f9704fa9562d0a32ebacc821da"]; assert current; assert all(j.get("integration_contract")=="synsigra_core_integration_v7" and j.get("generator_build_identity")=="signal_synth/"+j["generator_git_commit"] and j.get("generator_binary_sha256","").startswith("sha256:") and len(j["generator_binary_sha256"])==71 and j.get("challenge",{}).get("challenge_contract")=="synsigra_challenge_package_v3" and j.get("challenge",{}).get("scoring_manifest_contract")=="synsigra_scoring_manifest_v3" and j.get("challenge",{}).get("submission_contract")=="synsigra_submission_v1" and j.get("challenge",{}).get("submission_formats_contract")=="synsigra_submission_formats_v2" and j.get("challenge",{}).get("measurement_values_contract")=="synsigra_measurement_values_v2" and j.get("challenge",{}).get("measurement_truth_contract")=="synsigra_measurement_truth_v2" and j.get("challenge",{}).get("measurement_scoring_contract")=="synsigra_measurement_score_v2" and j.get("challenge",{}).get("local_verification_contract")=="synsigra_local_verification_v3" and j.get("challenge",{}).get("verification",{}).get("mode") in ("evidence","diagnostic") and j.get("challenge",{}).get("integrity",{}).get("ok") is True for j in current)'
viewer_job=$(printf '%s' "$jobs" | python3 -c \
  'import json,sys; jobs=json.load(sys.stdin)["jobs"]; print(next((j["job_id"] for j in jobs if j.get("status")=="succeeded" and j.get("generator_git_commit")=="2531c5c21a1917f9704fa9562d0a32ebacc821da" and j.get("package_id") and j.get("artifact_status")!="expired"), ""))')
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
  'import json,sys; jobs=json.load(sys.stdin)["jobs"]; j=next((j for j in jobs if j.get("status")=="succeeded" and j.get("generator_git_commit")=="2531c5c21a1917f9704fa9562d0a32ebacc821da" and j.get("package_id") and j.get("artifact_status")!="expired"), {}); print(j.get("job_id",""), j.get("package_id",""))')
set -- $artifact_pair
artifact_job=${1:-}
artifact_package=${2:-}
if [ -n "$artifact_job" ] && [ -n "$artifact_package" ]; then
  artifact_headers=$(mktemp /tmp/synsigra-artifact-head.XXXXXX)
  artifact_range=$(mktemp /tmp/synsigra-artifact-range.XXXXXX)
  artifact_kit=$(mktemp /tmp/synsigra-verification-kit.XXXXXX)
  trap 'rm -f "$artifact_headers" "$artifact_range" "$artifact_kit"' EXIT HUP INT TERM
  curl -fsSI --max-time 180 -H "Authorization: Bearer $key" \
    "$base/v1/jobs/$artifact_job/verification-kit.zip" >"$artifact_headers"
  grep -qi '^Accept-Ranges: bytes' "$artifact_headers"
  grep -qi '^ETag: "sha256-[0-9a-f]\{64\}"' "$artifact_headers"
  grep -qi '^X-Checksum-SHA256: [0-9a-f]\{64\}' "$artifact_headers"
  grep -qi '^X-Artifact-Expires-At:' "$artifact_headers"
  curl -fsS --max-time 180 -H "Authorization: Bearer $key" \
    -o "$artifact_kit" "$base/v1/jobs/$artifact_job/verification-kit.zip"
  python3 - "$artifact_kit" <<'PY'
import json
import sys
import zipfile
prefix = "verification-kit/"
with zipfile.ZipFile(sys.argv[1]) as archive:
    assert archive.testzip() is None
    names = set(archive.namelist())
    required = {
        prefix + "README.txt",
        prefix + "challenge/manifest.json",
        prefix + "challenge/ENGINEERING_CLAIM_BOUNDARY.txt",
        prefix + "submission/submission.json",
        prefix + "submission/formats.json",
    }
    assert required <= names
    assert not any(name.endswith("package.zip") for name in names)
    assert prefix + "ENGINEERING_CLAIM_BOUNDARY.txt" not in names
    assert prefix + "challenge-metadata.json" not in names
    manifest = json.loads(archive.read(prefix + "challenge/manifest.json"))
    assert manifest["contract"] == "synsigra_challenge_package_v3"
PY
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
  rm -f "$artifact_headers" "$artifact_range" "$artifact_kit"
  trap - EXIT HUP INT TERM
fi
printf 'check=services\n'
sudo systemctl is-active apache22
sudo systemctl is-active nginx.service
sudo systemctl is-active syn_sig_ra_worker.service
printf 'status=live-release-verified\n'
