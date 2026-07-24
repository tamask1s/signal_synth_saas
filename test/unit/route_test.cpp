#include "syn_sig_ra/route.h"

#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/runtime_config.h"
#include "syn_sig_ra/sha256.h"

#include <unistd.h>
#include <dirent.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output << content;
    require(static_cast<bool>(output), "test fixture should be writable: " + path);
}

std::string take_email_token(
    const std::string& directory,
    const std::string& parameter
) {
    DIR* stream = opendir(directory.c_str());
    require(stream != nullptr, "email capture directory should be readable");
    std::string token;
    for (dirent* entry = readdir(stream); entry != nullptr;
         entry = readdir(stream)) {
        const std::string name(entry->d_name);
        if (name.size() < 4 || name.substr(name.size() - 4) != ".eml") continue;
        const std::string path = directory + "/" + name;
        std::ifstream input(path.c_str(), std::ios::binary);
        std::ostringstream content;
        content << input.rdbuf();
        const std::string marker = parameter + "=";
        const std::string::size_type start = content.str().find(marker);
        if (start == std::string::npos) continue;
        const std::string::size_type value_start = start + marker.size();
        const std::string::size_type end = content.str().find_first_of(
            "& \r\n", value_start);
        token = content.str().substr(value_start, end - value_start);
        std::remove(path.c_str());
        break;
    }
    closedir(stream);
    require(!token.empty(), "captured email should contain a " + parameter + " token");
    return token;
}

}  // namespace

int main() {
    const syn_sig_ra::RouteResponse health =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/healthz");
    require(
        health.disposition == syn_sig_ra::RouteDisposition::handled,
        "health route should be handled"
    );
    require(health.status == 200, "health route should return HTTP 200");
    require(
        health.content_type == "application/json",
        "health route should return JSON"
    );
    require(
        health.body.find("\"service\":\"signal_synth_saas\"") !=
            std::string::npos,
        "health response should identify the service"
    );
    require(
        health.body.find("\"status\":\"ok\"") != std::string::npos,
        "health response should report ok"
    );
    require(
        health.body.find("\"build\":") != std::string::npos,
        "health response should include build metadata"
    );

    const syn_sig_ra::RouteResponse openapi =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/openapi.yaml");
    require(
        openapi.status == 200 &&
            openapi.content_type.find("application/yaml") != std::string::npos &&
            openapi.cache_control == "public, max-age=300" &&
            openapi.body.find("openapi: 3.0.3") != std::string::npos &&
            openapi.body.find("/v1/authoring/schema:") != std::string::npos &&
            openapi.body.find("/v1/account:") != std::string::npos &&
            openapi.body.find("/v1/account/export:") != std::string::npos &&
            openapi.body.find("/mcp:") != std::string::npos &&
            openapi.body.find("/v1/jobs:") != std::string::npos &&
            openapi.body.find("/v1/jobs/{job_id}/viewer/window:") !=
                std::string::npos &&
            openapi.body.find("/v1/jobs/{job_id}/viewer/overlays:") !=
                std::string::npos &&
            openapi.body.find("application/vnd.synsigra.signal-window.v1") !=
                std::string::npos,
        "live OpenAPI route should expose the complete embedded API contract"
    );
    require(
        syn_sig_ra::route_request("POST", "/syn_sig_ra/openapi.yaml").status == 405,
        "live OpenAPI route should be read-only"
    );

    const syn_sig_ra::RouteResponse wrong_method =
        syn_sig_ra::route_request("POST", "/syn_sig_ra/healthz");
    require(
        wrong_method.status == 405,
        "health route should reject methods other than GET"
    );

    const syn_sig_ra::RouteResponse ui =
        syn_sig_ra::route_request("GET", "/syn_sig_ra");
    require(ui.status == 200, "base route should serve the web UI");
    require(
        ui.content_type.find("text/html") != std::string::npos &&
            ui.body.find("Algorithm QA workspace") != std::string::npos &&
            ui.body.find("class=\"product-bar\"") != std::string::npos &&
            ui.body.find("Build custom tests") != std::string::npos &&
            ui.body.find("MCP assistant") != std::string::npos &&
            ui.body.find("header-account-link") != std::string::npos &&
            ui.body.find("app-toast") != std::string::npos &&
            ui.body.find("What do you want to do next?") != std::string::npos &&
            ui.body.find("A test space, not a single waveform") !=
                std::string::npos &&
            ui.body.find("142</strong><span>authoring fields") !=
                std::string::npos &&
            ui.body.find("A pack is a validated slice") != std::string::npos &&
            ui.body.find("Noise is a test condition") != std::string::npos &&
            ui.body.find("pack-target-goals") != std::string::npos &&
            ui.body.find("pack-intent-goals") != std::string::npos &&
            ui.body.find("Advanced pack filters") != std::string::npos &&
            ui.body.find("pack-comparison") == std::string::npos &&
            ui.body.find("verification-runbook") != std::string::npos &&
            ui.body.find("runbook-job-select") != std::string::npos &&
            ui.body.find("readiness-status") != std::string::npos &&
            ui.body.find("metrics-panel") != std::string::npos &&
            ui.body.find("load-more-jobs") != std::string::npos &&
            ui.body.find("load-scenario-template") != std::string::npos &&
            ui.body.find("verifier-downloads") != std::string::npos &&
            ui.body.find("scenario-template-select") != std::string::npos &&
            ui.body.find("What should your algorithm detect or measure?") != std::string::npos &&
            ui.body.find("show-all-scenario-sources") != std::string::npos &&
            ui.body.find("inherited-pack-targets") != std::string::npos &&
            ui.body.find("Advanced target override") != std::string::npos &&
            ui.body.find("Advanced JSON editor") != std::string::npos &&
            ui.body.find("custom-pack-review") != std::string::npos &&
            ui.body.find("custom-pack-scenario-search") != std::string::npos &&
            ui.body.find("scenario-groups") != std::string::npos &&
            ui.body.find("register-email") != std::string::npos &&
            ui.body.find("register-terms") != std::string::npos &&
            ui.body.find("profile-display-name") != std::string::npos &&
            ui.body.find("change-password") != std::string::npos &&
            ui.body.find("export-account") != std::string::npos &&
            ui.body.find("delete-account-confirmation") != std::string::npos &&
            ui.body.find("/syn_sig_ra/legal/terms") != std::string::npos &&
            ui.body.find("/syn_sig_ra/legal/privacy") != std::string::npos &&
            ui.body.find("save-key") == std::string::npos &&
            ui.body.find("/syn_sig_ra/docs/api") != std::string::npos &&
            ui.body.find("No PHI") != std::string::npos &&
            ui.body.find("Evidence path") != std::string::npos &&
            ui.body.find("/syn_sig_ra/viewer") != std::string::npos &&
            ui.body.find("SynSigRa") == std::string::npos &&
            ui.body.find("Sinsigra") == std::string::npos,
        "web UI route should return HTML"
    );
    const syn_sig_ra::RouteResponse packs_page =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/packs");
    require(
        packs_page.status == 200 &&
            packs_page.body.find("Choose a challenge pack") != std::string::npos,
        "pack chooser route should serve the web UI"
    );
    const syn_sig_ra::RouteResponse verify_page =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/verify");
    require(
        verify_page.status == 200 &&
            verify_page.body.find("Verification runbook") != std::string::npos,
        "verification route should serve the web UI"
    );
    const syn_sig_ra::RouteResponse legal_metadata =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/v1/legal");
    require(
        legal_metadata.status == 200 &&
            legal_metadata.body.find("private-beta-2026-07-17-r4") !=
                std::string::npos &&
            legal_metadata.body.find("\"operator_name\":\"Kis Tamás\"") !=
                std::string::npos &&
            legal_metadata.body.find("\"support_email\":\"synsigra@gmail.com\"") !=
                std::string::npos &&
            legal_metadata.body.find("\"billing_status\":\"free_beta\"") !=
                std::string::npos &&
            legal_metadata.body.find("\"uptime_sla\":null") !=
                std::string::npos,
        "public legal metadata should expose the current beta contract"
    );
    const syn_sig_ra::RouteResponse legal_terms =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/legal/terms");
    const syn_sig_ra::RouteResponse legal_privacy =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/legal/privacy");
    const syn_sig_ra::RouteResponse legal_support =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/legal/support");
    require(
        legal_terms.status == 200 &&
            legal_terms.body.find("Private Beta Terms") != std::string::npos &&
            legal_terms.body.find("not intended for diagnosis") !=
                std::string::npos &&
            legal_privacy.status == 200 &&
            legal_privacy.body.find("Privacy and No-PHI") != std::string::npos &&
            legal_support.status == 200 &&
            legal_support.body.find("no guaranteed uptime") != std::string::npos &&
            legal_support.body.find("No payment method") != std::string::npos &&
            legal_support.body.find("mailto:synsigra@gmail.com") !=
                std::string::npos,
        "public legal and support pages should state the beta boundaries"
    );
    const syn_sig_ra::RouteResponse docs_api =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/docs/api");
    require(
        docs_api.status == 200 &&
            docs_api.content_type.find("text/html") != std::string::npos &&
            docs_api.body.find("Rendered API reference") != std::string::npos &&
            docs_api.body.find("/v1/downloads/verifier") != std::string::npos &&
            docs_api.body.find("/v1/authoring/preview") != std::string::npos &&
            docs_api.body.find("verification-kit.zip") != std::string::npos &&
            docs_api.body.find("role-selected challenge and submission-template kit") !=
                std::string::npos &&
            docs_api.body.find("/v1/jobs/{job_id}/viewer/window") !=
                std::string::npos &&
            docs_api.body.find("/v1/jobs/{job_id}/viewer/overlays") !=
                std::string::npos,
        "rendered API docs should be served"
    );
    const syn_sig_ra::RouteResponse docs_quickstart =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/docs/quickstart");
    require(
        docs_quickstart.status == 200 &&
            docs_quickstart.body.find("One-page quickstart") != std::string::npos &&
            docs_quickstart.body.find("synsigra-verify") != std::string::npos,
        "quickstart docs should be served"
    );
    const syn_sig_ra::RouteResponse docs_troubleshooting =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/docs/troubleshooting");
    require(
        docs_troubleshooting.status == 200 &&
            docs_troubleshooting.body.find("401 unauthorized") != std::string::npos &&
            docs_troubleshooting.body.find("Verifier exit") != std::string::npos,
        "troubleshooting docs should be served"
    );
    const syn_sig_ra::RouteResponse ui_js =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/ui/app.js");
    require(
        ui_js.status == 200 &&
            ui_js.content_type.find("javascript") != std::string::npos &&
            ui_js.body.find("(() => {") == 0 &&
            ui_js.body.find("loadMetrics") != std::string::npos &&
            ui_js.body.find("loadAuthoring") != std::string::npos &&
            ui_js.body.find("loadVerifierDownloads") != std::string::npos &&
            ui_js.body.find("jobsNextOffset") != std::string::npos &&
            ui_js.body.find("cleanEcgTemplate") != std::string::npos &&
            ui_js.body.find("Verification kit ZIP") != std::string::npos &&
            ui_js.body.find("Advanced artifact downloads") != std::string::npos &&
            ui_js.body.find("prepareVerificationKit") != std::string::npos &&
            ui_js.body.find("Preparing exact package") != std::string::npos &&
            ui_js.body.find("REFERENCE_ONLY_TARGET") != std::string::npos &&
            ui_js.body.find("validationMessageClass") != std::string::npos &&
            ui_js.body.find("Detection templates ZIP") == std::string::npos &&
            ui_js.body.find("--profile") == std::string::npos &&
            ui_js.body.find("pre_specified_profile") == std::string::npos &&
            ui_js.body.find("--mode diagnostic") != std::string::npos &&
            ui_js.body.find("evidence_eligible=false") != std::string::npos &&
            ui_js.body.find("Reproducibility details") != std::string::npos &&
            ui_js.body.find("renderVerificationRunbook") != std::string::npos &&
            ui_js.body.find("selectPackForGeneration") != std::string::npos &&
            ui_js.body.find("packIntentCopy") != std::string::npos &&
            ui_js.body.find("First-run local verification recipe") != std::string::npos &&
            ui_js.body.find("<details class=\"verify-note\" open>\n        <summary>First-run local verification recipe") == std::string::npos &&
            ui_js.body.find("renderCustomPackReview") != std::string::npos &&
            ui_js.body.find("applyMissingTargetRequirements") != std::string::npos &&
            ui_js.body.find("syncInheritedPackTargets") != std::string::npos &&
            ui_js.body.find("target_intent: targetIntent") != std::string::npos &&
            ui_js.body.find("groupValidationErrors") != std::string::npos &&
            ui_js.body.find("saveResponseAsFile") != std::string::npos &&
            ui_js.body.find("startNativeDownload") != std::string::npos &&
            ui_js.body.find("method: \"HEAD\"") != std::string::npos &&
            ui_js.body.find("response.blob()") != std::string::npos &&
            ui_js.body.find(
                "Browser download started. Interrupted ZIP downloads support resume."
            ) != std::string::npos &&
            ui_js.body.find("showToast") != std::string::npos &&
            ui_js.body.find("safeNextPage") != std::string::npos &&
            ui_js.body.find("navigateTo(\"jobs\", { job_id: body.job_id })") !=
                std::string::npos &&
            ui_js.body.find(
                "navigateTo(\"packs\", { welcome: \"1\" }, { replace: true })"
            ) != std::string::npos &&
            ui_js.body.find("focusJobId") != std::string::npos &&
            ui_js.body.find("data-no-spa") != std::string::npos &&
            ui_js.body.find("link.hasAttribute(\"download\")") !=
                std::string::npos &&
            ui_js.body.find("url.protocol !== \"http:\"") !=
                std::string::npos &&
            ui_js.body.find("Independent per-case evidence protocol") !=
                std::string::npos &&
            ui_js.body.find("independent case verdicts") != std::string::npos,
        "web UI JavaScript asset should be served as an executable IIFE"
    );
    const syn_sig_ra::RouteResponse ui_css =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/ui/style.css");
    require(
        ui_css.status == 200 &&
            ui_css.content_type.find("text/css") != std::string::npos &&
            ui_css.body.find("Synsigra landing-aligned application shell") !=
                std::string::npos &&
            ui_css.body.find(".product-bar") != std::string::npos &&
            ui_css.body.find("#07111f") != std::string::npos,
        "web UI stylesheet should expose the landing-aligned application shell"
    );
    const syn_sig_ra::RouteResponse viewer_page =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/viewer");
    const syn_sig_ra::RouteResponse viewer_library =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/viewer/signal-viewer.js");
    const syn_sig_ra::RouteResponse viewer_app =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/viewer/app.js");
    const syn_sig_ra::RouteResponse viewer_css =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/viewer/style.css");
    require(
        viewer_page.status == 200 &&
            viewer_page.cache_control == "no-store" &&
            viewer_page.body.find("Synsigra Lab") != std::string::npos &&
            viewer_page.body.find("Ground truth overlays") != std::string::npos &&
            viewer_page.body.find("Local algorithm output") != std::string::npos &&
            viewer_page.body.find("spacing-in") != std::string::npos &&
            viewer_page.body.find("style.css?v=5") != std::string::npos &&
            viewer_page.body.find("__SYNSIGRA_BASE__") == std::string::npos &&
            viewer_page.body.find("/syn_sig_ra/viewer/app.js") != std::string::npos &&
            viewer_library.status == 200 &&
            viewer_library.body.find("HttpSignalDataSource") != std::string::npos &&
            viewer_library.body.find("decodeSignalWindow") != std::string::npos &&
            viewer_library.body.find("options.describePath") != std::string::npos &&
            viewer_library.body.find("options.windowPath") != std::string::npos &&
            viewer_library.body.find("options.overlayPath") != std::string::npos &&
            viewer_library.body.find("SignalWindowCache") != std::string::npos &&
            viewer_library.body.find("visibleBucketRange") != std::string::npos &&
            viewer_library.body.find("Math.max(centerY - halfHeight") ==
                std::string::npos &&
            viewer_app.status == 200 &&
            viewer_app.body.find("AbortController") != std::string::npos &&
            viewer_app.body.find("client cache") != std::string::npos &&
            viewer_app.body.find("file was not uploaded") != std::string::npos &&
            viewer_app.body.find("setEmptyState") != std::string::npos &&
            viewer_app.body.find("available.includes('r_peak')") != std::string::npos &&
            viewer_app.body.find("CHANNEL_SPACING_STEP = 1.5") != std::string::npos &&
            viewer_app.body.find("prefetchedRequest") != std::string::npos &&
            viewer_app.body.find("immediate ? 0 : 140") != std::string::npos &&
            viewer_css.status == 200 &&
            viewer_css.body.find(".vertical-controls") != std::string::npos &&
            viewer_css.body.find("height: max(430px, calc(100dvh - 140px))") != std::string::npos &&
            viewer_css.body.find("top: 76px") == std::string::npos,
        "signal viewer routes should serve the portable viewer and SaaS adapter"
    );
    require(
        syn_sig_ra::route_request("POST", "/syn_sig_ra/viewer").status == 405,
        "signal viewer assets should be read-only"
    );
    const syn_sig_ra::RouteResponse ui_trailing_slash =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/");
    require(
        ui_trailing_slash.status == 200,
        "base route with trailing slash should serve the web UI"
    );

    const syn_sig_ra::RouteResponse missing =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/v1/missing");
    require(
        missing.status == 404,
        "unknown routes under the owned prefix should return HTTP 404"
    );

    const syn_sig_ra::RouteResponse outside =
        syn_sig_ra::route_request("GET", "/unrelated");
    require(
        outside.disposition == syn_sig_ra::RouteDisposition::declined,
        "unrelated paths should be declined"
    );

    const syn_sig_ra::RouteResponse similar_prefix =
        syn_sig_ra::route_request("GET", "/syn_sig_rabbit/healthz");
    require(
        similar_prefix.disposition == syn_sig_ra::RouteDisposition::declined,
        "a similar path prefix should not be claimed"
    );

    const syn_sig_ra::RouteResponse custom_base = syn_sig_ra::route_request(
        "GET",
        "/syn_sig_ra/internal/healthz",
        "/syn_sig_ra/internal"
    );
    require(
        custom_base.status == 200,
        "a configured base path should move the health route"
    );

    const syn_sig_ra::RouteResponse old_base = syn_sig_ra::route_request(
        "GET",
        "/syn_sig_ra/healthz",
        "/syn_sig_ra/internal"
    );
    require(
        old_base.disposition == syn_sig_ra::RouteDisposition::declined,
        "a configured child base should not claim its former health route"
    );

    const syn_sig_ra::RouteResponse missing_storage =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/jobs/job_test",
            "/syn_sig_ra",
            "",
            nullptr
        );
    require(
        missing_storage.status == 503,
        "protected routes should fail safely when metadata is unavailable"
    );

    const std::string database_path =
        "/tmp/syn_sig_ra_route_auth_" + std::to_string(getpid()) + ".sqlite3";
    std::remove(database_path.c_str());
    syn_sig_ra::MetadataStore store(database_path);
    const std::string email_capture =
        "/tmp/syn_sig_ra_route_mail_" + std::to_string(getpid());
    mkdir(email_capture.c_str(), 0700);
    syn_sig_ra::EmailConfig email_config;
    email_config.transport = syn_sig_ra::EmailTransport::capture_file;
    email_config.public_origin = "https://example.test";
    email_config.from_email = "noreply@example.test";
    email_config.from_name = "Synsigra Test";
    email_config.capture_directory = email_capture;
    std::string key_hash;
    std::string error;
    require(
        syn_sig_ra::sha256_hex("route-test-key", key_hash, error),
        "route test key hashing should succeed"
    );
    syn_sig_ra::ApiKeyIdentity identity;
    identity.api_key_id = "route_key";
    identity.organization_id = "route_org";
    identity.user_id = "route_user";
    identity.role = "owner";
    require(
        store.bootstrap_owner(
            identity, "bootstrap@example.test", "Bootstrap Owner",
            key_hash, "route test", error),
        "route test API key creation should succeed: " + error
    );

    const syn_sig_ra::RouteResponse missing_terms =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/auth/register",
            "/syn_sig_ra",
            "",
            &store,
            "",
            "application/json",
            "{\"email\":\"missing-terms@example.com\","
            "\"password\":\"long-enough-password\","
            "\"display_name\":\"Missing Terms\"}",
            "",
            "",
            "",
            "",
            email_config
        );
    require(
        missing_terms.status == 400 &&
            missing_terms.body.find("terms_acceptance_required") !=
                std::string::npos,
        "registration should reject missing current terms acceptance"
    );

    const syn_sig_ra::RouteResponse registered =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/auth/register",
            "/syn_sig_ra",
            "",
            &store,
            "",
            "application/json",
            "{\"email\":\"new@example.com\","
            "\"password\":\"long-enough-password\","
            "\"display_name\":\"New User\","
            "\"accept_terms\":true,"
            "\"terms_version\":\"private-beta-2026-07-17-r4\"}",
            "",
            "",
            "",
            "",
            email_config
        );
    require(
        registered.status == 202 &&
            registered.body.find("verification_required") != std::string::npos &&
            registered.set_cookie.empty() &&
            registered.cache_control == "no-store",
        "registration should require deliverable email verification"
    );
    const std::string verification_token =
        take_email_token(email_capture, "verify");
    const syn_sig_ra::RouteResponse verified =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/auth/verify-email",
            "/syn_sig_ra",
            "",
            &store,
            "",
            "application/json",
            "{\"token\":\"" + verification_token + "\"}",
            "",
            "",
            "",
            "",
            email_config
        );
    require(
        verified.status == 200 &&
            verified.body.find("\"email_verified\":true") != std::string::npos &&
            verified.set_cookie.find("Secure") != std::string::npos &&
            verified.set_cookie.find("HttpOnly") != std::string::npos,
        "single-use email token should verify and sign in the account: status=" +
            std::to_string(verified.status) + " body=" + verified.body +
            " internal=" + verified.internal_error
    );
    const syn_sig_ra::RouteResponse account =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/auth/me",
            "/syn_sig_ra",
            "",
            &store,
            "",
            "",
            "",
            "",
            "",
            "",
            verified.set_cookie
        );
    require(
        account.status == 200 &&
            account.body.find("\"display_name\":\"New User\"") !=
                std::string::npos,
        "session cookie should authorize account identity"
    );
    const syn_sig_ra::RouteResponse personal_key =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/api-keys",
            "/syn_sig_ra",
            "",
            &store,
            "",
            "application/json",
            "{\"label\":\"test CI\"}",
            "",
            "",
            "",
            verified.set_cookie
        );
    require(
        personal_key.status == 201 &&
            personal_key.body.find("\"api_key\":\"ssk_") != std::string::npos,
        "signed-in account should create a one-time API key"
    );

    const syn_sig_ra::RouteResponse reset_requested =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/auth/password-reset/request",
            "/syn_sig_ra", "", &store, "", "application/json",
            "{\"email\":\"new@example.com\"}",
            "", "", "", "", email_config
        );
    require(
        reset_requested.status == 202 &&
            reset_requested.body.find("If the account is eligible") !=
                std::string::npos,
        "password reset request should use a generic response"
    );
    const std::string reset_token = take_email_token(email_capture, "reset");
    const syn_sig_ra::RouteResponse reset_completed =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/auth/password-reset/complete",
            "/syn_sig_ra", "", &store, "", "application/json",
            "{\"token\":\"" + reset_token +
                "\",\"password\":\"replacement-password-123\"}",
            "", "", "", "", email_config
        );
    require(
        reset_completed.status == 200 &&
            reset_completed.set_cookie.find("HttpOnly") != std::string::npos,
        "single-use reset token should change the password and issue a session"
    );
    const syn_sig_ra::RouteResponse reused_reset =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/auth/password-reset/complete",
            "/syn_sig_ra", "", &store, "", "application/json",
            "{\"token\":\"" + reset_token +
                "\",\"password\":\"another-password-123\"}"
        );
    require(
        reused_reset.status == 400,
        "consumed reset token should not be reusable"
    );

    const syn_sig_ra::RouteResponse profile_updated =
        syn_sig_ra::route_request(
            "PATCH", "/syn_sig_ra/v1/account", "/syn_sig_ra", "", &store,
            "", "application/json", "{\"display_name\":\"Updated User\"}",
            "", "", "", reset_completed.set_cookie
        );
    require(
        profile_updated.status == 200 &&
            profile_updated.body.find("\"display_name\":\"Updated User\"") !=
                std::string::npos,
        "signed-in users should update their display name"
    );

    const syn_sig_ra::RouteResponse account_export =
        syn_sig_ra::route_request(
            "GET", "/syn_sig_ra/v1/account/export", "/syn_sig_ra", "", &store,
            "", "", "", "", "", "", reset_completed.set_cookie
        );
    require(
        account_export.status == 200 &&
            account_export.content_disposition.find(
                "synsigra-account-export.json") != std::string::npos &&
            account_export.body.find("\"display_name\":\"Updated User\"") !=
                std::string::npos &&
            account_export.body.find("\"projects\"") != std::string::npos &&
            account_export.body.find("\"jobs\"") != std::string::npos &&
            account_export.body.find("\"scenario_drafts\"") !=
                std::string::npos &&
            account_export.body.find("\"legal_acceptances\"") !=
                std::string::npos &&
            account_export.body.find("\"audit_events\"") !=
                std::string::npos &&
            account_export.body.find("\"password_hash\"") ==
                std::string::npos &&
            account_export.body.find("\"password_salt\"") ==
                std::string::npos &&
            account_export.body.find("\"key_hash\"") == std::string::npos,
        "account export should contain owned data and omit credentials"
    );

    const syn_sig_ra::RouteResponse wrong_current_password =
        syn_sig_ra::route_request(
            "POST", "/syn_sig_ra/v1/account/password", "/syn_sig_ra", "",
            &store, "", "application/json",
            "{\"current_password\":\"wrong-password\","
            "\"new_password\":\"changed-password-456\"}",
            "", "", "", reset_completed.set_cookie
        );
    require(
        wrong_current_password.status == 401,
        "password change should require the current password"
    );

    const syn_sig_ra::RouteResponse password_changed =
        syn_sig_ra::route_request(
            "POST", "/syn_sig_ra/v1/account/password", "/syn_sig_ra", "",
            &store, "", "application/json",
            "{\"current_password\":\"replacement-password-123\","
            "\"new_password\":\"changed-password-456\"}",
            "", "", "", reset_completed.set_cookie
        );
    require(
        password_changed.status == 200 &&
            password_changed.set_cookie.find("HttpOnly") != std::string::npos,
        "password change should invalidate sessions and issue a replacement session"
    );
    require(
        syn_sig_ra::route_request(
            "GET", "/syn_sig_ra/v1/auth/me", "/syn_sig_ra", "", &store,
            "", "", "", "", "", "", reset_completed.set_cookie
        ).status == 401,
        "the session used before a password change should be invalidated"
    );
    require(
        syn_sig_ra::route_request(
            "GET", "/syn_sig_ra/v1/auth/me", "/syn_sig_ra", "", &store,
            "", "", "", "", "", "", password_changed.set_cookie
        ).status == 200,
        "the replacement session should remain signed in"
    );

    const syn_sig_ra::RouteResponse unsafe_deletion =
        syn_sig_ra::route_request(
            "DELETE", "/syn_sig_ra/v1/account", "/syn_sig_ra", "", &store,
            "", "application/json",
            "{\"current_password\":\"changed-password-456\","
            "\"confirmation\":\"delete\"}",
            "/tmp/syn_sig_ra_route_account_storage", "", "",
            password_changed.set_cookie
        );
    require(
        unsafe_deletion.status == 400,
        "account deletion should require the exact destructive confirmation"
    );

    const syn_sig_ra::RouteResponse account_deleted =
        syn_sig_ra::route_request(
            "DELETE", "/syn_sig_ra/v1/account", "/syn_sig_ra", "", &store,
            "", "application/json",
            "{\"current_password\":\"changed-password-456\","
            "\"confirmation\":\"DELETE MY ACCOUNT\"}",
            "/tmp/syn_sig_ra_route_account_storage", "", "",
            password_changed.set_cookie
        );
    require(
        account_deleted.status == 200 &&
            account_deleted.body.find("\"status\":\"deleted\"") !=
                std::string::npos &&
            account_deleted.body.find("\"server_workspace\":\"deleted\"") !=
                std::string::npos &&
            account_deleted.body.find("\"downloaded_artifacts\":\"not_revocable\"") !=
                std::string::npos &&
            account_deleted.set_cookie.find("Max-Age=0") != std::string::npos,
        "owner re-authentication should permanently delete the account workspace"
    );
    require(
        syn_sig_ra::route_request(
            "GET", "/syn_sig_ra/v1/auth/me", "/syn_sig_ra", "", &store,
            "", "", "", "", "", "", password_changed.set_cookie
        ).status == 401,
        "deleted account sessions should no longer authenticate"
    );
    require(
        syn_sig_ra::route_request(
            "POST", "/syn_sig_ra/v1/auth/login", "/syn_sig_ra", "", &store,
            "", "application/json",
            "{\"email\":\"new@example.com\","
            "\"password\":\"changed-password-456\"}"
        ).status == 401,
        "a deleted account should not be able to sign in again"
    );

    const syn_sig_ra::RouteResponse unauthorized =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/jobs/job_test",
            "/syn_sig_ra",
            "",
            &store
        );
    require(unauthorized.status == 401, "missing API key should return 401");
    require(
        !unauthorized.www_authenticate.empty(),
        "HTTP 401 should include a Bearer challenge"
    );

    const syn_sig_ra::RouteResponse authorized =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/jobs/job_test",
            "/syn_sig_ra",
            "Bearer route-test-key",
            &store
        );
    require(
        authorized.status == 404,
        "authenticated requests should pass auth and reach routing"
    );

    const syn_sig_ra::RouteResponse authoring_schema =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/authoring/schema",
            "/syn_sig_ra",
            "Bearer route-test-key",
            &store
        );
    require(
        authoring_schema.status == 200 &&
            authoring_schema.body.find("synsigra_authoring") != std::string::npos &&
            authoring_schema.body.find("\"targets\"") != std::string::npos,
        "authenticated caller should read core authoring schema"
    );
    const syn_sig_ra::RouteResponse authoring_templates =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/authoring/templates",
            "/syn_sig_ra",
            "Bearer route-test-key",
            &store
        );
    require(
        authoring_templates.status == 200 &&
            authoring_templates.body.find("ecg_rpeak_clean") != std::string::npos,
        "authenticated caller should read core authoring templates"
    );
    const std::string preview_request =
        "{\"scenario\":{\"schema_version\":2,\"scenario_id\":\"preview_case\","
        "\"name\":\"Preview\",\"description\":\"\",\"author\":\"Synsigra\","
        "\"tags\":[\"preview\"],\"duration_seconds\":10,"
        "\"sample_rate_hz\":500,\"seed\":12345,\"ecg\":{"
        "\"heart_rate_bpm\":70,\"rr_variability_seconds\":0,"
        "\"ectopic_every_n_beats\":0,"
        "\"second_degree_av_pattern\":\"unspecified\","
        "\"q_wave_territory\":\"unspecified\","
        "\"rhythm_episodes\":[],"
        "\"flutter_conduction_pattern\":\"fixed\","
        "\"pacing_mode\":\"ventricular\","
        "\"pacing_non_capture_every_n_beats\":0,"
        "\"fidelity_policy\":\"allow_parameterized\","
        "\"conditions\":[{\"code\":\"NORM\",\"severity\":1}]},"
        "\"ppg\":{\"enabled\":false,\"pulse_delay_ms\":180,"
        "\"rise_time_ms\":120,\"decay_time_ms\":300,"
        "\"amplitude_au\":1,\"baseline_au\":0,"
        "\"dicrotic_delay_ms\":180,\"dicrotic_width_ms\":80,"
        "\"dicrotic_amplitude_ratio\":0.15}},"
        "\"targets\":[\"r_peak\"]}";
    const syn_sig_ra::RouteResponse preview =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/authoring/preview",
            "/syn_sig_ra",
            "Bearer route-test-key",
            &store,
            "",
            "application/json",
            preview_request
        );
    require(
        preview.status == 200 &&
            preview.body.find("\"success\":true") != std::string::npos &&
            preview.body.find("\"estimated_package_bytes\"") != std::string::npos,
        "scenario preview should use core pack analysis: " +
            std::to_string(preview.status) + " " + preview.body + " " +
            preview.internal_error
    );

    const std::string download_root =
        "/tmp/syn_sig_ra_route_downloads_" + std::to_string(getpid());
    const std::string download_pack_root = download_root + "/packs";
    const std::string verifier_root = download_root + "/downloads/verifier";
    mkdir(download_root.c_str(), 0750);
    mkdir(download_pack_root.c_str(), 0750);
    mkdir((download_root + "/downloads").c_str(), 0750);
    mkdir(verifier_root.c_str(), 0750);
    write_file(verifier_root + "/metadata.json",
               "{\"schema_version\":1,\"package\":\"synsigra\"}\n");
    write_file(verifier_root + "/synsigra-verifier.zip", "zip");
    write_file(verifier_root + "/synsigra-0.14.0-py3-none-any.whl", "wheel");
    const syn_sig_ra::RouteResponse downloads =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/downloads/verifier",
            "/syn_sig_ra",
            "Bearer route-test-key",
            &store,
            download_pack_root
        );
    require(
        downloads.status == 200 &&
            downloads.content_type == "application/json" &&
            downloads.file_path.find("metadata.json") != std::string::npos,
        "verifier download metadata should resolve from pack-root sibling"
    );
    const syn_sig_ra::RouteResponse bundle =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/downloads/verifier/synsigra-verifier.zip",
            "/syn_sig_ra",
            "Bearer route-test-key",
            &store,
            download_pack_root
        );
    require(
        bundle.status == 200 &&
            bundle.content_type == "application/zip" &&
            bundle.content_disposition.find("synsigra-verifier.zip") !=
                std::string::npos,
        "verifier bundle should be downloadable without exposing generator files"
    );
    const syn_sig_ra::RouteResponse wheel =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/downloads/verifier/synsigra-0.14.0-py3-none-any.whl",
            "/syn_sig_ra",
            "Bearer route-test-key",
            &store,
            download_pack_root
        );
    require(
        wheel.status == 200 &&
            wheel.content_type == "application/octet-stream" &&
            wheel.content_disposition.find(
                "synsigra-0.14.0-py3-none-any.whl") != std::string::npos,
        "verifier wheel should use a pip-compatible filename"
    );
    const syn_sig_ra::RouteResponse invalid_wheel_alias =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/downloads/verifier/synsigra-wheel.whl",
            "/syn_sig_ra",
            "Bearer route-test-key",
            &store,
            download_pack_root
        );
    require(
        invalid_wheel_alias.status == 400,
        "non-standard wheel aliases should be rejected"
    );
    const syn_sig_ra::RouteResponse bad_download =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/downloads/verifier/../secret",
            "/syn_sig_ra",
            "Bearer route-test-key",
            &store,
            download_pack_root
        );
    require(
        bad_download.status == 400,
        "download traversal attempts should return HTTP 400"
    );
    const syn_sig_ra::RuntimeConfig runtime_config =
        syn_sig_ra::default_runtime_config();
    const syn_sig_ra::RouteResponse pack_list =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/packs",
            "/syn_sig_ra",
            "",
            nullptr,
            runtime_config.pack_root
        );
    require(pack_list.status == 200, "pack list route should return HTTP 200");
    require(
        pack_list.body.find("\"pack_id\":\"r_peak_rr_simple_stress_v1\"") !=
            std::string::npos &&
            pack_list.body.find("\"pack_id\":\"r_peak_rr_snr_ladder_v1\"") !=
                std::string::npos,
        "pack list route should return both simple per-case R-peak evidence packs"
    );

    const syn_sig_ra::RouteResponse pack_detail =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/packs/r_peak_stress_v1",
            "/syn_sig_ra",
            "",
            nullptr,
            runtime_config.pack_root
        );
    require(
        pack_detail.status == 200 &&
            pack_detail.body.find("\"version\":\"1.2\"") != std::string::npos &&
            pack_detail.body.find("\"targets\":[\"r_peak\",\"rr_interval\"]") !=
                std::string::npos &&
            pack_detail.body.find("\"available\":true") != std::string::npos,
        "R-peak detector pack should expose focused R-peak and RR evidence"
    );

    const syn_sig_ra::RouteResponse frontier_detail =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/packs/r_peak_noise_frontier_v1",
            "/syn_sig_ra",
            "",
            nullptr,
            runtime_config.pack_root
        );
    require(
        frontier_detail.status == 200 &&
            frontier_detail.body.find("\"version\":\"1.1\"") !=
                std::string::npos &&
            frontier_detail.body.find("\"total_seconds\":500") !=
                std::string::npos &&
            frontier_detail.body.find("mixed_snr_m3") != std::string::npos &&
            frontier_detail.body.find("mixed_snr_m11") != std::string::npos,
        "noise-frontier detail should expose the calibrated nine-case ladder"
    );
    const syn_sig_ra::RouteResponse ladder_detail =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/packs/r_peak_rr_snr_ladder_v1",
            "/syn_sig_ra",
            "",
            nullptr,
            runtime_config.pack_root
        );
    require(
        ladder_detail.status == 200 &&
            ladder_detail.body.find("\"version\":\"1.0\"") != std::string::npos &&
            ladder_detail.body.find("\"total_seconds\":720") != std::string::npos &&
            ladder_detail.body.find("\"verdict_scope\":\"per_case\"") != std::string::npos &&
            ladder_detail.body.find("\"case_id\":\"snr_m1\"") != std::string::npos &&
            ladder_detail.body.find("\"case_id\":\"snr_m11\"") != std::string::npos,
        "simple SNR ladder detail should expose all independent case verdicts"
    );

    const syn_sig_ra::RouteResponse curated_clone =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/authoring/curated-scenarios/r_peak_stress_v1/clean_70",
            "/syn_sig_ra",
            "Bearer route-test-key",
            &store,
            runtime_config.pack_root
        );
    require(
        curated_clone.status == 200 &&
            curated_clone.body.find("\"scenario_id\":\"rpeak_clean_70\"") !=
                std::string::npos,
        "curated scenario clone should return source scenario JSON"
    );

    const syn_sig_ra::RouteResponse traversal =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/packs/../secret",
            "/syn_sig_ra",
            "",
            nullptr,
            runtime_config.pack_root
        );
    require(
        traversal.status == 400,
        "pack traversal attempts should return HTTP 400"
    );

    std::remove((verifier_root + "/metadata.json").c_str());
    std::remove((verifier_root + "/synsigra-verifier.zip").c_str());
    std::remove((verifier_root + "/synsigra-0.14.0-py3-none-any.whl").c_str());
    rmdir(verifier_root.c_str());
    rmdir((download_root + "/downloads").c_str());
    rmdir(download_pack_root.c_str());
    rmdir(download_root.c_str());
    rmdir(email_capture.c_str());
    std::remove(database_path.c_str());

    return EXIT_SUCCESS;
}
