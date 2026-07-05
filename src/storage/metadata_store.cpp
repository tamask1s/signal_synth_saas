#include "syn_sig_ra/metadata_store.h"

#include "syn_sig_ra/random_id.h"

#include <sqlite3.h>

#include <cctype>
#include <string>

namespace {

const int kSchemaVersion = 5;
const int kRequestLimitPerMinute = 120;
const int kConcurrentJobLimit = 2;
const int kMonthlyJobLimit = 100;

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
    display_name TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    )
);

CREATE TABLE IF NOT EXISTS organization_memberships (
    organization_id TEXT NOT NULL,
    user_id TEXT NOT NULL,
    role TEXT NOT NULL CHECK (role IN ('owner', 'admin', 'developer', 'viewer')),
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    PRIMARY KEY (organization_id, user_id),
    FOREIGN KEY (organization_id) REFERENCES organizations(id),
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS projects (
    id TEXT PRIMARY KEY,
    organization_id TEXT NOT NULL,
    display_name TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    UNIQUE (organization_id, display_name),
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
    project_id TEXT NOT NULL,
    user_id TEXT NOT NULL,
    status TEXT NOT NULL
        CHECK (status IN ('queued', 'running', 'succeeded', 'failed', 'cancelled')),
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
    deleted_at TEXT,
    FOREIGN KEY (organization_id) REFERENCES organizations(id),
    FOREIGN KEY (project_id) REFERENCES projects(id),
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS packages (
    id TEXT PRIMARY KEY,
    job_id TEXT NOT NULL UNIQUE,
    organization_id TEXT NOT NULL,
    project_id TEXT NOT NULL,
    user_id TEXT NOT NULL,
    pack_fingerprint TEXT,
    package_fingerprint TEXT NOT NULL,
    generator_version TEXT,
    generator_build_identity TEXT,
    manifest_hash TEXT NOT NULL,
    artifact_storage_key TEXT NOT NULL UNIQUE,
    size_bytes INTEGER NOT NULL DEFAULT 0 CHECK (size_bytes >= 0),
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    deleted_at TEXT,
    FOREIGN KEY (job_id) REFERENCES jobs(id),
    FOREIGN KEY (organization_id) REFERENCES organizations(id),
    FOREIGN KEY (project_id) REFERENCES projects(id),
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

CREATE TABLE IF NOT EXISTS quota_decisions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    organization_id TEXT NOT NULL,
    api_key_id TEXT NOT NULL,
    decision TEXT NOT NULL,
    observed_value INTEGER NOT NULL,
    configured_limit INTEGER NOT NULL,
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    FOREIGN KEY (organization_id) REFERENCES organizations(id),
    FOREIGN KEY (api_key_id) REFERENCES api_keys(id)
);

CREATE TABLE IF NOT EXISTS worker_heartbeat (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    status TEXT NOT NULL,
    last_seen_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS jobs_owner_created_idx
    ON jobs (organization_id, project_id, created_at);
CREATE INDEX IF NOT EXISTS packages_owner_created_idx
    ON packages (organization_id, project_id, created_at);
CREATE INDEX IF NOT EXISTS audit_owner_created_idx
    ON audit_events (organization_id, user_id, created_at);
)SQL";

bool is_role(const std::string& role) {
    return role == "owner" || role == "admin" ||
           role == "developer" || role == "viewer";
}

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

    if (schema_version != 0 && schema_version != kSchemaVersion) {
        error = "SQLite metadata schema is obsolete; reset the pre-beta database";
        return false;
    }

    if (!execute("BEGIN IMMEDIATE;", error)) {
        return false;
    }
    bool succeeded = execute(kSchemaSql, error);
    if (succeeded) {
        succeeded = execute("PRAGMA user_version = 5;", error);
    }
    if (!succeeded || !execute("COMMIT;", error)) {
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
    const std::string role = identity.role.empty() ? "owner" : identity.role;
    if (identity.api_key_id.empty() ||
        identity.organization_id.empty() ||
        identity.user_id.empty() ||
        !is_role(role) ||
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
        "(id, display_name) VALUES (?1, ?1);",
        "INSERT OR REPLACE INTO organization_memberships "
        "(organization_id, user_id, role) VALUES (?1, ?2, ?3);",
        "INSERT OR IGNORE INTO projects "
        "(id, organization_id, display_name) VALUES (?1 || '_default', ?1, 'Default');",
        "INSERT INTO api_keys "
        "(id, organization_id, user_id, key_hash, label) "
        "VALUES (?1, ?2, ?3, ?4, ?5);"
    };

    bool succeeded = true;
    for (int index = 0; index < 5 && succeeded; ++index) {
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
            succeeded = bind_text(statement, 1, identity.user_id);
        } else if (succeeded && index == 2) {
            succeeded =
                bind_text(statement, 1, identity.organization_id) &&
                bind_text(statement, 2, identity.user_id) &&
                bind_text(statement, 3, role);
        } else if (succeeded && index == 3) {
            succeeded = bind_text(statement, 1, identity.organization_id);
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
        "SELECT k.id, k.organization_id, k.user_id, m.role "
        "FROM api_keys k JOIN organization_memberships m "
        "ON m.organization_id = k.organization_id AND m.user_id = k.user_id "
        "WHERE k.key_hash = ?1 AND k.active = 1;";
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
    identity.role = reinterpret_cast<const char*>(
        sqlite3_column_text(statement, 3)
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

bool MetadataStore::usage_summary(
    const ApiKeyIdentity& identity,
    UsageSummary& usage,
    std::string& error
) {
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT "
        "(SELECT count(*) FROM audit_events WHERE api_key_id = ?1 "
        " AND event_type = 'api_key.authenticated' "
        " AND created_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now','-1 minute')),"
        "(SELECT count(*) FROM jobs WHERE organization_id = ?2 "
        " AND status IN ('queued','running') AND deleted_at IS NULL),"
        "(SELECT count(*) FROM jobs WHERE organization_id = ?2 "
        " AND strftime('%Y-%m',created_at)=strftime('%Y-%m','now')),"
        "(SELECT count(*) FROM packages WHERE organization_id = ?2 "
        " AND strftime('%Y-%m',created_at)=strftime('%Y-%m','now')),"
        "(SELECT COALESCE(sum(size_bytes),0) FROM packages "
        " WHERE organization_id = ?2 "
        " AND strftime('%Y-%m',created_at)=strftime('%Y-%m','now')),"
        "(SELECT count(*) FROM jobs WHERE organization_id = ?2 "
        " AND status = 'queued' AND deleted_at IS NULL),"
        "(SELECT count(*) FROM jobs WHERE organization_id = ?2 "
        " AND status = 'running' AND deleted_at IS NULL),"
        "(SELECT count(*) FROM jobs WHERE organization_id = ?2 "
        " AND status = 'failed' "
        " AND strftime('%Y-%m',created_at)=strftime('%Y-%m','now')),"
        "(SELECT count(*) FROM quota_decisions WHERE organization_id = ?2 "
        " AND strftime('%Y-%m',created_at)=strftime('%Y-%m','now')),"
        "(SELECT COALESCE(last_seen_at,'') FROM worker_heartbeat WHERE singleton=1),"
        "(SELECT COALESCE(status,'') FROM worker_heartbeat WHERE singleton=1);";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, identity.api_key_id) ||
        !bind_text(statement, 2, identity.organization_id) ||
        sqlite3_step(statement) != SQLITE_ROW) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return false;
    }
    usage.requests_last_minute = sqlite3_column_int(statement, 0);
    usage.active_jobs = sqlite3_column_int(statement, 1);
    usage.jobs_this_month = sqlite3_column_int(statement, 2);
    usage.packages_this_month = sqlite3_column_int(statement, 3);
    usage.package_bytes_this_month = sqlite3_column_int64(statement, 4);
    usage.queued_jobs = sqlite3_column_int(statement, 5);
    usage.running_jobs = sqlite3_column_int(statement, 6);
    usage.failed_jobs_this_month = sqlite3_column_int(statement, 7);
    usage.quota_rejections_this_month = sqlite3_column_int(statement, 8);
    usage.worker_last_seen_at = column_text(statement, 9);
    usage.worker_last_status = column_text(statement, 10);
    usage.request_limit_per_minute = kRequestLimitPerMinute;
    usage.concurrent_job_limit = kConcurrentJobLimit;
    usage.monthly_job_limit = kMonthlyJobLimit;
    sqlite3_finalize(statement);
    return true;
}

bool MetadataStore::record_worker_heartbeat(
    const std::string& status,
    std::string& error
) {
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT OR REPLACE INTO worker_heartbeat "
        "(singleton,status,last_seen_at) VALUES "
        "(1,?1,strftime('%Y-%m-%dT%H:%M:%fZ','now'));";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, status) &&
        sqlite3_step(statement) == SQLITE_DONE;
    if (!succeeded) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return succeeded;
}

QuotaStatus MetadataStore::check_request_quota(
    const ApiKeyIdentity& identity,
    UsageSummary& usage,
    std::string& error
) {
    if (!usage_summary(identity, usage, error)) {
        return QuotaStatus::storage_error;
    }
    if (usage.requests_last_minute <= kRequestLimitPerMinute) {
        return QuotaStatus::allowed;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT INTO quota_decisions "
        "(organization_id,api_key_id,decision,observed_value,configured_limit) "
        "VALUES (?1,?2,'request_rate_limited',?3,?4);";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, identity.organization_id) ||
        !bind_text(statement, 2, identity.api_key_id) ||
        sqlite3_bind_int(statement, 3, usage.requests_last_minute) != SQLITE_OK ||
        sqlite3_bind_int(statement, 4, kRequestLimitPerMinute) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return QuotaStatus::storage_error;
    }
    sqlite3_finalize(statement);
    return QuotaStatus::rate_limited;
}

QuotaStatus MetadataStore::check_job_quota(
    const ApiKeyIdentity& identity,
    UsageSummary& usage,
    std::string& error
) {
    if (!usage_summary(identity, usage, error)) {
        return QuotaStatus::storage_error;
    }
    const char* decision = nullptr;
    int observed = 0;
    int limit = 0;
    QuotaStatus status = QuotaStatus::allowed;
    if (usage.active_jobs >= kConcurrentJobLimit) {
        decision = "concurrent_job_limit";
        observed = usage.active_jobs;
        limit = kConcurrentJobLimit;
        status = QuotaStatus::concurrent_limit;
    } else if (usage.jobs_this_month >= kMonthlyJobLimit) {
        decision = "monthly_job_limit";
        observed = usage.jobs_this_month;
        limit = kMonthlyJobLimit;
        status = QuotaStatus::monthly_limit;
    } else {
        return status;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT INTO quota_decisions "
        "(organization_id,api_key_id,decision,observed_value,configured_limit) "
        "VALUES (?1,?2,?3,?4,?5);";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, identity.organization_id) ||
        !bind_text(statement, 2, identity.api_key_id) ||
        !bind_text(statement, 3, decision) ||
        sqlite3_bind_int(statement, 4, observed) != SQLITE_OK ||
        sqlite3_bind_int(statement, 5, limit) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return QuotaStatus::storage_error;
    }
    sqlite3_finalize(statement);
    return status;
}

bool MetadataStore::list_projects(
    const ApiKeyIdentity& identity,
    std::vector<ProjectRecord>& projects,
    std::string& error
) {
    projects.clear();
    if (!initialize(error)) {
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT id, organization_id, display_name, created_at "
        "FROM projects WHERE organization_id = ?1 ORDER BY display_name, id;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, identity.organization_id)) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return false;
    }
    for (;;) {
        const int result = sqlite3_step(statement);
        if (result == SQLITE_DONE) {
            sqlite3_finalize(statement);
            return true;
        }
        if (result != SQLITE_ROW) {
            error = sqlite3_errmsg(database_);
            sqlite3_finalize(statement);
            return false;
        }
        ProjectRecord project;
        project.project_id = column_text(statement, 0);
        project.organization_id = column_text(statement, 1);
        project.display_name = column_text(statement, 2);
        project.created_at = column_text(statement, 3);
        projects.push_back(project);
    }
}

RecordLookupStatus MetadataStore::find_project(
    const std::string& project_id,
    const ApiKeyIdentity& identity,
    ProjectRecord& project,
    std::string& error
) {
    if (!initialize(error)) {
        return RecordLookupStatus::storage_error;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT id, organization_id, display_name, created_at "
        "FROM projects WHERE id = ?1 AND organization_id = ?2;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, project_id) ||
        !bind_text(statement, 2, identity.organization_id)) {
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
    project.project_id = column_text(statement, 0);
    project.organization_id = column_text(statement, 1);
    project.display_name = column_text(statement, 2);
    project.created_at = column_text(statement, 3);
    sqlite3_finalize(statement);
    return RecordLookupStatus::found;
}

bool MetadataStore::create_project(
    const ApiKeyIdentity& identity,
    const std::string& display_name,
    ProjectRecord& project,
    std::string& error
) {
    if ((identity.role != "owner" && identity.role != "admin") ||
        display_name.empty() || display_name.size() > 100) {
        error = "owner/admin role and a 1-100 character display name are required";
        return false;
    }
    std::string project_id;
    if (!initialize(error) || !random_id("project_", project_id, error)) {
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT INTO projects (id, organization_id, display_name) "
        "VALUES (?1, ?2, ?3);";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, project_id) &&
        bind_text(statement, 2, identity.organization_id) &&
        bind_text(statement, 3, display_name) &&
        sqlite3_step(statement) == SQLITE_DONE;
    if (!succeeded) {
        error = sqlite3_errmsg(database_);
    }
    sqlite3_finalize(statement);
    if (!succeeded) {
        return false;
    }
    return find_project(project_id, identity, project, error) ==
        RecordLookupStatus::found;
}

bool MetadataStore::create_job(
    const ApiKeyIdentity& owner,
    const std::string& project_id,
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
        "(id, organization_id, project_id, user_id, status, request_json, "
        "selected_pack_id, source_pack_path, pack_fingerprint) "
        "VALUES (?1, ?2, ?3, ?4, 'queued', ?5, ?6, ?7, ?8);";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) ==
            SQLITE_OK &&
        bind_text(statement, 1, job_id) &&
        bind_text(statement, 2, owner.organization_id) &&
        bind_text(statement, 3, project_id) &&
        bind_text(statement, 4, owner.user_id) &&
        bind_text(statement, 5, request_json) &&
        bind_text(statement, 6, pack_id) &&
        bind_text(statement, 7, source_pack_path) &&
        bind_text(statement, 8, pack_fingerprint) &&
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
        "SELECT j.id, j.organization_id, j.project_id, j.user_id, j.status, "
        "j.request_json, j.selected_pack_id, j.source_pack_path, "
        "j.pack_fingerprint, COALESCE(p.id, ''), "
        "j.package_fingerprint, j.generator_version, "
        "j.generator_build_identity, j.manifest_hash, "
        "j.artifact_storage_key, j.error_code, j.error_message, "
        "j.created_at, j.started_at, j.completed_at "
        "FROM jobs j LEFT JOIN packages p "
        "ON p.job_id = j.id AND p.deleted_at IS NULL "
        "WHERE j.id = ?1 AND j.organization_id = ?2 "
        "AND j.deleted_at IS NULL;";
    if (sqlite3_prepare_v2(
            database_,
            sql,
            -1,
            &statement,
            nullptr
        ) != SQLITE_OK ||
        !bind_text(statement, 1, job_id) ||
        !bind_text(statement, 2, owner.organization_id)) {
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
    loaded.project_id = column_text(statement, 2);
    loaded.user_id = column_text(statement, 3);
    loaded.status = column_text(statement, 4);
    loaded.request_json = column_text(statement, 5);
    loaded.selected_pack_id = column_text(statement, 6);
    loaded.source_pack_path = column_text(statement, 7);
    loaded.pack_fingerprint = column_text(statement, 8);
    loaded.package_id = column_text(statement, 9);
    loaded.package_fingerprint = column_text(statement, 10);
    loaded.generator_version = column_text(statement, 11);
    loaded.generator_build_identity = column_text(statement, 12);
    loaded.manifest_hash = column_text(statement, 13);
    loaded.artifact_storage_key = column_text(statement, 14);
    loaded.error_code = column_text(statement, 15);
    loaded.error_message = column_text(statement, 16);
    loaded.created_at = column_text(statement, 17);
    loaded.started_at = column_text(statement, 18);
    loaded.completed_at = column_text(statement, 19);
    sqlite3_finalize(statement);
    job = loaded;
    return RecordLookupStatus::found;
}

bool MetadataStore::list_jobs(
    const ApiKeyIdentity& owner,
    int limit,
    int offset,
    std::vector<JobRecord>& jobs,
    std::string& error
) {
    jobs.clear();
    if (limit <= 0 || limit > 100 || offset < 0) {
        error = "job list limit/offset is invalid";
        return false;
    }
    if (!initialize(error)) {
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT j.id, j.organization_id, j.project_id, j.user_id, j.status, "
        "j.request_json, j.selected_pack_id, j.source_pack_path, "
        "j.pack_fingerprint, COALESCE(p.id, ''), "
        "j.package_fingerprint, j.generator_version, "
        "j.generator_build_identity, j.manifest_hash, "
        "j.artifact_storage_key, j.error_code, j.error_message, "
        "j.created_at, j.started_at, j.completed_at "
        "FROM jobs j LEFT JOIN packages p "
        "ON p.job_id = j.id AND p.deleted_at IS NULL "
        "WHERE j.organization_id = ?1 "
        "AND j.deleted_at IS NULL "
        "ORDER BY j.created_at DESC, j.id DESC LIMIT ?2 OFFSET ?3;";
    if (sqlite3_prepare_v2(
            database_,
            sql,
            -1,
            &statement,
            nullptr
        ) != SQLITE_OK ||
        !bind_text(statement, 1, owner.organization_id) ||
        sqlite3_bind_int(statement, 2, limit) != SQLITE_OK ||
        sqlite3_bind_int(statement, 3, offset) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return false;
    }
    for (;;) {
        const int result = sqlite3_step(statement);
        if (result == SQLITE_DONE) {
            sqlite3_finalize(statement);
            return true;
        }
        if (result != SQLITE_ROW) {
            error = sqlite3_errmsg(database_);
            sqlite3_finalize(statement);
            return false;
        }
        JobRecord loaded;
        loaded.job_id = column_text(statement, 0);
        loaded.organization_id = column_text(statement, 1);
        loaded.project_id = column_text(statement, 2);
        loaded.user_id = column_text(statement, 3);
        loaded.status = column_text(statement, 4);
        loaded.request_json = column_text(statement, 5);
        loaded.selected_pack_id = column_text(statement, 6);
        loaded.source_pack_path = column_text(statement, 7);
        loaded.pack_fingerprint = column_text(statement, 8);
        loaded.package_id = column_text(statement, 9);
        loaded.package_fingerprint = column_text(statement, 10);
        loaded.generator_version = column_text(statement, 11);
        loaded.generator_build_identity = column_text(statement, 12);
        loaded.manifest_hash = column_text(statement, 13);
        loaded.artifact_storage_key = column_text(statement, 14);
        loaded.error_code = column_text(statement, 15);
        loaded.error_message = column_text(statement, 16);
        loaded.created_at = column_text(statement, 17);
        loaded.started_at = column_text(statement, 18);
        loaded.completed_at = column_text(statement, 19);
        jobs.push_back(loaded);
    }
}

JobLifecycleStatus MetadataStore::cancel_job(
    const std::string& job_id,
    const ApiKeyIdentity& owner,
    std::string& error
) {
    if (!initialize(error)) {
        return JobLifecycleStatus::storage_error;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "UPDATE jobs SET status = 'cancelled', "
        "completed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?1 AND organization_id = ?2 AND status = 'queued' "
        "AND deleted_at IS NULL;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, job_id) ||
        !bind_text(statement, 2, owner.organization_id) ||
        sqlite3_step(statement) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return JobLifecycleStatus::storage_error;
    }
    const bool changed = sqlite3_changes(database_) == 1;
    sqlite3_finalize(statement);
    if (changed) {
        return JobLifecycleStatus::succeeded;
    }
    JobRecord existing;
    const RecordLookupStatus lookup = find_job(job_id, owner, existing, error);
    if (lookup == RecordLookupStatus::not_found) {
        return JobLifecycleStatus::not_found;
    }
    return lookup == RecordLookupStatus::found
        ? JobLifecycleStatus::invalid_state
        : JobLifecycleStatus::storage_error;
}

JobLifecycleStatus MetadataStore::retry_job(
    const std::string& job_id,
    const ApiKeyIdentity& owner,
    std::string& new_job_id,
    std::string& error
) {
    JobRecord existing;
    const RecordLookupStatus lookup = find_job(job_id, owner, existing, error);
    if (lookup == RecordLookupStatus::not_found) {
        return JobLifecycleStatus::not_found;
    }
    if (lookup != RecordLookupStatus::found) {
        return JobLifecycleStatus::storage_error;
    }
    if (existing.status != "failed" && existing.status != "cancelled") {
        return JobLifecycleStatus::invalid_state;
    }
    if (!create_job(
            owner,
            existing.project_id,
            existing.request_json,
            existing.selected_pack_id,
            existing.source_pack_path,
            existing.pack_fingerprint,
            new_job_id,
            error)) {
        return JobLifecycleStatus::storage_error;
    }
    return JobLifecycleStatus::succeeded;
}

JobDeleteStatus MetadataStore::delete_job(
    const std::string& job_id,
    const ApiKeyIdentity& owner,
    std::string& error
) {
    if (job_id.empty()) {
        error = "job ID is required";
        return JobDeleteStatus::storage_error;
    }
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return JobDeleteStatus::storage_error;
    }

    sqlite3_stmt* select = nullptr;
    const char* select_sql =
        "SELECT status FROM jobs WHERE id = ?1 AND organization_id = ?2 "
        "AND deleted_at IS NULL;";
    if (sqlite3_prepare_v2(
            database_,
            select_sql,
            -1,
            &select,
            nullptr
        ) != SQLITE_OK ||
        !bind_text(select, 1, job_id) ||
        !bind_text(select, 2, owner.organization_id)) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(select);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return JobDeleteStatus::storage_error;
    }
    const int select_result = sqlite3_step(select);
    if (select_result == SQLITE_DONE) {
        sqlite3_finalize(select);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return JobDeleteStatus::not_found;
    }
    if (select_result != SQLITE_ROW) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(select);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return JobDeleteStatus::storage_error;
    }
    const std::string status = column_text(select, 0);
    sqlite3_finalize(select);
    if (status == "running") {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return JobDeleteStatus::running;
    }

    sqlite3_stmt* update_job = nullptr;
    const char* update_job_sql =
        "UPDATE jobs SET deleted_at = "
        "strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?1 AND organization_id = ?2 "
        "AND deleted_at IS NULL AND status != 'running';";
    bool succeeded =
        sqlite3_prepare_v2(
            database_,
            update_job_sql,
            -1,
            &update_job,
            nullptr
        ) == SQLITE_OK &&
        bind_text(update_job, 1, job_id) &&
        bind_text(update_job, 2, owner.organization_id) &&
        sqlite3_step(update_job) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    sqlite3_finalize(update_job);

    sqlite3_stmt* update_package = nullptr;
    const char* update_package_sql =
        "UPDATE packages SET deleted_at = "
        "strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE job_id = ?1 AND organization_id = ?2 "
        "AND deleted_at IS NULL;";
    succeeded = succeeded &&
        sqlite3_prepare_v2(
            database_,
            update_package_sql,
            -1,
            &update_package,
            nullptr
        ) == SQLITE_OK &&
        bind_text(update_package, 1, job_id) &&
        bind_text(update_package, 2, owner.organization_id) &&
        sqlite3_step(update_package) == SQLITE_DONE;
    sqlite3_finalize(update_package);

    sqlite3_stmt* audit = nullptr;
    const char* audit_sql =
        "INSERT INTO audit_events "
        "(organization_id, user_id, api_key_id, event_type, "
        "subject_type, subject_id) "
        "VALUES (?1, ?2, ?3, 'job.deleted', 'job', ?4);";
    succeeded = succeeded &&
        sqlite3_prepare_v2(
            database_,
            audit_sql,
            -1,
            &audit,
            nullptr
        ) == SQLITE_OK &&
        bind_text(audit, 1, owner.organization_id) &&
        bind_text(audit, 2, owner.user_id) &&
        bind_text(audit, 3, owner.api_key_id) &&
        bind_text(audit, 4, job_id) &&
        sqlite3_step(audit) == SQLITE_DONE;
    sqlite3_finalize(audit);

    if (!succeeded || !execute("COMMIT;", error)) {
        if (error.empty()) {
            error = sqlite3_errmsg(database_);
        }
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return JobDeleteStatus::storage_error;
    }
    return JobDeleteStatus::deleted;
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
        "SELECT id, organization_id, project_id, user_id, request_json, "
        "selected_pack_id, source_pack_path, pack_fingerprint, created_at "
        "FROM jobs WHERE status = 'queued' AND deleted_at IS NULL "
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
    claimed.project_id = column_text(select, 2);
    claimed.user_id = column_text(select, 3);
    claimed.request_json = column_text(select, 4);
    claimed.selected_pack_id = column_text(select, 5);
    claimed.source_pack_path = column_text(select, 6);
    claimed.pack_fingerprint = column_text(select, 7);
    claimed.created_at = column_text(select, 8);
    sqlite3_finalize(select);

    sqlite3_stmt* update = nullptr;
    const char* update_sql =
        "UPDATE jobs SET status = 'running', started_at = "
        "strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?1 AND status = 'queued' AND deleted_at IS NULL;";
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
        "WHERE id = ?1 AND status = 'running' AND deleted_at IS NULL;";
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
    long long size_bytes,
    std::string& error
) {
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return false;
    }
    sqlite3_stmt* package_statement = nullptr;
    const char* package_sql =
        "INSERT INTO packages "
        "(id, job_id, organization_id, project_id, user_id, pack_fingerprint, "
        "package_fingerprint, generator_version, "
        "generator_build_identity, manifest_hash, artifact_storage_key, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12);";
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
        bind_text(package_statement, 4, job.project_id) &&
        bind_text(package_statement, 5, job.user_id) &&
        bind_text(package_statement, 6, job.pack_fingerprint) &&
        bind_text(package_statement, 7, package_fingerprint) &&
        bind_text(package_statement, 8, generator_version) &&
        bind_text(package_statement, 9, generator_build_identity) &&
        bind_text(package_statement, 10, manifest_hash) &&
        bind_text(package_statement, 11, artifact_storage_key) &&
        sqlite3_bind_int64(package_statement, 12, size_bytes) == SQLITE_OK &&
        sqlite3_step(package_statement) == SQLITE_DONE;
    sqlite3_finalize(package_statement);

    sqlite3_stmt* job_statement = nullptr;
    const char* job_sql =
        "UPDATE jobs SET status = 'succeeded', package_fingerprint = ?2, "
        "generator_version = ?3, generator_build_identity = ?4, "
        "normalized_cli_command = ?5, manifest_hash = ?6, "
        "artifact_storage_key = ?7, "
        "completed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?1 AND status = 'running' AND deleted_at IS NULL;";
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
        "WHERE id = ?1 AND status = 'running' AND deleted_at IS NULL;";
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
        "SELECT p.id, p.job_id, p.organization_id, p.project_id, p.user_id, "
        "p.package_fingerprint, p.manifest_hash, p.artifact_storage_key "
        "FROM packages p JOIN jobs j ON j.id = p.job_id "
        "WHERE p.id = ?1 AND p.organization_id = ?2 "
        "AND p.deleted_at IS NULL "
        "AND j.deleted_at IS NULL;";
    if (sqlite3_prepare_v2(
            database_,
            sql,
            -1,
            &statement,
            nullptr
        ) != SQLITE_OK ||
        !bind_text(statement, 1, package_id) ||
        !bind_text(statement, 2, owner.organization_id)) {
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
    loaded.project_id = column_text(statement, 3);
    loaded.user_id = column_text(statement, 4);
    loaded.package_fingerprint = column_text(statement, 5);
    loaded.manifest_hash = column_text(statement, 6);
    loaded.artifact_storage_key = column_text(statement, 7);
    sqlite3_finalize(statement);
    package = loaded;
    return RecordLookupStatus::found;
}

bool MetadataStore::list_api_keys(
    const std::string& organization_id,
    std::vector<ApiKeyRecord>& api_keys,
    std::string& error
) {
    api_keys.clear();
    if (!initialize(error)) {
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const char* all_sql =
        "SELECT k.id, k.organization_id, k.user_id, m.role, k.label, k.active, "
        "k.created_at, COALESCE(k.last_used_at, '') "
        "FROM api_keys k JOIN organization_memberships m "
        "ON m.organization_id = k.organization_id AND m.user_id = k.user_id "
        "ORDER BY k.organization_id, k.user_id, k.created_at;";
    const char* org_sql =
        "SELECT k.id, k.organization_id, k.user_id, m.role, k.label, k.active, "
        "k.created_at, COALESCE(k.last_used_at, '') "
        "FROM api_keys k JOIN organization_memberships m "
        "ON m.organization_id = k.organization_id AND m.user_id = k.user_id "
        "WHERE k.organization_id = ?1 ORDER BY k.user_id, k.created_at;";
    const char* sql = organization_id.empty() ? all_sql : org_sql;
    if (sqlite3_prepare_v2(
            database_,
            sql,
            -1,
            &statement,
            nullptr
        ) != SQLITE_OK ||
        (!organization_id.empty() &&
         !bind_text(statement, 1, organization_id))) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return false;
    }
    for (;;) {
        const int result = sqlite3_step(statement);
        if (result == SQLITE_DONE) {
            sqlite3_finalize(statement);
            return true;
        }
        if (result != SQLITE_ROW) {
            error = sqlite3_errmsg(database_);
            sqlite3_finalize(statement);
            return false;
        }
        ApiKeyRecord loaded;
        loaded.api_key_id = column_text(statement, 0);
        loaded.organization_id = column_text(statement, 1);
        loaded.user_id = column_text(statement, 2);
        loaded.role = column_text(statement, 3);
        loaded.label = column_text(statement, 4);
        loaded.active = sqlite3_column_int(statement, 5) == 1;
        loaded.created_at = column_text(statement, 6);
        loaded.last_used_at = column_text(statement, 7);
        api_keys.push_back(loaded);
    }
}

bool MetadataStore::revoke_api_key(
    const std::string& api_key_id,
    std::string& error
) {
    if (api_key_id.empty()) {
        error = "API key ID is required";
        return false;
    }
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return false;
    }

    sqlite3_stmt* update = nullptr;
    const char* update_sql =
        "UPDATE api_keys SET active = 0 WHERE id = ?1 AND active = 1;";
    bool succeeded =
        sqlite3_prepare_v2(
            database_,
            update_sql,
            -1,
            &update,
            nullptr
        ) == SQLITE_OK &&
        bind_text(update, 1, api_key_id) &&
        sqlite3_step(update) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    sqlite3_finalize(update);

    sqlite3_stmt* audit = nullptr;
    const char* audit_sql =
        "INSERT INTO audit_events "
        "(organization_id, user_id, api_key_id, event_type, "
        "subject_type, subject_id) "
        "SELECT organization_id, user_id, id, 'api_key.revoked', "
        "'api_key', id FROM api_keys WHERE id = ?1;";
    succeeded = succeeded &&
        sqlite3_prepare_v2(
            database_,
            audit_sql,
            -1,
            &audit,
            nullptr
        ) == SQLITE_OK &&
        bind_text(audit, 1, api_key_id) &&
        sqlite3_step(audit) == SQLITE_DONE;
    sqlite3_finalize(audit);

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

}  // namespace syn_sig_ra
