#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/route.h"
#include "syn_sig_ra/sha256.h"
#include "syn_sig_ra/worker.h"

#include <sqlite3.h>
#include <dirent.h>
#include <sys/stat.h>
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

void write_file(
    const std::string& path,
    const std::string& content,
    mode_t mode
) {
    std::ofstream output(path.c_str(), std::ios::binary);
    output << content;
    output.close();
    require(output.good(), "test file should be written: " + path);
    require(chmod(path.c_str(), mode) == 0, "test file mode should be set");
}

bool remove_tree(const std::string& path) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0) return true;
    if (S_ISDIR(information.st_mode)) {
        chmod(path.c_str(), 0700);
        DIR* directory = opendir(path.c_str());
        if (directory == nullptr) return false;
        bool ok = true;
        for (dirent* entry = readdir(directory); entry != nullptr;
             entry = readdir(directory)) {
            const std::string name(entry->d_name);
            if (name != "." && name != ".." &&
                !remove_tree(path + "/" + name)) ok = false;
        }
        closedir(directory);
        return ok && rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

bool verify_user(const std::string& database_path, const std::string& user_id) {
    sqlite3* database = nullptr;
    sqlite3_stmt* statement = nullptr;
    bool ok = sqlite3_open(database_path.c_str(), &database) == SQLITE_OK &&
        sqlite3_prepare_v2(
            database, "UPDATE users SET email_verified=1 WHERE id=?1;",
            -1, &statement, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(statement) == SQLITE_DONE;
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return ok;
}

std::string replace_once(
    std::string value,
    const std::string& needle,
    const std::string& replacement
) {
    const std::string::size_type at = value.find(needle);
    require(at != std::string::npos, "receipt mutation fixture should exist");
    value.replace(at, needle.size(), replacement);
    return value;
}

}  // namespace

int main() {
    const std::string fingerprint =
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    syn_sig_ra::CliChallengeResult parsed;
    std::string error;
    syn_sig_ra::CoreIntegrationContract producer;
    require(
        syn_sig_ra::linked_core_integration_contract(producer, error),
        "linked contract should be valid: " + error
    );
    require(
        !syn_sig_ra::parse_challenge_receipt("", producer, parsed, error),
        "empty CLI stdout should be rejected"
    );
    require(
        !syn_sig_ra::parse_challenge_receipt(
            "status=challenge-rendered\nstatus=duplicate\n",
            producer,
            parsed,
            error
        ),
        "duplicate CLI fields should be rejected"
    );
    const std::string valid_receipt =
        "{\"schema_version\":1,\"contract\":\"" + producer.integration_contract + "\","
        "\"status\":\"challenge_rendered\",\"output_directory\":\"/tmp/out\","
        "\"package_id\":\"test_pack\",\"scenario_count\":1,"
        "\"pack_fingerprint\":\"" + fingerprint + "\","
        "\"package_fingerprint\":\"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
        "\"generator\":{\"name\":\"" + producer.generator_name +
        "\",\"version\":\"" + producer.generator_version + "\","
        "\"git_commit\":\"" + producer.generator_git_commit + "\","
        "\"build_identity\":\"" + producer.generator_build_identity + "\"},"
        "\"contracts\":{\"challenge_package\":\"" + producer.challenge_package + "\","
        "\"scoring_manifest\":\"" + producer.scoring_manifest + "\","
        "\"verification_protocol\":\"" + producer.verification_protocol + "\"}}";
    require(
        syn_sig_ra::parse_challenge_receipt(
            valid_receipt, producer, parsed, error) &&
            parsed.canonical_receipt_json.find("challenge_rendered") !=
                std::string::npos,
        "exact JSON receipt should be accepted: " + error
    );
    require(!syn_sig_ra::parse_challenge_receipt(
        valid_receipt + " trailing", producer, parsed, error),
        "receipt trailing output must be rejected");
    require(!syn_sig_ra::parse_challenge_receipt(
        replace_once(valid_receipt, "\"status\":", "\"status\":\"challenge_rendered\",\"status\":"),
        producer, parsed, error), "receipt duplicate keys must be rejected");
    require(!syn_sig_ra::parse_challenge_receipt(
        replace_once(valid_receipt, "{\"schema_version\":1,", "{\"schema_version\":1,\"extra\":true,"),
        producer, parsed, error), "receipt unknown fields must be rejected");
    require(!syn_sig_ra::parse_challenge_receipt(
        replace_once(valid_receipt, "\"status\":\"challenge_rendered\",", ""),
        producer, parsed, error), "receipt missing fields must be rejected");
    require(!syn_sig_ra::parse_challenge_receipt(
        replace_once(valid_receipt, "\"version\":\"0.10.0-dev\"", "\"version\":\"9.9.9\""),
        producer, parsed, error), "receipt producer mismatches must be rejected");

    std::ostringstream root_builder;
    root_builder << "/tmp/syn_sig_ra_worker_test_" << getpid();
    const std::string root = root_builder.str();
    const std::string work = root + "/work";
    const std::string packs = root + "/packs";
    const std::string packages = root + "/packages";
    const std::string noise_assets = root + "/noise_assets";
    require(mkdir(root.c_str(), 0700) == 0, "test root should be created");
    require(mkdir(work.c_str(), 0700) == 0, "work root should be created");
    require(mkdir(packs.c_str(), 0700) == 0, "pack root should be created");
    require(
        mkdir(packages.c_str(), 0700) == 0,
        "package root should be created"
    );
    require(mkdir(noise_assets.c_str(), 0700) == 0,
        "noise asset root should be created");

    const std::string pack_path = packs + "/test_pack.json";
    const std::string protocol_path = packs + "/protocol.json";
    const std::string noise_path = noise_assets + "/fixture.csv";
    write_file(
        pack_path,
        "{\"verification_protocol_path\":\"protocol.json\"}\n",
        0600);
    write_file(protocol_path, "{\"protocol\":\"immutable\"}\n", 0600);
    write_file(noise_path, "sample,noise\n0,0.25\n", 0600);
    const std::string success_cli = root + "/success-cli";
    write_file(
        success_cli,
        "#!/bin/sh\n"
        "if [ \"$1\" = contract ]; then\n"
        "  echo '" + producer.canonical_json + "'\n"
        "  exit 0\n"
        "fi\n"
        "test -f \"$(dirname \"$3\")/protocol.json\" || exit 41\n"
        "test -f \"$7/fixture.csv\" || exit 42\n"
        "mkdir -p \"$5/cases/test_case\"\n"
        "echo '{\"schema_version\":1}' > \"$5/manifest.json\"\n"
        "printf 'test_case 2 500 8\\n# generator=signal_synth test\\n# scenario_id=test_case\\nsignal.dat 16 200(0)/mV 16 0 0 0 0 ECG\\nsignal.dat 16 1000(0)/au 16 0 0 0 0 PPG\\n' > \"$5/cases/test_case/synsigra.hea\"\n"
        "printf '\\000\\000\\144\\000\\001\\000\\145\\000\\002\\000\\146\\000\\003\\000\\147\\000\\004\\000\\150\\000\\005\\000\\151\\000\\006\\000\\152\\000\\007\\000\\153\\000' > \"$5/cases/test_case/synsigra.dat\"\n"
        "printf '{\"schema_version\":1,\"contract\":\"" + producer.integration_contract + "\","
        "\"status\":\"challenge_rendered\",\"output_directory\":\"%s\","
        "\"package_id\":\"test_pack\",\"scenario_count\":1,"
        "\"pack_fingerprint\":\"" + fingerprint + "\","
        "\"package_fingerprint\":\"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
        "\"generator\":{\"name\":\"" + producer.generator_name +
        "\",\"version\":\"" + producer.generator_version + "\","
        "\"git_commit\":\"" + producer.generator_git_commit + "\","
        "\"build_identity\":\"" + producer.generator_build_identity + "\"},"
        "\"contracts\":{\"challenge_package\":\"" + producer.challenge_package + "\","
        "\"scoring_manifest\":\"" + producer.scoring_manifest + "\","
        "\"verification_protocol\":\"" + producer.verification_protocol + "\"}}\n' \"$5\"\n",
        0700
    );
    const std::string helper = root + "/challenge-helper.py";
    write_file(
        helper,
        "import json,sys\n"
        "print(json.dumps({"
        "'schema_version':1,"
        "'contract':'synsigra_saas_challenge_metadata_v1',"
        "'verifier_version':'0.11.0',"
        "'challenge_contract':'synsigra_challenge_package_v3',"
        "'scoring_manifest_contract':'synsigra_scoring_manifest_v3',"
        "'submission_contract':'synsigra_submission_v1',"
        "'submission_formats_contract':'synsigra_submission_formats_v2',"
        "'measurement_values_contract':'synsigra_measurement_values_v2',"
        "'measurement_truth_contract':'synsigra_measurement_truth_v2',"
        "'measurement_scoring_contract':'synsigra_measurement_score_v2',"
        "'local_verification_contract':'synsigra_local_verification_v3',"
        "'package_id':'test_pack',"
        "'name':'Test pack',"
        "'pack_version':'1.0',"
        "'pack_fingerprint':'" + fingerprint + "',"
        "'generator_version':'" + producer.generator_version + "',"
        "'generator_git_commit':'" + producer.generator_git_commit + "',"
        "'case_count':1,"
        "'targets':[{'target':'r_peak','supported':True}],"
        "'cases':[{'case_id':'test_case'}],"
        "'submission_outputs':[],"
        "'submission_formats':{},"
        "'verification_protocol':None,"
        "'verification':{'mode':'diagnostic','evidence_eligible':False,'matrix_complete':None,'evidence_result':'not_run','policy_result':'not_run','notice':'test','protocol':None},"
        "'external_noise':{'used':False,'release_allowed':True,'assets':[],'truth_paths':[]},"
        "'roles':{},"
        "'integrity':{'ok':True}},sort_keys=True,separators=(',',':')))\n",
        0600
    );
    const std::string wheel = root + "/synsigra-wheel.whl";
    write_file(wheel, "test wheel placeholder\n", 0600);
    const std::string failure_cli = root + "/failure-cli";
    write_file(
        failure_cli,
        "#!/bin/sh\n"
        "if [ \"$1\" = contract ]; then\n"
        "  echo '" + producer.canonical_json + "'\n"
        "  exit 0\n"
        "fi\n"
        "echo 'error=PACK_JSON_RANGE path=$ message=bad pack' >&2\n"
        "exit 4\n",
        0700
    );

    const std::string database = root + "/db.sqlite3";
    syn_sig_ra::MetadataStore store(database);
    syn_sig_ra::ApiKeyIdentity owner;
    owner.api_key_id = "worker_key";
    owner.organization_id = "worker_org";
    owner.user_id = "worker_user";
    owner.role = "owner";
    std::string key_hash;
    require(
        syn_sig_ra::sha256_hex("worker-secret", key_hash, error),
        "worker owner hash should succeed"
    );
    require(
        store.bootstrap_owner(
            owner, "worker@example.test", "Worker Owner", key_hash,
            "worker", error),
        "worker owner should be created: " + error
    );

    std::string succeeded_job;
    require(
        store.create_job(
            owner,
            owner.organization_id + "_default",
            "{\"pack_id\":\"test_pack\"}",
            "test_pack",
            pack_path,
            fingerprint,
            "1.0",
            "3.0",
            "sha256:3a8b53b43dbecdeb834ed3faf0fddb8a859464ff4b822caaaa31830f5a06c88f",
            succeeded_job,
            error
        ),
        "success job should be queued: " + error
    );
    syn_sig_ra::WorkerConfig config;
    config.database_path = database;
    config.signal_synth_cli = success_cli;
    config.pack_root = packs;
    config.data_root = root;
    config.noise_asset_root = noise_assets;
    config.challenge_helper = helper;
    config.verifier_wheel = wheel;
    std::string claimed_job;
    require(
        syn_sig_ra::run_worker_once(config, claimed_job, error) ==
            syn_sig_ra::WorkerRunStatus::succeeded,
        "worker should succeed: " + error
    );
    require(claimed_job == succeeded_job, "worker should claim queued job");
    syn_sig_ra::JobRecord job;
    require(
        store.find_job(succeeded_job, owner, job, error) ==
            syn_sig_ra::RecordLookupStatus::found &&
            job.status == "succeeded",
        "successful worker should persist succeeded status"
    );
    require(
        !job.package_id.empty() &&
            job.artifact_storage_key ==
                packages + "/" + job.package_id,
        "successful worker should persist immutable package metadata"
    );
    const std::string successful_package_id = job.package_id;
    const std::string successful_generator_identity =
        job.generator_build_identity;
    const std::string successful_generator_binary =
        job.generator_binary_sha256;
    require(
        job.source_pack_path.find(root + "/recipes/") == 0 &&
            access(job.source_pack_path.c_str(), R_OK) == 0 &&
            access(
                (root + "/generator_releases/" +
                 job.generator_binary_sha256.substr(7) +
                 "/signal-synth").c_str(),
                X_OK) == 0,
        "successful jobs should pin immutable recipes and generator releases"
    );
    require(
        access(
            (packages + "/" + job.package_id + "/extracted").c_str(),
            F_OK) != 0,
        "stored packages should not duplicate the archived extracted tree"
    );
    const syn_sig_ra::RouteResponse manifest_download =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/artifacts/" + job.package_id + "/manifest.json",
            "/syn_sig_ra",
            "Bearer worker-secret",
            &store,
            packs,
            "",
            "",
            root
        );
    require(
        manifest_download.status == 200 &&
            manifest_download.content_type == "application/json" &&
            !manifest_download.file_path.empty() &&
            manifest_download.checksum_sha256.size() == 64 &&
            manifest_download.accept_ranges,
        "package owner should resolve manifest download"
    );
    const std::string package_uri =
        "/syn_sig_ra/v1/artifacts/" + job.package_id + "/package.zip";
    const syn_sig_ra::RouteResponse package_head =
        syn_sig_ra::route_request(
            "HEAD", package_uri, "/syn_sig_ra", "Bearer worker-secret",
            &store, packs, "", "", root, "", "", "",
            syn_sig_ra::EmailConfig());
    require(
        package_head.status == 200 && package_head.headers_only &&
            package_head.file_length > 16 &&
            package_head.file_length == package_head.file_size &&
            package_head.checksum_sha256.size() == 64 &&
            !package_head.artifact_expires_at.empty(),
        "package HEAD should publish immutable delivery metadata"
    );
    const syn_sig_ra::RouteResponse package_range =
        syn_sig_ra::route_request(
            "GET", package_uri, "/syn_sig_ra", "Bearer worker-secret",
            &store, packs, "", "", root, "", "", "",
            syn_sig_ra::EmailConfig(), "bytes=0-15");
    require(
        package_range.status == 206 && package_range.file_offset == 0 &&
            package_range.file_length == 16 &&
            package_range.content_range.find("bytes 0-15/") == 0,
        "package range should resolve a bounded file-backed response"
    );
    const syn_sig_ra::RouteResponse invalid_package_range =
        syn_sig_ra::route_request(
            "GET", package_uri, "/syn_sig_ra", "Bearer worker-secret",
            &store, packs, "", "", root, "", "", "",
            syn_sig_ra::EmailConfig(), "bytes=999999999999-");
    require(
        invalid_package_range.status == 416 &&
            invalid_package_range.content_range.find("bytes */") == 0,
        "an unsatisfiable package range should return HTTP 416"
    );
    const syn_sig_ra::RouteResponse viewer_description =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/jobs/" + succeeded_job + "/viewer",
            "/syn_sig_ra",
            "Bearer worker-secret",
            &store,
            packs,
            "",
            "",
            root
        );
    require(
        viewer_description.status == 200 &&
            viewer_description.body.find("\"case_id\":\"test_case\"") !=
                std::string::npos &&
            viewer_description.body.find("\"name\":\"ECG\"") !=
                std::string::npos &&
            viewer_description.body.find("binary_http_viewports") !=
                std::string::npos,
        "completed WFDB packages should expose signal viewer metadata: " +
            viewer_description.body + " " + viewer_description.internal_error
    );
    const syn_sig_ra::RouteResponse viewer_window =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/jobs/" + succeeded_job + "/viewer/window",
            "/syn_sig_ra",
            "Bearer worker-secret",
            &store,
            packs,
            "",
            "",
            root,
            "case_id=test_case&start_sample=0&sample_count=8&points=8&channels=0,1"
        );
    require(
        viewer_window.status == 200 &&
            viewer_window.content_type ==
                "application/vnd.synsigra.signal-window.v1" &&
            viewer_window.body.size() == 152u &&
            viewer_window.body.compare(0, 8, "SYNSIGV1") == 0,
        "viewer window endpoint should return a bounded binary viewport"
    );
    const syn_sig_ra::RouteResponse invalid_viewer_window =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/jobs/" + succeeded_job + "/viewer/window",
            "/syn_sig_ra",
            "Bearer worker-secret",
            &store,
            packs,
            "",
            "",
            root,
            "case_id=test_case&sample_count=8&points=8&channels=0,0"
        );
    require(
        invalid_viewer_window.status == 400,
        "viewer window endpoint should reject duplicate channel selections"
    );
    syn_sig_ra::ApiKeyIdentity other_owner;
    other_owner.api_key_id = "worker_other_key";
    syn_sig_ra::AccountRecord other_account;
    require(
        store.create_account(
            "worker-other@example.test", "Other Worker", "salt", "hash",
            "test-terms", other_account, error) ==
            syn_sig_ra::AccountCreateStatus::created,
        "other account should be created: " + error
    );
    other_owner.organization_id = other_account.organization_id;
    other_owner.user_id = other_account.user_id;
    other_owner.role = "owner";
    require(verify_user(database, other_owner.user_id),
        "other account should be verified");
    require(
        syn_sig_ra::sha256_hex("worker-other-secret", key_hash, error) &&
            store.create_api_key(
                other_owner,
                key_hash,
                "worker other",
                error
            ),
        "other artifact owner should be created"
    );
    const syn_sig_ra::RouteResponse isolated_download =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/artifacts/" + job.package_id + "/manifest.json",
            "/syn_sig_ra",
            "Bearer worker-other-secret",
            &store,
            packs,
            "",
            "",
            root
        );
    require(
        isolated_download.status == 404,
        "another owner must not discover package artifacts: " +
            std::to_string(isolated_download.status) + " " + isolated_download.body
    );
    const syn_sig_ra::RouteResponse isolated_viewer =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/jobs/" + succeeded_job + "/viewer",
            "/syn_sig_ra",
            "Bearer worker-other-secret",
            &store,
            packs,
            "",
            "",
            root
        );
    require(
        isolated_viewer.status == 404,
        "another organization must not discover signal viewer metadata"
    );
    sqlite3* retention_database = nullptr;
    require(
        sqlite3_open(database.c_str(), &retention_database) == SQLITE_OK &&
            sqlite3_exec(
                retention_database,
                "UPDATE packages SET created_at='2000-01-01T00:00:00.000Z';",
                nullptr, nullptr, nullptr
            ) == SQLITE_OK,
        "retention fixture should age the package"
    );
    sqlite3_close(retention_database);
    std::vector<syn_sig_ra::RetentionCandidate> retention_candidates;
    require(
        store.list_retention_candidates(
            90, retention_candidates, error
        ) &&
            retention_candidates.size() == 1 &&
            retention_candidates[0].package_id == successful_package_id,
        "old immutable packages should become retention candidates"
    );
    require(
        store.mark_package_expired(successful_package_id, error),
        "retention cleanup should hide the package"
    );
    require(
        store.find_job(succeeded_job, owner, job, error) ==
            syn_sig_ra::RecordLookupStatus::found &&
            job.status == "succeeded" && job.package_id.empty(),
        "artifact expiry must preserve the succeeded job record"
    );
    const syn_sig_ra::RouteResponse expired_job =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/jobs/" + succeeded_job,
            "/syn_sig_ra",
            "Bearer worker-secret",
            &store,
            packs
        );
    require(
        expired_job.status == 200 &&
            expired_job.body.find("\"artifact_status\":\"expired\"") !=
                std::string::npos,
        "expired artifacts should be explicit in the job API"
    );
    const syn_sig_ra::RouteResponse expired_download =
        syn_sig_ra::route_request(
            "GET",
            "/syn_sig_ra/v1/artifacts/" + successful_package_id +
                "/manifest.json",
            "/syn_sig_ra",
            "Bearer worker-secret",
            &store,
            packs,
            "",
            "",
            root
        );
    require(
        expired_download.status == 404,
        "expired artifacts must no longer be downloadable"
    );

    const syn_sig_ra::RouteResponse rebuild =
        syn_sig_ra::route_request(
            "POST",
            "/syn_sig_ra/v1/jobs/" + succeeded_job + "/rebuild",
            "/syn_sig_ra",
            "Bearer worker-secret",
            &store,
            packs,
            "application/json",
            "",
            root
        );
    require(
        rebuild.status == 202 &&
            rebuild.body.find("\"exact_rebuild_of\":\"" + succeeded_job) !=
                std::string::npos,
        "expired artifacts should queue an explicit exact rebuild: " +
            rebuild.body
    );
    const std::string rebuild_marker("\"job_id\":\"");
    const std::string rebuild_start = rebuild.body.substr(
        rebuild.body.find(rebuild_marker) + rebuild_marker.size());
    const std::string rebuilt_job =
        rebuild_start.substr(0, rebuild_start.find('"'));
    require(
        unlink(protocol_path.c_str()) == 0 && unlink(noise_path.c_str()) == 0,
        "live protocol and noise files should be removable before rebuild");
    config.signal_synth_cli = failure_cli;
    require(
        syn_sig_ra::run_worker_once(config, claimed_job, error) ==
            syn_sig_ra::WorkerRunStatus::succeeded &&
            claimed_job == rebuilt_job,
        "exact rebuild should use the historical generator, not current CLI: " +
            error
    );
    require(
        store.find_job(rebuilt_job, owner, job, error) ==
            syn_sig_ra::RecordLookupStatus::found &&
            job.status == "succeeded" &&
            job.generator_build_identity == successful_generator_identity &&
            job.generator_binary_sha256 == successful_generator_binary &&
            job.package_fingerprint ==
                "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "exact rebuild should preserve generator and package identity"
    );
    write_file(protocol_path, "{\"protocol\":\"new-live-copy\"}\n", 0600);
    write_file(noise_path, "sample,noise\n0,9.0\n", 0600);

    std::string failed_job;
    require(
        store.create_job(
            owner,
            owner.organization_id + "_default",
            "{\"pack_id\":\"test_pack\"}",
            "test_pack",
            pack_path,
            fingerprint,
            "1.0",
            "3.0",
            "sha256:3a8b53b43dbecdeb834ed3faf0fddb8a859464ff4b822caaaa31830f5a06c88f",
            failed_job,
            error
        ),
        "failure job should be queued"
    );
    require(
        syn_sig_ra::run_worker_once(config, claimed_job, error) ==
            syn_sig_ra::WorkerRunStatus::failed_job,
        "CLI failure should fail the claimed job"
    );
    require(
        store.find_job(failed_job, owner, job, error) ==
            syn_sig_ra::RecordLookupStatus::found &&
            job.status == "failed" &&
            job.error_code == "PACK_JSON_RANGE" &&
            !job.generator_binary_sha256.empty(),
        "stable CLI error should be persisted"
    );
    std::string retried_job;
    require(
        store.retry_job(failed_job, owner, retried_job, error) ==
            syn_sig_ra::JobLifecycleStatus::succeeded,
        "a failed pinned job should queue an exact-input retry");
    config.signal_synth_cli = success_cli;
    require(
        syn_sig_ra::run_worker_once(config, claimed_job, error) ==
            syn_sig_ra::WorkerRunStatus::failed_job &&
            claimed_job == retried_job,
        "pinned retry should use its preserved failing generator and recipe");
    require(
        store.find_job(retried_job, owner, job, error) ==
            syn_sig_ra::RecordLookupStatus::found &&
            job.status == "failed" && job.error_code == "PACK_JSON_RANGE",
        "pinned retry should reproduce the original stable failure");

    require(remove_tree(root), "worker test tree should be removed");
    return EXIT_SUCCESS;
}
