#include "syn_sig_ra/metadata_store.h"

#include "syn_sig_ra/random_id.h"

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

std::string column_text(sqlite3_stmt* statement, int index) {
    const unsigned char* value = sqlite3_column_text(statement, index);
    return value == nullptr
        ? std::string()
        : std::string(reinterpret_cast<const char*>(value));
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

bool MetadataStore::create_job(
    const ApiKeyIdentity& owner,
    const std::string& request_json,
    const std::string& pack_id,
    const std::string& source_pack_path,
    const std::string& pack_fingerprint,
    std::string& job_id,
    std::string& error
) {
    if (!initialize(error) || !random_id("job_", job_id, error)) {
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT INTO jobs "
        "(id, organization_id, user_id, status, request_json, "
        "selected_pack_id, source_pack_path, pack_fingerprint) "
        "VALUES (?1, ?2, ?3, 'queued', ?4, ?5, ?6, ?7);";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) ==
            SQLITE_OK &&
        bind_text(statement, 1, job_id) &&
        bind_text(statement, 2, owner.organization_id) &&
        bind_text(statement, 3, owner.user_id) &&
        bind_text(statement, 4, request_json) &&
        bind_text(statement, 5, pack_id) &&
        bind_text(statement, 6, source_pack_path) &&
        bind_text(statement, 7, pack_fingerprint) &&
        sqlite3_step(statement) == SQLITE_DONE;
    if (!succeeded) {
        error = sqlite3_errmsg(database_);
    }
    sqlite3_finalize(statement);
    return succeeded;
}

RecordLookupStatus MetadataStore::find_job(
    const std::string& job_id,
    const ApiKeyIdentity& owner,
    JobRecord& job,
    std::string& error
) {
    if (!initialize(error)) {
        return RecordLookupStatus::storage_error;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT j.id, j.organization_id, j.user_id, j.status, "
        "j.request_json, j.selected_pack_id, j.source_pack_path, "
        "j.pack_fingerprint, COALESCE(p.id, ''), "
        "j.package_fingerprint, j.generator_version, "
        "j.generator_build_identity, j.manifest_hash, "
        "j.artifact_storage_key, j.error_code, j.error_message, "
        "j.created_at, j.started_at, j.completed_at "
        "FROM jobs j LEFT JOIN packages p ON p.job_id = j.id "
        "WHERE j.id = ?1 AND j.organization_id = ?2 AND j.user_id = ?3;";
    if (sqlite3_prepare_v2(
            database_,
            sql,
            -1,
            &statement,
            nullptr
        ) != SQLITE_OK ||
        !bind_text(statement, 1, job_id) ||
        !bind_text(statement, 2, owner.organization_id) ||
        !bind_text(statement, 3, owner.user_id)) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return RecordLookupStatus::storage_error;
    }
    const int result = sqlite3_step(statement);
    if (result == SQLITE_DONE) {
        sqlite3_finalize(statement);
        return RecordLookupStatus::not_found;
    }
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return RecordLookupStatus::storage_error;
    }
    JobRecord loaded;
    loaded.job_id = column_text(statement, 0);
    loaded.organization_id = column_text(statement, 1);
    loaded.user_id = column_text(statement, 2);
    loaded.status = column_text(statement, 3);
    loaded.request_json = column_text(statement, 4);
    loaded.selected_pack_id = column_text(statement, 5);
    loaded.source_pack_path = column_text(statement, 6);
    loaded.pack_fingerprint = column_text(statement, 7);
    loaded.package_id = column_text(statement, 8);
    loaded.package_fingerprint = column_text(statement, 9);
    loaded.generator_version = column_text(statement, 10);
    loaded.generator_build_identity = column_text(statement, 11);
    loaded.manifest_hash = column_text(statement, 12);
    loaded.artifact_storage_key = column_text(statement, 13);
    loaded.error_code = column_text(statement, 14);
    loaded.error_message = column_text(statement, 15);
    loaded.created_at = column_text(statement, 16);
    loaded.started_at = column_text(statement, 17);
    loaded.completed_at = column_text(statement, 18);
    sqlite3_finalize(statement);
    job = loaded;
    return RecordLookupStatus::found;
}

RecordLookupStatus MetadataStore::claim_next_job(
    JobRecord& job,
    std::string& error
) {
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return RecordLookupStatus::storage_error;
    }
    sqlite3_stmt* select = nullptr;
    const char* select_sql =
        "SELECT id, organization_id, user_id, request_json, "
        "selected_pack_id, source_pack_path, pack_fingerprint, created_at "
        "FROM jobs WHERE status = 'queued' "
        "ORDER BY created_at, id LIMIT 1;";
    if (sqlite3_prepare_v2(
            database_,
            select_sql,
            -1,
            &select,
            nullptr
        ) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return RecordLookupStatus::storage_error;
    }
    const int result = sqlite3_step(select);
    if (result == SQLITE_DONE) {
        sqlite3_finalize(select);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return RecordLookupStatus::not_found;
    }
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(select);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return RecordLookupStatus::storage_error;
    }
    JobRecord claimed;
    claimed.job_id = column_text(select, 0);
    claimed.organization_id = column_text(select, 1);
    claimed.user_id = column_text(select, 2);
    claimed.request_json = column_text(select, 3);
    claimed.selected_pack_id = column_text(select, 4);
    claimed.source_pack_path = column_text(select, 5);
    claimed.pack_fingerprint = column_text(select, 6);
    claimed.created_at = column_text(select, 7);
    sqlite3_finalize(select);

    sqlite3_stmt* update = nullptr;
    const char* update_sql =
        "UPDATE jobs SET status = 'running', started_at = "
        "strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?1 AND status = 'queued';";
    const bool updated =
        sqlite3_prepare_v2(database_, update_sql, -1, &update, nullptr) ==
            SQLITE_OK &&
        bind_text(update, 1, claimed.job_id) &&
        sqlite3_step(update) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    sqlite3_finalize(update);
    if (!updated || !execute("COMMIT;", error)) {
        if (error.empty()) {
            error = sqlite3_errmsg(database_);
        }
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return RecordLookupStatus::storage_error;
    }
    claimed.status = "running";
    job = claimed;
    return RecordLookupStatus::found;
}

bool MetadataStore::complete_job(
    const std::string& job_id,
    const std::string& package_fingerprint,
    const std::string& generator_version,
    const std::string& generator_build_identity,
    const std::string& normalized_cli_command,
    const std::string& artifact_storage_key,
    std::string& error
) {
    if (!initialize(error)) {
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "UPDATE jobs SET status = 'succeeded', package_fingerprint = ?2, "
        "generator_version = ?3, generator_build_identity = ?4, "
        "normalized_cli_command = ?5, artifact_storage_key = ?6, "
        "completed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?1 AND status = 'running';";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) ==
            SQLITE_OK &&
        bind_text(statement, 1, job_id) &&
        bind_text(statement, 2, package_fingerprint) &&
        bind_text(statement, 3, generator_version) &&
        bind_text(statement, 4, generator_build_identity) &&
        bind_text(statement, 5, normalized_cli_command) &&
        bind_text(statement, 6, artifact_storage_key) &&
        sqlite3_step(statement) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    if (!succeeded) {
        error = sqlite3_errmsg(database_);
    }
    sqlite3_finalize(statement);
    return succeeded;
}

bool MetadataStore::complete_job_with_package(
    const JobRecord& job,
    const std::string& package_id,
    const std::string& package_fingerprint,
    const std::string& generator_version,
    const std::string& generator_build_identity,
    const std::string& normalized_cli_command,
    const std::string& manifest_hash,
    const std::string& artifact_storage_key,
    std::string& error
) {
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return false;
    }
    sqlite3_stmt* package_statement = nullptr;
    const char* package_sql =
        "INSERT INTO packages "
        "(id, job_id, organization_id, user_id, pack_fingerprint, "
        "package_fingerprint, generator_version, "
        "generator_build_identity, manifest_hash, artifact_storage_key) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);";
    bool succeeded =
        sqlite3_prepare_v2(
            database_,
            package_sql,
            -1,
            &package_statement,
            nullptr
        ) == SQLITE_OK &&
        bind_text(package_statement, 1, package_id) &&
        bind_text(package_statement, 2, job.job_id) &&
        bind_text(package_statement, 3, job.organization_id) &&
        bind_text(package_statement, 4, job.user_id) &&
        bind_text(package_statement, 5, job.pack_fingerprint) &&
        bind_text(package_statement, 6, package_fingerprint) &&
        bind_text(package_statement, 7, generator_version) &&
        bind_text(package_statement, 8, generator_build_identity) &&
        bind_text(package_statement, 9, manifest_hash) &&
        bind_text(package_statement, 10, artifact_storage_key) &&
        sqlite3_step(package_statement) == SQLITE_DONE;
    sqlite3_finalize(package_statement);

    sqlite3_stmt* job_statement = nullptr;
    const char* job_sql =
        "UPDATE jobs SET status = 'succeeded', package_fingerprint = ?2, "
        "generator_version = ?3, generator_build_identity = ?4, "
        "normalized_cli_command = ?5, manifest_hash = ?6, "
        "artifact_storage_key = ?7, "
        "completed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?1 AND status = 'running';";
    succeeded = succeeded &&
        sqlite3_prepare_v2(
            database_,
            job_sql,
            -1,
            &job_statement,
            nullptr
        ) == SQLITE_OK &&
        bind_text(job_statement, 1, job.job_id) &&
        bind_text(job_statement, 2, package_fingerprint) &&
        bind_text(job_statement, 3, generator_version) &&
        bind_text(job_statement, 4, generator_build_identity) &&
        bind_text(job_statement, 5, normalized_cli_command) &&
        bind_text(job_statement, 6, manifest_hash) &&
        bind_text(job_statement, 7, artifact_storage_key) &&
        sqlite3_step(job_statement) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    sqlite3_finalize(job_statement);

    if (!succeeded || !execute("COMMIT;", error)) {
        if (error.empty()) {
            error = sqlite3_errmsg(database_);
        }
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    return true;
}

bool MetadataStore::fail_job(
    const std::string& job_id,
    const std::string& error_code,
    const std::string& error_message,
    std::string& error
) {
    if (!initialize(error)) {
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "UPDATE jobs SET status = 'failed', error_code = ?2, "
        "error_message = ?3, "
        "completed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?1 AND status = 'running';";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) ==
            SQLITE_OK &&
        bind_text(statement, 1, job_id) &&
        bind_text(statement, 2, error_code) &&
        bind_text(statement, 3, error_message) &&
        sqlite3_step(statement) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    if (!succeeded) {
        error = sqlite3_errmsg(database_);
    }
    sqlite3_finalize(statement);
    return succeeded;
}

RecordLookupStatus MetadataStore::find_package(
    const std::string& package_id,
    const ApiKeyIdentity& owner,
    PackageRecord& package,
    std::string& error
) {
    if (!initialize(error)) {
        return RecordLookupStatus::storage_error;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT id, job_id, organization_id, user_id, "
        "package_fingerprint, manifest_hash, artifact_storage_key "
        "FROM packages WHERE id = ?1 AND organization_id = ?2 "
        "AND user_id = ?3;";
    if (sqlite3_prepare_v2(
            database_,
            sql,
            -1,
            &statement,
            nullptr
        ) != SQLITE_OK ||
        !bind_text(statement, 1, package_id) ||
        !bind_text(statement, 2, owner.organization_id) ||
        !bind_text(statement, 3, owner.user_id)) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return RecordLookupStatus::storage_error;
    }
    const int result = sqlite3_step(statement);
    if (result == SQLITE_DONE) {
        sqlite3_finalize(statement);
        return RecordLookupStatus::not_found;
    }
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return RecordLookupStatus::storage_error;
    }
    PackageRecord loaded;
    loaded.package_id = column_text(statement, 0);
    loaded.job_id = column_text(statement, 1);
    loaded.organization_id = column_text(statement, 2);
    loaded.user_id = column_text(statement, 3);
    loaded.package_fingerprint = column_text(statement, 4);
    loaded.manifest_hash = column_text(statement, 5);
    loaded.artifact_storage_key = column_text(statement, 6);
    sqlite3_finalize(statement);
    package = loaded;
    return RecordLookupStatus::found;
}

}  // namespace syn_sig_ra
