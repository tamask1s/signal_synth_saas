#ifndef SYN_SIG_RA_WORKER_H
#define SYN_SIG_RA_WORKER_H

#include <string>

#include "syn_sig_ra/core_contract.h"

namespace syn_sig_ra {

struct WorkerConfig {
    std::string database_path;
    std::string signal_synth_cli;
    std::string pack_root;
    std::string data_root;
    std::string noise_asset_root;
    std::string challenge_helper;
    std::string verifier_wheel;
};

struct CliChallengeResult {
    std::string output_directory;
    std::string package_id;
    unsigned long scenario_count;
    std::string pack_fingerprint;
    std::string package_fingerprint;
    std::string canonical_receipt_json;
};

enum class WorkerRunStatus {
    succeeded,
    failed_job,
    no_job,
    worker_error
};

bool parse_challenge_receipt(
    const std::string& stdout_text,
    const CoreIntegrationContract& producer,
    CliChallengeResult& result,
    std::string& error
);

WorkerRunStatus run_worker_once(
    const WorkerConfig& config,
    std::string& job_id,
    std::string& error
);

}  // namespace syn_sig_ra

#endif
