#include "syn_sig_ra/artifact_store.h"

#include "syn_sig_ra/build_info.h"
#include "syn_sig_ra/random_id.h"
#include "syn_sig_ra/sha256.h"
#include "syn_sig_ra/signal_viewer.h"

#include <jansson.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool read_file(
    const std::string& path,
    std::string& content,
    std::string& error
) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        error = "unable to read generated manifest";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read complete generated manifest";
        return false;
    }
    content = buffer.str();
    return true;
}

bool write_file(
    const std::string& path,
    const std::string& content,
    std::string& error
) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        error = "unable to write stored manifest";
        return false;
    }
    return true;
}

bool run_zip(
    const std::string& extracted_directory,
    const std::string& archive_path,
    std::string& error
) {
    const pid_t child = fork();
    if (child < 0) {
        error = "unable to fork zip process";
        return false;
    }
    if (child == 0) {
        if (chdir(extracted_directory.c_str()) != 0) {
            _exit(126);
        }
        execl(
            "/usr/bin/zip",
            "/usr/bin/zip",
            "-q",
            "-r",
            archive_path.c_str(),
            ".",
            static_cast<char*>(nullptr)
        );
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "zip archive creation failed";
        return false;
    }
    return true;
}

bool make_read_only(const std::string& path, std::string& error) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0) {
        error = "unable to inspect stored package";
        return false;
    }
    if (S_ISLNK(information.st_mode)) {
        error = "stored packages must not contain symbolic links";
        return false;
    }
    if (S_ISDIR(information.st_mode)) {
        if (chmod(path.c_str(), 0700) != 0) {
            error = "unable to make stored directory removable";
            return false;
        }
        DIR* directory = opendir(path.c_str());
        if (directory == nullptr) {
            error = "unable to inspect stored package directory";
            return false;
        }
        bool succeeded = true;
        for (dirent* entry = readdir(directory);
             entry != nullptr;
             entry = readdir(directory)) {
            const std::string name(entry->d_name);
            if (name == "." || name == "..") {
                continue;
            }
            if (!make_read_only(path + "/" + name, error)) {
                succeeded = false;
                break;
            }
        }
        closedir(directory);
        if (!succeeded || chmod(path.c_str(), 0550) != 0) {
            if (error.empty()) {
                error = "unable to make package directory immutable";
            }
            return false;
        }
        return true;
    }
    if (!S_ISREG(information.st_mode) || chmod(path.c_str(), 0440) != 0) {
        error = "unable to make package file immutable";
        return false;
    }
    return true;
}

bool remove_tree(const std::string& path, std::string& error) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0) {
        if (errno == ENOENT) return true;
        error = "unable to inspect temporary rendered package";
        return false;
    }
    if (S_ISLNK(information.st_mode)) {
        error = "temporary rendered packages must not contain symbolic links";
        return false;
    }
    if (S_ISDIR(information.st_mode)) {
        if (chmod(path.c_str(), 0700) != 0) {
            error = "unable to make stored directory removable";
            return false;
        }
        DIR* directory = opendir(path.c_str());
        if (directory == nullptr) {
            error = "unable to open temporary rendered package";
            return false;
        }
        bool succeeded = true;
        for (dirent* entry = readdir(directory);
             entry != nullptr; entry = readdir(directory)) {
            const std::string name(entry->d_name);
            if (name != "." && name != ".." &&
                !remove_tree(path + "/" + name, error)) {
                succeeded = false;
                break;
            }
        }
        closedir(directory);
        if (!succeeded || rmdir(path.c_str()) != 0) {
            if (error.empty()) error = "unable to remove stored directory";
            return false;
        }
        return true;
    }
    if (!S_ISREG(information.st_mode) || unlink(path.c_str()) != 0) {
        error = "unable to remove temporary rendered package";
        return false;
    }
    return true;
}

bool tree_size(const std::string& path, long long& size, std::string& error) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0) {
        error = "unable to account stored package size";
        return false;
    }
    if (S_ISREG(information.st_mode)) {
        size += static_cast<long long>(information.st_size);
        return true;
    }
    if (!S_ISDIR(information.st_mode)) {
        error = "unexpected object in stored package";
        return false;
    }
    DIR* directory = opendir(path.c_str());
    if (directory == nullptr) {
        error = "unable to account stored package directory";
        return false;
    }
    bool succeeded = true;
    for (dirent* entry = readdir(directory);
         entry != nullptr; entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (name != "." && name != ".." &&
            !tree_size(path + "/" + name, size, error)) {
            succeeded = false;
            break;
        }
    }
    closedir(directory);
    return succeeded;
}

bool safe_component(const std::string& value) {
    if (value.empty() || value.size() > 160) return false;
    for (std::string::const_iterator it = value.begin();
         it != value.end(); ++it) {
        const unsigned char character = static_cast<unsigned char>(*it);
        if (!std::isalnum(character) && character != '_' &&
            character != '-' && character != '.') return false;
    }
    return value != "." && value != "..";
}

bool ensure_directory(
    const std::string& path,
    mode_t mode,
    std::string& error
) {
    struct stat information;
    if (lstat(path.c_str(), &information) == 0) {
        if (!S_ISDIR(information.st_mode) || S_ISLNK(information.st_mode)) {
            error = "derived artifact path is not a safe directory";
            return false;
        }
        return true;
    }
    if (errno != ENOENT) {
        error = "unable to create derived artifact directory";
        return false;
    }
    if (mkdir(path.c_str(), mode) == 0) return true;
    if (errno != EEXIST || lstat(path.c_str(), &information) != 0 ||
        !S_ISDIR(information.st_mode) || S_ISLNK(information.st_mode)) {
        error = "unable to create derived artifact directory";
        return false;
    }
    return true;
}

bool prepare_cache_directory(
    const std::string& data_root,
    const std::string& package_id,
    std::string& directory,
    std::string& error
) {
    if (data_root.empty() || package_id.compare(0, 4, "pkg_") != 0 ||
        !safe_component(package_id)) {
        error = "invalid derived artifact cache identity";
        return false;
    }
    const std::string root = data_root + "/derived-artifacts";
    directory = root + "/" + package_id;
    return ensure_directory(root, 0750, error) &&
        ensure_directory(directory, 0750, error);
}

bool regular_file(const std::string& path, long long& size) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0 ||
        !S_ISREG(information.st_mode) || S_ISLNK(information.st_mode) ||
        information.st_size < 0) return false;
    size = static_cast<long long>(information.st_size);
    return true;
}

bool valid_sha256(const std::string& value) {
    if (value.size() != 64) return false;
    for (std::string::const_iterator it = value.begin();
         it != value.end(); ++it) {
        if (!std::isxdigit(static_cast<unsigned char>(*it))) return false;
    }
    return true;
}

bool read_checksum(const std::string& path, std::string& checksum) {
    std::ifstream input(path.c_str());
    if (!input) return false;
    input >> checksum;
    return valid_sha256(checksum);
}

bool write_all(int descriptor, const char* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t count = write(
            descriptor, data + written, size - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        written += static_cast<std::size_t>(count);
    }
    return true;
}

bool write_checksum_atomic(
    const std::string& path,
    const std::string& checksum,
    std::string& error
) {
    std::vector<char> temporary(path.begin(), path.end());
    const char suffix[] = ".tmp-XXXXXX";
    temporary.insert(temporary.end(), suffix, suffix + sizeof(suffix));
    const int descriptor = mkstemp(temporary.data());
    if (descriptor < 0) {
        error = "unable to allocate checksum file";
        return false;
    }
    const std::string content = checksum + "\n";
    bool succeeded = fchmod(descriptor, 0640) == 0 &&
        write_all(descriptor, content.data(), content.size()) &&
        fsync(descriptor) == 0;
    if (close(descriptor) != 0) succeeded = false;
    if (succeeded && rename(temporary.data(), path.c_str()) != 0) {
        succeeded = false;
    }
    if (!succeeded) {
        unlink(temporary.data());
        error = "unable to publish checksum file atomically";
    }
    return succeeded;
}

int lock_cache(const std::string& directory, std::string& error) {
    const std::string path = directory + "/build.lock";
    const int descriptor = open(
        path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0640);
    if (descriptor < 0 || flock(descriptor, LOCK_EX) != 0) {
        if (descriptor >= 0) close(descriptor);
        error = "unable to lock derived artifact cache";
        return -1;
    }
    return descriptor;
}

void unlock_cache(int descriptor) {
    if (descriptor < 0) return;
    flock(descriptor, LOCK_UN);
    close(descriptor);
}

bool disk_has_capacity(
    const std::string& data_root,
    unsigned long long required
) {
    struct statvfs disk;
    if (statvfs(data_root.c_str(), &disk) != 0) return false;
    const unsigned long long free_bytes =
        static_cast<unsigned long long>(disk.f_bavail) * disk.f_frsize;
    const unsigned long long total_bytes =
        static_cast<unsigned long long>(disk.f_blocks) * disk.f_frsize;
    const unsigned long long reserve = std::max(
        1024ull * 1024ull * 1024ull,
        total_bytes / 10ull);
    return free_bytes > reserve && free_bytes - reserve > required;
}

void remove_stale_workspaces(const std::string& directory) {
    DIR* entries = opendir(directory.c_str());
    if (entries == nullptr) return;
    std::string ignored;
    for (dirent* entry = readdir(entries);
         entry != nullptr; entry = readdir(entries)) {
        const std::string name(entry->d_name);
        if (name.compare(0, 7, ".build-") == 0) {
            remove_tree(directory + "/" + name, ignored);
            ignored.clear();
        }
    }
    closedir(entries);
}

bool append_limited(
    int descriptor,
    std::string& output,
    std::size_t limit,
    std::string& error
) {
    char buffer[8192];
    const ssize_t count = read(descriptor, buffer, sizeof(buffer));
    if (count > 0) {
        if (output.size() + static_cast<std::size_t>(count) > limit) {
            error = "challenge helper output exceeded its limit";
            return false;
        }
        output.append(buffer, static_cast<std::size_t>(count));
    }
    return count != 0;
}

bool run_challenge_helper(
    const std::string& helper,
    const std::string& wheel,
    const std::vector<std::string>& arguments,
    std::string& output,
    std::string& error
) {
    long long ignored = 0;
    if (!regular_file(helper, ignored) || !regular_file(wheel, ignored)) {
        error = "challenge helper or verifier wheel is unavailable";
        return false;
    }
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        error = "unable to allocate challenge helper pipes";
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        error = "unable to fork challenge helper";
        return false;
    }
    if (child == 0) {
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        setenv("PYTHONPATH", wheel.c_str(), 1);
        setenv("PYTHONNOUSERSITE", "1", 1);
        setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
        std::vector<std::string> values;
        values.push_back("/usr/bin/python3");
        values.push_back(helper);
        values.insert(values.end(), arguments.begin(), arguments.end());
        std::vector<char*> raw;
        for (std::vector<std::string>::iterator it = values.begin();
             it != values.end(); ++it) raw.push_back(const_cast<char*>(it->c_str()));
        raw.push_back(nullptr);
        execv("/usr/bin/python3", raw.data());
        _exit(127);
    }
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    bool stdout_open = true;
    bool stderr_open = true;
    std::string stderr_text;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::minutes(10);
    while ((stdout_open || stderr_open) && error.empty()) {
        pollfd descriptors[2] = {
            {stdout_pipe[0], static_cast<short>(stdout_open ? POLLIN | POLLHUP : 0), 0},
            {stderr_pipe[0], static_cast<short>(stderr_open ? POLLIN | POLLHUP : 0), 0}
        };
        const int polled = poll(descriptors, 2, 250);
        if (polled < 0 && errno != EINTR) {
            error = "unable to poll challenge helper";
        } else {
            if (stdout_open && descriptors[0].revents)
                stdout_open = append_limited(
                    stdout_pipe[0], output, 8u * 1024u * 1024u, error);
            if (stderr_open && descriptors[1].revents)
                stderr_open = append_limited(
                    stderr_pipe[0], stderr_text, 64u * 1024u, error);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "challenge helper timed out";
        }
        if (!error.empty()) kill(child, SIGKILL);
    }
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (error.empty() && (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
        error = stderr_text.empty() ? "challenge helper failed" : stderr_text;
    if (error.empty() && !stderr_text.empty())
        error = "challenge helper wrote unexpected diagnostic output";
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    return error.empty() && !output.empty();
}

bool exact_json_string(json_t* object, const char* key, const char* value) {
    json_t* field = json_is_object(object) ? json_object_get(object, key) : nullptr;
    return json_is_string(field) &&
        std::string(json_string_value(field)) == std::string(value);
}

bool nonempty_json_string(json_t* object, const char* key) {
    json_t* field = json_is_object(object) ? json_object_get(object, key) : nullptr;
    return json_is_string(field) && json_string_length(field) > 0;
}

bool valid_verification_metadata(json_t* value) {
    if (!json_is_object(value) || json_object_size(value) != 7 ||
        !nonempty_json_string(value, "notice") ||
        !exact_json_string(value, "evidence_result", "not_run") ||
        !exact_json_string(value, "policy_result", "not_run")) {
        return false;
    }
    json_t* eligible = json_object_get(value, "evidence_eligible");
    json_t* complete = json_object_get(value, "matrix_complete");
    json_t* protocol = json_object_get(value, "protocol");
    if (exact_json_string(value, "mode", "diagnostic")) {
        return json_is_false(eligible) && json_is_null(complete) &&
            json_is_null(protocol);
    }
    if (!exact_json_string(value, "mode", "evidence") ||
        !json_is_true(eligible) || !json_is_true(complete) ||
        !json_is_object(protocol) || json_object_size(protocol) != 11 ||
        !nonempty_json_string(protocol, "protocol_id") ||
        !exact_json_string(
            protocol, "contract", "synsigra_verification_protocol_v2") ||
        !nonempty_json_string(protocol, "path") ||
        !json_is_integer(json_object_get(protocol, "size_bytes")) ||
        json_integer_value(json_object_get(protocol, "size_bytes")) < 1 ||
        !nonempty_json_string(protocol, "sha256") ||
        !nonempty_json_string(protocol, "context_of_use") ||
        !exact_json_string(
            protocol, "scoring_contract", "synsigra_local_verification_v3") ||
        !(exact_json_string(protocol, "verdict_scope", "aggregate") ||
          exact_json_string(protocol, "verdict_scope", "per_case")) ||
        !nonempty_json_string(protocol, "acceptance_profile_id") ||
        !json_is_integer(json_object_get(protocol, "required_case_target_count")) ||
        json_integer_value(json_object_get(protocol, "required_case_target_count")) < 1 ||
        !nonempty_json_string(protocol, "evidence_boundary")) {
        return false;
    }
    return true;
}

bool valid_challenge_metadata(const std::string& text) {
    json_error_t parse_error;
    json_t* root = json_loadb(
        text.data(), text.size(), JSON_REJECT_DUPLICATES, &parse_error);
    const bool valid = json_is_object(root) &&
        json_is_integer(json_object_get(root, "schema_version")) &&
        json_integer_value(json_object_get(root, "schema_version")) == 1 &&
        json_is_string(json_object_get(root, "contract")) &&
        std::string(json_string_value(json_object_get(root, "contract"))) ==
            "synsigra_saas_challenge_metadata_v1" &&
        json_is_string(json_object_get(root, "verifier_version")) &&
        std::string(json_string_value(json_object_get(root, "verifier_version"))) ==
            SYN_SIG_RA_EXPECTED_PYTHON_VERIFIER &&
        json_is_string(json_object_get(root, "challenge_contract")) &&
        std::string(json_string_value(json_object_get(root, "challenge_contract"))) ==
            "synsigra_challenge_package_v3" &&
        json_is_string(json_object_get(root, "scoring_manifest_contract")) &&
        std::string(json_string_value(json_object_get(root, "scoring_manifest_contract"))) ==
            "synsigra_scoring_manifest_v3" &&
        json_is_string(json_object_get(root, "submission_contract")) &&
        std::string(json_string_value(json_object_get(root, "submission_contract"))) ==
            "synsigra_submission_v1" &&
        json_is_string(json_object_get(root, "submission_formats_contract")) &&
        std::string(json_string_value(json_object_get(root, "submission_formats_contract"))) ==
            "synsigra_submission_formats_v2" &&
        json_is_string(json_object_get(root, "measurement_values_contract")) &&
        std::string(json_string_value(json_object_get(root, "measurement_values_contract"))) ==
            "synsigra_measurement_values_v2" &&
        json_is_string(json_object_get(root, "measurement_truth_contract")) &&
        std::string(json_string_value(json_object_get(root, "measurement_truth_contract"))) ==
            "synsigra_measurement_truth_v2" &&
        json_is_string(json_object_get(root, "measurement_scoring_contract")) &&
        std::string(json_string_value(json_object_get(root, "measurement_scoring_contract"))) ==
            "synsigra_measurement_score_v2" &&
        json_is_string(json_object_get(root, "local_verification_contract")) &&
        std::string(json_string_value(json_object_get(root, "local_verification_contract"))) ==
            "synsigra_local_verification_v3" &&
        valid_verification_metadata(json_object_get(root, "verification")) &&
        json_is_object(json_object_get(root, "external_noise")) &&
        json_is_boolean(json_object_get(
            json_object_get(root, "external_noise"), "used")) &&
        json_is_true(json_object_get(
            json_object_get(root, "external_noise"), "release_allowed")) &&
        json_is_array(json_object_get(
            json_object_get(root, "external_noise"), "assets")) &&
        json_is_array(json_object_get(
            json_object_get(root, "external_noise"), "truth_paths")) &&
        json_is_object(json_object_get(root, "integrity")) &&
        json_is_true(json_object_get(json_object_get(root, "integrity"), "ok"));
    if (root != nullptr) json_decref(root);
    return valid;
}

bool parse_nonnegative_integer(
    const std::string& value,
    long long& parsed
) {
    if (value.empty()) return false;
    unsigned long long number = 0;
    for (std::string::const_iterator it = value.begin();
         it != value.end(); ++it) {
        if (!std::isdigit(static_cast<unsigned char>(*it))) return false;
        const unsigned int digit = static_cast<unsigned int>(*it - '0');
        if (number >
            (static_cast<unsigned long long>(LLONG_MAX) - digit) / 10ull) {
            return false;
        }
        number = number * 10ull + digit;
    }
    parsed = static_cast<long long>(number);
    return true;
}

}  // namespace

namespace syn_sig_ra {

bool store_immutable_package(
    const std::string& data_root,
    const std::string& rendered_directory,
    StoredPackage& package,
    std::string& error
) {
    std::string package_id;
    if (!random_id("pkg_", package_id, error)) {
        return false;
    }
    const std::string package_directory =
        data_root + "/packages/" + package_id;
    const std::string extracted_directory =
        package_directory + "/extracted";
    if (mkdir(package_directory.c_str(), 0750) != 0) {
        error = "unable to allocate immutable package directory";
        return false;
    }
    if (rename(
            rendered_directory.c_str(),
            extracted_directory.c_str()
        ) != 0) {
        error = "unable to move rendered output into package storage";
        return false;
    }

    std::string manifest;
    if (!read_file(extracted_directory + "/manifest.json", manifest, error) ||
        !write_file(package_directory + "/manifest.json", manifest, error)) {
        return false;
    }
    std::string manifest_hash;
    if (!sha256_hex(manifest, manifest_hash, error)) {
        return false;
    }
    manifest_hash = "sha256:" + manifest_hash;
    const SignalViewerStatus viewer_status = prepare_signal_viewer_source(
        extracted_directory,
        package_directory + "/viewer",
        error
    );
    if (viewer_status != SignalViewerStatus::ok &&
        viewer_status != SignalViewerStatus::not_found) {
        return false;
    }
    if (viewer_status == SignalViewerStatus::not_found) error.clear();
    if (!run_zip(
            extracted_directory,
            package_directory + "/package.zip",
            error
        ) ||
        !remove_tree(extracted_directory, error) ||
        !make_read_only(package_directory, error)) {
        return false;
    }

    StoredPackage stored;
    stored.package_id = package_id;
    stored.package_directory = package_directory;
    stored.manifest_hash = manifest_hash;
    stored.size_bytes = 0;
    if (!tree_size(package_directory, stored.size_bytes, error)) {
        return false;
    }
    package = stored;
    return true;
}

bool discard_stored_package(
    const std::string& data_root,
    const std::string& package_id,
    std::string& error
) {
    if (data_root.empty() || package_id.compare(0, 4, "pkg_") != 0 ||
        !safe_component(package_id)) {
        error = "invalid stored package identity";
        return false;
    }
    return remove_tree(data_root + "/packages/" + package_id, error) &&
        remove_tree(data_root + "/derived-artifacts/" + package_id, error);
}

bool inspect_challenge_package(
    const std::string& helper_path,
    const std::string& verifier_wheel,
    const std::string& challenge_path,
    std::string& metadata_json,
    std::string& error
) {
    metadata_json.clear();
    std::vector<std::string> arguments;
    arguments.push_back("inspect");
    arguments.push_back(challenge_path);
    if (!run_challenge_helper(
            helper_path, verifier_wheel, arguments, metadata_json, error))
        return false;
    if (!valid_challenge_metadata(metadata_json)) {
        error = "challenge helper returned unsupported metadata";
        metadata_json.clear();
        return false;
    }
    return true;
}

DerivedArtifactStatus prepare_verification_kit(
    const std::string& data_root,
    const std::string& package_id,
    const std::string& source_zip,
    const std::string& helper_path,
    const std::string& verifier_wheel,
    PreparedArtifact& artifact,
    std::string& metadata_json,
    std::string& error
) {
    artifact = PreparedArtifact();
    metadata_json.clear();
    long long source_size = 0;
    if (!regular_file(source_zip, source_size)) {
        error = "challenge package ZIP is unavailable";
        return DerivedArtifactStatus::invalid_source;
    }
    std::string directory;
    if (!prepare_cache_directory(data_root, package_id, directory, error))
        return DerivedArtifactStatus::io_error;
    const int lock = lock_cache(directory, error);
    if (lock < 0) return DerivedArtifactStatus::io_error;
    remove_stale_workspaces(directory);
    const std::string final_path = directory + "/verification-kit-v2.zip";
    const std::string checksum_path = final_path + ".sha256";
    const std::string metadata_path = final_path + ".json";
    long long final_size = 0;
    std::string checksum;
    bool cached = regular_file(final_path, final_size) &&
        read_checksum(checksum_path, checksum) &&
        read_file(metadata_path, metadata_json, error) &&
        valid_challenge_metadata(metadata_json);
    if (cached) {
        unlock_cache(lock);
        artifact.path = final_path;
        artifact.sha256 = checksum;
        artifact.size_bytes = final_size;
        return DerivedArtifactStatus::ok;
    }
    error.clear();
    unlink(final_path.c_str());
    unlink(checksum_path.c_str());
    unlink(metadata_path.c_str());
    const unsigned long long required =
        static_cast<unsigned long long>(source_size) * 3ull +
        128ull * 1024ull * 1024ull;
    if (!disk_has_capacity(data_root, required)) {
        unlock_cache(lock);
        error = "insufficient disk reserve for verification kit";
        return DerivedArtifactStatus::storage_pressure;
    }
    const std::string workspace_pattern = directory + "/.build-XXXXXX";
    std::vector<char> workspace_template(
        workspace_pattern.begin(), workspace_pattern.end());
    workspace_template.push_back('\0');
    char* workspace_value = mkdtemp(workspace_template.data());
    if (workspace_value == nullptr) {
        unlock_cache(lock);
        error = "unable to allocate verification kit workspace";
        return DerivedArtifactStatus::io_error;
    }
    const std::string workspace(workspace_value);
    const std::string temporary = workspace + "/verification-kit.zip";
    std::vector<std::string> arguments;
    arguments.push_back("build-kit");
    arguments.push_back(source_zip);
    arguments.push_back(temporary);
    bool succeeded = run_challenge_helper(
        helper_path, verifier_wheel, arguments, metadata_json, error);
    if (succeeded) {
        json_error_t parse_error;
        json_t* metadata = json_loadb(
            metadata_json.data(), metadata_json.size(),
            JSON_REJECT_DUPLICATES, &parse_error);
        const bool kit_contract = json_is_object(metadata) &&
            json_is_string(json_object_get(metadata, "kit_contract")) &&
            std::string(json_string_value(json_object_get(metadata, "kit_contract"))) ==
                "synsigra_verification_kit_v3";
        if (metadata != nullptr) json_decref(metadata);
        succeeded = kit_contract && valid_challenge_metadata(metadata_json) &&
            regular_file(temporary, final_size);
        if (!succeeded) error = "verification kit helper returned invalid output";
    }
    if (succeeded && chmod(temporary.c_str(), 0440) != 0) {
        succeeded = false;
        error = "unable to protect verification kit";
    }
    if (succeeded && rename(temporary.c_str(), final_path.c_str()) != 0) {
        succeeded = false;
        error = "unable to publish verification kit";
    }
    if (succeeded) {
        succeeded = sha256_file_hex(final_path, checksum, error) &&
            write_checksum_atomic(checksum_path, checksum, error) &&
            write_file(metadata_path, metadata_json + "\n", error) &&
            chmod(metadata_path.c_str(), 0440) == 0;
        if (!succeeded && error.empty()) error = "unable to finalize verification kit";
    }
    std::string cleanup_error;
    if (!remove_tree(workspace, cleanup_error) && succeeded) {
        succeeded = false;
        error = cleanup_error;
    }
    unlock_cache(lock);
    if (!succeeded) return DerivedArtifactStatus::io_error;
    artifact.path = final_path;
    artifact.sha256 = checksum;
    artifact.size_bytes = final_size;
    artifact.created = true;
    return DerivedArtifactStatus::ok;
}

bool purge_account_storage(
    const std::string& data_root,
    const std::vector<std::string>& package_ids,
    const std::vector<std::string>& job_ids,
    const std::vector<std::string>& custom_pack_ids,
    std::string& error
) {
    if (data_root.empty()) {
        error = "account storage root is unavailable";
        return false;
    }
    const auto purge = [&data_root, &error](
        const std::string& collection,
        const std::string& expected_prefix,
        const std::vector<std::string>& ids) -> bool {
        for (std::vector<std::string>::const_iterator id = ids.begin();
             id != ids.end(); ++id) {
            if (id->compare(0, expected_prefix.size(), expected_prefix) != 0 ||
                !safe_component(*id) ||
                !remove_tree(data_root + "/" + collection + "/" + *id, error)) {
                if (error.empty()) error = "account storage identity is invalid";
                return false;
            }
        }
        return true;
    };
    return purge("packages", "pkg_", package_ids) &&
        purge("derived-artifacts", "pkg_", package_ids) &&
        purge("recipes", "job_", job_ids) &&
        purge("work", "job_", job_ids) &&
        purge("custom_packs", "custom_pack_", custom_pack_ids);
}

DerivedArtifactStatus prepare_cached_file_metadata(
    const std::string& data_root,
    const std::string& package_id,
    const std::string& source_path,
    const std::string& cache_key,
    PreparedArtifact& artifact,
    std::string& error
) {
    artifact = PreparedArtifact();
    if (!safe_component(cache_key)) {
        error = "invalid artifact metadata cache key";
        return DerivedArtifactStatus::invalid_source;
    }
    long long source_size = 0;
    if (!regular_file(source_path, source_size)) {
        error = "source artifact is not a regular file";
        return DerivedArtifactStatus::invalid_source;
    }
    std::string directory;
    if (!prepare_cache_directory(data_root, package_id, directory, error)) {
        return DerivedArtifactStatus::io_error;
    }
    const int lock = lock_cache(directory, error);
    if (lock < 0) return DerivedArtifactStatus::io_error;
    const std::string checksum_path =
        directory + "/" + cache_key + ".sha256";
    std::string checksum;
    bool succeeded = read_checksum(checksum_path, checksum);
    if (!succeeded) {
        error.clear();
        succeeded = sha256_file_hex(source_path, checksum, error) &&
            write_checksum_atomic(checksum_path, checksum, error);
    }
    unlock_cache(lock);
    if (!succeeded) return DerivedArtifactStatus::io_error;
    artifact.path = source_path;
    artifact.sha256 = checksum;
    artifact.size_bytes = source_size;
    return DerivedArtifactStatus::ok;
}

ByteRangeStatus parse_byte_range(
    const std::string& header,
    long long file_size,
    ByteRange& range
) {
    range = ByteRange();
    if (header.empty()) return ByteRangeStatus::none;
    if (file_size <= 0 || header.compare(0, 6, "bytes=") != 0 ||
        header.find(',') != std::string::npos) {
        return ByteRangeStatus::invalid;
    }
    const std::string value = header.substr(6);
    const std::string::size_type dash = value.find('-');
    if (dash == std::string::npos || value.find('-', dash + 1) !=
        std::string::npos) return ByteRangeStatus::invalid;
    const std::string first = value.substr(0, dash);
    const std::string last = value.substr(dash + 1);
    if (first.empty()) {
        long long suffix = 0;
        if (!parse_nonnegative_integer(last, suffix) || suffix <= 0) {
            return ByteRangeStatus::invalid;
        }
        range.length = std::min(suffix, file_size);
        range.offset = file_size - range.length;
        return ByteRangeStatus::valid;
    }
    long long start = 0;
    if (!parse_nonnegative_integer(first, start) || start >= file_size) {
        return ByteRangeStatus::invalid;
    }
    long long end = file_size - 1;
    if (!last.empty() &&
        (!parse_nonnegative_integer(last, end) || end < start)) {
        return ByteRangeStatus::invalid;
    }
    if (end >= file_size) end = file_size - 1;
    range.offset = start;
    range.length = end - start + 1;
    return ByteRangeStatus::valid;
}

}  // namespace syn_sig_ra
