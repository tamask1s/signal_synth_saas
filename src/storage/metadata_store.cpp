#include "syn_sig_ra/metadata_store.h"

#include "syn_sig_ra/random_id.h"
#include "syn_sig_ra/sha256.h"

#include <sqlite3.h>

#include <cctype>
#include <sstream>
#include <string>

namespace {

const int kSchemaVersion = 2;
const int kRequestLimitPerMinute = 120;
const int kConcurrentJobLimit = 2;
const int kMonthlyJobLimit = 100;

const char kSchemaSql[] = R"SQL(
CREATE TABLE metadata_schema (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    contract TEXT NOT NULL CHECK (contract = 'synsigra_saas_metadata_v2')
);
INSERT INTO metadata_schema (singleton,contract)
VALUES (1,'synsigra_saas_metadata_v2');

CREATE TABLE organizations (
    id TEXT PRIMARY KEY,
    display_name TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    )
);

CREATE TABLE users (
    id TEXT PRIMARY KEY,
    email TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL,
    password_salt TEXT NOT NULL,
    password_hash TEXT NOT NULL,
    email_verified INTEGER NOT NULL DEFAULT 1
        CHECK (email_verified IN (0, 1)),
    email_verified_at TEXT,
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    )
);

CREATE TABLE legal_acceptances (
    user_id TEXT NOT NULL,
    document_type TEXT NOT NULL,
    document_version TEXT NOT NULL,
    accepted_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    PRIMARY KEY (user_id, document_type, document_version),
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE organization_memberships (
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

CREATE TABLE projects (
    id TEXT PRIMARY KEY,
    organization_id TEXT NOT NULL,
    display_name TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    UNIQUE (organization_id, display_name),
    FOREIGN KEY (organization_id) REFERENCES organizations(id)
);

CREATE TABLE api_keys (
    id TEXT PRIMARY KEY,
    organization_id TEXT NOT NULL,
    user_id TEXT NOT NULL,
    key_hash TEXT NOT NULL UNIQUE
        CHECK (length(key_hash) = 64),
    label TEXT NOT NULL,
    kind TEXT NOT NULL DEFAULT 'personal'
        CHECK (kind IN ('personal', 'browser')),
    active INTEGER NOT NULL DEFAULT 1
        CHECK (active IN (0, 1)),
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    last_used_at TEXT,
    FOREIGN KEY (organization_id) REFERENCES organizations(id),
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE sessions (
    id TEXT PRIMARY KEY,
    token_hash TEXT NOT NULL UNIQUE CHECK (length(token_hash) = 64),
    api_key_id TEXT NOT NULL,
    expires_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '+7 days')
    ),
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    FOREIGN KEY (api_key_id) REFERENCES api_keys(id)
);

CREATE TABLE jobs (
    id TEXT PRIMARY KEY,
    organization_id TEXT NOT NULL,
    project_id TEXT NOT NULL,
    user_id TEXT NOT NULL,
    status TEXT NOT NULL
        CHECK (status IN ('queued', 'running', 'succeeded', 'failed', 'cancelled')),
    request_json TEXT NOT NULL,
    selected_pack_id TEXT NOT NULL,
    source_pack_path TEXT NOT NULL,
    pack_fingerprint TEXT NOT NULL,
    selected_pack_version TEXT NOT NULL,
    catalog_version TEXT NOT NULL,
    catalog_source_sha256 TEXT NOT NULL,
    package_fingerprint TEXT,
    integration_contract_version TEXT,
    integration_contract_json TEXT,
    generator_version TEXT,
    generator_git_commit TEXT,
    generator_build_identity TEXT,
    generator_binary_sha256 TEXT,
    challenge_receipt_json TEXT,
    challenge_metadata_json TEXT,
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

CREATE TABLE packages (
    id TEXT PRIMARY KEY,
    job_id TEXT NOT NULL UNIQUE,
    organization_id TEXT NOT NULL,
    project_id TEXT NOT NULL,
    user_id TEXT NOT NULL,
    pack_fingerprint TEXT NOT NULL,
    package_fingerprint TEXT NOT NULL,
    selected_pack_version TEXT NOT NULL,
    catalog_version TEXT NOT NULL,
    catalog_source_sha256 TEXT NOT NULL,
    integration_contract_version TEXT NOT NULL,
    integration_contract_json TEXT NOT NULL,
    generator_version TEXT,
    generator_git_commit TEXT,
    generator_build_identity TEXT,
    generator_binary_sha256 TEXT,
    challenge_receipt_json TEXT NOT NULL,
    challenge_metadata_json TEXT NOT NULL,
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

CREATE TABLE audit_events (
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

CREATE TABLE quota_decisions (
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

CREATE TABLE worker_heartbeat (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    status TEXT NOT NULL,
    last_seen_at TEXT NOT NULL
);

CREATE TABLE scenario_drafts (
    id TEXT PRIMARY KEY,
    organization_id TEXT NOT NULL,
    user_id TEXT NOT NULL,
    name TEXT NOT NULL,
    status TEXT NOT NULL CHECK (status IN ('valid','invalid')),
    document_json TEXT NOT NULL,
    document_fingerprint TEXT,
    target_intent_json TEXT NOT NULL DEFAULT '["r_peak"]',
    validation_errors_json TEXT NOT NULL DEFAULT '[]',
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    updated_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    FOREIGN KEY (organization_id) REFERENCES organizations(id),
    FOREIGN KEY (user_id) REFERENCES users(id)
);
CREATE INDEX scenario_drafts_owner_updated_idx
    ON scenario_drafts (organization_id,user_id,updated_at);

CREATE TABLE custom_packs (
    id TEXT PRIMARY KEY,
    organization_id TEXT NOT NULL,
    user_id TEXT NOT NULL,
    name TEXT NOT NULL,
    version TEXT NOT NULL,
    description TEXT NOT NULL,
    targets_json TEXT NOT NULL,
    scenario_ids_json TEXT NOT NULL,
    pack_fingerprint TEXT NOT NULL,
    source_pack_path TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    deleted_at TEXT,
    FOREIGN KEY (organization_id) REFERENCES organizations(id),
    FOREIGN KEY (user_id) REFERENCES users(id)
);
CREATE INDEX custom_packs_owner_created_idx
    ON custom_packs (organization_id,user_id,created_at);

CREATE TABLE email_tokens (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL,
    purpose TEXT NOT NULL CHECK (purpose IN ('email_verification','password_reset')),
    token_hash TEXT NOT NULL UNIQUE CHECK (length(token_hash) = 64),
    recipient_email TEXT NOT NULL,
    expires_at TEXT NOT NULL,
    consumed_at TEXT,
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    ),
    FOREIGN KEY (user_id) REFERENCES users(id)
);
CREATE INDEX email_tokens_user_purpose_created_idx
    ON email_tokens (user_id,purpose,created_at);
CREATE INDEX email_tokens_hash_idx
    ON email_tokens (token_hash);

CREATE TABLE email_send_attempts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    purpose TEXT NOT NULL,
    email_hash TEXT NOT NULL CHECK (length(email_hash) = 64),
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    )
);
CREATE INDEX email_send_attempts_rate_idx
    ON email_send_attempts (purpose,email_hash,created_at);

CREATE TABLE email_token_submissions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    purpose TEXT NOT NULL,
    token_hash TEXT NOT NULL CHECK (length(token_hash) = 64),
    accepted INTEGER NOT NULL DEFAULT 0 CHECK (accepted IN (0, 1)),
    created_at TEXT NOT NULL DEFAULT (
        strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    )
);
CREATE INDEX email_token_submissions_rate_idx
    ON email_token_submissions (purpose,token_hash,created_at);

CREATE INDEX jobs_owner_created_idx
    ON jobs (organization_id, project_id, created_at);
CREATE INDEX packages_owner_created_idx
    ON packages (organization_id, project_id, created_at);
CREATE INDEX audit_owner_created_idx
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

bool execute_bound(
    sqlite3* database,
    const char* sql,
    const std::vector<std::string>& values,
    std::string& error
) {
    sqlite3_stmt* statement = nullptr;
    bool succeeded = sqlite3_prepare_v2(
        database, sql, -1, &statement, nullptr) == SQLITE_OK;
    for (std::size_t index = 0; succeeded && index < values.size(); ++index) {
        succeeded = bind_text(
            statement, static_cast<int>(index + 1), values[index]);
    }
    succeeded = succeeded && sqlite3_step(statement) == SQLITE_DONE;
    if (!succeeded) error = sqlite3_errmsg(database);
    sqlite3_finalize(statement);
    return succeeded;
}

std::string column_text(sqlite3_stmt* statement, int index) {
    const unsigned char* value = sqlite3_column_text(statement, index);
    return value == nullptr
        ? std::string()
        : std::string(reinterpret_cast<const char*>(value));
}

syn_sig_ra::ScenarioDraftRecord scenario_draft_columns(
    sqlite3_stmt* statement
) {
    syn_sig_ra::ScenarioDraftRecord draft;
    draft.scenario_id = column_text(statement, 0);
    draft.organization_id = column_text(statement, 1);
    draft.user_id = column_text(statement, 2);
    draft.name = column_text(statement, 3);
    draft.status = column_text(statement, 4);
    draft.document_json = column_text(statement, 5);
    draft.document_fingerprint = column_text(statement, 6);
    draft.target_intent_json = column_text(statement, 7);
    draft.validation_errors_json = column_text(statement, 8);
    draft.created_at = column_text(statement, 9);
    draft.updated_at = column_text(statement, 10);
    return draft;
}

syn_sig_ra::CustomPackRecord custom_pack_columns(sqlite3_stmt* statement) {
    syn_sig_ra::CustomPackRecord pack;
    pack.pack_id = column_text(statement, 0);
    pack.organization_id = column_text(statement, 1);
    pack.user_id = column_text(statement, 2);
    pack.name = column_text(statement, 3);
    pack.version = column_text(statement, 4);
    pack.description = column_text(statement, 5);
    pack.targets_json = column_text(statement, 6);
    pack.scenario_ids_json = column_text(statement, 7);
    pack.pack_fingerprint = column_text(statement, 8);
    pack.source_pack_path = column_text(statement, 9);
    pack.created_at = column_text(statement, 10);
    return pack;
}

syn_sig_ra::JobRecord job_columns(sqlite3_stmt* statement) {
    syn_sig_ra::JobRecord job;
    job.job_id = column_text(statement, 0);
    job.organization_id = column_text(statement, 1);
    job.project_id = column_text(statement, 2);
    job.user_id = column_text(statement, 3);
    job.status = column_text(statement, 4);
    job.request_json = column_text(statement, 5);
    job.selected_pack_id = column_text(statement, 6);
    job.source_pack_path = column_text(statement, 7);
    job.pack_fingerprint = column_text(statement, 8);
    job.package_id = column_text(statement, 9);
    job.package_fingerprint = column_text(statement, 10);
    job.integration_contract_version = column_text(statement, 11);
    job.integration_contract_json = column_text(statement, 12);
    job.generator_version = column_text(statement, 13);
    job.generator_git_commit = column_text(statement, 14);
    job.generator_build_identity = column_text(statement, 15);
    job.generator_binary_sha256 = column_text(statement, 16);
    job.challenge_receipt_json = column_text(statement, 17);
    job.manifest_hash = column_text(statement, 18);
    job.artifact_storage_key = column_text(statement, 19);
    job.error_code = column_text(statement, 20);
    job.error_message = column_text(statement, 21);
    job.created_at = column_text(statement, 22);
    job.started_at = column_text(statement, 23);
    job.completed_at = column_text(statement, 24);
    job.selected_pack_version = column_text(statement, 25);
    job.catalog_version = column_text(statement, 26);
    job.catalog_source_sha256 = column_text(statement, 27);
    job.challenge_metadata_json = column_text(statement, 28);
    if (sqlite3_column_count(statement) == 30)
        job.deleted_at = column_text(statement, 29);
    return job;
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
        error = "SQLite metadata schema is unsupported; run the destructive pre-beta reset";
        return false;
    }

    if (schema_version == kSchemaVersion) {
        sqlite3_stmt* marker = nullptr;
        const bool valid = sqlite3_prepare_v2(
                database_,
                "SELECT contract FROM metadata_schema WHERE singleton=1;",
                -1, &marker, nullptr) == SQLITE_OK &&
            sqlite3_step(marker) == SQLITE_ROW &&
            column_text(marker, 0) == "synsigra_saas_metadata_v2";
        sqlite3_finalize(marker);
        if (!valid) {
            error = "SQLite schema marker is invalid; run the destructive pre-beta reset";
            return false;
        }
        initialized_ = true;
        return true;
    }

    sqlite3_stmt* tables = nullptr;
    int table_count = -1;
    if (sqlite3_prepare_v2(
            database_,
            "SELECT count(*) FROM sqlite_master WHERE type='table' "
            "AND name NOT LIKE 'sqlite_%';", -1, &tables, nullptr) == SQLITE_OK &&
        sqlite3_step(tables) == SQLITE_ROW) {
        table_count = sqlite3_column_int(tables, 0);
    }
    sqlite3_finalize(tables);
    if (table_count != 0) {
        error = "Unversioned SQLite database is not empty; run the destructive pre-beta reset";
        return false;
    }
    if (!execute("BEGIN IMMEDIATE;", error)) return false;
    const bool succeeded = execute(kSchemaSql, error) &&
        execute("PRAGMA user_version = 2;", error);
    if (!succeeded || !execute("COMMIT;", error)) {
        std::string rollback_error;
        execute("ROLLBACK;", rollback_error);
        return false;
    }
    initialized_ = true;
    return true;
}

AccountCreateStatus MetadataStore::create_account(
    const std::string& email,
    const std::string& display_name,
    const std::string& password_salt,
    const std::string& password_hash,
    const std::string& terms_version,
    AccountRecord& account,
    std::string& error
) {
    std::string user_id;
    std::string organization_id;
    std::string browser_key_id;
    std::string browser_secret;
    std::string browser_hash;
    if (!random_id("user_", user_id, error) ||
        !random_id("org_", organization_id, error) ||
        !random_id("key_browser_", browser_key_id, error) ||
        !random_id("internal_", browser_secret, error) ||
        !sha256_hex(browser_secret, browser_hash, error) ||
        !initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return AccountCreateStatus::storage_error;
    }

    const char* statements[] = {
        "INSERT INTO organizations (id,display_name) VALUES (?1,?2);",
        "INSERT INTO users "
        "(id,email,display_name,password_salt,password_hash,"
        "email_verified,email_verified_at) "
        "VALUES (?1,?2,?3,?4,?5,0,NULL);",
        "INSERT INTO legal_acceptances "
        "(user_id,document_type,document_version) "
        "VALUES (?1,'private_beta_terms',?2);",
        "INSERT INTO organization_memberships "
        "(organization_id,user_id,role) VALUES (?1,?2,'owner');",
        "INSERT INTO projects "
        "(id,organization_id,display_name) VALUES (?1 || '_default',?1,'Default');",
        "INSERT INTO api_keys "
        "(id,organization_id,user_id,key_hash,label,kind) "
        "VALUES (?1,?2,?3,?4,'Browser sessions','browser');"
    };
    bool succeeded = true;
    bool duplicate_email = false;
    for (int index = 0; index < 6 && succeeded; ++index) {
        sqlite3_stmt* statement = nullptr;
        succeeded = sqlite3_prepare_v2(
            database_, statements[index], -1, &statement, nullptr
        ) == SQLITE_OK;
        if (succeeded && index == 0) {
            succeeded = bind_text(statement, 1, organization_id) &&
                bind_text(statement, 2, display_name + " workspace");
        } else if (succeeded && index == 1) {
            succeeded = bind_text(statement, 1, user_id) &&
                bind_text(statement, 2, email) &&
                bind_text(statement, 3, display_name) &&
                bind_text(statement, 4, password_salt) &&
                bind_text(statement, 5, password_hash);
        } else if (succeeded && index == 2) {
            succeeded = bind_text(statement, 1, user_id) &&
                bind_text(statement, 2, terms_version);
        } else if (succeeded && index == 3) {
            succeeded = bind_text(statement, 1, organization_id) &&
                bind_text(statement, 2, user_id);
        } else if (succeeded && index == 4) {
            succeeded = bind_text(statement, 1, organization_id);
        } else if (succeeded) {
            succeeded = bind_text(statement, 1, browser_key_id) &&
                bind_text(statement, 2, organization_id) &&
                bind_text(statement, 3, user_id) &&
                bind_text(statement, 4, browser_hash);
        }
        if (succeeded) {
            const int result = sqlite3_step(statement);
            succeeded = result == SQLITE_DONE;
            duplicate_email = index == 1 && result == SQLITE_CONSTRAINT;
        }
        sqlite3_finalize(statement);
    }
    if (!succeeded || !execute("COMMIT;", error)) {
        if (!succeeded) error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return duplicate_email
            ? AccountCreateStatus::email_exists
            : AccountCreateStatus::storage_error;
    }
    account.user_id = user_id;
    account.organization_id = organization_id;
    account.email = email;
    account.display_name = display_name;
    account.password_salt = password_salt;
    account.password_hash = password_hash;
    account.role = "owner";
    account.email_verified = false;
    account.email_verified_at.clear();
    return AccountCreateStatus::created;
}

RecordLookupStatus MetadataStore::find_account_by_email(
    const std::string& email,
    AccountRecord& account,
    std::string& error
) {
    if (!initialize(error)) return RecordLookupStatus::storage_error;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT u.id,m.organization_id,u.email,u.display_name,"
        "u.password_salt,u.password_hash,m.role,u.email_verified,"
        "COALESCE(u.email_verified_at,'') "
        "FROM users u JOIN organization_memberships m ON m.user_id=u.id "
        "WHERE u.email=?1 LIMIT 1;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, email)) {
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
    account.user_id = column_text(statement, 0);
    account.organization_id = column_text(statement, 1);
    account.email = column_text(statement, 2);
    account.display_name = column_text(statement, 3);
    account.password_salt = column_text(statement, 4);
    account.password_hash = column_text(statement, 5);
    account.role = column_text(statement, 6);
    account.email_verified = sqlite3_column_int(statement, 7) == 1;
    account.email_verified_at = column_text(statement, 8);
    sqlite3_finalize(statement);
    return RecordLookupStatus::found;
}

RecordLookupStatus MetadataStore::find_account(
    const ApiKeyIdentity& identity,
    AccountRecord& account,
    std::string& error
) {
    if (!initialize(error)) return RecordLookupStatus::storage_error;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT u.id,m.organization_id,u.email,u.display_name,"
        "u.password_salt,u.password_hash,m.role,u.email_verified,"
        "COALESCE(u.email_verified_at,'') "
        "FROM users u JOIN organization_memberships m ON m.user_id=u.id "
        "WHERE u.id=?1 AND m.organization_id=?2;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, identity.user_id) ||
        !bind_text(statement, 2, identity.organization_id)) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return RecordLookupStatus::storage_error;
    }
    const int step = sqlite3_step(statement);
    if (step == SQLITE_DONE) {
        sqlite3_finalize(statement);
        return RecordLookupStatus::not_found;
    }
    if (step != SQLITE_ROW) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return RecordLookupStatus::storage_error;
    }
    account.user_id = column_text(statement, 0);
    account.organization_id = column_text(statement, 1);
    account.email = column_text(statement, 2);
    account.display_name = column_text(statement, 3);
    account.password_salt = column_text(statement, 4);
    account.password_hash = column_text(statement, 5);
    account.role = column_text(statement, 6);
    account.email_verified = sqlite3_column_int(statement, 7) == 1;
    account.email_verified_at = column_text(statement, 8);
    sqlite3_finalize(statement);
    return RecordLookupStatus::found;
}

RecordLookupStatus MetadataStore::update_account_display_name(
    const ApiKeyIdentity& identity,
    const std::string& display_name,
    AccountRecord& account,
    std::string& error
) {
    if (display_name.empty() || display_name.size() > 100u ||
        !initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        if (error.empty()) error = "display name is invalid";
        return RecordLookupStatus::storage_error;
    }
    bool succeeded = execute_bound(
        database_,
        "UPDATE users SET display_name=?3 WHERE id=?1 AND EXISTS ("
        "SELECT 1 FROM organization_memberships WHERE user_id=?1 "
        "AND organization_id=?2);",
        std::vector<std::string>{
            identity.user_id, identity.organization_id, display_name}, error);
    const bool found = succeeded && sqlite3_changes(database_) == 1;
    if (succeeded && found) {
        succeeded = execute_bound(
            database_,
            "INSERT INTO audit_events (organization_id,user_id,api_key_id,"
            "event_type,subject_type,subject_id) "
            "VALUES (?1,?2,?3,'account.profile_updated','account',?2);",
            std::vector<std::string>{identity.organization_id,
                identity.user_id, identity.api_key_id}, error);
    }
    if (!succeeded || !found || !execute("COMMIT;", error)) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return succeeded && !found
            ? RecordLookupStatus::not_found
            : RecordLookupStatus::storage_error;
    }
    return find_account(identity, account, error);
}

bool MetadataStore::change_account_password(
    const ApiKeyIdentity& identity,
    const std::string& password_salt,
    const std::string& password_hash,
    std::string& error
) {
    if (password_salt.empty() || password_hash.empty() ||
        !initialize(error) || !execute("BEGIN IMMEDIATE;", error)) return false;
    bool succeeded = execute_bound(
        database_,
        "UPDATE users SET password_salt=?3,password_hash=?4 WHERE id=?1 "
        "AND EXISTS (SELECT 1 FROM organization_memberships WHERE user_id=?1 "
        "AND organization_id=?2);",
        std::vector<std::string>{identity.user_id, identity.organization_id,
            password_salt, password_hash}, error) &&
        sqlite3_changes(database_) == 1;
    succeeded = succeeded && execute_bound(
        database_,
        "DELETE FROM sessions WHERE api_key_id IN (SELECT id FROM api_keys "
        "WHERE user_id=?1 AND organization_id=?2 AND kind='browser');",
        std::vector<std::string>{identity.user_id, identity.organization_id}, error);
    succeeded = succeeded && execute_bound(
        database_,
        "INSERT INTO audit_events (organization_id,user_id,api_key_id,"
        "event_type,subject_type,subject_id) "
        "VALUES (?1,?2,?3,'account.password_changed','account',?2);",
        std::vector<std::string>{identity.organization_id,
            identity.user_id, identity.api_key_id}, error);
    if (!succeeded || !execute("COMMIT;", error)) {
        if (error.empty()) error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    return true;
}

bool MetadataStore::list_legal_acceptances(
    const ApiKeyIdentity& identity,
    std::vector<LegalAcceptanceRecord>& acceptances,
    std::string& error
) {
    acceptances.clear();
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT document_type,document_version,accepted_at "
        "FROM legal_acceptances WHERE user_id=?1 AND EXISTS (SELECT 1 FROM "
        "organization_memberships WHERE user_id=?1 AND organization_id=?2) "
        "ORDER BY accepted_at,document_type,document_version;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, identity.user_id) ||
        !bind_text(statement, 2, identity.organization_id)) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return false;
    }
    for (;;) {
        const int step = sqlite3_step(statement);
        if (step == SQLITE_DONE) {
            sqlite3_finalize(statement);
            return true;
        }
        if (step != SQLITE_ROW) {
            error = sqlite3_errmsg(database_);
            sqlite3_finalize(statement);
            return false;
        }
        LegalAcceptanceRecord acceptance;
        acceptance.document_type = column_text(statement, 0);
        acceptance.document_version = column_text(statement, 1);
        acceptance.accepted_at = column_text(statement, 2);
        acceptances.push_back(acceptance);
    }
}

AccountDeleteStatus MetadataStore::delete_owned_account(
    const ApiKeyIdentity& identity,
    const std::string& verification_email_hash,
    const std::string& reset_email_hash,
    const std::string& deletion_receipt_id,
    AccountDeletionResult& result,
    std::string& error
) {
    result = AccountDeletionResult();
    if (!is_sha256_hex(verification_email_hash) ||
        !is_sha256_hex(reset_email_hash) || deletion_receipt_id.empty() ||
        deletion_receipt_id.size() > 200u || !initialize(error) ||
        !execute("BEGIN IMMEDIATE;", error)) {
        if (error.empty()) error = "account deletion input is invalid";
        return AccountDeleteStatus::storage_error;
    }
    sqlite3_stmt* membership = nullptr;
    const char* membership_sql =
        "SELECT role,(SELECT count(*) FROM organization_memberships "
        "WHERE organization_id=?1) FROM organization_memberships "
        "WHERE organization_id=?1 AND user_id=?2;";
    if (sqlite3_prepare_v2(database_, membership_sql, -1, &membership, nullptr) !=
            SQLITE_OK ||
        !bind_text(membership, 1, identity.organization_id) ||
        !bind_text(membership, 2, identity.user_id)) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(membership);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return AccountDeleteStatus::storage_error;
    }
    const int membership_step = sqlite3_step(membership);
    if (membership_step == SQLITE_DONE) {
        sqlite3_finalize(membership);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return AccountDeleteStatus::not_found;
    }
    if (membership_step != SQLITE_ROW) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(membership);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return AccountDeleteStatus::storage_error;
    }
    const std::string role = column_text(membership, 0);
    const int member_count = sqlite3_column_int(membership, 1);
    sqlite3_finalize(membership);
    if (role != "owner") {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return AccountDeleteStatus::forbidden;
    }
    if (member_count != 1) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return AccountDeleteStatus::shared_workspace;
    }

    sqlite3_stmt* active = nullptr;
    bool query_ok = sqlite3_prepare_v2(
            database_,
            "SELECT count(*) FROM jobs WHERE organization_id=?1 "
            "AND status='running' AND deleted_at IS NULL;",
            -1, &active, nullptr) == SQLITE_OK &&
        bind_text(active, 1, identity.organization_id) &&
        sqlite3_step(active) == SQLITE_ROW;
    const int running = query_ok ? sqlite3_column_int(active, 0) : -1;
    sqlite3_finalize(active);
    if (!query_ok) {
        error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return AccountDeleteStatus::storage_error;
    }
    if (running != 0) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return AccountDeleteStatus::running_jobs;
    }

    const auto collect_ids = [this, &error](
        const char* sql, const std::string& value,
        std::vector<std::string>& ids) -> bool {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) !=
                SQLITE_OK || !bind_text(statement, 1, value)) {
            error = sqlite3_errmsg(database_);
            sqlite3_finalize(statement);
            return false;
        }
        for (;;) {
            const int step = sqlite3_step(statement);
            if (step == SQLITE_DONE) {
                sqlite3_finalize(statement);
                return true;
            }
            if (step != SQLITE_ROW) {
                error = sqlite3_errmsg(database_);
                sqlite3_finalize(statement);
                return false;
            }
            ids.push_back(column_text(statement, 0));
        }
    };
    bool succeeded = collect_ids(
        "SELECT id FROM packages WHERE organization_id=?1;",
        identity.organization_id, result.package_ids) &&
        collect_ids("SELECT id FROM jobs WHERE organization_id=?1;",
            identity.organization_id, result.job_ids) &&
        collect_ids("SELECT id FROM custom_packs WHERE organization_id=?1;",
            identity.organization_id, result.custom_pack_ids);

    const char* statements[] = {
        "DELETE FROM email_token_submissions WHERE token_hash IN (SELECT "
            "token_hash FROM email_tokens WHERE user_id=?1);",
        "DELETE FROM sessions WHERE api_key_id IN (SELECT id FROM api_keys "
            "WHERE organization_id=?1);",
        "DELETE FROM quota_decisions WHERE organization_id=?1;",
        "DELETE FROM audit_events WHERE organization_id=?1;",
        "DELETE FROM packages WHERE organization_id=?1;",
        "DELETE FROM jobs WHERE organization_id=?1;",
        "DELETE FROM custom_packs WHERE organization_id=?1;",
        "DELETE FROM scenario_drafts WHERE organization_id=?1;",
        "DELETE FROM projects WHERE organization_id=?1;",
        "DELETE FROM email_tokens WHERE user_id=?1;",
        "DELETE FROM legal_acceptances WHERE user_id=?1;",
        "DELETE FROM organization_memberships WHERE organization_id=?1;",
        "DELETE FROM api_keys WHERE organization_id=?1;",
        "DELETE FROM organizations WHERE id=?1;",
        "DELETE FROM users WHERE id=?1;"
    };
    for (int index = 0; succeeded && index < 15; ++index) {
        const std::string value =
            (index == 0 || index == 9 || index == 10 || index == 14)
            ? identity.user_id : identity.organization_id;
        succeeded = execute_bound(
            database_, statements[index], std::vector<std::string>{value}, error);
    }
    succeeded = succeeded && execute_bound(
        database_,
        "DELETE FROM email_send_attempts WHERE email_hash IN (?1,?2);",
        std::vector<std::string>{verification_email_hash, reset_email_hash}, error);
    std::ostringstream details;
    details << "{\"custom_packs\":" << result.custom_pack_ids.size()
            << ",\"jobs\":" << result.job_ids.size()
            << ",\"packages\":" << result.package_ids.size()
            << ",\"retained\":\"anonymous_deletion_receipt_only\"}";
    succeeded = succeeded && execute_bound(
        database_,
        "INSERT INTO audit_events (event_type,subject_type,subject_id,details_json) "
        "VALUES ('account.deleted','deletion_receipt',?1,?2);",
        std::vector<std::string>{deletion_receipt_id, details.str()}, error);
    if (!succeeded || !execute("COMMIT;", error)) {
        if (error.empty()) error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        result = AccountDeletionResult();
        return AccountDeleteStatus::storage_error;
    }
    return AccountDeleteStatus::deleted;
}

bool MetadataStore::create_session(
    const AccountRecord& account,
    const std::string& session_id,
    const std::string& token_hash,
    std::string& error
) {
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT INTO sessions (id,token_hash,api_key_id) "
        "SELECT ?1,?2,k.id FROM api_keys k "
        "WHERE k.user_id=?3 AND k.organization_id=?4 "
        "AND k.kind='browser' AND k.active=1;";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, session_id) &&
        bind_text(statement, 2, token_hash) &&
        bind_text(statement, 3, account.user_id) &&
        bind_text(statement, 4, account.organization_id) &&
        sqlite3_step(statement) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    if (!succeeded) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return succeeded;
}

RecordLookupStatus MetadataStore::find_session(
    const std::string& token_hash,
    ApiKeyIdentity& identity,
    AccountRecord& account,
    std::string& error
) {
    if (!initialize(error)) return RecordLookupStatus::storage_error;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT k.id,k.organization_id,k.user_id,m.role,u.email,u.display_name,"
        "u.email_verified,COALESCE(u.email_verified_at,'') "
        "FROM sessions s JOIN api_keys k ON k.id=s.api_key_id "
        "JOIN organization_memberships m ON m.organization_id=k.organization_id "
        "AND m.user_id=k.user_id JOIN users u ON u.id=k.user_id "
        "WHERE s.token_hash=?1 AND s.expires_at > "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now') AND k.active=1;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, token_hash)) {
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
    identity.api_key_id = column_text(statement, 0);
    identity.organization_id = column_text(statement, 1);
    identity.user_id = column_text(statement, 2);
    identity.role = column_text(statement, 3);
    account.organization_id = identity.organization_id;
    account.user_id = identity.user_id;
    account.role = identity.role;
    account.email = column_text(statement, 4);
    account.display_name = column_text(statement, 5);
    account.email_verified = sqlite3_column_int(statement, 6) == 1;
    account.email_verified_at = column_text(statement, 7);
    sqlite3_finalize(statement);
    return RecordLookupStatus::found;
}

bool MetadataStore::delete_session(
    const std::string& token_hash,
    std::string& error
) {
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const bool succeeded = sqlite3_prepare_v2(
            database_, "DELETE FROM sessions WHERE token_hash=?1;",
            -1, &statement, nullptr
        ) == SQLITE_OK &&
        bind_text(statement, 1, token_hash) &&
        sqlite3_step(statement) == SQLITE_DONE;
    if (!succeeded) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return succeeded;
}

bool MetadataStore::delete_sessions_for_user(
    const std::string& user_id,
    std::string& error
) {
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "DELETE FROM sessions WHERE api_key_id IN "
        "(SELECT id FROM api_keys WHERE user_id=?1 AND kind='browser');";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, user_id) &&
        sqlite3_step(statement) == SQLITE_DONE;
    if (!succeeded) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return succeeded;
}

RateLimitStatus MetadataStore::record_email_send_attempt(
    const std::string& purpose,
    const std::string& email_hash,
    int max_attempts,
    int window_minutes,
    std::string& error
) {
    if (!is_sha256_hex(email_hash) || max_attempts < 1 ||
        window_minutes < 1 ||
        (purpose != "email_verification" && purpose != "password_reset")) {
        error = "invalid email send rate-limit input";
        return RateLimitStatus::storage_error;
    }
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return RateLimitStatus::storage_error;
    }
    std::ostringstream modifier;
    modifier << "-" << window_minutes << " minutes";
    sqlite3_stmt* statement = nullptr;
    const char* count_sql =
        "SELECT COUNT(*) FROM email_send_attempts "
        "WHERE purpose=?1 AND email_hash=?2 AND created_at >= "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now',?3);";
    bool succeeded =
        sqlite3_prepare_v2(database_, count_sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, purpose) &&
        bind_text(statement, 2, email_hash) &&
        bind_text(statement, 3, modifier.str()) &&
        sqlite3_step(statement) == SQLITE_ROW;
    const int count = succeeded ? sqlite3_column_int(statement, 0) : 0;
    sqlite3_finalize(statement);
    if (succeeded && count < max_attempts) {
        const char* insert_sql =
            "INSERT INTO email_send_attempts (purpose,email_hash) VALUES (?1,?2);";
        succeeded =
            sqlite3_prepare_v2(database_, insert_sql, -1, &statement, nullptr) == SQLITE_OK &&
            bind_text(statement, 1, purpose) && bind_text(statement, 2, email_hash) &&
            sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
    }
    if (!succeeded || !execute("COMMIT;", error)) {
        if (!succeeded) error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return RateLimitStatus::storage_error;
    }
    return count >= max_attempts
        ? RateLimitStatus::limited
        : RateLimitStatus::allowed;
}

RateLimitStatus MetadataStore::record_email_token_submission(
    const std::string& purpose,
    const std::string& token_hash,
    int max_attempts,
    int window_minutes,
    std::string& error
) {
    if (!is_sha256_hex(token_hash) || max_attempts < 1 ||
        window_minutes < 1 ||
        (purpose != "email_verification" && purpose != "password_reset")) {
        error = "invalid email token rate-limit input";
        return RateLimitStatus::storage_error;
    }
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return RateLimitStatus::storage_error;
    }
    std::ostringstream modifier;
    modifier << "-" << window_minutes << " minutes";
    sqlite3_stmt* statement = nullptr;
    const char* count_sql =
        "SELECT COUNT(*) FROM email_token_submissions "
        "WHERE purpose=?1 AND token_hash=?2 AND created_at >= "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now',?3);";
    bool succeeded =
        sqlite3_prepare_v2(database_, count_sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, purpose) && bind_text(statement, 2, token_hash) &&
        bind_text(statement, 3, modifier.str()) &&
        sqlite3_step(statement) == SQLITE_ROW;
    const int count = succeeded ? sqlite3_column_int(statement, 0) : 0;
    sqlite3_finalize(statement);
    if (succeeded && count < max_attempts) {
        const char* insert_sql =
            "INSERT INTO email_token_submissions (purpose,token_hash) VALUES (?1,?2);";
        succeeded =
            sqlite3_prepare_v2(database_, insert_sql, -1, &statement, nullptr) == SQLITE_OK &&
            bind_text(statement, 1, purpose) && bind_text(statement, 2, token_hash) &&
            sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
    }
    if (!succeeded || !execute("COMMIT;", error)) {
        if (!succeeded) error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return RateLimitStatus::storage_error;
    }
    return count >= max_attempts
        ? RateLimitStatus::limited
        : RateLimitStatus::allowed;
}

EmailTokenCreateStatus MetadataStore::create_email_token(
    const std::string& user_id,
    const std::string& purpose,
    const std::string& token_hash,
    const std::string& recipient_email,
    int ttl_minutes,
    std::string& error
) {
    if (user_id.empty() || recipient_email.empty() || !is_sha256_hex(token_hash) ||
        ttl_minutes < 5 || ttl_minutes > 10080 ||
        (purpose != "email_verification" && purpose != "password_reset")) {
        error = "invalid email token input";
        return EmailTokenCreateStatus::storage_error;
    }
    std::string token_id;
    if (!random_id("email_token_", token_id, error) ||
        !initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return EmailTokenCreateStatus::storage_error;
    }
    sqlite3_stmt* statement = nullptr;
    const char* count_sql =
        "SELECT COUNT(*) FROM email_tokens WHERE user_id=?1 AND purpose=?2 "
        "AND created_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now','-1 hour');";
    bool succeeded =
        sqlite3_prepare_v2(database_, count_sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, user_id) && bind_text(statement, 2, purpose) &&
        sqlite3_step(statement) == SQLITE_ROW;
    const int recent_count = succeeded ? sqlite3_column_int(statement, 0) : 0;
    sqlite3_finalize(statement);
    if (!succeeded) {
        error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return EmailTokenCreateStatus::storage_error;
    }
    if (recent_count >= 3) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return EmailTokenCreateStatus::rate_limited;
    }
    const char* invalidate_sql =
        "UPDATE email_tokens SET consumed_at=COALESCE(consumed_at,"
        "strftime('%Y-%m-%dT%H:%M:%fZ','now')) "
        "WHERE user_id=?1 AND purpose=?2 AND consumed_at IS NULL;";
    succeeded =
        sqlite3_prepare_v2(database_, invalidate_sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, user_id) && bind_text(statement, 2, purpose) &&
        sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    std::ostringstream expiry;
    expiry << "+" << ttl_minutes << " minutes";
    if (succeeded) {
        const char* insert_sql =
            "INSERT INTO email_tokens "
            "(id,user_id,purpose,token_hash,recipient_email,expires_at) "
            "VALUES (?1,?2,?3,?4,?5,"
            "strftime('%Y-%m-%dT%H:%M:%fZ','now',?6));";
        succeeded =
            sqlite3_prepare_v2(database_, insert_sql, -1, &statement, nullptr) == SQLITE_OK &&
            bind_text(statement, 1, token_id) && bind_text(statement, 2, user_id) &&
            bind_text(statement, 3, purpose) && bind_text(statement, 4, token_hash) &&
            bind_text(statement, 5, recipient_email) && bind_text(statement, 6, expiry.str()) &&
            sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
    }
    if (!succeeded || !execute("COMMIT;", error)) {
        if (!succeeded) error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return EmailTokenCreateStatus::storage_error;
    }
    return EmailTokenCreateStatus::created;
}

EmailTokenConsumeStatus MetadataStore::verify_email_token(
    const std::string& token_hash,
    AccountRecord& account,
    std::string& error
) {
    if (!is_sha256_hex(token_hash)) return EmailTokenConsumeStatus::invalid_or_expired;
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return EmailTokenConsumeStatus::storage_error;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT t.id,u.id,m.organization_id,u.email,u.display_name,"
        "u.password_salt,u.password_hash,m.role "
        "FROM email_tokens t JOIN users u ON u.id=t.user_id "
        "JOIN organization_memberships m ON m.user_id=u.id "
        "WHERE t.token_hash=?1 AND t.purpose='email_verification' "
        "AND t.consumed_at IS NULL AND t.expires_at > "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now') LIMIT 1;";
    bool prepared = sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, token_hash);
    const int result = prepared ? sqlite3_step(statement) : SQLITE_ERROR;
    if (!prepared || (result != SQLITE_ROW && result != SQLITE_DONE)) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return EmailTokenConsumeStatus::storage_error;
    }
    if (result == SQLITE_DONE) {
        sqlite3_finalize(statement);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return EmailTokenConsumeStatus::invalid_or_expired;
    }
    const std::string token_id = column_text(statement, 0);
    account.user_id = column_text(statement, 1);
    account.organization_id = column_text(statement, 2);
    account.email = column_text(statement, 3);
    account.display_name = column_text(statement, 4);
    account.password_salt = column_text(statement, 5);
    account.password_hash = column_text(statement, 6);
    account.role = column_text(statement, 7);
    sqlite3_finalize(statement);
    const char* update_token =
        "UPDATE email_tokens SET consumed_at="
        "strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE id=?1 AND consumed_at IS NULL;";
    bool succeeded =
        sqlite3_prepare_v2(database_, update_token, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, token_id) && sqlite3_step(statement) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    sqlite3_finalize(statement);
    if (succeeded) {
        const char* update_user =
            "UPDATE users SET email_verified=1,email_verified_at="
            "COALESCE(email_verified_at,strftime('%Y-%m-%dT%H:%M:%fZ','now')) "
            "WHERE id=?1;";
        succeeded =
            sqlite3_prepare_v2(database_, update_user, -1, &statement, nullptr) == SQLITE_OK &&
            bind_text(statement, 1, account.user_id) &&
            sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(database_) == 1;
        sqlite3_finalize(statement);
    }
    if (!succeeded || !execute("COMMIT;", error)) {
        if (!succeeded) error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return EmailTokenConsumeStatus::storage_error;
    }
    account.email_verified = true;
    account.email_verified_at.clear();
    return EmailTokenConsumeStatus::consumed;
}

EmailTokenConsumeStatus MetadataStore::consume_password_reset_token(
    const std::string& token_hash,
    AccountRecord& account,
    std::string& error
) {
    if (!is_sha256_hex(token_hash)) return EmailTokenConsumeStatus::invalid_or_expired;
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return EmailTokenConsumeStatus::storage_error;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT t.id,u.id,m.organization_id,u.email,u.display_name,"
        "u.password_salt,u.password_hash,m.role,COALESCE(u.email_verified_at,'') "
        "FROM email_tokens t JOIN users u ON u.id=t.user_id "
        "JOIN organization_memberships m ON m.user_id=u.id "
        "WHERE t.token_hash=?1 AND t.purpose='password_reset' "
        "AND u.email_verified=1 AND t.consumed_at IS NULL AND t.expires_at > "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now') LIMIT 1;";
    bool prepared = sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, token_hash);
    const int result = prepared ? sqlite3_step(statement) : SQLITE_ERROR;
    if (!prepared || (result != SQLITE_ROW && result != SQLITE_DONE)) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return EmailTokenConsumeStatus::storage_error;
    }
    if (result == SQLITE_DONE) {
        sqlite3_finalize(statement);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return EmailTokenConsumeStatus::invalid_or_expired;
    }
    const std::string token_id = column_text(statement, 0);
    account.user_id = column_text(statement, 1);
    account.organization_id = column_text(statement, 2);
    account.email = column_text(statement, 3);
    account.display_name = column_text(statement, 4);
    account.password_salt = column_text(statement, 5);
    account.password_hash = column_text(statement, 6);
    account.role = column_text(statement, 7);
    account.email_verified = true;
    account.email_verified_at = column_text(statement, 8);
    sqlite3_finalize(statement);
    const char* update_sql =
        "UPDATE email_tokens SET consumed_at="
        "strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE id=?1 AND consumed_at IS NULL;";
    const bool succeeded =
        sqlite3_prepare_v2(database_, update_sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, token_id) && sqlite3_step(statement) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    sqlite3_finalize(statement);
    if (!succeeded || !execute("COMMIT;", error)) {
        if (!succeeded) error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return EmailTokenConsumeStatus::storage_error;
    }
    return EmailTokenConsumeStatus::consumed;
}

bool MetadataStore::update_account_password(
    const std::string& user_id,
    const std::string& password_salt,
    const std::string& password_hash,
    std::string& error
) {
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "UPDATE users SET password_salt=?2,password_hash=?3 WHERE id=?1;";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, user_id) && bind_text(statement, 2, password_salt) &&
        bind_text(statement, 3, password_hash) &&
        sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(database_) == 1;
    if (!succeeded) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return succeeded;
}

bool MetadataStore::create_personal_api_key(
    const ApiKeyIdentity& owner,
    const std::string& api_key_id,
    const std::string& key_hash,
    const std::string& label,
    std::string& error
) {
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT INTO api_keys "
        "(id,organization_id,user_id,key_hash,label,kind) "
        "VALUES (?1,?2,?3,?4,?5,'personal');";
    bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, api_key_id) &&
        bind_text(statement, 2, owner.organization_id) &&
        bind_text(statement, 3, owner.user_id) &&
        bind_text(statement, 4, key_hash) &&
        bind_text(statement, 5, label) &&
        sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);

    sqlite3_stmt* audit = nullptr;
    const char* audit_sql =
        "INSERT INTO audit_events "
        "(organization_id,user_id,api_key_id,event_type,subject_type,subject_id) "
        "VALUES (?1,?2,?3,'api_key.created','api_key',?4);";
    succeeded = succeeded &&
        sqlite3_prepare_v2(database_, audit_sql, -1, &audit, nullptr) == SQLITE_OK &&
        bind_text(audit, 1, owner.organization_id) &&
        bind_text(audit, 2, owner.user_id) &&
        bind_text(audit, 3, owner.api_key_id) &&
        bind_text(audit, 4, api_key_id) &&
        sqlite3_step(audit) == SQLITE_DONE;
    sqlite3_finalize(audit);

    if (!succeeded || !execute("COMMIT;", error)) {
        if (error.empty()) error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    return true;
}

bool MetadataStore::list_personal_api_keys(
    const ApiKeyIdentity& owner,
    std::vector<ApiKeyRecord>& api_keys,
    std::string& error
) {
    api_keys.clear();
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT k.id,k.organization_id,k.user_id,m.role,k.label,k.active,"
        "k.created_at,coalesce(k.last_used_at,'') FROM api_keys k "
        "JOIN organization_memberships m ON m.organization_id=k.organization_id "
        "AND m.user_id=k.user_id WHERE k.organization_id=?1 AND k.user_id=?2 "
        "AND k.kind='personal' ORDER BY k.created_at DESC;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, owner.organization_id) ||
        !bind_text(statement, 2, owner.user_id)) {
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
        ApiKeyRecord key;
        key.api_key_id = column_text(statement, 0);
        key.organization_id = column_text(statement, 1);
        key.user_id = column_text(statement, 2);
        key.role = column_text(statement, 3);
        key.label = column_text(statement, 4);
        key.active = sqlite3_column_int(statement, 5) == 1;
        key.created_at = column_text(statement, 6);
        key.last_used_at = column_text(statement, 7);
        api_keys.push_back(key);
    }
}

RecordLookupStatus MetadataStore::revoke_personal_api_key(
    const ApiKeyIdentity& owner,
    const std::string& api_key_id,
    std::string& error
) {
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return RecordLookupStatus::storage_error;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "UPDATE api_keys SET active=0 WHERE id=?1 AND organization_id=?2 "
        "AND user_id=?3 AND kind='personal' AND active=1;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, api_key_id) ||
        !bind_text(statement, 2, owner.organization_id) ||
        !bind_text(statement, 3, owner.user_id) ||
        sqlite3_step(statement) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return RecordLookupStatus::storage_error;
    }
    const bool changed = sqlite3_changes(database_) == 1;
    sqlite3_finalize(statement);
    if (!changed) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return RecordLookupStatus::not_found;
    }

    sqlite3_stmt* audit = nullptr;
    const char* audit_sql =
        "INSERT INTO audit_events "
        "(organization_id,user_id,api_key_id,event_type,subject_type,subject_id) "
        "VALUES (?1,?2,?3,'api_key.revoked','api_key',?4);";
    const bool audited =
        sqlite3_prepare_v2(database_, audit_sql, -1, &audit, nullptr) == SQLITE_OK &&
        bind_text(audit, 1, owner.organization_id) &&
        bind_text(audit, 2, owner.user_id) &&
        bind_text(audit, 3, owner.api_key_id) &&
        bind_text(audit, 4, api_key_id) &&
        sqlite3_step(audit) == SQLITE_DONE;
    sqlite3_finalize(audit);
    if (!audited || !execute("COMMIT;", error)) {
        if (error.empty()) error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return RecordLookupStatus::storage_error;
    }
    return RecordLookupStatus::found;
}

RecordLookupStatus MetadataStore::rotate_personal_api_key(
    const ApiKeyIdentity& owner,
    const std::string& api_key_id,
    const std::string& replacement_api_key_id,
    const std::string& replacement_key_hash,
    ApiKeyRecord& replacement,
    std::string& error
) {
    if (api_key_id.empty() || replacement_api_key_id.empty() ||
        !is_sha256_hex(replacement_key_hash)) {
        error = "API key rotation input is invalid";
        return RecordLookupStatus::storage_error;
    }
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) {
        return RecordLookupStatus::storage_error;
    }

    sqlite3_stmt* select = nullptr;
    const char* select_sql =
        "SELECT label FROM api_keys WHERE id=?1 AND organization_id=?2 "
        "AND user_id=?3 AND kind='personal' AND active=1;";
    bool succeeded =
        sqlite3_prepare_v2(database_, select_sql, -1, &select, nullptr) == SQLITE_OK &&
        bind_text(select, 1, api_key_id) &&
        bind_text(select, 2, owner.organization_id) &&
        bind_text(select, 3, owner.user_id);
    const int selected = succeeded ? sqlite3_step(select) : SQLITE_ERROR;
    const std::string label = selected == SQLITE_ROW
        ? column_text(select, 0) : std::string();
    sqlite3_finalize(select);
    if (selected == SQLITE_DONE) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return RecordLookupStatus::not_found;
    }
    if (selected != SQLITE_ROW) succeeded = false;

    sqlite3_stmt* insert = nullptr;
    const char* insert_sql =
        "INSERT INTO api_keys "
        "(id,organization_id,user_id,key_hash,label,kind) "
        "VALUES (?1,?2,?3,?4,?5,'personal');";
    succeeded = succeeded &&
        sqlite3_prepare_v2(database_, insert_sql, -1, &insert, nullptr) == SQLITE_OK &&
        bind_text(insert, 1, replacement_api_key_id) &&
        bind_text(insert, 2, owner.organization_id) &&
        bind_text(insert, 3, owner.user_id) &&
        bind_text(insert, 4, replacement_key_hash) &&
        bind_text(insert, 5, label) &&
        sqlite3_step(insert) == SQLITE_DONE;
    sqlite3_finalize(insert);

    sqlite3_stmt* revoke = nullptr;
    const char* revoke_sql =
        "UPDATE api_keys SET active=0 WHERE id=?1 AND organization_id=?2 "
        "AND user_id=?3 AND kind='personal' AND active=1;";
    succeeded = succeeded &&
        sqlite3_prepare_v2(database_, revoke_sql, -1, &revoke, nullptr) == SQLITE_OK &&
        bind_text(revoke, 1, api_key_id) &&
        bind_text(revoke, 2, owner.organization_id) &&
        bind_text(revoke, 3, owner.user_id) &&
        sqlite3_step(revoke) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    sqlite3_finalize(revoke);

    sqlite3_stmt* audit = nullptr;
    const char* audit_sql =
        "INSERT INTO audit_events "
        "(organization_id,user_id,api_key_id,event_type,subject_type,subject_id,details_json) "
        "VALUES (?1,?2,?3,'api_key.rotated','api_key',?4,?5);";
    const std::string details =
        std::string("{\"replacement_api_key_id\":\"") +
        replacement_api_key_id + "\"}";
    succeeded = succeeded &&
        sqlite3_prepare_v2(database_, audit_sql, -1, &audit, nullptr) == SQLITE_OK &&
        bind_text(audit, 1, owner.organization_id) &&
        bind_text(audit, 2, owner.user_id) &&
        bind_text(audit, 3, owner.api_key_id) &&
        bind_text(audit, 4, api_key_id) &&
        bind_text(audit, 5, details) &&
        sqlite3_step(audit) == SQLITE_DONE;
    sqlite3_finalize(audit);

    if (!succeeded || !execute("COMMIT;", error)) {
        if (error.empty()) error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return RecordLookupStatus::storage_error;
    }
    replacement.api_key_id = replacement_api_key_id;
    replacement.organization_id = owner.organization_id;
    replacement.user_id = owner.user_id;
    replacement.role = owner.role;
    replacement.label = label;
    replacement.active = true;
    return RecordLookupStatus::found;
}

bool MetadataStore::record_audit_event(
    const ApiKeyIdentity& actor,
    const std::string& event_type,
    const std::string& subject_type,
    const std::string& subject_id,
    const std::string& details_json,
    std::string& error
) {
    if (actor.organization_id.empty() || actor.user_id.empty() ||
        actor.api_key_id.empty() || event_type.empty() || event_type.size() > 100u ||
        subject_type.size() > 100u || subject_id.size() > 200u ||
        details_json.empty() || details_json.size() > 4096u) {
        error = "Audit event input is invalid";
        return false;
    }
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT INTO audit_events "
        "(organization_id,user_id,api_key_id,event_type,subject_type,subject_id,details_json) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7);";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, actor.organization_id) &&
        bind_text(statement, 2, actor.user_id) &&
        bind_text(statement, 3, actor.api_key_id) &&
        bind_text(statement, 4, event_type) &&
        bind_text(statement, 5, subject_type) &&
        bind_text(statement, 6, subject_id) &&
        bind_text(statement, 7, details_json) &&
        sqlite3_step(statement) == SQLITE_DONE;
    if (!succeeded) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return succeeded;
}

bool MetadataStore::list_audit_events(
    const ApiKeyIdentity& owner,
    int limit,
    int offset,
    std::vector<AuditEventRecord>& events,
    std::string& error
) {
    events.clear();
    if (owner.organization_id.empty() || limit <= 0 || limit > 1000 || offset < 0) {
        error = "Audit list owner, limit, or offset is invalid";
        return false;
    }
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT id,organization_id,coalesce(user_id,''),coalesce(api_key_id,''),"
        "event_type,coalesce(subject_type,''),coalesce(subject_id,''),details_json,created_at "
        "FROM audit_events WHERE organization_id=?1 "
        "ORDER BY id DESC LIMIT ?2 OFFSET ?3;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
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
        AuditEventRecord event;
        event.audit_event_id = sqlite3_column_int64(statement, 0);
        event.organization_id = column_text(statement, 1);
        event.user_id = column_text(statement, 2);
        event.api_key_id = column_text(statement, 3);
        event.event_type = column_text(statement, 4);
        event.subject_type = column_text(statement, 5);
        event.subject_id = column_text(statement, 6);
        event.details_json = column_text(statement, 7);
        event.created_at = column_text(statement, 8);
        events.push_back(event);
    }
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
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT INTO api_keys "
        "(id,organization_id,user_id,key_hash,label) "
        "SELECT ?1,?2,?3,?4,?5 WHERE EXISTS ("
        "SELECT 1 FROM organization_memberships "
        "WHERE organization_id=?2 AND user_id=?3 AND role=?6);";
    bool succeeded = sqlite3_prepare_v2(
            database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, identity.api_key_id) &&
        bind_text(statement, 2, identity.organization_id) &&
        bind_text(statement, 3, identity.user_id) &&
        bind_text(statement, 4, key_hash) &&
        bind_text(statement, 5, label) &&
        bind_text(statement, 6, role) &&
        sqlite3_step(statement) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    sqlite3_finalize(statement);
    if (!succeeded) {
        error = "API key owner does not exist or does not have the requested role";
        std::string ignored;
        execute("ROLLBACK;", ignored);
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
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    return true;
}

bool MetadataStore::bootstrap_owner(
    const ApiKeyIdentity& identity,
    const std::string& email,
    const std::string& display_name,
    const std::string& key_hash,
    const std::string& label,
    std::string& error
) {
    if (identity.organization_id.empty() || identity.user_id.empty() ||
        identity.api_key_id.empty() || identity.role != "owner" ||
        email.find('@') == std::string::npos || display_name.empty() ||
        label.empty() || !is_sha256_hex(key_hash)) {
        error = "owner bootstrap input is invalid";
        return false;
    }
    if (!initialize(error) || !execute("BEGIN IMMEDIATE;", error)) return false;
    sqlite3_stmt* count = nullptr;
    bool succeeded = sqlite3_prepare_v2(
            database_,
            "SELECT (SELECT count(*) FROM users)+"
            "(SELECT count(*) FROM organizations)+"
            "(SELECT count(*) FROM api_keys);",
            -1, &count, nullptr) == SQLITE_OK &&
        sqlite3_step(count) == SQLITE_ROW && sqlite3_column_int(count, 0) == 0;
    sqlite3_finalize(count);
    const char* sql[] = {
        "INSERT INTO organizations (id,display_name) VALUES (?1,?2);",
        "INSERT INTO users (id,email,display_name,password_salt,password_hash,"
        "email_verified,email_verified_at) VALUES (?1,?2,?3,'','',1,"
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'));",
        "INSERT INTO organization_memberships (organization_id,user_id,role) "
        "VALUES (?1,?2,'owner');",
        "INSERT INTO projects (id,organization_id,display_name) "
        "VALUES (?1 || '_default',?1,'Default');",
        "INSERT INTO api_keys (id,organization_id,user_id,key_hash,label) "
        "VALUES (?1,?2,?3,?4,?5);",
        "INSERT INTO audit_events (organization_id,user_id,api_key_id,event_type,"
        "subject_type,subject_id) VALUES (?1,?2,?3,'operator.bootstrapped',"
        "'api_key',?3);"
    };
    for (int i = 0; i < 6 && succeeded; ++i) {
        sqlite3_stmt* insert = nullptr;
        succeeded = sqlite3_prepare_v2(database_, sql[i], -1, &insert, nullptr) == SQLITE_OK;
        if (succeeded && i == 0)
            succeeded = bind_text(insert, 1, identity.organization_id) &&
                bind_text(insert, 2, display_name);
        else if (succeeded && i == 1)
            succeeded = bind_text(insert, 1, identity.user_id) &&
                bind_text(insert, 2, email) && bind_text(insert, 3, display_name);
        else if (succeeded && i == 2)
            succeeded = bind_text(insert, 1, identity.organization_id) &&
                bind_text(insert, 2, identity.user_id);
        else if (succeeded && i == 3)
            succeeded = bind_text(insert, 1, identity.organization_id);
        else if (succeeded && i == 4)
            succeeded = bind_text(insert, 1, identity.api_key_id) &&
                bind_text(insert, 2, identity.organization_id) &&
                bind_text(insert, 3, identity.user_id) &&
                bind_text(insert, 4, key_hash) && bind_text(insert, 5, label);
        else if (succeeded)
            succeeded = bind_text(insert, 1, identity.organization_id) &&
                bind_text(insert, 2, identity.user_id) &&
                bind_text(insert, 3, identity.api_key_id);
        if (succeeded) succeeded = sqlite3_step(insert) == SQLITE_DONE;
        sqlite3_finalize(insert);
    }
    if (!succeeded || !execute("COMMIT;", error)) {
        if (error.empty()) error = "database is not empty or owner bootstrap failed";
        std::string ignored;
        execute("ROLLBACK;", ignored);
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
        "JOIN users u ON u.id = k.user_id "
        "WHERE k.key_hash = ?1 AND k.active = 1 "
        "AND u.email_verified = 1;";
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
    const std::string& selected_pack_version,
    const std::string& catalog_version,
    const std::string& catalog_source_sha256,
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
        "selected_pack_id, source_pack_path, pack_fingerprint, "
        "selected_pack_version, catalog_version, catalog_source_sha256) "
        "VALUES (?1, ?2, ?3, ?4, 'queued', ?5, ?6, ?7, ?8, ?9, ?10, ?11);";
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
        bind_text(statement, 9, selected_pack_version) &&
        bind_text(statement, 10, catalog_version) &&
        bind_text(statement, 11, catalog_source_sha256) &&
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
        "j.package_fingerprint, j.integration_contract_version, "
        "j.integration_contract_json, j.generator_version, "
        "j.generator_git_commit, j.generator_build_identity, "
        "j.generator_binary_sha256, j.challenge_receipt_json, j.manifest_hash, "
        "j.artifact_storage_key, j.error_code, j.error_message, "
        "j.created_at, j.started_at, j.completed_at, "
        "j.selected_pack_version, j.catalog_version, "
        "j.catalog_source_sha256, j.challenge_metadata_json "
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
    JobRecord loaded = job_columns(statement);
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
        "j.package_fingerprint, j.integration_contract_version, "
        "j.integration_contract_json, j.generator_version, "
        "j.generator_git_commit, j.generator_build_identity, "
        "j.generator_binary_sha256, j.challenge_receipt_json, j.manifest_hash, "
        "j.artifact_storage_key, j.error_code, j.error_message, "
        "j.created_at, j.started_at, j.completed_at, "
        "j.selected_pack_version, j.catalog_version, "
        "j.catalog_source_sha256, j.challenge_metadata_json "
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
        JobRecord loaded = job_columns(statement);
        jobs.push_back(loaded);
    }
}

bool MetadataStore::list_account_jobs(
    const ApiKeyIdentity& owner,
    std::vector<JobRecord>& jobs,
    std::string& error
) {
    jobs.clear();
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT j.id,j.organization_id,j.project_id,j.user_id,j.status,"
        "j.request_json,j.selected_pack_id,j.source_pack_path,"
        "j.pack_fingerprint,COALESCE(p.id,''),j.package_fingerprint,"
        "j.integration_contract_version,j.integration_contract_json,"
        "j.generator_version,j.generator_git_commit,"
        "j.generator_build_identity,j.generator_binary_sha256,"
        "j.challenge_receipt_json,j.manifest_hash,j.artifact_storage_key,"
        "j.error_code,j.error_message,j.created_at,j.started_at,j.completed_at,"
        "j.selected_pack_version,j.catalog_version,j.catalog_source_sha256,"
        "j.challenge_metadata_json,COALESCE(j.deleted_at,'') "
        "FROM jobs j LEFT JOIN packages p "
        "ON p.job_id=j.id WHERE j.organization_id=?1 "
        "ORDER BY j.created_at DESC,j.id DESC;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, owner.organization_id)) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return false;
    }
    for (;;) {
        const int step = sqlite3_step(statement);
        if (step == SQLITE_DONE) {
            sqlite3_finalize(statement);
            return true;
        }
        if (step != SQLITE_ROW) {
            error = sqlite3_errmsg(database_);
            sqlite3_finalize(statement);
            return false;
        }
        jobs.push_back(job_columns(statement));
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
    if (existing.generator_binary_sha256.empty()) {
        if (!create_job(
                owner,
                existing.project_id,
                existing.request_json,
                existing.selected_pack_id,
                existing.source_pack_path,
                existing.pack_fingerprint,
                existing.selected_pack_version,
                existing.catalog_version,
                existing.catalog_source_sha256,
                new_job_id,
                error)) {
            return JobLifecycleStatus::storage_error;
        }
        return JobLifecycleStatus::succeeded;
    }
    if (existing.source_pack_path.empty() ||
        existing.integration_contract_version.empty() ||
        existing.integration_contract_json.empty() ||
        existing.generator_version.empty() ||
        existing.generator_git_commit.empty() ||
        existing.generator_build_identity.empty()) {
        return JobLifecycleStatus::invalid_state;
    }
    if (!random_id("job_", new_job_id, error)) {
        return JobLifecycleStatus::storage_error;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT INTO jobs "
        "(id, organization_id, project_id, user_id, status, request_json, "
        "selected_pack_id, source_pack_path, pack_fingerprint, "
        "package_fingerprint, integration_contract_version, "
        "integration_contract_json, generator_version, generator_git_commit, "
        "generator_build_identity, generator_binary_sha256, "
        "selected_pack_version, catalog_version, catalog_source_sha256) "
        "VALUES (?1,?2,?3,?4,'queued',?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, new_job_id) &&
        bind_text(statement, 2, owner.organization_id) &&
        bind_text(statement, 3, existing.project_id) &&
        bind_text(statement, 4, owner.user_id) &&
        bind_text(statement, 5, existing.request_json) &&
        bind_text(statement, 6, existing.selected_pack_id) &&
        bind_text(statement, 7, existing.source_pack_path) &&
        bind_text(statement, 8, existing.pack_fingerprint) &&
        bind_text(statement, 9, existing.package_fingerprint) &&
        bind_text(statement, 10, existing.integration_contract_version) &&
        bind_text(statement, 11, existing.integration_contract_json) &&
        bind_text(statement, 12, existing.generator_version) &&
        bind_text(statement, 13, existing.generator_git_commit) &&
        bind_text(statement, 14, existing.generator_build_identity) &&
        bind_text(statement, 15, existing.generator_binary_sha256) &&
        bind_text(statement, 16, existing.selected_pack_version) &&
        bind_text(statement, 17, existing.catalog_version) &&
        bind_text(statement, 18, existing.catalog_source_sha256) &&
        sqlite3_step(statement) == SQLITE_DONE;
    if (!succeeded) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    if (!succeeded) {
        return JobLifecycleStatus::storage_error;
    }
    return JobLifecycleStatus::succeeded;
}

JobLifecycleStatus MetadataStore::rebuild_job_exact(
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
    if (existing.status != "succeeded" || !existing.package_id.empty() ||
        existing.source_pack_path.empty() ||
        existing.pack_fingerprint.empty() ||
        existing.package_fingerprint.empty() ||
        existing.integration_contract_version.empty() ||
        existing.generator_binary_sha256.empty()) {
        return JobLifecycleStatus::invalid_state;
    }
    if (!random_id("job_", new_job_id, error) ||
        !execute("BEGIN IMMEDIATE;", error)) {
        return JobLifecycleStatus::storage_error;
    }
    sqlite3_stmt* insert = nullptr;
    const char* insert_sql =
        "INSERT INTO jobs "
        "(id, organization_id, project_id, user_id, status, request_json, "
        "selected_pack_id, source_pack_path, pack_fingerprint, "
        "package_fingerprint, integration_contract_version, "
        "integration_contract_json, generator_version, generator_git_commit, "
        "generator_build_identity, generator_binary_sha256, "
        "selected_pack_version, catalog_version, catalog_source_sha256) "
        "VALUES (?1,?2,?3,?4,'queued',?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);";
    bool succeeded =
        sqlite3_prepare_v2(database_, insert_sql, -1, &insert, nullptr) ==
            SQLITE_OK &&
        bind_text(insert, 1, new_job_id) &&
        bind_text(insert, 2, owner.organization_id) &&
        bind_text(insert, 3, existing.project_id) &&
        bind_text(insert, 4, owner.user_id) &&
        bind_text(insert, 5, existing.request_json) &&
        bind_text(insert, 6, existing.selected_pack_id) &&
        bind_text(insert, 7, existing.source_pack_path) &&
        bind_text(insert, 8, existing.pack_fingerprint) &&
        bind_text(insert, 9, existing.package_fingerprint) &&
        bind_text(insert, 10, existing.integration_contract_version) &&
        bind_text(insert, 11, existing.integration_contract_json) &&
        bind_text(insert, 12, existing.generator_version) &&
        bind_text(insert, 13, existing.generator_git_commit) &&
        bind_text(insert, 14, existing.generator_build_identity) &&
        bind_text(insert, 15, existing.generator_binary_sha256) &&
        bind_text(insert, 16, existing.selected_pack_version) &&
        bind_text(insert, 17, existing.catalog_version) &&
        bind_text(insert, 18, existing.catalog_source_sha256) &&
        sqlite3_step(insert) == SQLITE_DONE;
    sqlite3_finalize(insert);

    sqlite3_stmt* audit = nullptr;
    const char* audit_sql =
        "INSERT INTO audit_events "
        "(organization_id, user_id, api_key_id, event_type, "
        "subject_type, subject_id, details_json) "
        "VALUES (?1, ?2, ?3, 'artifact.exact_rebuild_requested', "
        "'job', ?4, ?5);";
    const std::string details =
        std::string("{\"source_job_id\":\"") + job_id + "\"}";
    succeeded = succeeded &&
        sqlite3_prepare_v2(database_, audit_sql, -1, &audit, nullptr) ==
            SQLITE_OK &&
        bind_text(audit, 1, owner.organization_id) &&
        bind_text(audit, 2, owner.user_id) &&
        bind_text(audit, 3, owner.api_key_id) &&
        bind_text(audit, 4, new_job_id) &&
        bind_text(audit, 5, details) &&
        sqlite3_step(audit) == SQLITE_DONE;
    sqlite3_finalize(audit);

    if (!succeeded || !execute("COMMIT;", error)) {
        if (error.empty()) error = sqlite3_errmsg(database_);
        std::string ignored;
        execute("ROLLBACK;", ignored);
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
        "selected_pack_id, source_pack_path, pack_fingerprint, created_at, "
        "package_fingerprint, integration_contract_version, "
        "integration_contract_json, generator_version, generator_git_commit, "
        "generator_build_identity, generator_binary_sha256, "
        "selected_pack_version, catalog_version, catalog_source_sha256 "
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
    claimed.package_fingerprint = column_text(select, 9);
    claimed.integration_contract_version = column_text(select, 10);
    claimed.integration_contract_json = column_text(select, 11);
    claimed.generator_version = column_text(select, 12);
    claimed.generator_git_commit = column_text(select, 13);
    claimed.generator_build_identity = column_text(select, 14);
    claimed.generator_binary_sha256 = column_text(select, 15);
    claimed.selected_pack_version = column_text(select, 16);
    claimed.catalog_version = column_text(select, 17);
    claimed.catalog_source_sha256 = column_text(select, 18);
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

bool MetadataStore::pin_job_inputs(
    const std::string& job_id,
    const std::string& source_pack_path,
    const std::string& integration_contract_version,
    const std::string& integration_contract_json,
    const std::string& generator_version,
    const std::string& generator_git_commit,
    const std::string& generator_build_identity,
    const std::string& generator_binary_sha256,
    std::string& error
) {
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "UPDATE jobs SET source_pack_path=?2, integration_contract_version=?3, "
        "integration_contract_json=?4, generator_version=?5, "
        "generator_git_commit=?6, generator_build_identity=?7, "
        "generator_binary_sha256=?8 "
        "WHERE id=?1 AND status='running' AND deleted_at IS NULL;";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, job_id) &&
        bind_text(statement, 2, source_pack_path) &&
        bind_text(statement, 3, integration_contract_version) &&
        bind_text(statement, 4, integration_contract_json) &&
        bind_text(statement, 5, generator_version) &&
        bind_text(statement, 6, generator_git_commit) &&
        bind_text(statement, 7, generator_build_identity) &&
        bind_text(statement, 8, generator_binary_sha256) &&
        sqlite3_step(statement) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    if (!succeeded) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return succeeded;
}

bool MetadataStore::complete_job_with_package(
    const JobRecord& job,
    const std::string& package_id,
    const std::string& package_fingerprint,
    const std::string& integration_contract_version,
    const std::string& integration_contract_json,
    const std::string& generator_version,
    const std::string& generator_git_commit,
    const std::string& generator_build_identity,
    const std::string& generator_binary_sha256,
    const std::string& challenge_receipt_json,
    const std::string& challenge_metadata_json,
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
        "package_fingerprint, selected_pack_version, catalog_version, "
        "catalog_source_sha256, integration_contract_version, integration_contract_json, "
        "generator_version, generator_git_commit, generator_build_identity, "
        "generator_binary_sha256, challenge_receipt_json, challenge_metadata_json, manifest_hash, "
        "artifact_storage_key, size_bytes) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21);";
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
        bind_text(package_statement, 8, job.selected_pack_version) &&
        bind_text(package_statement, 9, job.catalog_version) &&
        bind_text(package_statement, 10, job.catalog_source_sha256) &&
        bind_text(package_statement, 11, integration_contract_version) &&
        bind_text(package_statement, 12, integration_contract_json) &&
        bind_text(package_statement, 13, generator_version) &&
        bind_text(package_statement, 14, generator_git_commit) &&
        bind_text(package_statement, 15, generator_build_identity) &&
        bind_text(package_statement, 16, generator_binary_sha256) &&
        bind_text(package_statement, 17, challenge_receipt_json) &&
        bind_text(package_statement, 18, challenge_metadata_json) &&
        bind_text(package_statement, 19, manifest_hash) &&
        bind_text(package_statement, 20, artifact_storage_key) &&
        sqlite3_bind_int64(package_statement, 21, size_bytes) == SQLITE_OK &&
        sqlite3_step(package_statement) == SQLITE_DONE;
    sqlite3_finalize(package_statement);

    sqlite3_stmt* job_statement = nullptr;
    const char* job_sql =
        "UPDATE jobs SET status = 'succeeded', package_fingerprint = ?2, "
        "integration_contract_version=?3, integration_contract_json=?4, "
        "generator_version=?5, generator_git_commit=?6, "
        "generator_build_identity=?7, generator_binary_sha256=?8, "
        "challenge_receipt_json=?9, normalized_cli_command=?10, manifest_hash=?11, "
        "artifact_storage_key=?12, challenge_metadata_json=?13, "
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
        bind_text(job_statement, 3, integration_contract_version) &&
        bind_text(job_statement, 4, integration_contract_json) &&
        bind_text(job_statement, 5, generator_version) &&
        bind_text(job_statement, 6, generator_git_commit) &&
        bind_text(job_statement, 7, generator_build_identity) &&
        bind_text(job_statement, 8, generator_binary_sha256) &&
        bind_text(job_statement, 9, challenge_receipt_json) &&
        bind_text(job_statement, 10, normalized_cli_command) &&
        bind_text(job_statement, 11, manifest_hash) &&
        bind_text(job_statement, 12, artifact_storage_key) &&
        bind_text(job_statement, 13, challenge_metadata_json) &&
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
        "p.package_fingerprint, p.integration_contract_version, "
        "p.integration_contract_json, p.generator_version, p.generator_git_commit, "
        "p.generator_build_identity, p.generator_binary_sha256, "
        "p.challenge_receipt_json, p.manifest_hash, p.artifact_storage_key, "
        "p.size_bytes, p.created_at, "
        "strftime('%Y-%m-%dT%H:%M:%fZ',p.created_at,'+7 days'), "
        "p.selected_pack_version, p.catalog_version, "
        "p.catalog_source_sha256, p.challenge_metadata_json "
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
    loaded.integration_contract_version = column_text(statement, 6);
    loaded.integration_contract_json = column_text(statement, 7);
    loaded.generator_version = column_text(statement, 8);
    loaded.generator_git_commit = column_text(statement, 9);
    loaded.generator_build_identity = column_text(statement, 10);
    loaded.generator_binary_sha256 = column_text(statement, 11);
    loaded.challenge_receipt_json = column_text(statement, 12);
    loaded.manifest_hash = column_text(statement, 13);
    loaded.artifact_storage_key = column_text(statement, 14);
    loaded.size_bytes = sqlite3_column_int64(statement, 15);
    loaded.created_at = column_text(statement, 16);
    loaded.expires_at = column_text(statement, 17);
    loaded.selected_pack_version = column_text(statement, 18);
    loaded.catalog_version = column_text(statement, 19);
    loaded.catalog_source_sha256 = column_text(statement, 20);
    loaded.challenge_metadata_json = column_text(statement, 21);
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

bool MetadataStore::list_retention_candidates(
    int retention_days,
    std::vector<RetentionCandidate>& candidates,
    std::string& error
) {
    candidates.clear();
    if (retention_days < 1 || retention_days > 3650 || !initialize(error)) {
        if (error.empty()) error = "retention days must be between 1 and 3650";
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT id,job_id,artifact_storage_key,deleted_at IS NOT NULL "
        "FROM packages WHERE deleted_at IS NOT NULL OR "
        "created_at <= strftime('%Y-%m-%dT%H:%M:%fZ','now',?1) "
        "ORDER BY created_at,id;";
    std::ostringstream modifier;
    modifier << '-' << retention_days << " days";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, modifier.str())) {
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
        RetentionCandidate candidate;
        candidate.package_id = column_text(statement, 0);
        candidate.job_id = column_text(statement, 1);
        candidate.artifact_storage_key = column_text(statement, 2);
        candidate.already_hidden = sqlite3_column_int(statement, 3) == 1;
        candidates.push_back(candidate);
    }
}

bool MetadataStore::mark_package_expired(
    const std::string& package_id,
    std::string& error
) {
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "UPDATE packages SET deleted_at=COALESCE(deleted_at,"
        "strftime('%Y-%m-%dT%H:%M:%fZ','now')) WHERE id=?1;";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, package_id) &&
        sqlite3_step(statement) == SQLITE_DONE &&
        sqlite3_changes(database_) == 1;
    if (!succeeded) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return succeeded;
}

bool MetadataStore::backup_database(
    const std::string& destination_path,
    std::string& error
) {
    if (destination_path.empty() || !initialize(error)) return false;
    sqlite3* destination = nullptr;
    if (sqlite3_open_v2(
            destination_path.c_str(), &destination,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        error = destination == nullptr ? "unable to open backup database" :
            sqlite3_errmsg(destination);
        if (destination != nullptr) sqlite3_close(destination);
        return false;
    }
    sqlite3_backup* backup =
        sqlite3_backup_init(destination, "main", database_, "main");
    bool succeeded = backup != nullptr &&
        sqlite3_backup_step(backup, -1) == SQLITE_DONE;
    if (backup != nullptr) sqlite3_backup_finish(backup);
    if (!succeeded) error = sqlite3_errmsg(destination);
    sqlite3_close(destination);
    return succeeded;
}

RecordLookupStatus MetadataStore::find_scenario_draft(
    const std::string& scenario_id,
    const ApiKeyIdentity& owner,
    ScenarioDraftRecord& draft,
    std::string& error
) {
    if (!initialize(error)) return RecordLookupStatus::storage_error;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT id,organization_id,user_id,name,status,document_json,"
        "COALESCE(document_fingerprint,''),target_intent_json,validation_errors_json,"
        "created_at,updated_at FROM scenario_drafts "
        "WHERE id=?1 AND organization_id=?2 AND user_id=?3;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, scenario_id) ||
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
    draft = scenario_draft_columns(statement);
    sqlite3_finalize(statement);
    return RecordLookupStatus::found;
}

bool MetadataStore::create_scenario_draft(
    const ApiKeyIdentity& owner,
    const std::string& name,
    const std::string& status,
    const std::string& document_json,
    const std::string& document_fingerprint,
    const std::string& target_intent_json,
    const std::string& validation_errors_json,
    ScenarioDraftRecord& draft,
    std::string& error
) {
    std::string scenario_id;
    if (!initialize(error) || !random_id("scenario_", scenario_id, error)) {
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT INTO scenario_drafts "
        "(id,organization_id,user_id,name,status,document_json,"
        "document_fingerprint,target_intent_json,validation_errors_json) "
        "VALUES (?1,?2,?3,?4,?5,?6,NULLIF(?7,''),?8,?9);";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, scenario_id) &&
        bind_text(statement, 2, owner.organization_id) &&
        bind_text(statement, 3, owner.user_id) &&
        bind_text(statement, 4, name) &&
        bind_text(statement, 5, status) &&
        bind_text(statement, 6, document_json) &&
        bind_text(statement, 7, document_fingerprint) &&
        bind_text(statement, 8, target_intent_json) &&
        bind_text(statement, 9, validation_errors_json) &&
        sqlite3_step(statement) == SQLITE_DONE;
    if (!succeeded) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return succeeded &&
        find_scenario_draft(scenario_id, owner, draft, error) ==
            RecordLookupStatus::found;
}

bool MetadataStore::list_scenario_drafts(
    const ApiKeyIdentity& owner,
    std::vector<ScenarioDraftRecord>& drafts,
    std::string& error
) {
    drafts.clear();
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT id,organization_id,user_id,name,status,document_json,"
        "COALESCE(document_fingerprint,''),target_intent_json,validation_errors_json,"
        "created_at,updated_at FROM scenario_drafts "
        "WHERE organization_id=?1 AND user_id=?2 "
        "ORDER BY updated_at DESC,id DESC;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, owner.organization_id) ||
        !bind_text(statement, 2, owner.user_id)) {
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
        drafts.push_back(scenario_draft_columns(statement));
    }
}

RecordLookupStatus MetadataStore::update_scenario_draft(
    const std::string& scenario_id,
    const ApiKeyIdentity& owner,
    const std::string& name,
    const std::string& status,
    const std::string& document_json,
    const std::string& document_fingerprint,
    const std::string& target_intent_json,
    const std::string& validation_errors_json,
    ScenarioDraftRecord& draft,
    std::string& error
) {
    if (!initialize(error)) return RecordLookupStatus::storage_error;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "UPDATE scenario_drafts SET name=?4,status=?5,document_json=?6,"
        "document_fingerprint=NULLIF(?7,''),target_intent_json=?8,"
        "validation_errors_json=?9,"
        "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE id=?1 AND organization_id=?2 AND user_id=?3;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, scenario_id) ||
        !bind_text(statement, 2, owner.organization_id) ||
        !bind_text(statement, 3, owner.user_id) ||
        !bind_text(statement, 4, name) ||
        !bind_text(statement, 5, status) ||
        !bind_text(statement, 6, document_json) ||
        !bind_text(statement, 7, document_fingerprint) ||
        !bind_text(statement, 8, target_intent_json) ||
        !bind_text(statement, 9, validation_errors_json) ||
        sqlite3_step(statement) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return RecordLookupStatus::storage_error;
    }
    const bool changed = sqlite3_changes(database_) == 1;
    sqlite3_finalize(statement);
    return changed
        ? find_scenario_draft(scenario_id, owner, draft, error)
        : RecordLookupStatus::not_found;
}

RecordLookupStatus MetadataStore::delete_scenario_draft(
    const std::string& scenario_id,
    const ApiKeyIdentity& owner,
    std::string& error
) {
    if (!initialize(error)) return RecordLookupStatus::storage_error;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "DELETE FROM scenario_drafts "
        "WHERE id=?1 AND organization_id=?2 AND user_id=?3;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, scenario_id) ||
        !bind_text(statement, 2, owner.organization_id) ||
        !bind_text(statement, 3, owner.user_id) ||
        sqlite3_step(statement) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return RecordLookupStatus::storage_error;
    }
    const bool changed = sqlite3_changes(database_) == 1;
    sqlite3_finalize(statement);
    return changed ? RecordLookupStatus::found : RecordLookupStatus::not_found;
}

RecordLookupStatus MetadataStore::find_custom_pack(
    const std::string& pack_id,
    const ApiKeyIdentity& owner,
    CustomPackRecord& pack,
    std::string& error
) {
    if (!initialize(error)) return RecordLookupStatus::storage_error;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT id,organization_id,user_id,name,version,description,"
        "targets_json,scenario_ids_json,pack_fingerprint,source_pack_path,"
        "created_at FROM custom_packs WHERE id=?1 AND organization_id=?2 "
        "AND user_id=?3 AND deleted_at IS NULL;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, pack_id) ||
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
    pack = custom_pack_columns(statement);
    sqlite3_finalize(statement);
    return RecordLookupStatus::found;
}

bool MetadataStore::create_custom_pack(
    const ApiKeyIdentity& owner,
    const CustomPackRecord& input,
    CustomPackRecord& pack,
    std::string& error
) {
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT INTO custom_packs "
        "(id,organization_id,user_id,name,version,description,targets_json,"
        "scenario_ids_json,pack_fingerprint,source_pack_path) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);";
    const bool succeeded =
        sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK &&
        bind_text(statement, 1, input.pack_id) &&
        bind_text(statement, 2, owner.organization_id) &&
        bind_text(statement, 3, owner.user_id) &&
        bind_text(statement, 4, input.name) &&
        bind_text(statement, 5, input.version) &&
        bind_text(statement, 6, input.description) &&
        bind_text(statement, 7, input.targets_json) &&
        bind_text(statement, 8, input.scenario_ids_json) &&
        bind_text(statement, 9, input.pack_fingerprint) &&
        bind_text(statement, 10, input.source_pack_path) &&
        sqlite3_step(statement) == SQLITE_DONE;
    if (!succeeded) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return succeeded &&
        find_custom_pack(input.pack_id, owner, pack, error) ==
            RecordLookupStatus::found;
}

bool MetadataStore::list_custom_packs(
    const ApiKeyIdentity& owner,
    std::vector<CustomPackRecord>& packs,
    std::string& error
) {
    packs.clear();
    if (!initialize(error)) return false;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT id,organization_id,user_id,name,version,description,"
        "targets_json,scenario_ids_json,pack_fingerprint,source_pack_path,"
        "created_at FROM custom_packs WHERE organization_id=?1 AND user_id=?2 "
        "AND deleted_at IS NULL ORDER BY created_at DESC,id DESC;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, owner.organization_id) ||
        !bind_text(statement, 2, owner.user_id)) {
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
        packs.push_back(custom_pack_columns(statement));
    }
}

RecordLookupStatus MetadataStore::delete_custom_pack(
    const std::string& pack_id,
    const ApiKeyIdentity& owner,
    std::string& error
) {
    if (!initialize(error)) return RecordLookupStatus::storage_error;
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "UPDATE custom_packs SET deleted_at="
        "strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE id=?1 AND organization_id=?2 AND user_id=?3 "
        "AND deleted_at IS NULL;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_text(statement, 1, pack_id) ||
        !bind_text(statement, 2, owner.organization_id) ||
        !bind_text(statement, 3, owner.user_id) ||
        sqlite3_step(statement) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return RecordLookupStatus::storage_error;
    }
    const bool changed = sqlite3_changes(database_) == 1;
    sqlite3_finalize(statement);
    return changed ? RecordLookupStatus::found : RecordLookupStatus::not_found;
}

}  // namespace syn_sig_ra
