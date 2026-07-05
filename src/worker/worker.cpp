#include "syn_sig_ra/worker.h"

#include "syn_sig_ra/artifact_store.h"
#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/pack_catalog.h"
#include "syn_sig_ra/sha256.h"

#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {

bool regular_file(const std::string& path) {
    struct stat information;
    return lstat(path.c_str(), &information) == 0 &&
           S_ISREG(information.st_mode);
}

bool directory_exists(const std::string& path) {
    struct stat information;
    return lstat(path.c_str(), &information) == 0 &&
           S_ISDIR(information.st_mode);
}

bool valid_fingerprint(const std::string& value) {
    if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
        return false;
    }
    for (std::size_t index = 7; index < value.size(); ++index) {
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool read_binary_file(
    const std::string& path,
    std::string& content,
    std::string& error
) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        error = "unable to read generator binary";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to hash complete generator binary";
        return false;
    }
    content = buffer.str();
    return true;
}

bool append_pipe(int descriptor, std::string& output, std::string& error) {
    char buffer[8192];
    const ssize_t count = read(descriptor, buffer, sizeof(buffer));
    if (count > 0) {
        if (output.size() + static_cast<std::size_t>(count) > 1024u * 1024u) {
            error = "CLI output exceeded 1 MiB";
            return false;
        }
        output.append(buffer, static_cast<std::size_t>(count));
    }
    return count != 0;
}

bool run_cli(
    const std::string& cli,
    const std::string& pack,
    const std::string& output_directory,
    int& exit_code,
    std::string& stdout_text,
    std::string& stderr_text,
    std::string& error
) {
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        error = "unable to allocate CLI pipes";
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        error = "unable to fork CLI worker";
        return false;
    }
    if (child == 0) {
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        execl(
            cli.c_str(),
            cli.c_str(),
            "pack",
            "challenge",
            pack.c_str(),
            "--out",
            output_directory.c_str(),
            static_cast<char*>(nullptr)
        );
        _exit(127);
    }
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    bool stdout_open = true;
    bool stderr_open = true;
    while (stdout_open || stderr_open) {
        pollfd descriptors[2];
        descriptors[0].fd = stdout_pipe[0];
        descriptors[0].events = stdout_open ? POLLIN | POLLHUP : 0;
        descriptors[0].revents = 0;
        descriptors[1].fd = stderr_pipe[0];
        descriptors[1].events = stderr_open ? POLLIN | POLLHUP : 0;
        descriptors[1].revents = 0;
        if (poll(descriptors, 2, -1) < 0 && errno != EINTR) {
            error = "unable to poll CLI output";
            break;
        }
        if (stdout_open && descriptors[0].revents != 0) {
            stdout_open = append_pipe(stdout_pipe[0], stdout_text, error);
        }
        if (stderr_open && descriptors[1].revents != 0) {
            stderr_open = append_pipe(stderr_pipe[0], stderr_text, error);
        }
        if (!error.empty()) {
            kill(child, SIGKILL);
            break;
        }
    }
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (!error.empty()) {
        return false;
    }
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return true;
}

std::string stable_cli_error(const std::string& stderr_text) {
    const std::string prefix("error=");
    if (stderr_text.compare(0, prefix.size(), prefix) != 0) {
        return "GENERATOR_FAILED";
    }
    const std::string::size_type end =
        stderr_text.find_first_of(" \t\r\n", prefix.size());
    const std::string code = stderr_text.substr(
        prefix.size(),
        end == std::string::npos
            ? std::string::npos
            : end - prefix.size()
    );
    if (code.empty() || code.size() > 64) {
        return "GENERATOR_FAILED";
    }
    for (std::string::const_iterator it = code.begin(); it != code.end();
         ++it) {
        if (!((*it >= 'A' && *it <= 'Z') ||
              (*it >= '0' && *it <= '9') ||
              *it == '_')) {
            return "GENERATOR_FAILED";
        }
    }
    return code;
}

}  // namespace

namespace syn_sig_ra {

bool parse_challenge_stdout(
    const std::string& stdout_text,
    CliChallengeResult& result,
    std::string& error
) {
    error.clear();
    if (stdout_text.empty()) {
        error = "CLI stdout was empty";
        return false;
    }
    std::map<std::string, std::string> fields;
    std::istringstream lines(stdout_text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        const std::string::size_type separator = line.find('=');
        if (separator == std::string::npos ||
            !fields.insert(std::make_pair(
                line.substr(0, separator),
                line.substr(separator + 1)
            )).second) {
            error = "CLI stdout contains malformed or duplicate fields";
            return false;
        }
    }
    const char* required[] = {
        "status",
        "output_directory",
        "package_id",
        "scenario_count",
        "pack_fingerprint",
        "package_fingerprint"
    };
    for (std::size_t index = 0;
         index < sizeof(required) / sizeof(required[0]);
         ++index) {
        if (fields.count(required[index]) == 0) {
            error = "CLI stdout is missing required fields";
            return false;
        }
    }
    char* end = nullptr;
    const unsigned long count = std::strtoul(
        fields["scenario_count"].c_str(),
        &end,
        10
    );
    if (fields["status"] != "challenge-rendered" ||
        fields["output_directory"].empty() ||
        !is_valid_pack_id(fields["package_id"]) ||
        count == 0 ||
        end == nullptr ||
        *end != '\0' ||
        !valid_fingerprint(fields["pack_fingerprint"]) ||
        !valid_fingerprint(fields["package_fingerprint"])) {
        error = "CLI stdout contains invalid field values";
        return false;
    }
    CliChallengeResult parsed;
    parsed.output_directory = fields["output_directory"];
    parsed.package_id = fields["package_id"];
    parsed.scenario_count = count;
    parsed.pack_fingerprint = fields["pack_fingerprint"];
    parsed.package_fingerprint = fields["package_fingerprint"];
    result = parsed;
    return true;
}

WorkerRunStatus run_worker_once(
    const WorkerConfig& config,
    std::string& job_id,
    std::string& error
) {
    error.clear();
    job_id.clear();
    MetadataStore store(config.database_path);
    JobRecord job;
    const RecordLookupStatus claim = store.claim_next_job(job, error);
    if (claim == RecordLookupStatus::not_found) {
        return WorkerRunStatus::no_job;
    }
    if (claim == RecordLookupStatus::storage_error) {
        return WorkerRunStatus::worker_error;
    }
    job_id = job.job_id;
    const std::string expected_pack =
        config.pack_root + "/" + job.selected_pack_id + ".json";
    const std::string output_directory =
        config.data_root + "/work/" + job.job_id;
    std::string failure_code;
    std::string failure_message;

    if (!regular_file(config.signal_synth_cli) ||
        !regular_file(expected_pack) ||
        job.source_pack_path != expected_pack ||
        !directory_exists(config.data_root + "/work") ||
        directory_exists(output_directory) ||
        regular_file(output_directory)) {
        failure_code = "WORKER_CONFIG_INVALID";
        failure_message = "Worker paths are invalid.";
    }

    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
    if (failure_code.empty() &&
        !run_cli(
            config.signal_synth_cli,
            expected_pack,
            output_directory,
            exit_code,
            stdout_text,
            stderr_text,
            error
        )) {
        failure_code = "WORKER_EXECUTION_FAILED";
        failure_message = "Generator execution could not be completed.";
    }
    if (failure_code.empty() && exit_code != 0) {
        failure_code = stable_cli_error(stderr_text);
        failure_message = "Generator rejected the challenge job.";
    }

    CliChallengeResult challenge;
    if (failure_code.empty() &&
        !parse_challenge_stdout(stdout_text, challenge, error)) {
        failure_code = "GENERATOR_OUTPUT_INVALID";
        failure_message = "Generator returned invalid machine output.";
    }
    if (failure_code.empty() &&
        (challenge.output_directory != output_directory ||
         challenge.pack_fingerprint != job.pack_fingerprint)) {
        failure_code = "GENERATOR_OUTPUT_MISMATCH";
        failure_message = "Generator output did not match the claimed job.";
    }

    if (!failure_code.empty()) {
        std::string storage_error;
        if (!store.fail_job(
                job.job_id,
                failure_code,
                failure_message,
                storage_error
            )) {
            error = storage_error;
            return WorkerRunStatus::worker_error;
        }
        error.clear();
        return WorkerRunStatus::failed_job;
    }

    std::string generator_binary;
    std::string generator_hash;
    if (!read_binary_file(config.signal_synth_cli, generator_binary, error) ||
        !sha256_hex(generator_binary, generator_hash, error)) {
        std::string ignored;
        store.fail_job(
            job.job_id,
            "GENERATOR_IDENTITY_FAILED",
            "Generator build identity could not be recorded.",
            ignored
        );
        return WorkerRunStatus::worker_error;
    }
    generator_hash = "sha256:" + generator_hash;
    const std::string normalized_command =
        config.signal_synth_cli + " pack challenge " + expected_pack +
        " --out " + output_directory;
    StoredPackage package;
    if (!directory_exists(config.data_root + "/packages") ||
        !store_immutable_package(
            config.data_root,
            output_directory,
            package,
            error
        )) {
        std::string ignored;
        store.fail_job(
            job.job_id,
            "ARTIFACT_STORAGE_FAILED",
            "Generated package could not be stored.",
            ignored
        );
        return WorkerRunStatus::failed_job;
    }
    if (!store.complete_job_with_package(
            job,
            package.package_id,
            challenge.package_fingerprint,
            "signal_synth-cli",
            generator_hash,
            normalized_command,
            package.manifest_hash,
            package.package_directory,
            package.size_bytes,
            error
        )) {
        return WorkerRunStatus::worker_error;
    }
    return WorkerRunStatus::succeeded;
}

}  // namespace syn_sig_ra
