#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/sha256.h"

#include <cstdlib>
#include <iostream>
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
        << "\ncreate-api-key reads the plaintext API key as one line from stdin.\n";
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

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
