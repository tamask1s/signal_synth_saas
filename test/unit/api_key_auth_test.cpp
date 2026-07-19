#include "syn_sig_ra/api_key_auth.h"
#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/password_auth.h"
#include "syn_sig_ra/sha256.h"

#include <sqlite3.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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
    const std::string backup_path = database_path + ".backup";
    std::remove(database_path.c_str());
    std::remove(backup_path.c_str());

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
    identity.role = "owner";

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
            store.bootstrap_owner(
                identity, "bootstrap@example.test", "Bootstrap Owner",
                key_hash, "unit test", error),
            "API key creation should succeed: " + error
        );

        std::string normalized_email;
        require(
            syn_sig_ra::normalize_email(" Account@Example.COM ", normalized_email) &&
                normalized_email == "account@example.com",
            "email should be normalized"
        );
        std::string password_salt;
        std::string password_hash;
        require(
            syn_sig_ra::hash_password(
                "correct horse battery staple",
                password_salt,
                password_hash,
                error
            ),
            "password hashing should succeed: " + error
        );
        syn_sig_ra::AccountRecord account;
        require(
            store.create_account(
                normalized_email,
                "Account User",
                password_salt,
                password_hash,
                "private-beta-2026-07-17-r4",
                account,
                error
            ) == syn_sig_ra::AccountCreateStatus::created,
            "account creation should succeed: " + error
        );
        bool password_matches = false;
        require(
            syn_sig_ra::verify_password(
                "correct horse battery staple",
                password_salt,
                password_hash,
                password_matches,
                error
            ) && password_matches,
            "stored password should verify"
        );
        std::string session_hash;
        require(
            syn_sig_ra::sha256_hex(
                "session-secret", session_hash, error
            ) &&
                store.create_session(
                    account, "session_test", session_hash, error
                ),
            "session creation should succeed: " + error
        );
        const syn_sig_ra::AuthenticationResult session =
            syn_sig_ra::authenticate_session(
                "other=value; syn_sig_ra_session=session-secret", store
            );
        require(
            session.status == syn_sig_ra::AuthenticationStatus::authenticated &&
                session.identity.user_id == account.user_id,
            "session cookie should authenticate"
        );
        require(
            store.delete_session(session_hash, error),
            "session logout should succeed: " + error
        );
        require(
            syn_sig_ra::authenticate_session(
                "syn_sig_ra_session=session-secret", store
            ).status == syn_sig_ra::AuthenticationStatus::invalid_credentials,
            "deleted session should not authenticate"
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
                authenticated.identity.user_id == "user_test" &&
                authenticated.identity.role == "owner",
            "authentication should return the tenant and role scope"
        );
        for (int request = 0; request < 120; ++request) {
            require(
                store.record_api_key_use(authenticated.identity, error),
                "request usage should be recorded"
            );
        }
        syn_sig_ra::UsageSummary usage;
        require(
            store.check_request_quota(
                authenticated.identity, usage, error
            ) == syn_sig_ra::QuotaStatus::rate_limited,
            "per-key request rate quota should reject abusive traffic"
        );

        std::vector<syn_sig_ra::ApiKeyRecord> api_keys;
        require(
            store.list_api_keys("org_test", api_keys, error),
            "API key listing should succeed: " + error
        );
        require(api_keys.size() == 1, "one API key should be listed");
        require(
            api_keys[0].api_key_id == "key_test" &&
                api_keys[0].active &&
                !api_keys[0].created_at.empty() &&
                !api_keys[0].last_used_at.empty(),
            "API key listing should expose safe lifecycle metadata"
        );

        require(
            store.revoke_api_key("key_test", error),
            "API key revocation should succeed: " + error
        );
        api_keys.clear();
        require(
            store.list_api_keys("org_test", api_keys, error),
            "API key listing after revoke should succeed: " + error
        );
        require(
            api_keys.size() == 1 && !api_keys[0].active,
            "revoked API keys should remain visible but inactive"
        );
        const syn_sig_ra::AuthenticationResult revoked =
            syn_sig_ra::authenticate_bearer(
                "bearer " + plaintext_key,
                store
            );
        require(
            revoked.status ==
                syn_sig_ra::AuthenticationStatus::invalid_credentials,
            "revoked API keys should no longer authenticate"
        );
        require(
            store.backup_database(backup_path, error),
            "SQLite online backup should succeed: " + error
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
            "AND name IN ('organizations', 'users', 'organization_memberships', "
            "'projects', 'api_keys', 'jobs', 'packages', 'audit_events', "
            "'quota_decisions', 'worker_heartbeat', 'scenario_drafts', "
            "'custom_packs', 'sessions', 'legal_acceptances');"
        ) == 14,
        "all metadata tables should exist"
    );
    require(
        scalar_int(
            verification_database,
            "SELECT count(*) FROM legal_acceptances "
            "WHERE document_type='private_beta_terms' "
            "AND document_version='private-beta-2026-07-17-r4';"
        ) == 1,
        "account creation should record the accepted terms version"
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
        ) == 124,
        "key lifecycle and request usage should be audited"
    );
    require(
        scalar_int(
            verification_database,
            "SELECT count(*) FROM quota_decisions "
            "WHERE decision = 'request_rate_limited';"
        ) == 1,
        "rate-limit decisions should be persisted"
    );
    require(
        scalar_int(verification_database, "PRAGMA user_version;") == 2,
        "schema version should be deterministic"
    );
    sqlite3_close(verification_database);
    sqlite3* backup_database = nullptr;
    require(
        sqlite3_open_v2(
            backup_path.c_str(), &backup_database, SQLITE_OPEN_READONLY, nullptr
        ) == SQLITE_OK &&
            scalar_int(backup_database, "SELECT count(*) FROM api_keys;") == 2,
        "backup database should open with copied metadata"
    );
    sqlite3_close(backup_database);
    std::remove(database_path.c_str());
    std::remove(backup_path.c_str());

    return EXIT_SUCCESS;
}
