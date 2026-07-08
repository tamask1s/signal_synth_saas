#include "syn_sig_ra/route.h"

#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/runtime_config.h"
#include "syn_sig_ra/sha256.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
            ui.body.find("Guided workflow") != std::string::npos &&
            ui.body.find("What do you want to do next?") != std::string::npos &&
            ui.body.find("pack-intent-filter") != std::string::npos &&
            ui.body.find("pack-comparison") != std::string::npos &&
            ui.body.find("verification-runbook") != std::string::npos &&
            ui.body.find("runbook-job-select") != std::string::npos &&
            ui.body.find("readiness-status") != std::string::npos &&
            ui.body.find("metrics-panel") != std::string::npos &&
            ui.body.find("load-more-jobs") != std::string::npos &&
            ui.body.find("load-scenario-template") != std::string::npos &&
            ui.body.find("verifier-downloads") != std::string::npos &&
            ui.body.find("scenario-template-select") != std::string::npos &&
            ui.body.find("Advanced JSON editor") != std::string::npos &&
            ui.body.find("custom-pack-review") != std::string::npos &&
            ui.body.find("register-email") != std::string::npos &&
            ui.body.find("save-key") == std::string::npos &&
            ui.body.find("/syn_sig_ra/docs/api") != std::string::npos &&
            ui.body.find("No PHI") != std::string::npos &&
            ui.body.find("Evidence path") != std::string::npos,
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
    const syn_sig_ra::RouteResponse docs_api =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/docs/api");
    require(
        docs_api.status == 200 &&
            docs_api.content_type.find("text/html") != std::string::npos &&
            docs_api.body.find("Rendered API reference") != std::string::npos &&
            docs_api.body.find("/v1/downloads/verifier") != std::string::npos &&
            docs_api.body.find("/v1/authoring/preview") != std::string::npos &&
            docs_api.body.find("detection-templates.zip") != std::string::npos &&
            docs_api.body.find("verification-kit.zip") != std::string::npos,
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
            ui_js.body.find("Detection templates ZIP") != std::string::npos &&
            ui_js.body.find("Reproducibility details") != std::string::npos &&
            ui_js.body.find("renderVerificationRunbook") != std::string::npos &&
            ui_js.body.find("selectPackForGeneration") != std::string::npos &&
            ui_js.body.find("renderCustomPackReview") != std::string::npos &&
            ui_js.body.find("groupValidationErrors") != std::string::npos &&
            ui_js.body.find("saveResponseAsFile") != std::string::npos &&
            ui_js.body.find("data-no-spa") != std::string::npos &&
            ui_js.body.find("link.hasAttribute(\"download\")") !=
                std::string::npos &&
            ui_js.body.find("url.protocol !== \"http:\"") !=
                std::string::npos,
        "web UI JavaScript asset should be served as an executable IIFE"
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
    require(
        store.create_api_key(identity, key_hash, "route test", error),
        "route test API key creation should succeed: " + error
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
            "\"display_name\":\"New User\"}"
        );
    require(
        registered.status == 200 &&
            registered.body.find("\"email\":\"new@example.com\"") !=
                std::string::npos &&
            registered.set_cookie.find("Secure") != std::string::npos &&
            registered.set_cookie.find("HttpOnly") != std::string::npos &&
            registered.cache_control == "no-store",
        "registration should create a secure browser session"
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
            registered.set_cookie
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
            registered.set_cookie
        );
    require(
        personal_key.status == 201 &&
            personal_key.body.find("\"api_key\":\"ssk_") != std::string::npos,
        "signed-in account should create a one-time API key"
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
        "\"episode_type\":\"none\",\"episode_start_seconds\":2,"
        "\"episode_duration_seconds\":4,\"episode_rate_bpm\":170,"
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
        "scenario preview should use core pack analysis"
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
    write_file(verifier_root + "/synsigra-wheel.whl", "wheel");
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
        pack_list.body.find("\"pack_id\":\"r_peak_stress_v1\"") !=
            std::string::npos,
        "pack list route should return the built-in example pack"
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
        pack_detail.status == 200,
        "known pack detail route should return HTTP 200"
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
    std::remove((verifier_root + "/synsigra-wheel.whl").c_str());
    rmdir(verifier_root.c_str());
    rmdir((download_root + "/downloads").c_str());
    rmdir(download_pack_root.c_str());
    rmdir(download_root.c_str());
    std::remove(database_path.c_str());

    return EXIT_SUCCESS;
}
