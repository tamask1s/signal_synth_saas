#include "syn_sig_ra/job_request.h"
#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/route.h"
#include "syn_sig_ra/runtime_config.h"
#include "syn_sig_ra/sha256.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
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

std::string read_file(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

}  // namespace

int main() {
    syn_sig_ra::JobRequest parsed;
    std::string error;
    require(
        syn_sig_ra::parse_job_request(
            "{\"project_id\":\"org_job_owner_default\","
            "\"pack_id\":\"r_peak_stress_v1\"}",
            parsed,
            error
        ) == syn_sig_ra::JobRequestStatus::valid,
        "valid job request should parse: " + error
    );
    require(
        syn_sig_ra::parse_job_request(
            "{\"pack_id\":\"r_peak_stress_v1\",\"extra\":true}",
            parsed,
            error
        ) == syn_sig_ra::JobRequestStatus::unsupported_field,
        "unknown fields should be rejected"
    );
    require(
        syn_sig_ra::parse_job_request(
            "{\"project_id\":\"org_job_owner_default\","
            "\"pack_id\":\"r_peak_stress_v1\","
            "\"export_formats\":[\"wfdb\"]}",
            parsed,
            error
        ) == syn_sig_ra::JobRequestStatus::unsupported_field,
        "non-operative export options should be rejected"
    );
    require(
        syn_sig_ra::parse_job_request(
            "{\"pack_id\":\"../secret\"}",
            parsed,
            error
        ) == syn_sig_ra::JobRequestStatus::invalid_value,
        "unsafe pack IDs should be rejected"
    );

    std::ostringstream path;
    path << "/tmp/syn_sig_ra_job_test_" << getpid() << ".sqlite3";
    std::remove(path.str().c_str());
    syn_sig_ra::MetadataStore store(path.str());

    syn_sig_ra::ApiKeyIdentity owner;
    owner.api_key_id = "key_job_owner";
    owner.organization_id = "org_job_owner";
    owner.user_id = "user_job_owner";
    std::string key_hash;
    require(
        syn_sig_ra::sha256_hex("job-owner-secret", key_hash, error),
        "owner key hash should succeed"
    );
    require(
        store.create_api_key(owner, key_hash, "job owner", error),
        "owner key should be created: " + error
    );

    const syn_sig_ra::RuntimeConfig config =
        syn_sig_ra::default_runtime_config();
    const std::string scenario_json = read_file(
        config.pack_root +
        "/../../signal_synth/examples/scenarios/ecg_clean.json"
    );
    require(!scenario_json.empty(), "scenario fixture should be readable");
    const syn_sig_ra::RouteResponse scenario_created =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/scenarios",
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root,
            "application/json",
            std::string("{\"name\":\"Clean draft\",\"target_intent\":[\"r_peak\"],\"scenario\":") +
                scenario_json + "}"
        );
    require(
        scenario_created.status == 201 &&
            scenario_created.body.find("\"status\":\"valid\"") !=
                std::string::npos &&
            scenario_created.body.find("\"target_intent\":[\"r_peak\"]") !=
                std::string::npos &&
            scenario_created.body.find("sha256:") != std::string::npos,
        "valid scenario draft should be stored with a fingerprint"
    );
    const std::string scenario_marker("\"scenario_id\":\"");
    const std::string::size_type scenario_start =
        scenario_created.body.rfind(scenario_marker) + scenario_marker.size();
    const std::string scenario_id = scenario_created.body.substr(
        scenario_start,
        scenario_created.body.find('"', scenario_start) - scenario_start
    );
    const syn_sig_ra::RouteResponse invalid_scenario =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/scenarios",
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root,
            "application/json",
            "{\"name\":\"Incomplete\",\"target_intent\":[\"r_peak\"],\"scenario\":{}}"
        );
    require(
        invalid_scenario.status == 422 &&
            invalid_scenario.body.find("scenario_invalid") != std::string::npos &&
            invalid_scenario.body.find("\"validation_errors\":[{") !=
                std::string::npos,
        "invalid scenario should be saved with actionable validation errors"
    );
    const syn_sig_ra::RouteResponse custom_pack_created =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/custom-packs",
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root,
            "application/json",
            std::string("{\"name\":\"Custom clean pack\","
                "\"description\":\"Unit test custom pack\","
                "\"targets\":[\"r_peak\"],\"scenario_ids\":[\"") +
                scenario_id + "\"]}",
            config.data_root
        );
    require(
        custom_pack_created.status == 201 &&
            custom_pack_created.body.find("\"source\":\"custom\"") !=
                std::string::npos &&
            custom_pack_created.body.find("sha256:") != std::string::npos,
        "valid owned drafts should compose an immutable custom pack: " +
            custom_pack_created.body
    );
    const std::string pack_marker("\"pack_id\":\"");
    const std::string::size_type pack_start =
        custom_pack_created.body.find(pack_marker) + pack_marker.size();
    const std::string custom_pack_id = custom_pack_created.body.substr(
        pack_start,
        custom_pack_created.body.find('"', pack_start) - pack_start
    );
    const syn_sig_ra::RouteResponse custom_job =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/jobs",
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root,
            "application/json",
            std::string("{\"project_id\":\"org_job_owner_default\","
                "\"pack_id\":\"") + custom_pack_id + "\"}"
        );
    require(
        custom_job.status == 202,
        "custom pack should be accepted by normal job creation: " +
            custom_job.body
    );
    const std::string job_marker("\"job_id\":\"");
    const std::string::size_type custom_job_start =
        custom_job.body.find(job_marker) + job_marker.size();
    const std::string custom_job_id = custom_job.body.substr(
        custom_job_start,
        custom_job.body.find('"', custom_job_start) - custom_job_start
    );
    require(
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/jobs/" + custom_job_id + "/cancel",
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root
        ).status == 200,
        "custom pack test job should cancel cleanly"
    );
    const syn_sig_ra::RouteResponse created = syn_sig_ra::route_request(
        "POST",
        "/syn_sig_ra/v1/jobs",
        "/syn_sig_ra",
        "Bearer job-owner-secret",
        &store,
        config.pack_root,
        "application/json; charset=utf-8",
        "{\"project_id\":\"org_job_owner_default\","
        "\"pack_id\":\"r_peak_stress_v1\"}"
    );
    require(created.status == 202, "job creation should return HTTP 202");
    const std::string marker("\"job_id\":\"");
    const std::string::size_type marker_position = created.body.find(marker);
    require(
        marker_position != std::string::npos,
        "job creation response should contain a job ID"
    );
    const std::string::size_type id_start =
        marker_position + marker.size();
    const std::string::size_type id_end = created.body.find('"', id_start);
    require(
        id_end != std::string::npos,
        "job creation response should contain a job ID"
    );
    const std::string job_id =
        created.body.substr(id_start, id_end - id_start);

    const syn_sig_ra::RouteResponse status = syn_sig_ra::route_request(
        "GET",
        "/syn_sig_ra/v1/jobs/" + job_id,
        "/syn_sig_ra",
        "Bearer job-owner-secret",
        &store,
        config.pack_root
    );
    require(status.status == 200, "owner should read the queued job");
    require(
        status.body.find("\"status\":\"queued\"") != std::string::npos,
        "new job should have queued status"
    );

    syn_sig_ra::ApiKeyIdentity viewer;
    viewer.api_key_id = "key_job_viewer";
    viewer.organization_id = owner.organization_id;
    viewer.user_id = "user_job_viewer";
    viewer.role = "viewer";
    require(
        syn_sig_ra::sha256_hex("viewer-secret", key_hash, error) &&
            store.create_api_key(viewer, key_hash, "job viewer", error),
        "viewer key should be created: " + error
    );
    const syn_sig_ra::RouteResponse isolated_scenario =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/scenarios/" + scenario_id,
            "/syn_sig_ra",
            "Bearer viewer-secret",
            &store,
            config.pack_root
        );
    require(
        isolated_scenario.status == 404,
        "scenario drafts must be scoped to the owning user (" + scenario_id + "): " +
            isolated_scenario.body
    );
    require(
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/custom-packs/" + custom_pack_id,
            "/syn_sig_ra",
            "Bearer viewer-secret",
            &store,
            config.pack_root
        ).status == 404,
        "custom packs must be scoped to the owning user"
    );
    const syn_sig_ra::RouteResponse scenario_updated =
        syn_sig_ra::route_request(
            "PUT",
            "/syn_sig_ra/v1/scenarios/" + scenario_id,
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root,
            "application/json",
            std::string("{\"name\":\"Updated clean\",\"target_intent\":[\"r_peak\"],\"scenario\":") +
                scenario_json + "}"
        );
    require(
        scenario_updated.status == 200 &&
            scenario_updated.body.find("Updated clean") != std::string::npos,
        "scenario owner should update and revalidate a draft"
    );
    const syn_sig_ra::RouteResponse scenario_list =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/scenarios",
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root
        );
    require(
        scenario_list.status == 200 &&
            scenario_list.body.find(scenario_id) != std::string::npos,
        "scenario owner should list drafts"
    );
    const syn_sig_ra::RouteResponse viewer_read = syn_sig_ra::route_request(
        "GET",
        "/syn_sig_ra/v1/jobs/" + job_id,
        "/syn_sig_ra",
        "Bearer viewer-secret",
        &store,
        config.pack_root
    );
    require(
        viewer_read.status == 200,
        "a viewer in the organization should read project jobs"
    );
    const syn_sig_ra::RouteResponse viewer_create = syn_sig_ra::route_request(
        "POST",
        "/syn_sig_ra/v1/jobs",
        "/syn_sig_ra",
        "Bearer viewer-secret",
        &store,
        config.pack_root,
        "application/json",
        "{\"project_id\":\"org_job_owner_default\","
        "\"pack_id\":\"r_peak_stress_v1\"}"
    );
    require(
        viewer_create.status == 403,
        "viewer role must not create jobs"
    );
    const syn_sig_ra::RouteResponse viewer_delete = syn_sig_ra::route_request(
        "DELETE",
        "/syn_sig_ra/v1/jobs/" + job_id,
        "/syn_sig_ra",
        "Bearer viewer-secret",
        &store,
        config.pack_root
    );
    require(
        viewer_delete.status == 403,
        "viewer role must not delete jobs"
    );

    const syn_sig_ra::RouteResponse projects = syn_sig_ra::route_request(
        "GET",
        "/syn_sig_ra/v1/projects",
        "/syn_sig_ra",
        "Bearer job-owner-secret",
        &store,
        config.pack_root
    );
    require(
        projects.status == 200 &&
            projects.body.find("org_job_owner_default") != std::string::npos,
        "organization members should list their projects"
    );
    const syn_sig_ra::RouteResponse project_created =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/projects",
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root,
            "application/json",
            "{\"display_name\":\"Validation\"}"
        );
    require(
        project_created.status == 201,
        "owner role should create projects"
    );
    const syn_sig_ra::RouteResponse viewer_project_create =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/projects",
            "/syn_sig_ra",
            "Bearer viewer-secret",
            &store,
            config.pack_root,
            "application/json",
            "{\"display_name\":\"Forbidden\"}"
        );
    require(
        viewer_project_create.status == 403,
        "viewer role must not create projects"
    );

    const syn_sig_ra::RouteResponse job_list = syn_sig_ra::route_request(
        "GET",
        "/syn_sig_ra/v1/jobs",
        "/syn_sig_ra",
        "Bearer job-owner-secret",
        &store,
        config.pack_root
    );
    require(job_list.status == 200, "owner should list jobs");
    require(
        job_list.body.find("\"jobs\":[") != std::string::npos &&
            job_list.body.find(job_id) != std::string::npos,
        "job list should contain the queued job"
    );
    const syn_sig_ra::RouteResponse paged_list = syn_sig_ra::route_request(
        "GET",
        "/syn_sig_ra/v1/jobs",
        "/syn_sig_ra",
        "Bearer job-owner-secret",
        &store,
        config.pack_root,
        "",
        "",
        "",
        "limit=1&offset=0"
    );
    require(
        paged_list.status == 200 &&
            paged_list.body.find("\"limit\":1") != std::string::npos &&
            paged_list.body.find("\"offset\":0") != std::string::npos,
        "job list should support bounded offset pagination"
    );

    syn_sig_ra::ApiKeyIdentity other;
    other.api_key_id = "key_other";
    other.organization_id = "org_other";
    other.user_id = "user_other";
    require(
        syn_sig_ra::sha256_hex("other-secret", key_hash, error),
        "other key hash should succeed"
    );
    require(
        store.create_api_key(other, key_hash, "other owner", error),
        "other key should be created"
    );
    const syn_sig_ra::RouteResponse isolated = syn_sig_ra::route_request(
        "GET",
        "/syn_sig_ra/v1/jobs/" + job_id,
        "/syn_sig_ra",
        "Bearer other-secret",
        &store,
        config.pack_root
    );
    require(
        isolated.status == 404,
        "another owner must not discover or read the job"
    );

    const syn_sig_ra::RouteResponse cancelled = syn_sig_ra::route_request(
        "POST",
        "/syn_sig_ra/v1/jobs/" + job_id + "/cancel",
        "/syn_sig_ra",
        "Bearer job-owner-secret",
        &store,
        config.pack_root
    );
    require(
        cancelled.status == 200 &&
            cancelled.body.find("\"status\":\"cancelled\"") !=
                std::string::npos,
        "queued jobs should cancel deterministically"
    );
    const syn_sig_ra::RouteResponse retried = syn_sig_ra::route_request(
        "POST",
        "/syn_sig_ra/v1/jobs/" + job_id + "/retry",
        "/syn_sig_ra",
        "Bearer job-owner-secret",
        &store,
        config.pack_root
    );
    require(
        retried.status == 202 &&
            retried.body.find("\"retry_of\":\"" + job_id + "\"") !=
                std::string::npos,
        "cancelled jobs should retry as a new queued job"
    );
    const syn_sig_ra::RouteResponse second_active =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/jobs",
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root,
            "application/json",
            "{\"project_id\":\"org_job_owner_default\","
            "\"pack_id\":\"r_peak_stress_v1\"}"
        );
    require(
        second_active.status == 202,
        "organization should use its configured concurrent job capacity"
    );
    const syn_sig_ra::RouteResponse quota_rejected =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/jobs",
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root,
            "application/json",
            "{\"project_id\":\"org_job_owner_default\","
            "\"pack_id\":\"r_peak_stress_v1\"}"
        );
    require(
        quota_rejected.status == 429 &&
            quota_rejected.body.find("concurrent_job_limit") !=
                std::string::npos,
        "concurrent job quota should return a stable HTTP 429"
    );
    const syn_sig_ra::RouteResponse usage = syn_sig_ra::route_request(
        "GET",
        "/syn_sig_ra/v1/usage",
        "/syn_sig_ra",
        "Bearer job-owner-secret",
        &store,
        config.pack_root
    );
    require(
        usage.status == 200 &&
            usage.body.find("\"active_jobs\":2") != std::string::npos &&
            usage.body.find("\"concurrent_jobs\":2") != std::string::npos,
        "usage API should expose current counters and configured limits"
    );

    const syn_sig_ra::RouteResponse deleted = syn_sig_ra::route_request(
        "DELETE",
        "/syn_sig_ra/v1/jobs/" + job_id,
        "/syn_sig_ra",
        "Bearer job-owner-secret",
        &store,
        config.pack_root
    );
    require(deleted.status == 200, "owner should soft-delete the job");
    require(
        deleted.body.find("\"status\":\"deleted\"") != std::string::npos,
        "delete response should confirm deletion"
    );

    const syn_sig_ra::RouteResponse hidden = syn_sig_ra::route_request(
        "GET",
        "/syn_sig_ra/v1/jobs/" + job_id,
        "/syn_sig_ra",
        "Bearer job-owner-secret",
        &store,
        config.pack_root
    );
    require(hidden.status == 404, "deleted jobs should not be readable");

    const syn_sig_ra::RouteResponse list_after_delete =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/jobs",
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root
        );
    require(
        list_after_delete.status == 200 &&
            list_after_delete.body.find(job_id) == std::string::npos,
        "deleted jobs should not appear in the job list"
    );
    const syn_sig_ra::RouteResponse scenario_deleted =
        syn_sig_ra::route_request(
            "DELETE",
            "/syn_sig_ra/v1/scenarios/" + scenario_id,
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root
        );
    require(
        scenario_deleted.status == 200,
        "scenario owner should delete a draft"
    );
    const syn_sig_ra::RouteResponse scenario_hidden =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/scenarios/" + scenario_id,
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root
        );
    require(
        scenario_hidden.status == 404,
        "deleted scenario draft should not remain readable"
    );
    require(
        syn_sig_ra::route_request(
            "DELETE",
            "/syn_sig_ra/v1/custom-packs/" + custom_pack_id,
            "/syn_sig_ra",
            "Bearer job-owner-secret",
            &store,
            config.pack_root
        ).status == 200,
        "custom pack owner should remove it from the composer"
    );

    std::remove(path.str().c_str());
    return EXIT_SUCCESS;
}
