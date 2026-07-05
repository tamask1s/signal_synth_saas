#include "syn_sig_ra/artifact_store.h"

#include "syn_sig_ra/random_id.h"
#include "syn_sig_ra/sha256.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <sstream>
#include <string>

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
    if (!run_zip(
            extracted_directory,
            package_directory + "/package.zip",
            error
        ) ||
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

}  // namespace syn_sig_ra
