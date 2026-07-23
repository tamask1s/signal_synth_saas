#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/route.h"
#include "syn_sig_ra/runtime_config.h"
#include "syn_sig_ra/sha256.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

syn_sig_ra::RouteResponse mcp(
    syn_sig_ra::MetadataStore& store,
    const syn_sig_ra::RuntimeConfig& config,
    const std::string& body,
    const std::string& authorization = "Bearer mcp-owner-secret",
    const std::string& accept = "application/json, text/event-stream",
    const std::string& origin = "https://www.timeonion.com",
    const std::string& protocol = "2025-11-25"
) {
    syn_sig_ra::EmailConfig email;
    email.public_origin = "https://www.timeonion.com/syn_sig_ra";
    return syn_sig_ra::route_request(
        "POST", "/syn_sig_ra/mcp", "/syn_sig_ra", authorization, &store,
        config.pack_root, "application/json", body, config.data_root, "",
        "", "", email, "", accept, origin, protocol);
}

bool complete_next_job(
    syn_sig_ra::MetadataStore& store,
    const std::string& package_id,
    const std::string& challenge_metadata,
    syn_sig_ra::JobRecord& job,
    std::string& error
) {
    if (store.claim_next_job(job, error) !=
        syn_sig_ra::RecordLookupStatus::found) {
        return false;
    }
    return store.complete_job_with_package(
        job,
        package_id,
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "synsigra_core_integration_v7",
        "{}",
        "0.10.0-dev",
        "9ea4ff5d018d105966959c43c392316b2353e94d",
        "signal_synth/9ea4ff5d018d105966959c43c392316b2353e94d",
        "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "{}",
        challenge_metadata,
        "signal-synth pack challenge",
        "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
        "/tmp/" + package_id,
        1024,
        error);
}

}  // namespace

int main() {
    std::ostringstream path;
    path << "/tmp/syn_sig_ra_mcp_test_" << getpid() << ".sqlite3";
    std::remove(path.str().c_str());
    syn_sig_ra::MetadataStore store(path.str());
    syn_sig_ra::ApiKeyIdentity owner;
    owner.api_key_id = "key_mcp_owner";
    owner.organization_id = "org_mcp_owner";
    owner.user_id = "user_mcp_owner";
    owner.role = "owner";
    std::string hash;
    std::string error;
    require(
        syn_sig_ra::sha256_hex("mcp-owner-secret", hash, error) &&
        store.bootstrap_owner(
            owner, "mcp-owner@example.test", "MCP Owner", hash,
            "MCP unit test", error),
        "MCP fixture should bootstrap: " + error);
    const syn_sig_ra::RuntimeConfig config = syn_sig_ra::default_runtime_config();

    require(
        mcp(store, config,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
            "\"params\":{\"protocolVersion\":\"2025-11-25\","
            "\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}",
            "").status == 401,
        "MCP must require a personal Bearer API key");
    require(
        mcp(store, config,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}",
            "Bearer mcp-owner-secret", "application/json, text/event-stream",
            "https://attacker.example").status == 403,
        "MCP must reject a cross-origin Origin header");
    require(
        mcp(store, config,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}",
            "Bearer mcp-owner-secret", "application/json").status == 406,
        "MCP must enforce Streamable HTTP Accept media types");

    const syn_sig_ra::RouteResponse initialized = mcp(
        store, config,
        "{\"jsonrpc\":\"2.0\",\"id\":\"init-1\",\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"2025-11-25\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"unit\",\"version\":\"1\"}}}");
    require(
        initialized.status == 200 &&
        initialized.body.find("\"protocolVersion\":\"2025-11-25\"") !=
            std::string::npos &&
        initialized.body.find("\"tools\":{}") != std::string::npos &&
        initialized.body.find("never send PHI") != std::string::npos,
        "initialize should negotiate current tools/prompts capabilities: " +
            initialized.body);

    const syn_sig_ra::RouteResponse tools = mcp(
        store, config,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}");
    require(
        tools.status == 200 &&
        tools.body.find("synsigra_recommend_packs") != std::string::npos &&
        tools.body.find("synsigra_get_verification_guide") != std::string::npos &&
        tools.body.find("synsigra_create_custom_pack") != std::string::npos &&
        tools.body.find("\"readOnlyHint\":false") != std::string::npos,
        "tool discovery should expose guided and modifying workflows: " + tools.body);

    const syn_sig_ra::RouteResponse recommendation = mcp(
        store, config,
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"synsigra_recommend_packs\",\"arguments\":{"
        "\"goal\":\"Validate ECG R peak, RR, HRV LF HF SDNN RMSSD with noise\","
        "\"duration_seconds\":300,\"sampling_rate_hz\":500}}}");
    require(
        recommendation.status == 200 &&
        recommendation.body.find("\"interpreted_targets\"") != std::string::npos &&
        recommendation.body.find("\"r_peak\"") != std::string::npos &&
        recommendation.body.find("\"rr_interval\"") != std::string::npos &&
        recommendation.body.find("\"hrv\"") != std::string::npos &&
        recommendation.body.find(
            "\"interpreted_targets\":[\"hrv\",\"r_peak\",\"rr_interval\"]") !=
            std::string::npos &&
        recommendation.body.find("recommended_workflow") != std::string::npos,
        "noise stress must not invent a signal-quality algorithm output: " +
            recommendation.body);

    const syn_sig_ra::RouteResponse peak_noise_recommendation = mcp(
        store, config,
        "{\"jsonrpc\":\"2.0\",\"id\":\"peak-noise\",\"method\":\"tools/call\","
        "\"params\":{\"name\":\"synsigra_recommend_packs\",\"arguments\":{"
        "\"goal\":\"Test an R-peak detector under escalating baseline wander and noise\","
        "\"sampling_rate_hz\":500}}}");
    require(
        peak_noise_recommendation.status == 200 &&
        peak_noise_recommendation.body.find(
            "\"interpreted_targets\":[\"r_peak\"]") != std::string::npos &&
        peak_noise_recommendation.body.find("r_peak_noise_frontier_v1") !=
            std::string::npos &&
        peak_noise_recommendation.body.find("r_peak_stress_v1") !=
            std::string::npos &&
        peak_noise_recommendation.body.find("r_peak_noise_frontier_v1") <
            peak_noise_recommendation.body.find("r_peak_stress_v1"),
        "R-peak noise requests should recommend the peak-only frontier: " +
            peak_noise_recommendation.body);

    const syn_sig_ra::RouteResponse quality_recommendation = mcp(
        store, config,
        "{\"jsonrpc\":\"2.0\",\"id\":\"quality\",\"method\":\"tools/call\","
        "\"params\":{\"name\":\"synsigra_recommend_packs\",\"arguments\":{"
        "\"goal\":\"Validate a signal-quality detector that labels artifact intervals\"}}}");
    require(
        quality_recommendation.status == 200 &&
        quality_recommendation.body.find(
            "\"interpreted_targets\":[\"signal_quality\"]") != std::string::npos,
        "explicit quality-interval requests should retain signal-quality scope: " +
            quality_recommendation.body);

    const syn_sig_ra::RouteResponse projects = mcp(
        store, config,
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"synsigra_list_projects\",\"arguments\":{}}}");
    require(
        projects.status == 200 &&
        projects.body.find("org_mcp_owner_default") != std::string::npos &&
        projects.body.find("\"http_status\":200") != std::string::npos,
        "MCP should delegate project access to the existing API: " + projects.body);

    const syn_sig_ra::RouteResponse created = mcp(
        store, config,
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"synsigra_create_job\",\"arguments\":{"
        "\"project_id\":\"org_mcp_owner_default\","
        "\"pack_id\":\"r_peak_stress_v1\"}}}");
    require(
        created.status == 200 &&
        created.body.find("\"http_status\":202") != std::string::npos &&
        created.body.find("\"status\":\"queued\"") != std::string::npos,
        "MCP job creation should preserve REST validation and status: " + created.body);

    const std::string diagnostic_metadata =
        "{\"contract\":\"synsigra_saas_challenge_metadata_v1\","
        "\"challenge_contract\":\"synsigra_challenge_package_v3\","
        "\"local_verification_contract\":\"synsigra_local_verification_v3\","
        "\"verifier_version\":\"0.14.0\",\"case_count\":2,"
        "\"targets\":[{\"target\":\"r_peak\"},{\"target\":\"signal_quality\"}],"
        "\"verification\":{\"mode\":\"diagnostic\","
        "\"evidence_eligible\":false}}";
    syn_sig_ra::JobRecord diagnostic_job;
    require(
        complete_next_job(
            store, "pkg_mcp_diagnostic", diagnostic_metadata,
            diagnostic_job, error),
        "diagnostic MCP fixture should complete: " + error);
    const syn_sig_ra::RouteResponse diagnostic_guide = mcp(
        store, config,
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"synsigra_get_verification_guide\","
        "\"arguments\":{\"job_id\":\"" + diagnostic_job.job_id + "\"}}}");
    require(
        diagnostic_guide.status == 200 &&
        diagnostic_guide.body.find("\"verification_mode\":\"diagnostic\"") !=
            std::string::npos &&
        diagnostic_guide.body.find(
            "synsigra-verify challenge submission verification-results --mode diagnostic --force") !=
            std::string::npos &&
        diagnostic_guide.body.find(
            "synsigra-0.14.0-py3-none-any.whl") != std::string::npos &&
        diagnostic_guide.body.find("\"job_summary\"") != std::string::npos &&
        diagnostic_guide.body.find("\"canonical_evidence\":"
            "\"verification-results/evidence.json\"") != std::string::npos &&
        diagnostic_guide.body.find("\"evidence_command\"") == std::string::npos &&
        diagnostic_guide.body.find("\"submission_formats\"") == std::string::npos &&
        diagnostic_guide.body.size() < 12000,
        "diagnostic guide should be exact and concise: " + diagnostic_guide.body);

    std::string evidence_job_id;
    require(
        store.create_job(
            owner, owner.organization_id + "_default",
            "{\"pack_id\":\"r_peak_rr_noise_v1\"}",
            "r_peak_rr_noise_v1", "r_peak_rr_noise_v1.json",
            "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
            "1.3", "3.1",
            "sha256:34725e1b879904dd70000a42b422822beb6133e48b628b8a8ae8bc71277bb765",
            evidence_job_id, error),
        "evidence MCP fixture should queue: " + error);
    const std::string evidence_metadata =
        "{\"contract\":\"synsigra_saas_challenge_metadata_v1\","
        "\"challenge_contract\":\"synsigra_challenge_package_v3\","
        "\"local_verification_contract\":\"synsigra_local_verification_v3\","
        "\"verifier_version\":\"0.14.0\",\"case_count\":1,"
        "\"targets\":[{\"target\":\"r_peak\"},{\"target\":\"rr_interval\"}],"
        "\"verification\":{\"mode\":\"evidence\","
        "\"evidence_eligible\":true,\"protocol\":{"
        "\"protocol_id\":\"r_peak_rr_noise_v1\"}}}";
    syn_sig_ra::JobRecord evidence_job;
    require(
        complete_next_job(
            store, "pkg_mcp_evidence", evidence_metadata,
            evidence_job, error) && evidence_job.job_id == evidence_job_id,
        "evidence MCP fixture should complete: " + error);
    const syn_sig_ra::RouteResponse evidence_guide = mcp(
        store, config,
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"synsigra_get_verification_guide\","
        "\"arguments\":{\"job_id\":\"" + evidence_job.job_id + "\"}}}");
    require(
        evidence_guide.status == 200 &&
        evidence_guide.body.find("\"verification_mode\":\"evidence\"") !=
            std::string::npos &&
        evidence_guide.body.find("\"evidence_eligible\":true") !=
            std::string::npos &&
        evidence_guide.body.find(
            "synsigra-verify challenge submission verification-results --force") !=
            std::string::npos &&
        evidence_guide.body.find("--mode diagnostic") == std::string::npos &&
        evidence_guide.body.find("\"archive_checklist\"") != std::string::npos,
        "evidence guide should use package-authoritative mode: " +
            evidence_guide.body);

    const syn_sig_ra::RouteResponse notification = mcp(
        store, config,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    require(notification.status == 202 && notification.body.empty(),
            "MCP notifications should return HTTP 202 without a body");

    syn_sig_ra::EmailConfig email;
    email.public_origin = "https://www.timeonion.com/syn_sig_ra";
    const syn_sig_ra::RouteResponse get = syn_sig_ra::route_request(
        "GET", "/syn_sig_ra/mcp", "/syn_sig_ra", "Bearer mcp-owner-secret",
        &store, config.pack_root, "", "", config.data_root, "", "", "",
        email, "", "text/event-stream", "https://www.timeonion.com",
        "2025-11-25");
    require(get.status == 405, "stateless MCP GET should explicitly decline SSE");

    std::remove(path.str().c_str());
    std::cout << "MCP server tests passed\n";
    return EXIT_SUCCESS;
}
