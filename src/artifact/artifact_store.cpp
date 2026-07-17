#include "syn_sig_ra/artifact_store.h"

#include "syn_sig_ra/random_id.h"
#include "syn_sig_ra/sha256.h"
#include "syn_sig_ra/signal_viewer.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
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

bool safe_relative_path(const std::string& value) {
    if (value.empty() || value.size() > 512 || value[0] == '/' ||
        value.find('\\') != std::string::npos ||
        value.find("//") != std::string::npos) return false;
    std::string::size_type start = 0;
    while (start <= value.size()) {
        const std::string::size_type end = value.find('/', start);
        const std::string part = value.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == "..") return false;
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

bool copy_file(
    const std::string& source,
    const std::string& destination,
    std::string& error
) {
    const int input = open(
        source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0) {
        error = "unable to open source artifact";
        return false;
    }
    const int output = open(
        destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
        0600);
    if (output < 0) {
        close(input);
        error = "unable to allocate derived artifact";
        return false;
    }
    std::vector<char> buffer(1024 * 1024);
    bool succeeded = true;
    for (;;) {
        const ssize_t count = read(input, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            succeeded = false;
            break;
        }
        if (count == 0) break;
        if (!write_all(output, buffer.data(), static_cast<std::size_t>(count))) {
            succeeded = false;
            break;
        }
    }
    if (succeeded && fsync(output) != 0) succeeded = false;
    if (close(input) != 0 || close(output) != 0) succeeded = false;
    if (!succeeded) {
        unlink(destination.c_str());
        error = "unable to stream source artifact into derived cache";
    }
    return succeeded;
}

bool write_overlay_entry(
    const std::string& root,
    const syn_sig_ra::ArtifactOverlayEntry& entry,
    std::string& error
) {
    if (!safe_relative_path(entry.path)) {
        error = "derived ZIP entry path is unsafe";
        return false;
    }
    std::string::size_type separator = entry.path.find('/');
    while (separator != std::string::npos) {
        const std::string directory =
            root + "/" + entry.path.substr(0, separator);
        if (!ensure_directory(directory, 0700, error)) return false;
        separator = entry.path.find('/', separator + 1);
    }
    const std::string path = root + "/" + entry.path;
    const int output = open(
        path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600);
    bool succeeded = output >= 0;
    if (succeeded) {
        succeeded = write_all(
            output, entry.content.data(), entry.content.size()) &&
            fsync(output) == 0;
        if (close(output) != 0) succeeded = false;
    }
    if (!succeeded) {
        if (output >= 0 && fcntl(output, F_GETFD) != -1) close(output);
        unlink(path.c_str());
        error = "unable to write derived ZIP entry";
        return false;
    }
    return true;
}

bool run_zip_overlay(
    const std::string& workspace,
    const std::vector<syn_sig_ra::ArtifactOverlayEntry>& entries,
    std::string& error
) {
    const pid_t child = fork();
    if (child < 0) {
        error = "unable to fork derived ZIP process";
        return false;
    }
    if (child == 0) {
        if (chdir(workspace.c_str()) != 0) _exit(126);
        std::vector<std::string> arguments;
        arguments.push_back("/usr/bin/zip");
        arguments.push_back("-q");
        arguments.push_back("-r");
        arguments.push_back("verification-kit.zip");
        for (std::vector<syn_sig_ra::ArtifactOverlayEntry>::const_iterator it =
                 entries.begin(); it != entries.end(); ++it) {
            arguments.push_back(it->path);
        }
        std::vector<char*> raw;
        for (std::vector<std::string>::iterator it = arguments.begin();
             it != arguments.end(); ++it) {
            raw.push_back(const_cast<char*>(it->c_str()));
        }
        raw.push_back(nullptr);
        execv("/usr/bin/zip", raw.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "derived ZIP creation failed";
        return false;
    }
    return true;
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

DerivedArtifactStatus prepare_cached_zip_overlay(
    const std::string& data_root,
    const std::string& package_id,
    const std::string& source_zip,
    const std::string& cache_key,
    const std::vector<ArtifactOverlayEntry>& entries,
    PreparedArtifact& artifact,
    std::string& error
) {
    artifact = PreparedArtifact();
    long long source_size = 0;
    if (!safe_component(cache_key) || entries.empty() ||
        entries.size() > 10000u || !regular_file(source_zip, source_size)) {
        error = "invalid derived ZIP source or cache key";
        return DerivedArtifactStatus::invalid_source;
    }
    unsigned long long overlay_bytes = 0;
    for (std::vector<ArtifactOverlayEntry>::const_iterator it = entries.begin();
         it != entries.end(); ++it) {
        if (!safe_relative_path(it->path) ||
            it->content.size() > 16u * 1024u * 1024u ||
            overlay_bytes > 64ull * 1024ull * 1024ull - it->content.size()) {
            error = "derived ZIP entries exceed safe limits";
            return DerivedArtifactStatus::invalid_source;
        }
        overlay_bytes += it->content.size();
    }
    std::string directory;
    if (!prepare_cache_directory(data_root, package_id, directory, error)) {
        return DerivedArtifactStatus::io_error;
    }
    const int lock = lock_cache(directory, error);
    if (lock < 0) return DerivedArtifactStatus::io_error;
    remove_stale_workspaces(directory);
    const std::string final_path = directory + "/" + cache_key + ".zip";
    const std::string checksum_path = final_path + ".sha256";
    long long final_size = 0;
    std::string checksum;
    if (regular_file(final_path, final_size)) {
        bool succeeded = read_checksum(checksum_path, checksum);
        if (!succeeded) {
            error.clear();
            succeeded = sha256_file_hex(final_path, checksum, error) &&
                write_checksum_atomic(checksum_path, checksum, error);
        }
        unlock_cache(lock);
        if (!succeeded) return DerivedArtifactStatus::io_error;
        artifact.path = final_path;
        artifact.sha256 = checksum;
        artifact.size_bytes = final_size;
        return DerivedArtifactStatus::ok;
    }
    const unsigned long long required =
        static_cast<unsigned long long>(source_size) + overlay_bytes +
        64ull * 1024ull * 1024ull;
    if (!disk_has_capacity(data_root, required)) {
        unlock_cache(lock);
        error = "insufficient disk reserve for derived verification kit";
        return DerivedArtifactStatus::storage_pressure;
    }
    const std::string workspace_pattern = directory + "/.build-XXXXXX";
    std::vector<char> workspace_template(
        workspace_pattern.begin(), workspace_pattern.end());
    workspace_template.push_back('\0');
    char* workspace_value = mkdtemp(workspace_template.data());
    if (workspace_value == nullptr) {
        unlock_cache(lock);
        error = "unable to allocate derived ZIP workspace";
        return DerivedArtifactStatus::io_error;
    }
    const std::string workspace(workspace_value);
    const std::string temporary_zip = workspace + "/verification-kit.zip";
    bool succeeded = copy_file(source_zip, temporary_zip, error);
    for (std::vector<ArtifactOverlayEntry>::const_iterator it = entries.begin();
         succeeded && it != entries.end(); ++it) {
        succeeded = write_overlay_entry(workspace, *it, error);
    }
    if (succeeded) succeeded = run_zip_overlay(workspace, entries, error);
    if (succeeded) {
        const int descriptor = open(
            temporary_zip.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        succeeded = descriptor >= 0 && fsync(descriptor) == 0;
        if (descriptor >= 0) close(descriptor);
        if (!succeeded) error = "unable to sync derived ZIP";
    }
    if (succeeded && chmod(temporary_zip.c_str(), 0440) != 0) {
        succeeded = false;
        error = "unable to protect derived ZIP";
    }
    if (succeeded && rename(temporary_zip.c_str(), final_path.c_str()) != 0) {
        succeeded = false;
        error = "unable to publish derived ZIP atomically";
    }
    std::string cleanup_error;
    if (!remove_tree(workspace, cleanup_error) && succeeded) {
        succeeded = false;
        error = cleanup_error;
    }
    if (succeeded) {
        succeeded = regular_file(final_path, final_size) &&
            sha256_file_hex(final_path, checksum, error) &&
            write_checksum_atomic(checksum_path, checksum, error);
        if (!succeeded && error.empty()) {
            error = "unable to finalize derived ZIP metadata";
        }
    }
    unlock_cache(lock);
    if (!succeeded) return DerivedArtifactStatus::io_error;
    artifact.path = final_path;
    artifact.sha256 = checksum;
    artifact.size_bytes = final_size;
    artifact.created = true;
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
