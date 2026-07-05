#include "syn_sig_ra/route.h"

#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/runtime_config.h"
#include "syn_sig_ra/sha256.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
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
            ui.body.find("Challenge package generator") != std::string::npos &&
            ui.body.find("readiness-status") != std::string::npos &&
            ui.body.find("metrics-panel") != std::string::npos &&
            ui.body.find("load-more-jobs") != std::string::npos &&
            ui.body.find("load-scenario-template") != std::string::npos &&
            ui.body.find("Recommended workflow") != std::string::npos,
        "web UI route should return HTML"
    );
    const syn_sig_ra::RouteResponse ui_js =
        syn_sig_ra::route_request("GET", "/syn_sig_ra/ui/app.js");
    require(
        ui_js.status == 200 &&
            ui_js.content_type.find("javascript") != std::string::npos &&
            ui_js.body.find("(() => {") == 0 &&
            ui_js.body.find("loadMetrics") != std::string::npos &&
            ui_js.body.find("jobsNextOffset") != std::string::npos &&
            ui_js.body.find("cleanEcgTemplate") != std::string::npos &&
            ui_js.body.find("Reproducibility details") != std::string::npos,
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
    std::remove(database_path.c_str());

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

    return EXIT_SUCCESS;
}
