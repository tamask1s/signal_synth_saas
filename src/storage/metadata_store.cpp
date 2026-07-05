#include "syn_sig_ra/metadata_store.h"

#include <sqlite3.h>

#include <cctype>
#include <string>

namespace {

const int kSchemaVersion = 1;

const char kSchemaSql[] = R"SQL(
CREATE TABLE IF NOT EXISTS organizations (
    id TEXT PRIMARY KEY,
    display_name TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    )
);

CREATE TABLE IF NOT EXISTS users (
    id TEXT PRIMARY KEY,
    organization_id TEXT NOT NULL,
    display_name TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    FOREIGN KEY (organization_id) REFERENCES organizations(id)
);

CREATE TABLE IF NOT EXISTS api_keys (
    id TEXT PRIMARY KEY,
    organization_id TEXT NOT NULL,
    user_id TEXT NOT NULL,
    key_hash TEXT NOT NULL UNIQUE
        CHECK (length(key_hash) = 64),
    label TEXT NOT NULL,
    active INTEGER NOT NULL DEFAULT 1
        CHECK (active IN (0, 1)),
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    last_used_at TEXT,
    FOREIGN KEY (organization_id) REFERENCES organizations(id),
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS jobs (
    id TEXT PRIMARY KEY,
    organization_id TEXT NOT NULL,
    user_id TEXT NOT NULL,
    status TEXT NOT NULL
        CHECK (status IN ('queued', 'running', 'succeeded', 'failed')),
    request_json TEXT NOT NULL,
    selected_pack_id TEXT,
    source_pack_path TEXT,
    pack_fingerprint TEXT,
    package_fingerprint TEXT,
    generator_version TEXT,
    generator_build_identity TEXT,
    normalized_cli_command TEXT,
    manifest_hash TEXT,
    artifact_storage_key TEXT,
    error_code TEXT,
    error_message TEXT,
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    started_at TEXT,
    completed_at TEXT,
    FOREIGN KEY (organization_id) REFERENCES organizations(id),
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS packages (
    id TEXT PRIMARY KEY,
    job_id TEXT NOT NULL UNIQUE,
    organization_id TEXT NOT NULL,
    user_id TEXT NOT NULL,
    pack_fingerprint TEXT,
    package_fingerprint TEXT NOT NULL,
    generator_version TEXT,
    generator_build_identity TEXT,
    manifest_hash TEXT NOT NULL,
    artifact_storage_key TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    FOREIGN KEY (job_id) REFERENCES jobs(id),
    FOREIGN KEY (organization_id) REFERENCES organizations(id),
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS audit_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    organization_id TEXT,
    user_id TEXT,
    api_key_id TEXT,
    event_type TEXT NOT NULL,
    subject_type TEXT,
    subject_id TEXT,
    details_json TEXT NOT NULL DEFAULT '{}',
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    FOREIGN KEY (organization_id) REFERENCES organizations(id),
    FOREIGN KEY (user_id) REFERENCES users(id),
    FOREIGN KEY (api_key_id) REFERENCES api_keys(id)
);

CREATE INDEX IF NOT EXISTS jobs_owner_created_idx
    ON jobs (organization_id, user_id, created_at);
CREATE INDEX IF NOT EXISTS packages_owner_created_idx
    ON packages (organization_id, user_id, created_at);
CREATE INDEX IF NOT EXISTS audit_owner_created_idx
    ON audit_events (organization_id, user_id, created_at);
)SQL";

bool is_sha256_hex(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    for (std::string::const_iterator it = value.begin(); it != value.end();
         ++it) {
        if (!std::isdigit(static_cast<unsigned char>(*it)) &&
            (*it < 'a' || *it > 'f')) {
            return false;
        }
    }
    return true;
}

bool bind_text(
    sqlite3_stmt* statement,
    int index,
    const std::string& value
) {
    return sqlite3_bind_text(
        statement,
        index,
        value.c_str(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT
    ) == SQLITE_OK;
}

}  // namespace

namespace syn_sig_ra {

MetadataStore::MetadataStore(const std::string& database_path)
    : database_path_(database_path),
      database_(nullptr),
      initialized_(false) {}

MetadataStore::~MetadataStore() {
    if (database_ != nullptr) {
        sqlite3_close(database_);
    }
}

bool MetadataStore::open(std::string& error) {
    if (database_ != nullptr) {
        return true;
    }

    const int result = sqlite3_open_v2(
        database_path_.c_str(),
        &database_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );
    if (result != SQLITE_OK) {
        error = database_ == nullptr
            ? "unable to open SQLite metadata database"
            : sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_busy_timeout(database_, 5000);
    return true;
}

bool MetadataStore::execute(const char* sql, std::string& error) {
    char* message = nullptr;
    const int result = sqlite3_exec(database_, sql, nullptr, nullptr, &message);
    if (result == SQLITE_OK) {
        return true;
    }
    error = message == nullptr ? sqlite3_errmsg(database_) : message;
    sqlite3_free(message);
    return false;
}

bool MetadataStore::initialize(std::string& error) {
    if (initialized_) {
        return true;
    }
    if (!open(error)) {
        return false;
    }
    if (!execute("PRAGMA foreign_keys = ON;", error)) {
        return false;
    }

    sqlite3_stmt* version_statement = nullptr;
    if (sqlite3_prepare_v2(
            database_,
            "PRAGMA user_version;",
            -1,
            &version_statement,
            nullptr
        ) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    const int step_result = sqlite3_step(version_statement);
    const int schema_version = step_result == SQLITE_ROW
        ? sqlite3_column_int(version_statement, 0)
        : -1;
    sqlite3_finalize(version_statement);

    if (schema_version < 0 || schema_version > kSchemaVersion) {
        error = "unsupported SQLite metadata schema version";
        return false;
    }

    if (!execute("BEGIN IMMEDIATE;", error)) {
        return false;
    }
    if (!execute(kSchemaSql, error) ||
        !execute("PRAGMA user_version = 1;", error) ||
        !execute("COMMIT;", error)) {
        std::string rollback_error;
        execute("ROLLBACK;", rollback_error);
        return false;
    }
    initialized_ = true;
    return true;
}

bool MetadataStore::create_api_key(
    const ApiKeyIdentity& identity,
    const std::string& key_hash,
    const std::string& label,
    std::string& error
) {
    if (identity.api_key_id.empty() ||
        identity.organization_id.empty() ||
        identity.user_id.empty() ||
        label.empty() ||
        !is_sha256_hex(key_hash)) {
        error = "API key identity, label, or SHA-256 hash is invalid";
        return false;
    }
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return false;
    }

    const char* statements[] = {
        "INSERT OR IGNORE INTO organizations "
        "(id, display_name) VALUES (?1, ?1);",
        "INSERT OR IGNORE INTO users "
        "(id, organization_id, display_name) VALUES (?1, ?2, ?1);",
        "INSERT INTO api_keys "
        "(id, organization_id, user_id, key_hash, label) "
        "VALUES (?1, ?2, ?3, ?4, ?5);"
    };

    bool succeeded = true;
    for (int index = 0; index < 3 && succeeded; ++index) {
        sqlite3_stmt* statement = nullptr;
        succeeded = sqlite3_prepare_v2(
            database_,
            statements[index],
            -1,
            &statement,
            nullptr
        ) == SQLITE_OK;
        if (succeeded && index == 0) {
            succeeded = bind_text(
                statement,
                1,
                identity.organization_id
            );
        } else if (succeeded && index == 1) {
            succeeded =
                bind_text(statement, 1, identity.user_id) &&
                bind_text(statement, 2, identity.organization_id);
        } else if (succeeded) {
            succeeded =
                bind_text(statement, 1, identity.api_key_id) &&
                bind_text(statement, 2, identity.organization_id) &&
                bind_text(statement, 3, identity.user_id) &&
                bind_text(statement, 4, key_hash) &&
                bind_text(statement, 5, label);
        }
        if (succeeded) {
            succeeded = sqlite3_step(statement) == SQLITE_DONE;
        }
        sqlite3_finalize(statement);
    }

    if (!succeeded) {
        error = sqlite3_errmsg(database_);
        std::string rollback_error;
        execute("ROLLBACK;", rollback_error);
        return false;
    }

    sqlite3_stmt* audit = nullptr;
    const char* audit_sql =
        "INSERT INTO audit_events "
        "(organization_id, user_id, api_key_id, event_type, "
        "subject_type, subject_id) "
        "VALUES (?1, ?2, ?3, 'api_key.created', 'api_key', ?3);";
    succeeded = sqlite3_prepare_v2(
        database_,
        audit_sql,
        -1,
        &audit,
        nullptr
    ) == SQLITE_OK &&
        bind_text(audit, 1, identity.organization_id) &&
        bind_text(audit, 2, identity.user_id) &&
        bind_text(audit, 3, identity.api_key_id) &&
        sqlite3_step(audit) == SQLITE_DONE;
    sqlite3_finalize(audit);

    if (!succeeded || !execute("COMMIT;", error)) {
        if (error.empty()) {
            error = sqlite3_errmsg(database_);
        }
        std::string rollback_error;
        execute("ROLLBACK;", rollback_error);
        return false;
    }
    return true;
}

ApiKeyLookupStatus MetadataStore::find_active_api_key(
    const std::string& key_hash,
    ApiKeyIdentity& identity,
    std::string& error
) {
    if (!initialize(error)) {
        return ApiKeyLookupStatus::storage_error;
    }

    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT id, organization_id, user_id "
        "FROM api_keys WHERE key_hash = ?1 AND active = 1;";
    if (sqlite3_prepare_v2(
            database_,
            sql,
            -1,
            &statement,
            nullptr
        ) != SQLITE_OK ||
        !bind_text(statement, 1, key_hash)) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return ApiKeyLookupStatus::storage_error;
    }

    const int result = sqlite3_step(statement);
    if (result == SQLITE_DONE) {
        sqlite3_finalize(statement);
        return ApiKeyLookupStatus::not_found;
    }
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return ApiKeyLookupStatus::storage_error;
    }

    identity.api_key_id = reinterpret_cast<const char*>(
        sqlite3_column_text(statement, 0)
    );
    identity.organization_id = reinterpret_cast<const char*>(
        sqlite3_column_text(statement, 1)
    );
    identity.user_id = reinterpret_cast<const char*>(
        sqlite3_column_text(statement, 2)
    );
    sqlite3_finalize(statement);
    return ApiKeyLookupStatus::found;
}

bool MetadataStore::record_api_key_use(
    const ApiKeyIdentity& identity,
    std::string& error
) {
    if (!execute("BEGIN IMMEDIATE;", error)) {
        return false;
    }

    sqlite3_stmt* update = nullptr;
    const char* update_sql =
        "UPDATE api_keys SET last_used_at = "
        "strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = ?1;";
    bool succeeded = sqlite3_prepare_v2(
        database_,
        update_sql,
        -1,
        &update,
        nullptr
    ) == SQLITE_OK &&
        bind_text(update, 1, identity.api_key_id) &&
        sqlite3_step(update) == SQLITE_DONE;
    sqlite3_finalize(update);

    sqlite3_stmt* audit = nullptr;
    const char* audit_sql =
        "INSERT INTO audit_events "
        "(organization_id, user_id, api_key_id, event_type, "
        "subject_type, subject_id) "
        "VALUES (?1, ?2, ?3, 'api_key.authenticated', 'api_key', ?3);";
    succeeded = succeeded &&
        sqlite3_prepare_v2(
            database_,
            audit_sql,
            -1,
            &audit,
            nullptr
        ) == SQLITE_OK &&
        bind_text(audit, 1, identity.organization_id) &&
        bind_text(audit, 2, identity.user_id) &&
        bind_text(audit, 3, identity.api_key_id) &&
        sqlite3_step(audit) == SQLITE_DONE;
    sqlite3_finalize(audit);

    if (!succeeded || !execute("COMMIT;", error)) {
        if (error.empty()) {
            error = sqlite3_errmsg(database_);
        }
        std::string rollback_error;
        execute("ROLLBACK;", rollback_error);
        return false;
    }
    return true;
}

}  // namespace syn_sig_ra
