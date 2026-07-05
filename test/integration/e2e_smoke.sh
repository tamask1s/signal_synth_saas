#!/bin/sh
set -eu

HTTPD_PID=""
WORK_ROOT=""
APACHE_ERROR_LOG=""

info() {
    printf 'e2e: %s\n' "$*"
}

dump_file() {
    path=$1
    label=$2
    if [ -f "$path" ]; then
        printf '\n--- %s (%s) ---\n' "$label" "$path" >&2
        tail -n 120 "$path" >&2 || true
    fi
}

dump_apache_logs() {
    if [ -n "$WORK_ROOT" ]; then
        dump_file "$WORK_ROOT/httpd-config.stdout" "httpd -t stdout"
        dump_file "$WORK_ROOT/httpd-config.stderr" "httpd -t stderr"
        dump_file "$WORK_ROOT/httpd.stdout" "httpd stdout"
        dump_file "$WORK_ROOT/httpd.stderr" "httpd stderr"
    fi
    if [ -n "$APACHE_ERROR_LOG" ]; then
        dump_file "$APACHE_ERROR_LOG" "Apache ErrorLog"
    fi
}

fail() {
    printf 'e2e: ERROR: %s\n' "$*" >&2
    dump_apache_logs
    exit 1
}

cleanup() {
    code=$?
    if [ -n "$HTTPD_PID" ] && kill -0 "$HTTPD_PID" 2>/dev/null; then
        kill "$HTTPD_PID" 2>/dev/null || true
        wait "$HTTPD_PID" 2>/dev/null || true
    fi
    if [ -n "$WORK_ROOT" ] && [ "${SYN_SIG_RA_E2E_KEEP_TMP:-0}" != "1" ]; then
        chmod -R u+w "$WORK_ROOT" 2>/dev/null || true
        rm -rf "$WORK_ROOT"
    elif [ -n "$WORK_ROOT" ]; then
        printf 'e2e: kept temporary directory: %s\n' "$WORK_ROOT" >&2
    fi
    exit "$code"
}

trap cleanup EXIT INT TERM

need_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        fail "missing required command: $1"
    fi
}

detect_apxs() {
    if [ -x /usr/local/apache2/bin/apxs ]; then
        printf '%s\n' /usr/local/apache2/bin/apxs
        return 0
    fi
    if command -v apxs2 >/dev/null 2>&1; then
        command -v apxs2
        return 0
    fi
    if command -v apxs >/dev/null 2>&1; then
        command -v apxs
        return 0
    fi
    return 1
}

detect_httpd() {
    sbin=$("$APXS_EXECUTABLE" -q SBINDIR 2>/dev/null || true)
    target=$("$APXS_EXECUTABLE" -q TARGET 2>/dev/null || true)
    if [ -n "$sbin" ] && [ -n "$target" ] && [ -x "$sbin/$target" ]; then
        printf '%s\n' "$sbin/$target"
        return 0
    fi
    if [ -x /usr/local/apache2/bin/httpd ]; then
        printf '%s\n' /usr/local/apache2/bin/httpd
        return 0
    fi
    if command -v apache2 >/dev/null 2>&1; then
        command -v apache2
        return 0
    fi
    if command -v httpd >/dev/null 2>&1; then
        command -v httpd
        return 0
    fi
    return 1
}

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SIGNAL_SYNTH_ROOT=${SIGNAL_SYNTH_ROOT:-"$REPO_ROOT/../signal_synth"}
SIGNAL_SYNTH_BUILD_DIR=${SIGNAL_SYNTH_BUILD_DIR:-"$SIGNAL_SYNTH_ROOT/build"}
SIGNAL_SYNTH_CLI=${SIGNAL_SYNTH_CLI:-"$SIGNAL_SYNTH_BUILD_DIR/signal-synth"}
SYN_SIG_RA_BUILD_DIR=${SYN_SIG_RA_BUILD_DIR:-"$REPO_ROOT/build/e2e"}
PACK_ROOT=${SYN_SIG_RA_PACK_ROOT:-"$REPO_ROOT/packs"}

need_command cmake
need_command curl
need_command python3

if [ -z "${APXS_EXECUTABLE:-}" ]; then
    APXS_EXECUTABLE=$(detect_apxs) || fail "unable to find apxs/apxs2; set APXS_EXECUTABLE"
fi
if [ ! -x "$APXS_EXECUTABLE" ]; then
    fail "APXS_EXECUTABLE is not executable: $APXS_EXECUTABLE"
fi

if [ -z "${APACHE_HTTPD:-}" ]; then
    APACHE_HTTPD=$(detect_httpd) || fail "unable to find Apache httpd; set APACHE_HTTPD"
fi
if [ ! -x "$APACHE_HTTPD" ]; then
    fail "APACHE_HTTPD is not executable: $APACHE_HTTPD"
fi

if [ ! -d "$SIGNAL_SYNTH_ROOT" ]; then
    fail "sibling signal_synth checkout is missing: $SIGNAL_SYNTH_ROOT"
fi

if [ ! -x "$SIGNAL_SYNTH_CLI" ]; then
    info "building sibling signal_synth CLI"
    cmake \
        -S "$SIGNAL_SYNTH_ROOT" \
        -B "$SIGNAL_SYNTH_BUILD_DIR" \
        -DSIGNAL_SYNTH_BUILD_CLI=ON \
        -DSIGNAL_SYNTH_BUILD_TESTS=OFF
    cmake --build "$SIGNAL_SYNTH_BUILD_DIR" --target signal_synth_cli
fi
if [ ! -x "$SIGNAL_SYNTH_CLI" ]; then
    fail "signal-synth CLI is not executable after build: $SIGNAL_SYNTH_CLI"
fi

MODULE_PATH=${SYN_SIG_RA_MODULE:-"$SYN_SIG_RA_BUILD_DIR/mod_syn_sig_ra.so"}
ADMIN_BIN=${SYN_SIG_RA_ADMIN:-"$SYN_SIG_RA_BUILD_DIR/syn_sig_ra_admin"}
WORKER_BIN=${SYN_SIG_RA_WORKER:-"$SYN_SIG_RA_BUILD_DIR/syn_sig_ra_worker"}

if [ ! -f "$SYN_SIG_RA_BUILD_DIR/CMakeCache.txt" ]; then
    info "building signal_synth_saas module and tools"
    cmake \
        -S "$REPO_ROOT" \
        -B "$SYN_SIG_RA_BUILD_DIR" \
        -DSIGNAL_SYNTH_ROOT="$SIGNAL_SYNTH_ROOT" \
        -DAPXS_EXECUTABLE="$APXS_EXECUTABLE" \
        -DBUILD_TESTING=ON
fi
cmake --build "$SYN_SIG_RA_BUILD_DIR" --target \
    mod_syn_sig_ra \
    syn_sig_ra_admin \
    syn_sig_ra_worker

if [ ! -f "$MODULE_PATH" ]; then
    fail "Apache module was not built: $MODULE_PATH"
fi
if [ ! -x "$ADMIN_BIN" ]; then
    fail "admin tool was not built: $ADMIN_BIN"
fi
if [ ! -x "$WORKER_BIN" ]; then
    fail "worker tool was not built: $WORKER_BIN"
fi

WORK_ROOT=$(mktemp -d /tmp/syn_sig_ra_e2e.XXXXXX)
DATA_ROOT="$WORK_ROOT/data"
SERVER_ROOT="$WORK_ROOT/apache-root"
DOC_ROOT="$WORK_ROOT/htdocs"
RUN_ROOT="$WORK_ROOT/run"
DB_PATH="$DATA_ROOT/db.sqlite3"
APACHE_ERROR_LOG="$WORK_ROOT/apache-error.log"
HTTPD_CONF="$WORK_ROOT/httpd.conf"

mkdir -p \
    "$DATA_ROOT/jobs" \
    "$DATA_ROOT/work" \
    "$DATA_ROOT/packages" \
    "$SERVER_ROOT/logs" \
    "$DOC_ROOT" \
    "$RUN_ROOT"

"$ADMIN_BIN" init-db "$DB_PATH" >/dev/null

API_KEY=$(python3 - <<'PY'
import secrets
print(secrets.token_urlsafe(32))
PY
)
printf '%s\n' "$API_KEY" |
    "$ADMIN_BIN" create-api-key \
        "$DB_PATH" \
        org_e2e \
        user_e2e \
        key_e2e \
        "integration e2e" >/dev/null

PORT=${SYN_SIG_RA_E2E_PORT:-}
if [ -z "$PORT" ]; then
    PORT=$(python3 - <<'PY'
import socket
sock = socket.socket()
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
)
fi
BASE_URL="http://127.0.0.1:$PORT/syn_sig_ra"

cat > "$HTTPD_CONF" <<EOF
ServerRoot "$SERVER_ROOT"
ServerName 127.0.0.1:$PORT
Listen 127.0.0.1:$PORT
PidFile "$RUN_ROOT/httpd.pid"
ErrorLog "$APACHE_ERROR_LOG"
LogLevel warn
DocumentRoot "$DOC_ROOT"

LoadModule syn_sig_ra_module "$MODULE_PATH"

SynSigRaDataRoot "$DATA_ROOT"
SynSigRaSignalSynthCli "$SIGNAL_SYNTH_CLI"
SynSigRaPackRoot "$PACK_ROOT"
SynSigRaPublicBasePath /syn_sig_ra

<Location "/syn_sig_ra">
    SetHandler syn_sig_ra
</Location>
EOF

if ! "$APACHE_HTTPD" -t -f "$HTTPD_CONF" \
    >"$WORK_ROOT/httpd-config.stdout" \
    2>"$WORK_ROOT/httpd-config.stderr"; then
    fail "Apache rejected the generated test configuration"
fi

"$APACHE_HTTPD" -X -f "$HTTPD_CONF" \
    >"$WORK_ROOT/httpd.stdout" \
    2>"$WORK_ROOT/httpd.stderr" &
HTTPD_PID=$!

ready=0
attempt=0
while [ "$attempt" -lt 100 ]; do
    if curl -fsS "$BASE_URL/healthz" >"$WORK_ROOT/healthz.json" 2>"$WORK_ROOT/healthz.err"; then
        ready=1
        break
    fi
    if ! kill -0 "$HTTPD_PID" 2>/dev/null; then
        fail "Apache exited before healthz became available"
    fi
    attempt=$((attempt + 1))
    sleep 0.1
done
if [ "$ready" != "1" ]; then
    fail "Apache healthz did not become available at $BASE_URL/healthz"
fi

curl -fsS "$BASE_URL" >"$WORK_ROOT/ui.html" ||
    fail "web UI HTML request failed"
if ! grep -q 'Challenge package generator' "$WORK_ROOT/ui.html"; then
    dump_file "$WORK_ROOT/ui.html" "web UI HTML"
    fail "web UI HTML did not contain the expected title"
fi
curl -fsS "$BASE_URL/" >"$WORK_ROOT/ui-trailing-slash.html" ||
    fail "web UI trailing-slash HTML request failed"
if ! grep -q 'Challenge package generator' "$WORK_ROOT/ui-trailing-slash.html"; then
    dump_file "$WORK_ROOT/ui-trailing-slash.html" "web UI trailing-slash HTML"
    fail "web UI trailing-slash HTML did not contain the expected title"
fi
curl -fsS "$BASE_URL/ui/app.js" >"$WORK_ROOT/app.js" ||
    fail "web UI JavaScript request failed"
if ! grep -q '^(() => {' "$WORK_ROOT/app.js" ||
    ! grep -q '/v1/jobs' "$WORK_ROOT/app.js"; then
    dump_file "$WORK_ROOT/app.js" "web UI JavaScript"
    fail "web UI JavaScript was not executable or did not contain API wiring"
fi

curl -fsS "$BASE_URL/v1/packs" >"$WORK_ROOT/packs.json" ||
    fail "pack catalog request failed"

CREATE_HTTP=$(
    curl -sS \
        -o "$WORK_ROOT/job-create.json" \
        -w '%{http_code}' \
        -H "Authorization: Bearer $API_KEY" \
        -H "Content-Type: application/json" \
        -d '{"pack_id":"r_peak_stress_v1"}' \
        "$BASE_URL/v1/jobs"
)
if [ "$CREATE_HTTP" != "202" ]; then
    dump_file "$WORK_ROOT/job-create.json" "job create response"
    fail "job create returned HTTP $CREATE_HTTP, expected 202"
fi

JOB_ID=$(
    python3 - "$WORK_ROOT/job-create.json" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    body = json.load(handle)
job_id = body.get("job_id", "")
if not job_id.startswith("job_"):
    raise SystemExit("missing valid job_id")
print(job_id)
PY
) || fail "job create response did not contain a valid job_id"

JOBS_HTTP=$(
    curl -sS \
        -o "$WORK_ROOT/jobs-list.json" \
        -w '%{http_code}' \
        -H "Authorization: Bearer $API_KEY" \
        "$BASE_URL/v1/jobs"
)
if [ "$JOBS_HTTP" != "200" ]; then
    dump_file "$WORK_ROOT/jobs-list.json" "jobs list response"
    fail "jobs list returned HTTP $JOBS_HTTP, expected 200"
fi
python3 - "$WORK_ROOT/jobs-list.json" "$JOB_ID" <<'PY' ||
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    body = json.load(handle)
if not any(job.get("job_id") == sys.argv[2] for job in body.get("jobs", [])):
    raise SystemExit("created job was not found in jobs list")
PY
    fail "jobs list did not contain the created job"

if ! "$WORKER_BIN" run-once "$DB_PATH" "$SIGNAL_SYNTH_CLI" "$PACK_ROOT" "$DATA_ROOT" \
    >"$WORK_ROOT/worker.stdout" \
    2>"$WORK_ROOT/worker.stderr"; then
    dump_file "$WORK_ROOT/worker.stdout" "worker stdout"
    dump_file "$WORK_ROOT/worker.stderr" "worker stderr"
    fail "worker did not complete the queued job"
fi
if ! grep -q '^status=succeeded$' "$WORK_ROOT/worker.stdout"; then
    dump_file "$WORK_ROOT/worker.stdout" "worker stdout"
    fail "worker did not report status=succeeded"
fi

STATUS_HTTP=$(
    curl -sS \
        -o "$WORK_ROOT/job-status.json" \
        -w '%{http_code}' \
        -H "Authorization: Bearer $API_KEY" \
        "$BASE_URL/v1/jobs/$JOB_ID"
)
if [ "$STATUS_HTTP" != "200" ]; then
    dump_file "$WORK_ROOT/job-status.json" "job status response"
    fail "job status returned HTTP $STATUS_HTTP, expected 200"
fi

PACKAGE_ID=$(
    python3 - "$WORK_ROOT/job-status.json" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    body = json.load(handle)
if body.get("status") != "succeeded":
    raise SystemExit("job did not succeed")
package_id = body.get("package_id", "")
if not package_id.startswith("pkg_"):
    raise SystemExit("missing valid package_id")
for key in ("package_fingerprint", "generator_build_identity"):
    value = body.get(key, "")
    if not (isinstance(value, str) and value.startswith("sha256:") and len(value) == 71):
        raise SystemExit("invalid " + key)
for key in ("manifest_url", "archive_url"):
    value = body.get(key, "")
    if not value.startswith("/syn_sig_ra/v1/artifacts/" + package_id + "/"):
        raise SystemExit("invalid " + key)
print(package_id)
PY
) || fail "job status response was not a valid succeeded job"

UNAUTH_HTTP=$(
    curl -sS \
        -o "$WORK_ROOT/unauthorized-artifact.json" \
        -w '%{http_code}' \
        "$BASE_URL/v1/artifacts/$PACKAGE_ID/manifest.json"
)
if [ "$UNAUTH_HTTP" != "401" ]; then
    dump_file "$WORK_ROOT/unauthorized-artifact.json" "unauthorized artifact response"
    fail "unauthenticated artifact request returned HTTP $UNAUTH_HTTP, expected 401"
fi

curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/manifest.json" \
    "$BASE_URL/v1/artifacts/$PACKAGE_ID/manifest.json" ||
    fail "manifest download failed"

curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/package.zip" \
    "$BASE_URL/v1/artifacts/$PACKAGE_ID/package.zip" ||
    fail "package archive download failed"

python3 - "$WORK_ROOT/manifest.json" "$WORK_ROOT/package.zip" <<'PY' ||
import json
import posixpath
import sys
import zipfile

manifest_path, archive_path = sys.argv[1], sys.argv[2]
with open(manifest_path, "r", encoding="utf-8") as handle:
    manifest = json.load(handle)
if manifest.get("schema_version") != 1:
    raise SystemExit("manifest schema_version is not 1")
if manifest.get("package_type") not in ("challenge", "scenario_pack"):
    raise SystemExit("manifest package_type is not a known challenge package type")
if not manifest.get("cases"):
    raise SystemExit("manifest does not contain cases")

with zipfile.ZipFile(archive_path) as archive:
    bad_member = archive.testzip()
    if bad_member is not None:
        raise SystemExit("zip member failed CRC: " + bad_member)
    names = archive.namelist()

if "manifest.json" not in names:
    raise SystemExit("archive does not contain root manifest.json")
if not any(name.startswith("cases/") and name.endswith("/scenario.json") for name in names):
    raise SystemExit("archive does not contain case scenario.json files")
for name in names:
    normalized = posixpath.normpath(name)
    if name.startswith("/") or normalized.startswith("../") or "/../" in normalized:
        raise SystemExit("archive contains unsafe member path: " + name)
PY
    fail "downloaded artifacts failed package layout validation"

printf 'status=e2e-succeeded\n'
printf 'job_id=%s\n' "$JOB_ID"
printf 'package_id=%s\n' "$PACKAGE_ID"
