#include "syn_sig_ra/api_key_auth.h"
#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/sha256.h"

#include <sqlite3.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

int scalar_int(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    require(
        sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) ==
            SQLITE_OK,
        "test query should prepare"
    );
    require(sqlite3_step(statement) == SQLITE_ROW, "test query should return");
    const int value = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return value;
}

}  // namespace

int main() {
    std::ostringstream path_builder;
    path_builder << "/tmp/syn_sig_ra_auth_test_" << getpid() << ".sqlite3";
    const std::string database_path = path_builder.str();
    std::remove(database_path.c_str());

    const std::string plaintext_key = "test-key-with-enough-randomness";
    std::string key_hash;
    std::string error;
    require(
        syn_sig_ra::sha256_hex(plaintext_key, key_hash, error),
        "API key hashing should succeed: " + error
    );
    require(
        key_hash.size() == 64 && key_hash != plaintext_key,
        "API key hash should be a non-plaintext SHA-256 value"
    );

    syn_sig_ra::ApiKeyIdentity identity;
    identity.api_key_id = "key_test";
    identity.organization_id = "org_test";
    identity.user_id = "user_test";

    {
        syn_sig_ra::MetadataStore store(database_path);
        require(store.initialize(error), "schema init should succeed: " + error);
        error.clear();
        require(
            store.initialize(error),
            "schema init should be deterministic: " + error
        );
        error.clear();
        require(
            store.create_api_key(identity, key_hash, "unit test", error),
            "API key creation should succeed: " + error
        );

        const syn_sig_ra::AuthenticationResult missing =
            syn_sig_ra::authenticate_bearer("", store);
        require(
            missing.status ==
                syn_sig_ra::AuthenticationStatus::missing_credentials,
            "missing credentials should be distinguished"
        );

        const syn_sig_ra::AuthenticationResult malformed =
            syn_sig_ra::authenticate_bearer("Basic value", store);
        require(
            malformed.status ==
                syn_sig_ra::AuthenticationStatus::malformed_credentials,
            "non-Bearer credentials should be rejected"
        );

        const syn_sig_ra::AuthenticationResult invalid =
            syn_sig_ra::authenticate_bearer("Bearer wrong-key", store);
        require(
            invalid.status ==
                syn_sig_ra::AuthenticationStatus::invalid_credentials,
            "unknown API keys should be rejected"
        );

        const syn_sig_ra::AuthenticationResult authenticated =
            syn_sig_ra::authenticate_bearer(
                "bearer " + plaintext_key,
                store
            );
        require(
            authenticated.status ==
                syn_sig_ra::AuthenticationStatus::authenticated,
            "a valid API key should authenticate"
        );
        require(
            authenticated.identity.organization_id == "org_test" &&
                authenticated.identity.user_id == "user_test",
            "authentication should return the owner scope"
        );
    }

    sqlite3* verification_database = nullptr;
    require(
        sqlite3_open_v2(
            database_path.c_str(),
            &verification_database,
            SQLITE_OPEN_READONLY,
            nullptr
        ) == SQLITE_OK,
        "verification database should open"
    );
    require(
        scalar_int(
            verification_database,
            "SELECT count(*) FROM sqlite_master WHERE type = 'table' "
            "AND name IN ('organizations', 'users', 'api_keys', 'jobs', "
            "'packages', 'audit_events');"
        ) == 6,
        "all metadata tables should exist"
    );
    require(
        scalar_int(
            verification_database,
            "SELECT count(*) FROM api_keys "
            "WHERE key_hash = "
            "'test-key-with-enough-randomness';"
        ) == 0,
        "plaintext API keys must not be stored"
    );
    require(
        scalar_int(
            verification_database,
            "SELECT count(*) FROM audit_events;"
        ) == 2,
        "key creation and successful authentication should be audited"
    );
    require(
        scalar_int(verification_database, "PRAGMA user_version;") == 1,
        "schema version should be deterministic"
    );
    sqlite3_close(verification_database);
    std::remove(database_path.c_str());
    return EXIT_SUCCESS;
}
