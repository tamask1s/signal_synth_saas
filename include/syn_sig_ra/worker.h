#ifndef SYN_SIG_RA_WORKER_H
#define SYN_SIG_RA_WORKER_H

#include <string>

namespace syn_sig_ra {

struct WorkerConfig {
    std::string database_path;
    std::string signal_synth_cli;
    std::string pack_root;
    std::string data_root;
};

struct CliChallengeResult {
    std::string output_directory;
    std::string package_id;
    unsigned long scenario_count;
    std::string pack_fingerprint;
    std::string package_fingerprint;
};

enum class WorkerRunStatus {
    succeeded,
    failed_job,
    no_job,
    worker_error
};

bool parse_challenge_stdout(
    const std::string& stdout_text,
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
