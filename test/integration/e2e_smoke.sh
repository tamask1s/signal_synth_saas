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
SYN_SIG_RA_BUILD_DIR=${SYN_SIG_RA_BUILD_DIR:-"$REPO_ROOT/build/e2e"}
SIGNAL_SYNTH_BUILD_DIR=${SIGNAL_SYNTH_BUILD_DIR:-"$SYN_SIG_RA_BUILD_DIR/signal_synth_cli"}
build_signal_synth_cli=0
if [ -z "${SIGNAL_SYNTH_CLI:-}" ]; then
    SIGNAL_SYNTH_CLI="$SIGNAL_SYNTH_BUILD_DIR/signal-synth"
    build_signal_synth_cli=1
fi
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

if [ "$build_signal_synth_cli" = 1 ]; then
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
VERIFIER_ROOT="$WORK_ROOT/verifier"
CHALLENGE_HELPER="$REPO_ROOT/scripts/challenge_artifact.py"
NOISE_ASSET_ROOT="$PACK_ROOT/noise_assets"
RUNTIME_BIN="$WORK_ROOT/runtime-bin"

"$REPO_ROOT/scripts/build_verifier_downloads.sh" "$VERIFIER_ROOT" >/dev/null
VERIFIER_WHEEL="$VERIFIER_ROOT/synsigra-wheel.whl"
[ -f "$VERIFIER_WHEEL" ] || fail "verifier wheel build did not produce an artifact"
[ -x "$CHALLENGE_HELPER" ] || fail "challenge helper is not executable"
[ -d "$NOISE_ASSET_ROOT" ] || fail "approved noise asset root is missing"
mkdir -p "$RUNTIME_BIN"
install -m 0755 "$SIGNAL_SYNTH_CLI" "$RUNTIME_BIN/signal-synth"
install -m 0755 "$CHALLENGE_HELPER" "$RUNTIME_BIN/challenge_artifact.py"
install -m 0644 "$VERIFIER_WHEEL" "$RUNTIME_BIN/synsigra-wheel.whl"
SIGNAL_SYNTH_CLI="$RUNTIME_BIN/signal-synth"
CHALLENGE_HELPER="$RUNTIME_BIN/challenge_artifact.py"
VERIFIER_WHEEL="$RUNTIME_BIN/synsigra-wheel.whl"

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
    "$ADMIN_BIN" bootstrap-owner \
        "$DB_PATH" \
        org_e2e \
        user_e2e \
        e2e@example.test \
        "E2E Owner" \
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
SERVER_USER=${SYN_SIG_RA_E2E_USER:-$(id -un)}
SERVER_GROUP=${SYN_SIG_RA_E2E_GROUP:-$(id -gn)}
EXTRA_MODULE_LOADS=
for module_spec in ${SYN_SIG_RA_E2E_LOAD_MODULES:-}; do
    module_name=${module_spec%%=*}
    module_path=${module_spec#*=}
    case "$module_name" in
        ''|*[!A-Za-z0-9_]*) fail "invalid Apache module name in SYN_SIG_RA_E2E_LOAD_MODULES" ;;
    esac
    if [ "$module_path" = "$module_spec" ] || [ ! -f "$module_path" ]; then
        fail "configured Apache module is missing: $module_name"
    fi
    EXTRA_MODULE_LOADS="${EXTRA_MODULE_LOADS}LoadModule $module_name \"$module_path\"
"
done

cat > "$HTTPD_CONF" <<EOF
ServerRoot "$SERVER_ROOT"
ServerName 127.0.0.1:$PORT
Listen 127.0.0.1:$PORT
User "$SERVER_USER"
Group "$SERVER_GROUP"
PidFile "$RUN_ROOT/httpd.pid"
ErrorLog "$APACHE_ERROR_LOG"
LogLevel warn
DocumentRoot "$DOC_ROOT"

$EXTRA_MODULE_LOADS
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
    ${SYN_SIG_RA_E2E_LOCATION_ACCESS:-}
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
grep -q '"generation_capacity":true' "$WORK_ROOT/ready.json" ||
    fail "readiness endpoint did not report generation disk capacity"
grep -q '"disk_reserve_bytes":' "$WORK_ROOT/ready.json" ||
    fail "readiness endpoint did not report disk reserve"

curl -fsS "$BASE_URL" >"$WORK_ROOT/ui.html" ||
    fail "web UI HTML request failed"
if ! grep -q 'Algorithm QA workspace' "$WORK_ROOT/ui.html" ||
    ! grep -q 'class="product-bar"' "$WORK_ROOT/ui.html" ||
    ! grep -q 'header-account-link' "$WORK_ROOT/ui.html" ||
    ! grep -q 'register-terms' "$WORK_ROOT/ui.html" ||
    ! grep -q '/syn_sig_ra/legal/privacy' "$WORK_ROOT/ui.html" ||
    ! grep -q 'Build custom tests' "$WORK_ROOT/ui.html" ||
    ! grep -q 'MCP assistant' "$WORK_ROOT/ui.html" ||
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
grep -q 'What does your algorithm output?' "$WORK_ROOT/ui-packs.html" ||
    fail "pack chooser route did not expose the goal-first step"
grep -q 'Advanced pack filters' "$WORK_ROOT/ui-packs.html" ||
    fail "pack chooser route did not retain advanced filters"
curl -fsS "$BASE_URL/openapi.yaml" >"$WORK_ROOT/openapi.yaml" ||
    fail "live OpenAPI route request failed"
grep -q '^openapi: 3.0.3' "$WORK_ROOT/openapi.yaml" ||
    fail "live OpenAPI route did not return an OpenAPI document"
grep -q '^  /v1/authoring/schema:' "$WORK_ROOT/openapi.yaml" ||
    fail "live OpenAPI document did not include authoring routes"
grep -q '^  /v1/account:' "$WORK_ROOT/openapi.yaml" ||
    fail "live OpenAPI document did not include account lifecycle routes"
grep -q '^  /v1/jobs:' "$WORK_ROOT/openapi.yaml" ||
    fail "live OpenAPI document did not include job routes"
grep -q '^  /mcp:' "$WORK_ROOT/openapi.yaml" ||
    fail "live OpenAPI document did not include the MCP endpoint"
curl -fsS "$BASE_URL/mcp-setup" >"$WORK_ROOT/ui-mcp.html" ||
    fail "MCP setup route request failed"
grep -q 'https://www.timeonion.com/syn_sig_ra/mcp' "$WORK_ROOT/ui-mcp.html" ||
    fail "MCP setup route did not expose the production endpoint"
curl -fsS "$BASE_URL/verify" >"$WORK_ROOT/ui-verify.html" ||
    fail "verification route request failed"
grep -q 'Verification runbook' "$WORK_ROOT/ui-verify.html" ||
    fail "verification route did not serve the guided UI"
curl -fsS "$BASE_URL/ui/app.js" >"$WORK_ROOT/app.js" ||
    fail "web UI JavaScript request failed"
if ! grep -q '^(() => {' "$WORK_ROOT/app.js" ||
    ! grep -q '/v1/jobs' "$WORK_ROOT/app.js" ||
    ! grep -q 'renderVerificationRunbook' "$WORK_ROOT/app.js" ||
    ! grep -q 'Advanced artifact downloads' "$WORK_ROOT/app.js" ||
    ! grep -q 'prepareVerificationKit' "$WORK_ROOT/app.js" ||
    ! grep -q 'Preparing exact package' "$WORK_ROOT/app.js" ||
    ! grep -q 'validationMessageClass' "$WORK_ROOT/app.js" ||
    ! grep -q 'selectPackForGeneration' "$WORK_ROOT/app.js" ||
    ! grep -q 'packIntentCopy' "$WORK_ROOT/app.js" ||
    ! grep -q 'renderCustomPackReview' "$WORK_ROOT/app.js" ||
    ! grep -q 'applyMissingTargetRequirements' "$WORK_ROOT/app.js" ||
    ! grep -q 'syncInheritedPackTargets' "$WORK_ROOT/app.js" ||
    ! grep -q 'target_intent: targetIntent' "$WORK_ROOT/app.js" ||
    ! grep -q 'conditionEditorHtml' "$WORK_ROOT/app.js" ||
    ! grep -q 'artifactEditorHtml' "$WORK_ROOT/app.js" ||
    ! grep -q 'renderCustomPackAnalysis' "$WORK_ROOT/app.js" ||
    ! grep -q 'saveResponseAsFile' "$WORK_ROOT/app.js" ||
    ! grep -q 'showToast' "$WORK_ROOT/app.js" ||
    ! grep -q 'safeNextPage' "$WORK_ROOT/app.js" ||
    ! grep -q 'private-beta-2026-07-17-r4' "$WORK_ROOT/app.js" ||
    ! grep -q 'navigateTo("packs", { welcome: "1" }, { replace: true })' "$WORK_ROOT/app.js" ||
    ! grep -q 'navigateTo("jobs", { job_id: body.job_id })' "$WORK_ROOT/app.js" ||
    ! grep -q 'focusJobId' "$WORK_ROOT/app.js" ||
    ! grep -q 'data-no-spa' "$WORK_ROOT/app.js" ||
    ! grep -q 'link.hasAttribute("download")' "$WORK_ROOT/app.js" ||
    ! grep -q 'verifyEmailFromLink' "$WORK_ROOT/app.js" ||
    ! grep -q 'completePasswordReset' "$WORK_ROOT/app.js" ||
    ! grep -q 'exportAccount' "$WORK_ROOT/app.js" ||
    ! grep -q 'deleteAccount' "$WORK_ROOT/app.js"; then
    dump_file "$WORK_ROOT/app.js" "web UI JavaScript"
    fail "web UI JavaScript was not executable or did not contain API wiring"
fi
if grep -q -- '--profile' "$WORK_ROOT/app.js" ||
    grep -q 'pre_specified_profile' "$WORK_ROOT/app.js" ||
    ! grep -q -- '--mode diagnostic' "$WORK_ROOT/app.js" ||
    ! grep -q 'evidence_eligible=false' "$WORK_ROOT/app.js"; then
    dump_file "$WORK_ROOT/app.js" "web UI verification workflow"
    fail "web UI exposed a legacy evidence override or omitted diagnostic labelling"
fi

curl -fsS "$BASE_URL/v1/packs" >"$WORK_ROOT/packs.json" ||
    fail "pack catalog request failed"

MCP_ACCEPT='Accept: application/json, text/event-stream'
MCP_VERSION='MCP-Protocol-Version: 2025-11-25'
MCP_UNAUTHORIZED=$(
    curl -sS -o "$WORK_ROOT/mcp-unauthorized.json" -w '%{http_code}' \
        -H "$MCP_ACCEPT" -H 'Content-Type: application/json' \
        -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"e2e","version":"1"}}}' \
        "$BASE_URL/mcp"
)
[ "$MCP_UNAUTHORIZED" = "401" ] ||
    fail "MCP endpoint accepted a request without a Bearer API key"
curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -H "$MCP_ACCEPT" -H 'Content-Type: application/json' \
    -H "Origin: http://127.0.0.1:$PORT" \
    -d '{"jsonrpc":"2.0","id":"e2e-init","method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"e2e","version":"1"}}}' \
    "$BASE_URL/mcp" >"$WORK_ROOT/mcp-initialize.json" ||
    fail "MCP initialize failed through Apache"
grep -q '"protocolVersion":"2025-11-25"' "$WORK_ROOT/mcp-initialize.json" ||
    fail "MCP initialize did not negotiate the current protocol"
curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -H "$MCP_ACCEPT" -H "$MCP_VERSION" -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
    "$BASE_URL/mcp" >"$WORK_ROOT/mcp-tools.json" ||
    fail "MCP tools/list failed through Apache"
grep -q 'synsigra_recommend_packs' "$WORK_ROOT/mcp-tools.json" ||
    fail "MCP tool discovery omitted pack recommendation"
grep -q 'synsigra_get_verification_guide' "$WORK_ROOT/mcp-tools.json" ||
    fail "MCP tool discovery omitted verification guidance"
curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -H "$MCP_ACCEPT" -H "$MCP_VERSION" -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"synsigra_recommend_packs","arguments":{"goal":"Validate ECG R peaks, RR, HRV LF HF SDNN RMSSD under noise","duration_seconds":300,"sampling_rate_hz":500}}}' \
    "$BASE_URL/mcp" >"$WORK_ROOT/mcp-recommend.json" ||
    fail "MCP pack recommendation failed through Apache"
grep -q '"interpreted_targets"' "$WORK_ROOT/mcp-recommend.json" ||
    fail "MCP recommendation omitted interpreted targets"
grep -q '"recommended_workflow"' "$WORK_ROOT/mcp-recommend.json" ||
    fail "MCP recommendation omitted its next workflow"

curl -fsS "$BASE_URL/v1/legal" >"$WORK_ROOT/legal.json" ||
    fail "legal metadata request failed"
grep -q '"terms_version":"private-beta-2026-07-17-r4"' "$WORK_ROOT/legal.json" ||
    fail "legal metadata did not expose current terms"
grep -q '"billing_status":"free_beta"' "$WORK_ROOT/legal.json" ||
    fail "legal metadata did not expose beta billing status"
grep -q '"operator_name":"Kis Tamás"' "$WORK_ROOT/legal.json" ||
    fail "legal metadata did not identify the operator"
grep -q '"support_email":"synsigra@gmail.com"' "$WORK_ROOT/legal.json" ||
    fail "legal metadata did not expose private support"
for legal_page in terms privacy support; do
    curl -fsS "$BASE_URL/legal/$legal_page" >"$WORK_ROOT/legal-$legal_page.html" ||
        fail "legal page request failed: $legal_page"
done
grep -q 'not intended for diagnosis' "$WORK_ROOT/legal-terms.html" ||
    fail "terms page lacks medical-use boundary"
grep -q 'No-PHI' "$WORK_ROOT/legal-privacy.html" ||
    fail "privacy page lacks no-PHI boundary"
grep -q 'no guaranteed uptime' "$WORK_ROOT/legal-support.html" ||
    fail "support page lacks no-SLA boundary"
grep -q 'mailto:synsigra@gmail.com' "$WORK_ROOT/legal-support.html" ||
    fail "support page lacks private email contact"

REGISTER_HTTP=$(
    curl -sS \
        -D "$WORK_ROOT/register.headers" \
        -o "$WORK_ROOT/register.json" \
        -w '%{http_code}' \
        -H "Content-Type: application/json" \
        -d '{"email":"browser@example.com","password":"browser-test-password","display_name":"Browser User","accept_terms":true,"terms_version":"private-beta-2026-07-17-r4"}' \
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
read -r PERSONAL_KEY_ID PERSONAL_KEY <<EOF
$(python3 - "$WORK_ROOT/personal-key.json" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    body = json.load(handle)
print(body.get("api_key_id", ""), body.get("api_key", ""))
PY
)
EOF
if [ -z "$PERSONAL_KEY_ID" ] || [ -z "$PERSONAL_KEY" ]; then
    fail "personal API key secret was not returned once"
fi
curl -fsS -H "Authorization: Bearer $PERSONAL_KEY" \
    "$BASE_URL/v1/projects" >"$WORK_ROOT/personal-key-projects.json" ||
    fail "generated personal API key did not authenticate"

curl -fsS -H "Cookie: $SESSION_COOKIE" -X POST \
    "$BASE_URL/v1/api-keys/$PERSONAL_KEY_ID/rotate" \
    >"$WORK_ROOT/personal-key-rotated.json" ||
    fail "personal API key rotation failed"
ROTATED_KEY=$(python3 - "$WORK_ROOT/personal-key-rotated.json" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    print(json.load(handle).get("api_key", ""))
PY
)
[ -n "$ROTATED_KEY" ] || fail "API key rotation did not return a one-time secret"
OLD_KEY_HTTP=$(curl -sS -o /dev/null -w '%{http_code}' \
    -H "Authorization: Bearer $PERSONAL_KEY" "$BASE_URL/v1/projects")
[ "$OLD_KEY_HTTP" = "401" ] || fail "rotated API key remained active"
curl -fsS -H "Authorization: Bearer $ROTATED_KEY" \
    "$BASE_URL/v1/projects" >"$WORK_ROOT/rotated-key-projects.json" ||
    fail "replacement API key did not authenticate"

curl -fsS -H "Cookie: $SESSION_COOKIE" \
    "$BASE_URL/v1/audit-events?limit=100" >"$WORK_ROOT/audit-events.json" ||
    fail "audit JSON export failed"
python3 - "$WORK_ROOT/audit-events.json" <<'PY' || fail "audit export missed key lifecycle events"
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    events = json.load(handle).get("audit_events", [])
types = {event.get("event_type") for event in events}
required = {"api_key.created", "api_key.rotated", "api_key.authenticated"}
missing = required - types
if missing:
    raise SystemExit("missing audit types: " + ",".join(sorted(missing)))
PY
curl -fsS -H "Cookie: $SESSION_COOKIE" \
    "$BASE_URL/v1/audit-events?format=csv&limit=1000" \
    >"$WORK_ROOT/audit-events.csv" || fail "audit CSV export failed"
grep -q '^id,created_at,event_type,' "$WORK_ROOT/audit-events.csv" ||
    fail "audit CSV header is invalid"
grep -q 'api_key.rotated' "$WORK_ROOT/audit-events.csv" ||
    fail "audit CSV omitted the rotation event"

curl -fsS -X PATCH -H "Cookie: $SESSION_COOKIE" \
    -H "Content-Type: application/json" \
    -d '{"display_name":"Browser User Updated"}' \
    "$BASE_URL/v1/account" >"$WORK_ROOT/profile-updated.json" ||
    fail "signed-in profile update failed"
grep -q '"display_name":"Browser User Updated"' \
    "$WORK_ROOT/profile-updated.json" || fail "profile update was not persisted"
curl -fsS -H "Cookie: $SESSION_COOKIE" \
    "$BASE_URL/v1/account/export" >"$WORK_ROOT/account-export.json" ||
    fail "account export failed"
python3 - "$WORK_ROOT/account-export.json" <<'PY' || fail "account export leaked secrets or omitted owned data"
import json
import pathlib
import sys
text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
body = json.loads(text)
for key in ("account", "projects", "jobs", "scenario_drafts", "custom_packs",
            "api_keys", "legal_acceptances", "audit_events"):
    assert key in body, key
for forbidden in ('"password_hash"', '"password_salt"', '"key_hash"'):
    assert forbidden not in text, forbidden
PY
curl -fsS -D "$WORK_ROOT/password-change.headers" \
    -H "Cookie: $SESSION_COOKIE" -H "Content-Type: application/json" \
    -d '{"current_password":"replacement-browser-password","new_password":"final-browser-password"}' \
    "$BASE_URL/v1/account/password" >"$WORK_ROOT/password-change.json" ||
    fail "signed-in password change failed"
OLD_SESSION_COOKIE=$SESSION_COOKIE
SESSION_COOKIE=$(sed -n 's/^[Ss]et-[Cc]ookie: \([^;]*\).*/\1/p' \
    "$WORK_ROOT/password-change.headers" | tr -d '\r')
[ -n "$SESSION_COOKIE" ] || fail "password change did not issue a replacement session"
OLD_SESSION_HTTP=$(curl -sS -o /dev/null -w '%{http_code}' \
    -H "Cookie: $OLD_SESSION_COOKIE" "$BASE_URL/v1/auth/me")
[ "$OLD_SESSION_HTTP" = "401" ] || fail "password change did not invalidate its prior session"

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
    "$NOISE_ASSET_ROOT" "$CHALLENGE_HELPER" "$VERIFIER_WHEEL" \
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
for key in ("package_fingerprint", "generator_binary_sha256"):
    value = body.get(key, "")
    if not (isinstance(value, str) and value.startswith("sha256:") and len(value) == 71):
        raise SystemExit("invalid " + key)
if body.get("integration_contract") != "synsigra_core_integration_v7":
    raise SystemExit("invalid integration contract")
if body.get("generator_git_commit") != "a80a06179de8c04fdb59732fa922bfc764549df9":
    raise SystemExit("invalid pinned generator commit")
challenge = body.get("challenge", {})
if challenge.get("challenge_contract") != "synsigra_challenge_package_v3":
    raise SystemExit("invalid challenge contract")
if challenge.get("scoring_manifest_contract") != "synsigra_scoring_manifest_v3":
    raise SystemExit("invalid scoring contract")
if challenge.get("submission_contract") != "synsigra_submission_v1":
    raise SystemExit("invalid submission contract")
if challenge.get("submission_formats_contract") != "synsigra_submission_formats_v2":
    raise SystemExit("invalid submission formats contract")
for key, value in {
    "measurement_values_contract": "synsigra_measurement_values_v2",
    "measurement_truth_contract": "synsigra_measurement_truth_v2",
    "measurement_scoring_contract": "synsigra_measurement_score_v2",
    "local_verification_contract": "synsigra_local_verification_v3",
}.items():
    if challenge.get(key) != value:
        raise SystemExit("invalid " + key)
verification = challenge.get("verification", {})
if verification.get("mode") != "diagnostic" or verification.get("evidence_eligible") is not False or verification.get("matrix_complete") is not None or verification.get("protocol") is not None:
    raise SystemExit("protocol-free challenge must be explicitly diagnostic")
if not challenge.get("integrity", {}).get("ok"):
    raise SystemExit("challenge integrity was not verified")
if len(body.get("generator_git_commit", "")) != 40:
    raise SystemExit("invalid generator git commit")
if body.get("generator_build_identity") != "signal_synth/" + body["generator_git_commit"]:
    raise SystemExit("invalid generator build identity")
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

curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -H "$MCP_ACCEPT" -H "$MCP_VERSION" -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":\"verification-guide\",\"method\":\"tools/call\",\"params\":{\"name\":\"synsigra_get_verification_guide\",\"arguments\":{\"job_id\":\"$JOB_ID\"}}}" \
    "$BASE_URL/mcp" >"$WORK_ROOT/mcp-verification-guide.json" ||
    fail "MCP verification guide failed through Apache"
python3 - "$WORK_ROOT/mcp-verification-guide.json" "$JOB_ID" <<'PY' ||
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    response = json.load(handle)
result = response.get("result", {})
guide = result.get("structuredContent", {})
expected_command = (
    "synsigra-verify challenge submission verification-results "
    "--mode diagnostic --force"
)
if guide.get("job_id") != sys.argv[2] or guide.get("status") != "succeeded":
    raise SystemExit("guide omitted concise job identity")
if guide.get("verification_mode") != "diagnostic":
    raise SystemExit("guide omitted diagnostic mode")
if guide.get("evidence_eligible") is not False:
    raise SystemExit("diagnostic guide claimed evidence eligibility")
if guide.get("verification_command") != expected_command:
    raise SystemExit("guide returned the wrong diagnostic command")
if "job" in guide or "evidence_command" in guide:
    raise SystemExit("guide retained a legacy/full job field")
downloads = guide.get("downloads", {})
if not downloads.get("verification_kit_url", "").endswith(
        "/v1/jobs/" + sys.argv[2] + "/verification-kit.zip"):
    raise SystemExit("guide omitted the direct kit URL")
if not downloads.get("verifier_wheel_url", "").endswith(
        "/v1/downloads/verifier/synsigra-0.14.0-py3-none-any.whl"):
    raise SystemExit("guide omitted the canonical verifier wheel URL")
report = guide.get("result", {})
if report.get("entrypoint") != "verification-results/index.html" or \
        report.get("canonical_evidence") != "verification-results/evidence.json":
    raise SystemExit("guide omitted canonical result entry points")
if len(json.dumps(guide, separators=(",", ":"))) >= 8000:
    raise SystemExit("verification guide is not concise")
content = result.get("content", [])
if not content or len(content[0].get("text", "")) >= 1000:
    raise SystemExit("human MCP guide summary is not concise")
PY
    fail "MCP verification guide was not exact and concise"

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
    -o "$WORK_ROOT/verification-kit-a.zip" \
    "$BASE_URL/v1/jobs/$JOB_ID/verification-kit.zip" &
KIT_PID_A=$!
curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/verification-kit-b.zip" \
    "$BASE_URL/v1/jobs/$JOB_ID/verification-kit.zip" &
KIT_PID_B=$!
wait "$KIT_PID_A" || fail "first concurrent verification kit download failed"
wait "$KIT_PID_B" || fail "second concurrent verification kit download failed"
cmp "$WORK_ROOT/verification-kit-a.zip" "$WORK_ROOT/verification-kit-b.zip" ||
    fail "concurrent verification kit requests returned different artifacts"
mv "$WORK_ROOT/verification-kit-a.zip" "$WORK_ROOT/verification-kit.zip"
DERIVED_ZIP_COUNT=$(find \
    "$DATA_ROOT/derived-artifacts/$PACKAGE_ID" \
    -maxdepth 1 -type f -name 'verification-kit-v2.zip' | wc -l)
[ "$DERIVED_ZIP_COUNT" = "1" ] ||
    fail "concurrent kit requests did not publish exactly one immutable ZIP"
if find "$DATA_ROOT/derived-artifacts/$PACKAGE_ID" \
    -maxdepth 1 -name '.build-*' | grep -q .; then
    fail "verification kit preparation left a temporary workspace"
fi

curl -fsSI \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/verification-kit-head.txt" \
    "$BASE_URL/v1/jobs/$JOB_ID/verification-kit.zip" ||
    fail "verification kit HEAD failed"
grep -qi '^Accept-Ranges: bytes' "$WORK_ROOT/verification-kit-head.txt" ||
    fail "verification kit HEAD lacks Accept-Ranges"
grep -qi '^ETag: "sha256-[0-9a-f]\{64\}"' "$WORK_ROOT/verification-kit-head.txt" ||
    fail "verification kit HEAD lacks stable SHA-256 ETag"
grep -qi '^X-Checksum-SHA256: [0-9a-f]\{64\}' "$WORK_ROOT/verification-kit-head.txt" ||
    fail "verification kit HEAD lacks checksum"
grep -qi '^X-Artifact-Expires-At:' "$WORK_ROOT/verification-kit-head.txt" ||
    fail "verification kit HEAD lacks expiry metadata"

curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -H 'Range: bytes=0-127' \
    -D "$WORK_ROOT/verification-kit-range-head.txt" \
    -o "$WORK_ROOT/verification-kit-range.bin" \
    "$BASE_URL/v1/jobs/$JOB_ID/verification-kit.zip" ||
    fail "verification kit byte range failed"
grep -q ' 206 ' "$WORK_ROOT/verification-kit-range-head.txt" ||
    fail "verification kit range did not return HTTP 206"
grep -qi '^Content-Range: bytes 0-127/' "$WORK_ROOT/verification-kit-range-head.txt" ||
    fail "verification kit range lacks Content-Range"
head -c 128 "$WORK_ROOT/verification-kit.zip" \
    > "$WORK_ROOT/verification-kit-range-expected.bin"
cmp "$WORK_ROOT/verification-kit-range-expected.bin" \
    "$WORK_ROOT/verification-kit-range.bin" ||
    fail "verification kit range bytes differ from the complete artifact"

head -c 128 "$WORK_ROOT/verification-kit.zip" \
    > "$WORK_ROOT/verification-kit-resumed.zip"
curl -fsS -C - \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/verification-kit-resumed.zip" \
    "$BASE_URL/v1/jobs/$JOB_ID/verification-kit.zip" ||
    fail "verification kit resume request failed"
cmp "$WORK_ROOT/verification-kit.zip" "$WORK_ROOT/verification-kit-resumed.zip" ||
    fail "resumed verification kit differs from the immutable artifact"

INVALID_RANGE_HTTP=$(curl -sS \
    -H "Authorization: Bearer $API_KEY" \
    -H 'Range: bytes=999999999999-' \
    -o "$WORK_ROOT/invalid-range.json" \
    -w '%{http_code}' \
    "$BASE_URL/v1/jobs/$JOB_ID/verification-kit.zip")
[ "$INVALID_RANGE_HTTP" = "416" ] ||
    fail "invalid verification kit range returned HTTP $INVALID_RANGE_HTTP"

curl -fsSI \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/package-head.txt" \
    "$BASE_URL/v1/artifacts/$PACKAGE_ID/package.zip" ||
    fail "package ZIP HEAD failed"
grep -qi '^Accept-Ranges: bytes' "$WORK_ROOT/package-head.txt" ||
    fail "package ZIP HEAD lacks Accept-Ranges"
grep -qi '^X-Checksum-SHA256: [0-9a-f]\{64\}' "$WORK_ROOT/package-head.txt" ||
    fail "package ZIP HEAD lacks checksum"

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

python3 - "$WORK_ROOT/verification-kit.zip" <<'PY' ||
import json
import sys
import zipfile

kit_path = sys.argv[1]
prefix = "verification-kit/"
expected = {
    prefix + "README.txt",
    prefix + "challenge/manifest.json",
    prefix + "challenge/ENGINEERING_CLAIM_BOUNDARY.txt",
    prefix + "submission/submission.json",
    prefix + "submission/formats.json",
}
with zipfile.ZipFile(kit_path) as archive:
    bad_member = archive.testzip()
    if bad_member is not None:
        raise SystemExit("verification kit member failed CRC: " + bad_member)
    names = set(archive.namelist())
    missing = expected - names
    if missing:
        raise SystemExit("verification kit missing: " + ", ".join(sorted(missing)))
    if any(name.endswith("package.zip") for name in names):
        raise SystemExit("verification kit still contains a redundant nested package ZIP")
    if prefix + "ENGINEERING_CLAIM_BOUNDARY.txt" in names or \
            prefix + "challenge-metadata.json" in names:
        raise SystemExit("verification kit contains redundant top-level metadata")
    readme = archive.read(prefix + "README.txt").decode("utf-8")
    if "synsigra-verify challenge submission verification-results" not in readme:
        raise SystemExit("verification kit README lacks first-run command")
    manifest = json.loads(archive.read(prefix + "challenge/manifest.json"))
    if manifest.get("contract") != "synsigra_challenge_package_v3":
        raise SystemExit("kit challenge contract mismatch")
    if "--mode diagnostic" not in readme:
        raise SystemExit("protocol-free kit README does not state diagnostic mode")
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
for key, value in {
    "version": "0.14.0",
    "measurement_values_contract": "synsigra_measurement_values_v2",
    "measurement_truth_contract": "synsigra_measurement_truth_v2",
    "measurement_scoring_contract": "synsigra_measurement_score_v2",
    "local_verification_contract": "synsigra_local_verification_v3",
}.items():
    if metadata.get(key) != value:
        raise SystemExit("verifier metadata mismatch: " + key)
files = {item.get("filename") for item in metadata.get("files", [])}
if "synsigra-verifier.zip" not in files or "synsigra-0.14.0-py3-none-any.whl" not in files:
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
json.dump({"name": "E2E clean draft", "scenario": scenario, "target_intent": ["r_peak"]}, sys.stdout)
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
    "$NOISE_ASSET_ROOT" "$CHALLENGE_HELPER" "$VERIFIER_WHEEL" \
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
if body.get("integration_contract") != "synsigra_core_integration_v7":
    raise SystemExit("custom pack job used the wrong integration contract")
if body.get("generator_git_commit") != "a80a06179de8c04fdb59732fa922bfc764549df9":
    raise SystemExit("custom pack job used the wrong generator commit")
challenge = body.get("challenge", {})
for key, value in {
    "challenge_contract": "synsigra_challenge_package_v3",
    "scoring_manifest_contract": "synsigra_scoring_manifest_v3",
    "submission_contract": "synsigra_submission_v1",
    "submission_formats_contract": "synsigra_submission_formats_v2",
    "measurement_values_contract": "synsigra_measurement_values_v2",
    "measurement_truth_contract": "synsigra_measurement_truth_v2",
    "measurement_scoring_contract": "synsigra_measurement_score_v2",
    "local_verification_contract": "synsigra_local_verification_v3",
}.items():
    if challenge.get(key) != value:
        raise SystemExit("custom pack challenge mismatch: " + key)
verification = challenge.get("verification", {})
if verification.get("mode") != "diagnostic" or verification.get("evidence_eligible") is not False or verification.get("matrix_complete") is not None or verification.get("protocol") is not None:
    raise SystemExit("custom pack must use explicit diagnostic verification")
if not challenge.get("integrity", {}).get("ok"):
    raise SystemExit("custom pack challenge integrity was not verified")
print(body["package_id"])
PY
) || fail "custom pack job response was invalid"
curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/custom-manifest.json" \
    "$BASE_URL/v1/artifacts/$CUSTOM_PACKAGE_ID/manifest.json" ||
    fail "custom pack manifest download failed"
curl -fsS \
    -H "Authorization: Bearer $API_KEY" \
    -o "$WORK_ROOT/custom-verification-kit.zip" \
    "$BASE_URL/v1/jobs/$CUSTOM_JOB_ID/verification-kit.zip" ||
    fail "custom pack verification kit download failed"
python3 - "$WORK_ROOT/custom-verification-kit.zip" <<'PY' ||
import json
import sys
import zipfile

prefix = "verification-kit/"
with zipfile.ZipFile(sys.argv[1]) as archive:
    if archive.testzip() is not None:
        raise SystemExit("custom verification kit failed CRC validation")
    names = set(archive.namelist())
    required = {
        prefix + "README.txt",
        prefix + "challenge/manifest.json",
        prefix + "challenge/ENGINEERING_CLAIM_BOUNDARY.txt",
        prefix + "submission/submission.json",
        prefix + "submission/formats.json",
    }
    if not required <= names:
        raise SystemExit("custom verification kit is incomplete")
    if any(name.endswith("package.zip") for name in names):
        raise SystemExit("custom verification kit contains a nested package ZIP")
    if prefix + "ENGINEERING_CLAIM_BOUNDARY.txt" in names or \
            prefix + "challenge-metadata.json" in names:
        raise SystemExit("custom verification kit contains redundant top-level metadata")
    readme = archive.read(prefix + "README.txt").decode("utf-8")
    if "--mode diagnostic" not in readme:
        raise SystemExit("custom verification kit lacks the diagnostic command")
    manifest = json.loads(archive.read(prefix + "challenge/manifest.json"))
    if manifest.get("contract") != "synsigra_challenge_package_v3":
        raise SystemExit("custom verification kit has the wrong challenge contract")
PY
    fail "custom pack verification kit validation failed"

curl -fsS -X DELETE -H "Cookie: $SESSION_COOKIE" \
    -H "Content-Type: application/json" \
    -d '{"current_password":"final-browser-password","confirmation":"DELETE MY ACCOUNT"}' \
    "$BASE_URL/v1/account" >"$WORK_ROOT/account-deleted.json" ||
    fail "password-confirmed account deletion failed"
grep -q '"status":"deleted"' "$WORK_ROOT/account-deleted.json" ||
    fail "account deletion did not return a deletion receipt"
DELETED_SESSION_HTTP=$(curl -sS -o /dev/null -w '%{http_code}' \
    -H "Cookie: $SESSION_COOKIE" "$BASE_URL/v1/auth/me")
[ "$DELETED_SESSION_HTTP" = "401" ] ||
    fail "deleted account session remained active"
DELETED_KEY_HTTP=$(curl -sS -o /dev/null -w '%{http_code}' \
    -H "Authorization: Bearer $ROTATED_KEY" "$BASE_URL/v1/projects")
[ "$DELETED_KEY_HTTP" = "401" ] ||
    fail "deleted account API key remained active"

printf 'status=e2e-succeeded\n'
printf 'job_id=%s\n' "$JOB_ID"
printf 'package_id=%s\n' "$PACKAGE_ID"
printf 'custom_package_id=%s\n' "$CUSTOM_PACKAGE_ID"
