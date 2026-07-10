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
MAIL_ROOT="$WORK_ROOT/mail"
DB_PATH="$DATA_ROOT/db.sqlite3"
APACHE_ERROR_LOG="$WORK_ROOT/apache-error.log"
HTTPD_CONF="$WORK_ROOT/httpd.conf"

mkdir -p \
    "$DATA_ROOT/jobs" \
    "$DATA_ROOT/work" \
    "$DATA_ROOT/packages" \
    "$SERVER_ROOT/logs" \
    "$DOC_ROOT" \
    "$RUN_ROOT" \
    "$MAIL_ROOT"

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
SynSigRaEmailTransport capture_file
SynSigRaEmailPublicOrigin "$BASE_URL"
SynSigRaEmailFrom noreply@example.test
SynSigRaEmailFromName "Synsigra Test"
SynSigRaEmailCaptureDirectory "$MAIL_ROOT"

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
curl -fsS "$BASE_URL/readyz" >"$WORK_ROOT/ready.json" ||
    fail "readiness request failed"
grep -q '"status":"ready"' "$WORK_ROOT/ready.json" ||
    fail "readiness endpoint did not report ready"

curl -fsS "$BASE_URL" >"$WORK_ROOT/ui.html" ||
    fail "web UI HTML request failed"
if ! grep -q 'Algorithm QA workspace' "$WORK_ROOT/ui.html" ||
    ! grep -q 'class="product-bar"' "$WORK_ROOT/ui.html" ||
    ! grep -q 'header-account-link' "$WORK_ROOT/ui.html" ||
    ! grep -q 'Build custom tests' "$WORK_ROOT/ui.html" ||
    ! grep -q 'verification-runbook' "$WORK_ROOT/ui.html" ||
    ! grep -q 'custom-pack-review' "$WORK_ROOT/ui.html" ||
    ! grep -q 'custom-pack-scenario-search' "$WORK_ROOT/ui.html" ||
    ! grep -q 'scenario-groups' "$WORK_ROOT/ui.html" ||
    ! grep -q 'Advanced JSON editor' "$WORK_ROOT/ui.html"; then
    dump_file "$WORK_ROOT/ui.html" "web UI HTML"
    fail "web UI HTML did not contain the expected title"
fi
curl -fsS "$BASE_URL/" >"$WORK_ROOT/ui-trailing-slash.html" ||
    fail "web UI trailing-slash HTML request failed"
if ! grep -q 'Algorithm QA workspace' "$WORK_ROOT/ui-trailing-slash.html"; then
    dump_file "$WORK_ROOT/ui-trailing-slash.html" "web UI trailing-slash HTML"
    fail "web UI trailing-slash HTML did not contain the expected title"
fi
curl -fsS "$BASE_URL/packs" >"$WORK_ROOT/ui-packs.html" ||
    fail "pack chooser route request failed"
grep -q 'Choose a challenge pack' "$WORK_ROOT/ui-packs.html" ||
    fail "pack chooser route did not serve the guided UI"
curl -fsS "$BASE_URL/verify" >"$WORK_ROOT/ui-verify.html" ||
    fail "verification route request failed"
grep -q 'Verification runbook' "$WORK_ROOT/ui-verify.html" ||
    fail "verification route did not serve the guided UI"
curl -fsS "$BASE_URL/ui/app.js" >"$WORK_ROOT/app.js" ||
    fail "web UI JavaScript request failed"
if ! grep -q '^(() => {' "$WORK_ROOT/app.js" ||
    ! grep -q '/v1/jobs' "$WORK_ROOT/app.js" ||
    ! grep -q 'renderVerificationRunbook' "$WORK_ROOT/app.js" ||
    ! grep -q 'selectPackForGeneration' "$WORK_ROOT/app.js" ||
    ! grep -q 'renderCustomPackReview' "$WORK_ROOT/app.js" ||
    ! grep -q 'conditionEditorHtml' "$WORK_ROOT/app.js" ||
    ! grep -q 'artifactEditorHtml' "$WORK_ROOT/app.js" ||
    ! grep -q 'renderCustomPackAnalysis' "$WORK_ROOT/app.js" ||
    ! grep -q 'saveResponseAsFile' "$WORK_ROOT/app.js" ||
    ! grep -q 'showToast' "$WORK_ROOT/app.js" ||
    ! grep -q 'safeNextPage' "$WORK_ROOT/app.js" ||
    ! grep -q 'navigateTo("packs", { welcome: "1" }, { replace: true })' "$WORK_ROOT/app.js" ||
    ! grep -q 'navigateTo("jobs", { job_id: body.job_id })' "$WORK_ROOT/app.js" ||
    ! grep -q 'focusJobId' "$WORK_ROOT/app.js" ||
    ! grep -q 'data-no-spa' "$WORK_ROOT/app.js" ||
    ! grep -q 'link.hasAttribute("download")' "$WORK_ROOT/app.js" ||
    ! grep -q 'verifyEmailFromLink' "$WORK_ROOT/app.js" ||
    ! grep -q 'completePasswordReset' "$WORK_ROOT/app.js"; then
    dump_file "$WORK_ROOT/app.js" "web UI JavaScript"
    fail "web UI JavaScript was not executable or did not contain API wiring"
fi

curl -fsS "$BASE_URL/v1/packs" >"$WORK_ROOT/packs.json" ||
    fail "pack catalog request failed"

REGISTER_HTTP=$(
    curl -sS \
        -D "$WORK_ROOT/register.headers" \
        -o "$WORK_ROOT/register.json" \
        -w '%{http_code}' \
        -H "Content-Type: application/json" \
        -d '{"email":"browser@example.com","password":"browser-test-password","display_name":"Browser User"}' \
        "$BASE_URL/v1/auth/register"
)
if [ "$REGISTER_HTTP" != "202" ]; then
    dump_file "$WORK_ROOT/register.json" "account registration response"
    fail "account registration returned HTTP $REGISTER_HTTP"
fi
grep -q '"status":"verification_required"' "$WORK_ROOT/register.json" ||
    fail "account registration did not require email verification"
VERIFY_TOKEN=$(python3 - "$MAIL_ROOT" <<'PY'
import pathlib
import re
import sys
for path in sorted(pathlib.Path(sys.argv[1]).glob("*.eml")):
    text = path.read_text(encoding="utf-8")
    match = re.search(r"[?&]verify=([^&\s]+)", text)
    if match:
        print(match.group(1))
        path.unlink()
        break
PY
)
[ -n "$VERIFY_TOKEN" ] || fail "verification email did not contain a token"
VERIFY_HTTP=$(
    curl -sS \
        -D "$WORK_ROOT/verify-email.headers" \
        -o "$WORK_ROOT/verify-email.json" \
        -w '%{http_code}' \
        -H "Content-Type: application/json" \
        -d "{\"token\":\"$VERIFY_TOKEN\"}" \
        "$BASE_URL/v1/auth/verify-email"
)
[ "$VERIFY_HTTP" = "200" ] || fail "email verification returned HTTP $VERIFY_HTTP"
grep -q '"email_verified":true' "$WORK_ROOT/verify-email.json" ||
    fail "email verification did not mark the account verified"
SESSION_COOKIE=$(sed -n 's/^[Ss]et-[Cc]ookie: \([^;]*\).*/\1/p' \
    "$WORK_ROOT/verify-email.headers" | tr -d '\r')
[ -n "$SESSION_COOKIE" ] || fail "email verification did not return a session cookie"
curl -fsS -H "Cookie: $SESSION_COOKIE" \
    "$BASE_URL/v1/auth/me" >"$WORK_ROOT/account.json" ||
    fail "session account lookup failed"
grep -q '"email":"browser@example.com"' "$WORK_ROOT/account.json" ||
    fail "session account lookup returned the wrong account"

curl -fsS \
    -H "Content-Type: application/json" \
    -d '{"email":"browser@example.com"}' \
    "$BASE_URL/v1/auth/password-reset/request" \
    >"$WORK_ROOT/reset-request.json" ||
    fail "password reset request failed"
grep -q '"status":"accepted"' "$WORK_ROOT/reset-request.json" ||
    fail "password reset request did not return the generic response"
RESET_TOKEN=$(python3 - "$MAIL_ROOT" <<'PY'
import pathlib
import re
import sys
for path in sorted(pathlib.Path(sys.argv[1]).glob("*.eml")):
    text = path.read_text(encoding="utf-8")
    match = re.search(r"[?&]reset=([^&\s]+)", text)
    if match:
        print(match.group(1))
        path.unlink()
        break
PY
)
[ -n "$RESET_TOKEN" ] || fail "password reset email did not contain a token"
OLD_SESSION_COOKIE=$SESSION_COOKIE
RESET_HTTP=$(
    curl -sS \
        -D "$WORK_ROOT/reset-complete.headers" \
        -o "$WORK_ROOT/reset-complete.json" \
        -w '%{http_code}' \
        -H "Content-Type: application/json" \
        -d "{\"token\":\"$RESET_TOKEN\",\"password\":\"replacement-browser-password\"}" \
        "$BASE_URL/v1/auth/password-reset/complete"
)
[ "$RESET_HTTP" = "200" ] || fail "password reset completion returned HTTP $RESET_HTTP"
OLD_SESSION_HTTP=$(curl -sS -o /dev/null -w '%{http_code}' \
    -H "Cookie: $OLD_SESSION_COOKIE" "$BASE_URL/v1/auth/me")
[ "$OLD_SESSION_HTTP" = "401" ] || fail "password reset did not invalidate the old session"
SESSION_COOKIE=$(sed -n 's/^[Ss]et-[Cc]ookie: \([^;]*\).*/\1/p' \
    "$WORK_ROOT/reset-complete.headers" | tr -d '\r')
[ -n "$SESSION_COOKIE" ] || fail "password reset did not return a new session cookie"

curl -fsS -H "Cookie: $SESSION_COOKIE" \
    -H "Content-Type: application/json" \
    -d '{"label":"e2e automation"}' \
    "$BASE_URL/v1/api-keys" >"$WORK_ROOT/personal-key.json" ||
    fail "personal API key creation failed"
PERSONAL_KEY=$(python3 - "$WORK_ROOT/personal-key.json" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    print(json.load(handle).get("api_key", ""))
PY
)
if [ -z "$PERSONAL_KEY" ]; then
    fail "personal API key secret was not returned once"
fi
curl -fsS -H "Authorization: Bearer $PERSONAL_KEY" \
    "$BASE_URL/v1/projects" >"$WORK_ROOT/personal-key-projects.json" ||
    fail "generated personal API key did not authenticate"

CREATE_HTTP=$(
    curl -sS \
        -o "$WORK_ROOT/job-create.json" \
        -w '%{http_code}' \
        -H "Authorization: Bearer $API_KEY" \
        -H "Content-Type: application/json" \
        -d '{"project_id":"org_e2e_default","pack_id":"r_peak_stress_v1"}' \
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
verification_kit_url = body.get("verification_kit_url", "")
if verification_kit_url != "/syn_sig_ra/v1/jobs/" + body.get("job_id", "") + "/verification-kit.zip":
    raise SystemExit("invalid verification_kit_url")
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

curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/detection-templates.zip" \
    "$BASE_URL/v1/jobs/$JOB_ID/detection-templates.zip" ||
    fail "detection template archive download failed"

curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/verification-kit.zip" \
    "$BASE_URL/v1/jobs/$JOB_ID/verification-kit.zip" ||
    fail "verification kit archive download failed"

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
if "provenance.json" not in names:
    raise SystemExit("archive does not contain root provenance.json")
if "ENGINEERING_CLAIM_BOUNDARY.txt" not in names:
    raise SystemExit("archive does not contain engineering claim boundary")
if not any(name.startswith("cases/") and name.endswith("/scenario.json") for name in names):
    raise SystemExit("archive does not contain case scenario.json files")
for name in names:
    normalized = posixpath.normpath(name)
    if name.startswith("/") or normalized.startswith("../") or "/../" in normalized:
        raise SystemExit("archive contains unsafe member path: " + name)
PY
    fail "downloaded artifacts failed package layout validation"

python3 - "$WORK_ROOT/detection-templates.zip" "$PACKAGE_ID" <<'PY' ||
import sys
import zipfile

archive_path, package_id = sys.argv[1], sys.argv[2]
expected = {
    "README.md",
    "detections/clean_70_r_peak.csv",
    "detections/slow_45_r_peak.csv",
    "detections/fast_120_r_peak.csv",
    "detections/baseline_powerline_r_peak.csv",
}
with zipfile.ZipFile(archive_path) as archive:
    bad_member = archive.testzip()
    if bad_member is not None:
        raise SystemExit("template zip member failed CRC: " + bad_member)
    names = set(archive.namelist())
    missing = expected - names
    if missing:
        raise SystemExit("template zip missing: " + ", ".join(sorted(missing)))
    readme = archive.read("README.md").decode("utf-8")
    if "synsigra-verify" not in readme or package_id not in readme:
        raise SystemExit("template README does not match verifier workflow")
    csv = archive.read("detections/clean_70_r_peak.csv").decode("utf-8")
    if not csv.startswith("time_seconds,sample_index,channel,label,confidence\n"):
        raise SystemExit("R-peak template CSV header is invalid")
PY
    fail "detection template archive failed validation"

python3 - "$WORK_ROOT/verification-kit.zip" "$WORK_ROOT/package.zip" <<'PY' ||
import sys
import zipfile

kit_path, package_path = sys.argv[1], sys.argv[2]
expected = {
    "README.md",
    "manifest.json",
    "package.zip",
    "detections/clean_70_r_peak.csv",
    "detections/slow_45_r_peak.csv",
    "detections/fast_120_r_peak.csv",
    "detections/baseline_powerline_r_peak.csv",
}
with zipfile.ZipFile(kit_path) as archive:
    bad_member = archive.testzip()
    if bad_member is not None:
        raise SystemExit("verification kit member failed CRC: " + bad_member)
    names = set(archive.namelist())
    missing = expected - names
    if missing:
        raise SystemExit("verification kit missing: " + ", ".join(sorted(missing)))
    readme = archive.read("README.md").decode("utf-8")
    if "synsigra-verify package.zip detections/" not in readme:
        raise SystemExit("verification kit README lacks first-run command")
    nested_package = archive.read("package.zip")
with open(package_path, "rb") as handle:
    if nested_package != handle.read():
        raise SystemExit("verification kit package.zip differs from artifact download")
with zipfile.ZipFile(__import__("io").BytesIO(nested_package)) as package_zip:
    bad_member = package_zip.testzip()
    if bad_member is not None:
        raise SystemExit("nested package zip member failed CRC: " + bad_member)
    if "manifest.json" not in set(package_zip.namelist()):
        raise SystemExit("nested package zip does not contain manifest.json")
    if "provenance.json" not in set(package_zip.namelist()):
        raise SystemExit("nested package zip does not contain provenance.json")
    if "ENGINEERING_CLAIM_BOUNDARY.txt" not in set(package_zip.namelist()):
        raise SystemExit("nested package zip does not contain claim boundary")
PY
    fail "verification kit archive failed validation"

curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/verifier-downloads.json" \
    "$BASE_URL/v1/downloads/verifier" ||
    fail "verifier download metadata failed"

python3 - "$WORK_ROOT/verifier-downloads.json" <<'PY' ||
import json
import sys

metadata = json.load(open(sys.argv[1], "r", encoding="utf-8"))
if metadata.get("package") != "synsigra":
    raise SystemExit("unexpected verifier package")
if metadata.get("generator_included") is not False:
    raise SystemExit("verifier download must not include generator")
files = {item.get("filename") for item in metadata.get("files", [])}
if "synsigra-verifier.zip" not in files or "synsigra-wheel.whl" not in files:
    raise SystemExit("expected verifier bundle and wheel metadata")
PY
    fail "verifier download metadata validation failed"

curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/synsigra-verifier.zip" \
    "$BASE_URL/v1/downloads/verifier/synsigra-verifier.zip" ||
    fail "verifier bundle download failed"

python3 - "$WORK_ROOT/synsigra-verifier.zip" <<'PY' ||
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1]) as archive:
    bad = archive.testzip()
    if bad:
        raise SystemExit("bad verifier zip member: " + bad)
    names = set(archive.namelist())
    if "README.md" not in names or "verify_smoke.sh" not in names:
        raise SystemExit("verifier bundle missing helper files")
    if not any(name.startswith("wheels/synsigra-") and name.endswith(".whl") for name in names):
        raise SystemExit("verifier bundle missing wheel")
PY
    fail "verifier bundle validation failed"

DELETE_HTTP=$(
    curl -sS \
        -o "$WORK_ROOT/job-delete.json" \
        -w '%{http_code}' \
        -X DELETE \
        -H "Authorization: Bearer $API_KEY" \
        "$BASE_URL/v1/jobs/$JOB_ID"
)
if [ "$DELETE_HTTP" != "200" ]; then
    dump_file "$WORK_ROOT/job-delete.json" "job delete response"
    fail "job delete returned HTTP $DELETE_HTTP, expected 200"
fi

POST_DELETE_JOB_HTTP=$(
    curl -sS \
        -o "$WORK_ROOT/job-after-delete.json" \
        -w '%{http_code}' \
        -H "Authorization: Bearer $API_KEY" \
        "$BASE_URL/v1/jobs/$JOB_ID"
)
if [ "$POST_DELETE_JOB_HTTP" != "404" ]; then
    dump_file "$WORK_ROOT/job-after-delete.json" "job after delete response"
    fail "deleted job returned HTTP $POST_DELETE_JOB_HTTP, expected 404"
fi

POST_DELETE_ARTIFACT_HTTP=$(
    curl -sS \
        -o "$WORK_ROOT/artifact-after-delete.json" \
        -w '%{http_code}' \
        -H "Authorization: Bearer $API_KEY" \
        "$BASE_URL/v1/artifacts/$PACKAGE_ID/manifest.json"
)
if [ "$POST_DELETE_ARTIFACT_HTTP" != "404" ]; then
    dump_file "$WORK_ROOT/artifact-after-delete.json" "artifact after delete response"
    fail "deleted job artifact returned HTTP $POST_DELETE_ARTIFACT_HTTP, expected 404"
fi

curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/jobs-after-delete.json" \
    "$BASE_URL/v1/jobs" ||
    fail "jobs list after delete failed"
python3 - "$WORK_ROOT/jobs-after-delete.json" "$JOB_ID" <<'PY' ||
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    body = json.load(handle)
if any(job.get("job_id") == sys.argv[2] for job in body.get("jobs", [])):
    raise SystemExit("deleted job was still present in jobs list")
PY
    fail "jobs list still contained the deleted job"

python3 - "$SIGNAL_SYNTH_ROOT/examples/scenarios/ecg_clean.json" \
    >"$WORK_ROOT/scenario-create-request.json" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    scenario = json.load(handle)
json.dump({"name": "E2E clean draft", "scenario": scenario}, sys.stdout)
PY
curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -H "Content-Type: application/json" \
    --data-binary "@$WORK_ROOT/scenario-create-request.json" \
    -o "$WORK_ROOT/scenario-created.json" \
    "$BASE_URL/v1/scenarios" ||
    fail "scenario draft creation failed"
SCENARIO_ID=$(python3 - "$WORK_ROOT/scenario-created.json" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["scenario_id"])
PY
)
python3 - "$SCENARIO_ID" >"$WORK_ROOT/custom-pack-incompatible-request.json" <<'PY'
import json
import sys
json.dump({
    "name": "E2E incompatible pack",
    "description": "Must be rejected before snapshot creation",
    "targets": ["ppg_systolic_peak"],
    "scenario_ids": [sys.argv[1]],
}, sys.stdout)
PY
INCOMPATIBLE_PACK_HTTP=$(
    curl -sS \
        -H "Authorization: Bearer $API_KEY" \
        -H "Content-Type: application/json" \
        --data-binary "@$WORK_ROOT/custom-pack-incompatible-request.json" \
        -o "$WORK_ROOT/custom-pack-incompatible-response.json" \
        -w '%{http_code}' \
        "$BASE_URL/v1/custom-packs"
)
if [ "$INCOMPATIBLE_PACK_HTTP" != "422" ]; then
    dump_file "$WORK_ROOT/custom-pack-incompatible-response.json" \
        "incompatible custom pack response"
    fail "incompatible custom pack returned HTTP $INCOMPATIBLE_PACK_HTTP, expected 422"
fi
grep -q '"custom_pack_incompatible"' \
    "$WORK_ROOT/custom-pack-incompatible-response.json" ||
    fail "incompatible custom pack response did not include the expected code"
python3 - "$SCENARIO_ID" >"$WORK_ROOT/custom-pack-request.json" <<'PY'
import json
import sys
json.dump({
    "name": "E2E custom pack",
    "description": "Custom pack integration smoke",
    "targets": ["r_peak"],
    "scenario_ids": [sys.argv[1]],
}, sys.stdout)
PY
curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -H "Content-Type: application/json" \
    --data-binary "@$WORK_ROOT/custom-pack-request.json" \
    -o "$WORK_ROOT/custom-pack-created.json" \
    "$BASE_URL/v1/custom-packs" ||
    fail "custom pack creation failed"
CUSTOM_PACK_ID=$(python3 - "$WORK_ROOT/custom-pack-created.json" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["pack_id"])
PY
)
printf '{"project_id":"org_e2e_default","pack_id":"%s"}' "$CUSTOM_PACK_ID" \
    >"$WORK_ROOT/custom-job-request.json"
curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -H "Content-Type: application/json" \
    --data-binary "@$WORK_ROOT/custom-job-request.json" \
    -o "$WORK_ROOT/custom-job-created.json" \
    "$BASE_URL/v1/jobs" ||
    fail "custom pack job creation failed"
CUSTOM_JOB_ID=$(python3 - "$WORK_ROOT/custom-job-created.json" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["job_id"])
PY
)
"$WORKER_BIN" run-once "$DB_PATH" "$SIGNAL_SYNTH_CLI" "$PACK_ROOT" "$DATA_ROOT" \
    >"$WORK_ROOT/custom-worker.stdout" \
    2>"$WORK_ROOT/custom-worker.stderr" || {
    dump_file "$WORK_ROOT/custom-worker.stdout" "custom worker stdout"
    dump_file "$WORK_ROOT/custom-worker.stderr" "custom worker stderr"
    curl -sS -H "Authorization: Bearer $API_KEY" \
        "$BASE_URL/v1/jobs/$CUSTOM_JOB_ID" >&2 || true
    fail "custom pack worker run failed"
}
curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/custom-job-status.json" \
    "$BASE_URL/v1/jobs/$CUSTOM_JOB_ID" ||
    fail "custom pack job status failed"
CUSTOM_PACKAGE_ID=$(python3 - "$WORK_ROOT/custom-job-status.json" <<'PY'
import json
import sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
if body.get("status") != "succeeded":
    raise SystemExit("custom pack job did not succeed")
print(body["package_id"])
PY
) || fail "custom pack job response was invalid"
curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/custom-manifest.json" \
    "$BASE_URL/v1/artifacts/$CUSTOM_PACKAGE_ID/manifest.json" ||
    fail "custom pack manifest download failed"

printf 'status=e2e-succeeded\n'
printf 'job_id=%s\n' "$JOB_ID"
printf 'package_id=%s\n' "$PACKAGE_ID"
printf 'custom_package_id=%s\n' "$CUSTOM_PACKAGE_ID"
