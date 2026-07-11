#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/sha256.h"

#include <cstdlib>
#include <cerrno>
#include <dirent.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace {

void print_usage(const char* executable) {
    std::cerr
        << "Usage:\n"
        << "  " << executable << " init-db <database-path>\n"
        << "  " << executable
        << " list-api-keys <database-path> [organization-id]\n"
        << "  " << executable
        << " create-api-key <database-path> <organization-id>"
        << " <user-id> <key-id> <label> [owner|admin|developer|viewer]\n"
        << "  " << executable
        << " revoke-api-key <database-path> <key-id>\n"
        << "  " << executable
        << " backup-db <database-path> <destination-path>\n"
        << "  " << executable
        << " cleanup-retention <database-path> <data-root> <days> [--apply]\n"
        << "  " << executable
        << " compact-artifacts <data-root> [--apply]\n"
        << "\ncreate-api-key reads the plaintext API key as one line from stdin.\n";
}

bool remove_tree(const std::string& path, std::string& error) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0) {
        if (errno == ENOENT) return true;
        error = "unable to inspect retention path";
        return false;
    }
    if (S_ISLNK(information.st_mode)) {
        error = "retention path must not contain symlinks";
        return false;
    }
    if (S_ISDIR(information.st_mode)) {
        chmod(path.c_str(), 0700);
        DIR* directory = opendir(path.c_str());
        if (directory == nullptr) {
            error = "unable to open retention directory";
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
        return succeeded && rmdir(path.c_str()) == 0;
    }
    chmod(path.c_str(), 0600);
    if (unlink(path.c_str()) != 0) {
        error = "unable to remove retained artifact";
        return false;
    }
    return true;
}

bool regular_file_without_symlink(const std::string& path) {
    struct stat information;
    return lstat(path.c_str(), &information) == 0 &&
        S_ISREG(information.st_mode);
}

bool compact_artifacts(
    const std::string& data_root,
    bool apply,
    int& candidates,
    int& compacted,
    std::string& error
) {
    candidates = 0;
    compacted = 0;
    const std::string packages_root = data_root + "/packages";
    DIR* packages = opendir(packages_root.c_str());
    if (packages == nullptr) {
        error = "unable to open package store";
        return false;
    }
    bool succeeded = true;
    for (dirent* entry = readdir(packages);
         entry != nullptr; entry = readdir(packages)) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        const std::string package_root = packages_root + "/" + name;
        const std::string extracted = package_root + "/extracted";
        struct stat information;
        if (lstat(extracted.c_str(), &information) != 0) {
            if (errno == ENOENT) continue;
            error = "unable to inspect extracted package tree";
            succeeded = false;
            break;
        }
        if (!S_ISDIR(information.st_mode) ||
            !regular_file_without_symlink(package_root + "/manifest.json") ||
            !regular_file_without_symlink(package_root + "/package.zip")) {
            error = "package is not safe to compact";
            succeeded = false;
            break;
        }
        ++candidates;
        std::cout << "candidate=" << name << '\n';
        if (apply) {
            if (!remove_tree(extracted, error)) {
                succeeded = false;
                break;
            }
            ++compacted;
        }
    }
    closedir(packages);
    return succeeded;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "init-db") {
        syn_sig_ra::MetadataStore store(argv[2]);
        std::string error;
        if (!store.initialize(error)) {
            std::cerr << "error=metadata-init-failed message=" << error << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "status=metadata-initialized\n";
        std::cout << "database=" << argv[2] << '\n';
        return EXIT_SUCCESS;
    }

    if ((argc == 3 || argc == 4) &&
        std::string(argv[1]) == "list-api-keys") {
        syn_sig_ra::MetadataStore store(argv[2]);
        std::string error;
        std::vector<syn_sig_ra::ApiKeyRecord> api_keys;
        const std::string organization_id = argc == 4 ? argv[3] : "";
        if (!store.list_api_keys(organization_id, api_keys, error)) {
            std::cerr << "error=api-key-list-failed message=" << error << '\n';
            return EXIT_FAILURE;
        }
        std::cout
            << "api_key_id\torganization_id\tuser_id\trole\tactive\tcreated_at"
            << "\tlast_used_at\tlabel\n";
        for (std::vector<syn_sig_ra::ApiKeyRecord>::const_iterator it =
                 api_keys.begin();
             it != api_keys.end();
             ++it) {
            std::cout
                << it->api_key_id << '\t'
                << it->organization_id << '\t'
                << it->user_id << '\t'
                << it->role << '\t'
                << (it->active ? "1" : "0") << '\t'
                << it->created_at << '\t'
                << it->last_used_at << '\t'
                << it->label << '\n';
        }
        return EXIT_SUCCESS;
    }

    if ((argc == 7 || argc == 8) &&
        std::string(argv[1]) == "create-api-key") {
        std::string plaintext_key;
        if (!std::getline(std::cin, plaintext_key) || plaintext_key.empty()) {
            std::cerr << "error=missing-api-key\n";
            return EXIT_FAILURE;
        }

        std::string key_hash;
        std::string error;
        if (!syn_sig_ra::sha256_hex(plaintext_key, key_hash, error)) {
            std::cerr << "error=api-key-hash-failed message=" << error << '\n';
            return EXIT_FAILURE;
        }

        syn_sig_ra::ApiKeyIdentity identity;
        identity.organization_id = argv[3];
        identity.user_id = argv[4];
        identity.api_key_id = argv[5];
        identity.role = argc == 8 ? argv[7] : "owner";
        syn_sig_ra::MetadataStore store(argv[2]);
        if (!store.create_api_key(identity, key_hash, argv[6], error)) {
            std::cerr << "error=api-key-create-failed message=" << error << '\n';
            return EXIT_FAILURE;
        }

        std::cout << "status=api-key-created\n";
        std::cout << "api_key_id=" << identity.api_key_id << '\n';
        std::cout << "organization_id=" << identity.organization_id << '\n';
        std::cout << "user_id=" << identity.user_id << '\n';
        std::cout << "role=" << identity.role << '\n';
        return EXIT_SUCCESS;
    }

    if (argc == 4 && std::string(argv[1]) == "revoke-api-key") {
        syn_sig_ra::MetadataStore store(argv[2]);
        std::string error;
        if (!store.revoke_api_key(argv[3], error)) {
            std::cerr << "error=api-key-revoke-failed message=" << error << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "status=api-key-revoked\n";
        std::cout << "api_key_id=" << argv[3] << '\n';
        return EXIT_SUCCESS;
    }

    if (argc == 4 && std::string(argv[1]) == "backup-db") {
        syn_sig_ra::MetadataStore store(argv[2]);
        std::string error;
        if (!store.backup_database(argv[3], error)) {
            std::cerr << "error=database-backup-failed message=" << error << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "status=database-backed-up\npath=" << argv[3] << '\n';
        return EXIT_SUCCESS;
    }

    if ((argc == 5 || argc == 6) &&
        std::string(argv[1]) == "cleanup-retention") {
        const int days = std::atoi(argv[4]);
        const bool apply = argc == 6 && std::string(argv[5]) == "--apply";
        if (argc == 6 && !apply) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        syn_sig_ra::MetadataStore store(argv[2]);
        std::vector<syn_sig_ra::RetentionCandidate> candidates;
        std::string error;
        if (!store.list_retention_candidates(days, candidates, error)) {
            std::cerr << "error=retention-list-failed message=" << error << '\n';
            return EXIT_FAILURE;
        }
        int removed = 0;
        for (std::vector<syn_sig_ra::RetentionCandidate>::const_iterator it =
                 candidates.begin(); it != candidates.end(); ++it) {
            const std::string expected =
                std::string(argv[3]) + "/packages/" + it->package_id;
            std::cout << "candidate=" << it->package_id
                      << " job_id=" << it->job_id << '\n';
            if (!apply) continue;
            if (it->artifact_storage_key != expected ||
                !store.mark_package_expired(it->package_id, error) ||
                !remove_tree(expected, error)) {
                std::cerr << "error=retention-cleanup-failed package_id="
                          << it->package_id << " message=" << error << '\n';
                return EXIT_FAILURE;
            }
            ++removed;
        }
        std::cout << "status=" << (apply ? "retention-applied" : "retention-dry-run")
                  << "\ncandidates=" << candidates.size()
                  << "\nremoved=" << removed << '\n';
        return EXIT_SUCCESS;
    }

    if ((argc == 3 || argc == 4) &&
        std::string(argv[1]) == "compact-artifacts") {
        const bool apply = argc == 4 && std::string(argv[3]) == "--apply";
        if (argc == 4 && !apply) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        int candidates = 0;
        int compacted = 0;
        std::string error;
        if (!compact_artifacts(
                argv[2], apply, candidates, compacted, error)) {
            std::cerr << "error=artifact-compaction-failed message="
                      << error << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "status=artifact-compaction-"
                  << (apply ? "applied" : "dry-run")
                  << "\ncandidates=" << candidates
                  << "\ncompacted=" << compacted << '\n';
        return EXIT_SUCCESS;
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
