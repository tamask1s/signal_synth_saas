#include "syn_sig_ra/route.h"

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

    return EXIT_SUCCESS;
}
