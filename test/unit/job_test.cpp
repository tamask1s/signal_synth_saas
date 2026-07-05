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
            "{\"pack_id\":\"r_peak_stress_v1\","
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
        "{\"pack_id\":\"r_peak_stress_v1\"}"
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

    std::remove(path.str().c_str());
    return EXIT_SUCCESS;
}
