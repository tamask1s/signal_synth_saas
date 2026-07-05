#include "syn_sig_ra/worker.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 6 || std::string(argv[1]) != "run-once") {
        std::cerr
            << "usage: " << argv[0]
            << " run-once <database> <signal-synth-cli>"
            << " <pack-root> <data-root>\n";
        return EXIT_FAILURE;
    }
    syn_sig_ra::WorkerConfig config;
    config.database_path = argv[2];
    config.signal_synth_cli = argv[3];
    config.pack_root = argv[4];
    config.data_root = argv[5];
    std::string job_id;
    std::string error;
    const syn_sig_ra::WorkerRunStatus status =
        syn_sig_ra::run_worker_once(config, job_id, error);
    if (status == syn_sig_ra::WorkerRunStatus::no_job) {
        std::cout << "status=no-job\n";
        return EXIT_SUCCESS;
    }
    if (status == syn_sig_ra::WorkerRunStatus::succeeded) {
        std::cout << "status=succeeded\njob_id=" << job_id << '\n';
        return EXIT_SUCCESS;
    }
    if (status == syn_sig_ra::WorkerRunStatus::failed_job) {
        std::cout << "status=failed\njob_id=" << job_id << '\n';
        return 2;
    }
    std::cerr << "error=worker-failed message=" << error << '\n';
    return EXIT_FAILURE;
}
