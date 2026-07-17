#!/bin/sh
set -eu

base=${SYN_SIG_RA_PUBLIC_BASE:-https://www.timeonion.com/syn_sig_ra}
key=$(sudo cat /root/syn_sig_ra_api_key)
authorization="Authorization: Bearer $key"

jobs=$(curl -fsS -H "$authorization" "$base/v1/jobs?limit=100")
job_id=$(printf '%s' "$jobs" | python3 -c 'import json,sys
jobs=json.load(sys.stdin).get("jobs",[])
matches=[item for item in jobs if item.get("status")=="succeeded" and item.get("package_id") and item.get("artifact_status")!="expired"]
if not matches: raise SystemExit("no retained succeeded job")
print(matches[0]["job_id"])')

description=$(curl -fsS -H "$authorization" "$base/v1/jobs/$job_id/viewer")
case_id=$(printf '%s' "$description" | python3 -c 'import json,sys
cases=json.load(sys.stdin).get("cases",[])
if not cases: raise SystemExit("no viewer cases")
print(cases[0]["case_id"])')

overlays=$(curl -fsS -H "$authorization" \
  "$base/v1/jobs/$job_id/viewer/overlays?case_id=$case_id&start_sample=0&sample_count=5000&max_items=4000")
printf '%s' "$overlays" | python3 -c 'import json,sys
data=json.load(sys.stdin)
assert data.get("schema_version")==1
assert isinstance(data.get("available_kinds"),list)
assert len(data.get("items",[]))<=4000
print("status=viewer-overlays-ok")
print("available_kinds="+",".join(data["available_kinds"]))
print("items="+str(len(data["items"])))
print("aggregated="+str(bool(data.get("aggregated"))).lower())'

page=$(curl -fsS "$base/viewer")
printf '%s' "$page" | grep -q 'Ground truth overlays'
printf '%s' "$page" | grep -q 'Local algorithm output'
printf '%s' "$page" | grep -q 'id="spacing-in"'
printf '%s' "$page" | grep -q 'style.css?v=4'
library=$(curl -fsS "$base/viewer/signal-viewer.js")
printf '%s' "$library" | grep -q 'SignalWindowCache'
printf '%s' "$library" | grep -q 'setChannelSpacing'
application=$(curl -fsS "$base/viewer/app.js")
printf '%s' "$application" | grep -q 'file was not uploaded'
printf '%s' "$application" | grep -q 'setEmptyState'
printf '%s' "$application" | grep -q "available.includes('r_peak')"
stylesheet=$(curl -fsS "$base/viewer/style.css")
printf '%s' "$stylesheet" | grep -q 'height: calc(100dvh - 76px)'
printf '%s' "$stylesheet" | grep -q 'position: sticky'
printf 'status=viewer-ui-ok\njob_id=%s\ncase_id=%s\n' "$job_id" "$case_id"
