#include "syn_sig_ra/worker.h"
#include "syn_sig_ra/metadata_store.h"

#include <signal.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

volatile sig_atomic_t running = 1;

void stop_worker(int) {
    running = 0;
}

int run_once(
    const syn_sig_ra::WorkerConfig& config,
    bool report_no_job
) {
    std::string job_id;
    std::string error;
    const syn_sig_ra::WorkerRunStatus status =
        syn_sig_ra::run_worker_once(config, job_id, error);
    const char* status_name =
        status == syn_sig_ra::WorkerRunStatus::no_job ? "idle" :
        status == syn_sig_ra::WorkerRunStatus::succeeded ? "succeeded" :
        status == syn_sig_ra::WorkerRunStatus::failed_job ? "failed_job" :
        "worker_error";
    syn_sig_ra::MetadataStore heartbeat_store(config.database_path);
    std::string heartbeat_error;
    if (!heartbeat_store.record_worker_heartbeat(
            status_name, heartbeat_error)) {
        std::cerr << "event=worker_heartbeat_failed message="
                  << heartbeat_error << '\n';
    }
    if (status == syn_sig_ra::WorkerRunStatus::no_job) {
        if (report_no_job) {
            std::cout << "status=no-job\n";
        }
        return EXIT_SUCCESS;
    }
    if (status == syn_sig_ra::WorkerRunStatus::succeeded) {
        std::cout << "event=job_finished status=succeeded job_id="
                  << job_id << "\nstatus=succeeded\njob_id=" << job_id << '\n';
        return EXIT_SUCCESS;
    }
    if (status == syn_sig_ra::WorkerRunStatus::failed_job) {
        std::cout << "event=job_finished status=failed job_id="
                  << job_id << "\nstatus=failed\njob_id=" << job_id << '\n';
        return 2;
    }
    std::cerr << "event=worker_failed message=" << error << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (argc != 6 || (mode != "run-once" && mode != "run-loop")) {
        std::cerr
            << "usage: " << argv[0]
            << " <run-once|run-loop> <database> <signal-synth-cli>"
            << " <pack-root> <data-root>\n";
        return EXIT_FAILURE;
    }
    syn_sig_ra::WorkerConfig config;
    config.database_path = argv[2];
    config.signal_synth_cli = argv[3];
    config.pack_root = argv[4];
    config.data_root = argv[5];
    if (mode == "run-once") {
        return run_once(config, true);
    }
    signal(SIGTERM, stop_worker);
    signal(SIGINT, stop_worker);
    while (running) {
        const int result = run_once(config, false);
        if (result == EXIT_FAILURE) {
            return result;
        }
        if (running) {
            sleep(result == EXIT_SUCCESS ? 2 : 1);
        }
    }
    return EXIT_SUCCESS;
}
