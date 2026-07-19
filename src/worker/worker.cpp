#include "syn_sig_ra/worker.h"

#include "syn_sig_ra/artifact_store.h"
#include "syn_sig_ra/core_contract.h"
#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/pack_catalog.h"
#include "syn_sig_ra/sha256.h"

#include <poll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <jansson.h>

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <climits>
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

bool read_binary_file(
    const std::string& path,
    std::string& content,
    std::string& error
);

bool path_below(const std::string& path, const std::string& root) {
    return path.size() > root.size() &&
        path.compare(0, root.size(), root) == 0 &&
        path[root.size()] == '/';
}

bool safe_relative_path(const std::string& value) {
    if (value.empty() || value.size() > 512 || value[0] == '/' ||
        value.find('\\') != std::string::npos ||
        value.find("//") != std::string::npos) return false;
    std::string::size_type start = 0;
    while (start < value.size()) {
        const std::string::size_type end = value.find('/', start);
        const std::string part = value.substr(
            start, end == std::string::npos
                ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == ".." ||
            part.find(':') != std::string::npos) return false;
        for (std::string::const_iterator character = part.begin();
             character != part.end(); ++character) {
            if (static_cast<unsigned char>(*character) < 0x20u) return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool ensure_directory(
    const std::string& path,
    mode_t mode,
    std::string& error
) {
    if (mkdir(path.c_str(), mode) == 0 || errno == EEXIST) {
        return directory_exists(path);
    }
    error = "unable to create immutable execution storage";
    return false;
}

bool write_binary_file(
    const std::string& path,
    const std::string& content,
    mode_t mode,
    std::string& error
) {
    std::ofstream output(
        path.c_str(), std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output || chmod(path.c_str(), mode) != 0) {
        error = "unable to preserve immutable generator release";
        return false;
    }
    return true;
}

bool copy_regular_file(
    const std::string& source,
    const std::string& destination,
    mode_t mode,
    std::string& error
) {
    std::string content;
    if (!read_binary_file(source, content, error)) return false;
    return write_binary_file(destination, content, mode, error);
}

bool copy_tree(
    const std::string& source,
    const std::string& destination,
    std::string& error
) {
    struct stat information;
    if (lstat(source.c_str(), &information) != 0 ||
        S_ISLNK(information.st_mode)) {
        error = "pack recipe contains an unsafe path";
        return false;
    }
    if (S_ISREG(information.st_mode)) {
        return copy_regular_file(source, destination, 0440, error);
    }
    if (!S_ISDIR(information.st_mode) ||
        !ensure_directory(destination, 0750, error)) {
        error = "pack recipe contains an unsupported object";
        return false;
    }
    DIR* directory = opendir(source.c_str());
    if (directory == nullptr) {
        error = "unable to read pack recipe";
        return false;
    }
    bool succeeded = true;
    for (dirent* entry = readdir(directory);
         entry != nullptr; entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (name != "." && name != ".." &&
            !copy_tree(
                source + "/" + name,
                destination + "/" + name,
                error)) {
            succeeded = false;
            break;
        }
    }
    closedir(directory);
    if (succeeded && chmod(destination.c_str(), 0550) != 0) {
        error = "unable to lock pack recipe";
        succeeded = false;
    }
    return succeeded;
}

bool ensure_dependency_parent(
    const std::string& root,
    const std::string& relative,
    std::string& error
) {
    std::string directory = root;
    std::string::size_type start = 0;
    for (;;) {
        const std::string::size_type separator = relative.find('/', start);
        if (separator == std::string::npos) return true;
        directory += "/" + relative.substr(start, separator - start);
        if (!ensure_directory(directory, 0750, error)) return false;
        start = separator + 1;
    }
}

bool protect_tree(const std::string& path, std::string& error) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0 || S_ISLNK(information.st_mode)) {
        error = "immutable input snapshot contains an unsafe path";
        return false;
    }
    if (S_ISREG(information.st_mode)) {
        if (chmod(path.c_str(), 0440) == 0) return true;
        error = "unable to protect immutable input file";
        return false;
    }
    if (!S_ISDIR(information.st_mode)) {
        error = "immutable input snapshot contains an unsupported object";
        return false;
    }
    DIR* directory = opendir(path.c_str());
    if (directory == nullptr) {
        error = "unable to inspect immutable input snapshot";
        return false;
    }
    bool succeeded = true;
    for (dirent* entry = readdir(directory);
         entry != nullptr; entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (name != "." && name != ".." &&
            !protect_tree(path + "/" + name, error)) {
            succeeded = false;
            break;
        }
    }
    closedir(directory);
    if (succeeded && chmod(path.c_str(), 0550) != 0) {
        error = "unable to protect immutable input directory";
        succeeded = false;
    }
    return succeeded;
}

bool copy_declared_verification_protocol(
    const std::string& source_pack,
    const std::string& source_root,
    const std::string& snapshot_root,
    std::string& error
) {
    std::string document;
    if (!read_binary_file(source_pack, document, error)) return false;
    json_error_t parse_error;
    json_t* root = json_loadb(
        document.data(), document.size(), JSON_REJECT_DUPLICATES, &parse_error);
    json_t* value = root == nullptr
        ? nullptr : json_object_get(root, "verification_protocol_path");
    if (!json_is_object(root) ||
        (value != nullptr && !json_is_string(value))) {
        if (root != nullptr) json_decref(root);
        error = "pack recipe has an invalid verification protocol path";
        return false;
    }
    if (value == nullptr) {
        json_decref(root);
        return true;
    }
    const std::string relative(json_string_value(value));
    json_decref(root);
    if (!safe_relative_path(relative) ||
        !ensure_dependency_parent(snapshot_root, relative, error) ||
        !copy_tree(
            source_root + "/" + relative,
            snapshot_root + "/" + relative,
            error)) {
        if (error.empty()) error = "verification protocol snapshot failed";
        return false;
    }
    return true;
}

bool remove_tree(const std::string& path) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0) return errno == ENOENT;
    if (S_ISLNK(information.st_mode)) return false;
    if (S_ISDIR(information.st_mode)) {
        chmod(path.c_str(), 0700);
        DIR* directory = opendir(path.c_str());
        if (directory == nullptr) return false;
        bool succeeded = true;
        for (dirent* entry = readdir(directory);
             entry != nullptr; entry = readdir(directory)) {
            const std::string name(entry->d_name);
            if (name != "." && name != ".." &&
                !remove_tree(path + "/" + name)) {
                succeeded = false;
                break;
            }
        }
        closedir(directory);
        return succeeded && rmdir(path.c_str()) == 0;
    }
    return S_ISREG(information.st_mode) && unlink(path.c_str()) == 0;
}

bool disk_has_generation_reserve(
    const std::string& data_root,
    unsigned long long* free_bytes = nullptr,
    unsigned long long* reserve_bytes = nullptr
) {
    struct statvfs disk;
    if (statvfs(data_root.c_str(), &disk) != 0) return false;
    const unsigned long long available =
        static_cast<unsigned long long>(disk.f_bavail) * disk.f_frsize;
    const unsigned long long total =
        static_cast<unsigned long long>(disk.f_blocks) * disk.f_frsize;
    const unsigned long long reserve =
        std::max(1024ull * 1024ull * 1024ull, total / 10ull);
    if (free_bytes != nullptr) *free_bytes = available;
    if (reserve_bytes != nullptr) *reserve_bytes = reserve;
    return available > reserve;
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

bool json_string_equals(
    json_t* object,
    const char* key,
    const std::string& expected
) {
    json_t* value = json_object_get(object, key);
    return json_is_string(value) && json_string_value(value) == expected;
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
    const std::string& data_root,
    const std::string& noise_asset_root,
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
        setpgid(0, 0);
        rlimit limit;
        limit.rlim_cur = limit.rlim_max = 600;
        setrlimit(RLIMIT_CPU, &limit);
        limit.rlim_cur = limit.rlim_max = 4ull * 1024ull * 1024ull * 1024ull;
        setrlimit(RLIMIT_AS, &limit);
        limit.rlim_cur = limit.rlim_max = 16ull * 1024ull * 1024ull * 1024ull;
        setrlimit(RLIMIT_FSIZE, &limit);
        limit.rlim_cur = limit.rlim_max = 256;
        setrlimit(RLIMIT_NOFILE, &limit);
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
            "--noise-assets",
            noise_asset_root.c_str(),
            static_cast<char*>(nullptr)
        );
        _exit(127);
    }
    setpgid(child, child);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    bool stdout_open = true;
    bool stderr_open = true;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::minutes(15);
    while (stdout_open || stderr_open) {
        pollfd descriptors[2];
        descriptors[0].fd = stdout_pipe[0];
        descriptors[0].events = stdout_open ? POLLIN | POLLHUP : 0;
        descriptors[0].revents = 0;
        descriptors[1].fd = stderr_pipe[0];
        descriptors[1].events = stderr_open ? POLLIN | POLLHUP : 0;
        descriptors[1].revents = 0;
        const int polled = poll(descriptors, 2, 250);
        if (polled < 0 && errno != EINTR) {
            error = "unable to poll CLI output";
            break;
        }
        if (!disk_has_generation_reserve(data_root)) {
            error = "DISK_PRESSURE";
            kill(-child, SIGKILL);
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "GENERATION_TIMEOUT";
            kill(-child, SIGKILL);
            break;
        }
        if (stdout_open && descriptors[0].revents != 0) {
            stdout_open = append_pipe(stdout_pipe[0], stdout_text, error);
        }
        if (stderr_open && descriptors[1].revents != 0) {
            stderr_open = append_pipe(stderr_pipe[0], stderr_text, error);
        }
        if (!error.empty()) {
            kill(-child, SIGKILL);
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

bool archive_generator_release(
    const std::string& data_root,
    const std::string& source_cli,
    std::string& identity,
    std::string& release_cli,
    std::string& error
) {
    std::string binary;
    std::string hash;
    if (!read_binary_file(source_cli, binary, error) ||
        !syn_sig_ra::sha256_hex(binary, hash, error)) {
        return false;
    }
    identity = "sha256:" + hash;
    const std::string releases = data_root + "/generator_releases";
    const std::string release = releases + "/" + hash;
    release_cli = release + "/signal-synth";
    if (!ensure_directory(releases, 0750, error) ||
        !ensure_directory(release, 0750, error)) {
        return false;
    }
    if (!regular_file(release_cli) &&
        !write_binary_file(release_cli, binary, 0550, error)) {
        return false;
    }
    std::string preserved;
    std::string preserved_hash;
    if (!read_binary_file(release_cli, preserved, error) ||
        !syn_sig_ra::sha256_hex(preserved, preserved_hash, error) ||
        preserved_hash != hash) {
        error = "preserved generator release hash mismatch";
        return false;
    }
    chmod(release.c_str(), 0550);
    return true;
}

bool resolve_generator_release(
    const std::string& data_root,
    const std::string& identity,
    std::string& release_cli
) {
    if (!valid_fingerprint(identity)) return false;
    release_cli =
        data_root + "/generator_releases/" + identity.substr(7) +
        "/signal-synth";
    std::string binary;
    std::string hash;
    std::string error;
    return regular_file(release_cli) &&
        read_binary_file(release_cli, binary, error) &&
        syn_sig_ra::sha256_hex(binary, hash, error) &&
        hash == identity.substr(7);
}

bool snapshot_pack_recipe(
    const std::string& data_root,
    const std::string& job_id,
    const std::string& source_pack,
    const std::string& noise_asset_root,
    std::string& snapshot_pack,
    std::string& snapshot_noise_assets,
    std::string& error
) {
    const std::string recipes = data_root + "/recipes";
    const std::string recipe = recipes + "/" + job_id;
    if (!ensure_directory(recipes, 0750, error) ||
        !ensure_directory(recipe, 0750, error)) {
        return false;
    }
    snapshot_pack = recipe + "/pack.json";
    snapshot_noise_assets = recipe + "/noise_assets";
    if (!copy_regular_file(source_pack, snapshot_pack, 0440, error)) {
        remove_tree(recipe);
        return false;
    }
    const std::string::size_type separator = source_pack.rfind('/');
    if (separator == std::string::npos) {
        error = "pack recipe source has no trusted root";
        remove_tree(recipe);
        return false;
    }
    const std::string source_root = source_pack.substr(0, separator);
    if (directory_exists(source_root + "/scenarios") &&
        !copy_tree(
            source_root + "/scenarios",
            recipe + "/scenarios",
            error)) {
        remove_tree(recipe);
        return false;
    }
    if (!copy_declared_verification_protocol(
            source_pack, source_root, recipe, error) ||
        !copy_tree(noise_asset_root, snapshot_noise_assets, error) ||
        !protect_tree(recipe, error)) {
        remove_tree(recipe);
        return false;
    }
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

bool challenge_metadata_matches_job(
    const std::string& text,
    const syn_sig_ra::JobRecord& job,
    const syn_sig_ra::CoreIntegrationContract& producer,
    const syn_sig_ra::CliChallengeResult& challenge,
    std::string& error
) {
    json_error_t parse_error;
    json_t* root = json_loadb(
        text.data(), text.size(), JSON_REJECT_DUPLICATES, &parse_error);
    json_t* case_count = root == nullptr
        ? nullptr : json_object_get(root, "case_count");
    json_t* cases = root == nullptr
        ? nullptr : json_object_get(root, "cases");
    json_t* targets = root == nullptr
        ? nullptr : json_object_get(root, "targets");
    json_t* outputs = root == nullptr
        ? nullptr : json_object_get(root, "submission_outputs");
    json_t* roles = root == nullptr
        ? nullptr : json_object_get(root, "roles");
    const bool matches = json_is_object(root) &&
        json_string_equals(root, "package_id", job.selected_pack_id) &&
        json_string_equals(root, "package_id", challenge.package_id) &&
        json_string_equals(root, "pack_version", job.selected_pack_version) &&
        json_string_equals(root, "pack_fingerprint", job.pack_fingerprint) &&
        json_string_equals(
            root, "pack_fingerprint", challenge.pack_fingerprint) &&
        json_string_equals(
            root, "generator_version", producer.generator_version) &&
        json_string_equals(
            root, "generator_git_commit", producer.generator_git_commit) &&
        json_is_integer(case_count) && json_integer_value(case_count) > 0 &&
        static_cast<unsigned long long>(json_integer_value(case_count)) ==
            challenge.scenario_count &&
        json_is_array(cases) && json_array_size(cases) ==
            static_cast<std::size_t>(challenge.scenario_count) &&
        json_is_array(targets) && json_array_size(targets) > 0 &&
        json_is_array(outputs) && json_is_object(roles);
    if (root != nullptr) json_decref(root);
    if (!matches) {
        error = "challenge metadata does not match the reserved job and receipt";
    }
    return matches;
}

}  // namespace

namespace syn_sig_ra {

bool parse_challenge_receipt(
    const std::string& stdout_text,
    const CoreIntegrationContract& producer,
    CliChallengeResult& result,
    std::string& error
) {
    error.clear();
    json_error_t parse_error;
    json_t* root = json_loadb(
        stdout_text.data(), stdout_text.size(), JSON_REJECT_DUPLICATES,
        &parse_error);
    if (!json_is_object(root) || json_object_size(root) != 10) {
        if (root != nullptr) json_decref(root);
        error = "CLI stdout is not the exact challenge receipt object";
        return false;
    }
    json_t* schema = json_object_get(root, "schema_version");
    json_t* output = json_object_get(root, "output_directory");
    json_t* package_id = json_object_get(root, "package_id");
    json_t* count = json_object_get(root, "scenario_count");
    json_t* pack_fingerprint = json_object_get(root, "pack_fingerprint");
    json_t* package_fingerprint = json_object_get(root, "package_fingerprint");
    json_t* generator = json_object_get(root, "generator");
    json_t* contracts = json_object_get(root, "contracts");
    const bool generator_shape = json_is_object(generator) &&
        json_object_size(generator) == 4;
    const bool contracts_shape = json_is_object(contracts) &&
        json_object_size(contracts) == 3;
    const bool valid = json_is_integer(schema) && json_integer_value(schema) == 1 &&
        json_string_equals(root, "contract", producer.integration_contract) &&
        json_string_equals(root, "status", "challenge_rendered") &&
        json_is_string(output) && json_string_length(output) > 0 &&
        json_is_string(package_id) &&
        is_valid_pack_id(json_string_value(package_id)) &&
        json_is_integer(count) && json_integer_value(count) > 0 &&
        static_cast<unsigned long long>(json_integer_value(count)) <= ULONG_MAX &&
        json_is_string(pack_fingerprint) &&
        valid_fingerprint(json_string_value(pack_fingerprint)) &&
        json_is_string(package_fingerprint) &&
        valid_fingerprint(json_string_value(package_fingerprint)) &&
        generator_shape && contracts_shape &&
        json_string_equals(generator, "name", producer.generator_name) &&
        json_string_equals(generator, "version", producer.generator_version) &&
        json_string_equals(
            generator, "git_commit", producer.generator_git_commit) &&
        json_string_equals(
            generator, "build_identity", producer.generator_build_identity) &&
        json_string_equals(
            contracts, "challenge_package", producer.challenge_package) &&
        json_string_equals(
            contracts, "scoring_manifest", producer.scoring_manifest);
    const bool protocol_valid = contracts_shape && json_string_equals(
        contracts, "verification_protocol", producer.verification_protocol);
    char* canonical = valid && protocol_valid
        ? json_dumps(root, JSON_COMPACT | JSON_SORT_KEYS) : nullptr;
    if (!valid || !protocol_valid || canonical == nullptr) {
        json_decref(root);
        error = "CLI challenge receipt fields do not match the accepted producer";
        return false;
    }
    CliChallengeResult parsed;
    parsed.output_directory = json_string_value(output);
    parsed.package_id = json_string_value(package_id);
    parsed.scenario_count = static_cast<unsigned long>(json_integer_value(count));
    parsed.pack_fingerprint = json_string_value(pack_fingerprint);
    parsed.package_fingerprint = json_string_value(package_fingerprint);
    parsed.canonical_receipt_json = canonical;
    free(canonical);
    json_decref(root);
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
    CoreIntegrationContract current_producer;
    if (!validate_core_integration(
            config.signal_synth_cli, current_producer, error)) {
        return WorkerRunStatus::worker_error;
    }
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
    const bool pinned_execution = !job.generator_binary_sha256.empty();
    const std::string built_in_pack =
        config.pack_root + "/" + job.selected_pack_id + ".json";
    const std::string custom_pack =
        config.data_root + "/custom_packs/" + job.selected_pack_id +
        "/pack.json";
    const bool current_pack_path_allowed =
        job.source_pack_path == built_in_pack ||
        (job.selected_pack_id.compare(0, 12, "custom_pack_") == 0 &&
         job.source_pack_path == custom_pack);
    std::string expected_pack = job.source_pack_path;
    std::string execution_cli;
    std::string execution_noise_asset_root;
    std::string generator_binary_sha256 = job.generator_binary_sha256;
    CoreIntegrationContract execution_producer = current_producer;
    const std::string output_directory =
        config.data_root + "/work/" + job.job_id;
    std::string failure_code;
    std::string failure_message;

    if (!directory_exists(config.data_root + "/work") ||
        !directory_exists(config.data_root + "/packages") ||
        !directory_exists(config.noise_asset_root) ||
        !regular_file(config.challenge_helper) ||
        !regular_file(config.verifier_wheel) ||
        directory_exists(output_directory) ||
        regular_file(output_directory)) {
        failure_code = "WORKER_CONFIG_INVALID";
        failure_message = "Worker paths are invalid.";
    }
    if (failure_code.empty() && !disk_has_generation_reserve(config.data_root)) {
        failure_code = "DISK_PRESSURE";
        failure_message =
            "Generation is paused to preserve the server disk reserve.";
    }
    if (failure_code.empty() && pinned_execution) {
        const std::string::size_type separator = expected_pack.rfind('/');
        execution_noise_asset_root = separator == std::string::npos
            ? std::string() : expected_pack.substr(0, separator) + "/noise_assets";
        if (!path_below(
                expected_pack, config.data_root + "/recipes") ||
            !regular_file(expected_pack) ||
            !path_below(
                execution_noise_asset_root, config.data_root + "/recipes") ||
            !directory_exists(execution_noise_asset_root) ||
            !resolve_generator_release(
                config.data_root,
                generator_binary_sha256,
                execution_cli)) {
            failure_code = "HISTORICAL_INPUT_UNAVAILABLE";
            failure_message =
                "The exact pack recipe or generator release is unavailable.";
        }
    }
    if (failure_code.empty() && !pinned_execution) {
        const std::string source_pack = expected_pack;
        if (!regular_file(config.signal_synth_cli) ||
            !regular_file(expected_pack) ||
            !current_pack_path_allowed ||
            !archive_generator_release(
                config.data_root,
                config.signal_synth_cli,
                generator_binary_sha256,
                execution_cli,
                error) ||
            !snapshot_pack_recipe(
                config.data_root,
                job.job_id,
                source_pack,
                config.noise_asset_root,
                expected_pack,
                execution_noise_asset_root,
                error) ||
            !store.pin_job_inputs(
                job.job_id,
                expected_pack,
                current_producer.integration_contract,
                current_producer.canonical_json,
                current_producer.generator_version,
                current_producer.generator_git_commit,
                current_producer.generator_build_identity,
                generator_binary_sha256,
                error)) {
            failure_code = "EXECUTION_INPUT_SNAPSHOT_FAILED";
            failure_message =
                "Immutable generator and pack inputs could not be preserved.";
        }
    }

    if (failure_code.empty() && pinned_execution &&
        !cli_core_integration_contract(
            execution_cli, execution_producer, error)) {
        failure_code = "HISTORICAL_INPUT_UNAVAILABLE";
        failure_message = "The preserved generator contract is unavailable.";
    }
    if (failure_code.empty() && pinned_execution &&
        (job.integration_contract_version != execution_producer.integration_contract ||
         job.integration_contract_json != execution_producer.canonical_json ||
         job.generator_version != execution_producer.generator_version ||
         job.generator_git_commit != execution_producer.generator_git_commit ||
         job.generator_build_identity != execution_producer.generator_build_identity)) {
        failure_code = "HISTORICAL_INPUT_MISMATCH";
        failure_message = "The preserved generator identity does not match the job.";
    }

    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
    if (failure_code.empty() &&
        !run_cli(
            execution_cli,
            expected_pack,
            output_directory,
            config.data_root,
            execution_noise_asset_root,
            exit_code,
            stdout_text,
            stderr_text,
            error
        )) {
        failure_code = error == "DISK_PRESSURE" ? "DISK_PRESSURE" :
            error == "GENERATION_TIMEOUT" ? "GENERATION_TIMEOUT" :
            "WORKER_EXECUTION_FAILED";
        failure_message = error == "DISK_PRESSURE"
            ? "Generation stopped before exhausting the server disk reserve."
            : error == "GENERATION_TIMEOUT"
                ? "Generation exceeded the bounded execution time."
                : "Generator execution could not be completed.";
    }
    if (failure_code.empty() && exit_code != 0) {
        failure_code = stable_cli_error(stderr_text);
        failure_message = "Generator rejected the challenge job.";
    }

    CliChallengeResult challenge;
    if (failure_code.empty() &&
        !parse_challenge_receipt(
            stdout_text, execution_producer, challenge, error)) {
        failure_code = "GENERATOR_OUTPUT_INVALID";
        failure_message = "Generator returned invalid machine output.";
    }
    if (failure_code.empty() &&
        (challenge.output_directory != output_directory ||
         challenge.package_id != job.selected_pack_id ||
         challenge.pack_fingerprint != job.pack_fingerprint ||
         (!job.package_fingerprint.empty() &&
          challenge.package_fingerprint != job.package_fingerprint))) {
        failure_code = "GENERATOR_OUTPUT_MISMATCH";
        failure_message =
            "Exact rebuild output did not match the preserved job identity.";
    }

    std::string challenge_metadata_json;
    if (failure_code.empty() && !inspect_challenge_package(
            config.challenge_helper,
            config.verifier_wheel,
            output_directory,
            challenge_metadata_json,
            error)) {
        failure_code = "CHALLENGE_VALIDATION_FAILED";
        failure_message =
            "Generated challenge failed the verifier trust-boundary checks.";
    }
    if (failure_code.empty() && !challenge_metadata_matches_job(
            challenge_metadata_json,
            job,
            execution_producer,
            challenge,
            error)) {
        failure_code = "CHALLENGE_IDENTITY_MISMATCH";
        failure_message =
            "Generated challenge identity does not match the reserved job.";
    }

    if (!failure_code.empty()) {
        remove_tree(output_directory);
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

    const std::string normalized_command =
        "signal-synth pack challenge <immutable-pack-recipe>"
        " --out <new-directory> --noise-assets <approved-registry>";
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
    std::string archived_metadata_json;
    if (!inspect_challenge_package(
            config.challenge_helper,
            config.verifier_wheel,
            package.package_directory + "/package.zip",
            archived_metadata_json,
            error) ||
        archived_metadata_json != challenge_metadata_json) {
        std::string ignored;
        discard_stored_package(config.data_root, package.package_id, ignored);
        store.fail_job(
            job.job_id,
            "CHALLENGE_ROUNDTRIP_FAILED",
            "Stored challenge failed the verifier round-trip checks.",
            ignored);
        return WorkerRunStatus::failed_job;
    }
    if (!store.complete_job_with_package(
            job,
            package.package_id,
            challenge.package_fingerprint,
            execution_producer.integration_contract,
            execution_producer.canonical_json,
            execution_producer.generator_version,
            execution_producer.generator_git_commit,
            execution_producer.generator_build_identity,
            generator_binary_sha256,
            challenge.canonical_receipt_json,
            challenge_metadata_json,
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
