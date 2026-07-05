#include "syn_sig_ra/job_request.h"
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

}  // namespace

int main() {
    syn_sig_ra::JobRequest parsed;
    std::string error;
    require(
        syn_sig_ra::parse_job_request(
            "{\"project_id\":\"org_job_owner_default\","
            "\"pack_id\":\"r_peak_stress_v1\","
            "\"export_formats\":[\"wfdb\",\"edf\"],"
            "\"report_format\":\"html\"}",
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

    std::remove(path.str().c_str());
    return EXIT_SUCCESS;
}
