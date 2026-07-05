#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/sha256.h"
#include "syn_sig_ra/worker.h"

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

}  // namespace

int main() {
    const std::string fingerprint =
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    syn_sig_ra::CliChallengeResult parsed;
    std::string error;
    require(
        !syn_sig_ra::parse_challenge_stdout("", parsed, error),
        "empty CLI stdout should be rejected"
    );
    require(
        !syn_sig_ra::parse_challenge_stdout(
            "status=challenge-rendered\nstatus=duplicate\n",
            parsed,
            error
        ),
        "duplicate CLI fields should be rejected"
    );

    std::ostringstream root_builder;
    root_builder << "/tmp/syn_sig_ra_worker_test_" << getpid();
    const std::string root = root_builder.str();
    const std::string work = root + "/work";
    const std::string packs = root + "/packs";
    require(mkdir(root.c_str(), 0700) == 0, "test root should be created");
    require(mkdir(work.c_str(), 0700) == 0, "work root should be created");
    require(mkdir(packs.c_str(), 0700) == 0, "pack root should be created");

    const std::string pack_path = packs + "/test_pack.json";
    write_file(pack_path, "{}\n", 0600);
    const std::string success_cli = root + "/success-cli";
    write_file(
        success_cli,
        "#!/bin/sh\n"
        "mkdir \"$5\"\n"
        "echo status=challenge-rendered\n"
        "echo output_directory=\"$5\"\n"
        "echo package_id=test_pack\n"
        "echo scenario_count=1\n"
        "echo pack_fingerprint=" + fingerprint + "\n"
        "echo package_fingerprint=sha256:"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n",
        0700
    );
    const std::string failure_cli = root + "/failure-cli";
    write_file(
        failure_cli,
        "#!/bin/sh\n"
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
    std::string key_hash;
    require(
        syn_sig_ra::sha256_hex("worker-secret", key_hash, error),
        "worker owner hash should succeed"
    );
    require(
        store.create_api_key(owner, key_hash, "worker", error),
        "worker owner should be created: " + error
    );

    std::string succeeded_job;
    require(
        store.create_job(
            owner,
            "{\"pack_id\":\"test_pack\"}",
            "test_pack",
            pack_path,
            fingerprint,
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

    std::string failed_job;
    require(
        store.create_job(
            owner,
            "{\"pack_id\":\"test_pack\"}",
            "test_pack",
            pack_path,
            fingerprint,
            failed_job,
            error
        ),
        "failure job should be queued"
    );
    config.signal_synth_cli = failure_cli;
    require(
        syn_sig_ra::run_worker_once(config, claimed_job, error) ==
            syn_sig_ra::WorkerRunStatus::failed_job,
        "CLI failure should fail the claimed job"
    );
    require(
        store.find_job(failed_job, owner, job, error) ==
            syn_sig_ra::RecordLookupStatus::found &&
            job.status == "failed" &&
            job.error_code == "PACK_JSON_RANGE",
        "stable CLI error should be persisted"
    );

    unlink((work + "/" + succeeded_job).c_str());
    unlink(success_cli.c_str());
    unlink(failure_cli.c_str());
    unlink(pack_path.c_str());
    unlink(database.c_str());
    rmdir((work + "/" + succeeded_job).c_str());
    rmdir(work.c_str());
    rmdir(packs.c_str());
    rmdir(root.c_str());
    return EXIT_SUCCESS;
}
