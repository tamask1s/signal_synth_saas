#ifndef SYN_SIG_RA_METADATA_STORE_H
#define SYN_SIG_RA_METADATA_STORE_H

#include <string>
#include <vector>

struct sqlite3;

namespace syn_sig_ra {

struct ApiKeyIdentity {
    std::string api_key_id;
    std::string organization_id;
    std::string user_id;
    std::string role;
};

struct AccountRecord {
    std::string user_id;
    std::string organization_id;
    std::string email;
    std::string display_name;
    std::string password_salt;
    std::string password_hash;
    std::string role;
    bool email_verified = false;
    std::string email_verified_at;
};

struct ProjectRecord {
    std::string project_id;
    std::string organization_id;
    std::string display_name;
    std::string created_at;
};

struct JobRecord {
    std::string job_id;
    std::string organization_id;
    std::string project_id;
    std::string user_id;
    std::string status;
    std::string request_json;
    std::string selected_pack_id;
    std::string source_pack_path;
    std::string pack_fingerprint;
    std::string package_id;
    std::string package_fingerprint;
    std::string generator_version;
    std::string generator_build_identity;
    std::string manifest_hash;
    std::string artifact_storage_key;
    std::string error_code;
    std::string error_message;
    std::string created_at;
    std::string started_at;
    std::string completed_at;
    std::string deleted_at;
};

struct PackageRecord {
    std::string package_id;
    std::string job_id;
    std::string organization_id;
    std::string project_id;
    std::string user_id;
    std::string package_fingerprint;
    std::string manifest_hash;
    std::string artifact_storage_key;
    long long size_bytes;
};

struct UsageSummary {
    int requests_last_minute;
    int active_jobs;
    int jobs_this_month;
    int packages_this_month;
    long long package_bytes_this_month;
    int queued_jobs;
    int running_jobs;
    int failed_jobs_this_month;
    int quota_rejections_this_month;
    std::string worker_last_seen_at;
    std::string worker_last_status;
    int request_limit_per_minute;
    int concurrent_job_limit;
    int monthly_job_limit;
};

enum class QuotaStatus {
    allowed,
    rate_limited,
    concurrent_limit,
    monthly_limit,
    storage_error
};

struct ApiKeyRecord {
    std::string api_key_id;
    std::string organization_id;
    std::string user_id;
    std::string role;
    std::string label;
    bool active;
    std::string created_at;
    std::string last_used_at;
};

struct RetentionCandidate {
    std::string package_id;
    std::string job_id;
    std::string artifact_storage_key;
    bool already_hidden;
};

struct ScenarioDraftRecord {
    std::string scenario_id;
    std::string organization_id;
    std::string user_id;
    std::string name;
    std::string status;
    std::string document_json;
    std::string document_fingerprint;
    std::string validation_errors_json;
    std::string created_at;
    std::string updated_at;
};

struct CustomPackRecord {
    std::string pack_id;
    std::string organization_id;
    std::string user_id;
    std::string name;
    std::string version;
    std::string description;
    std::string targets_json;
    std::string scenario_ids_json;
    std::string pack_fingerprint;
    std::string source_pack_path;
    std::string created_at;
};

enum class RecordLookupStatus {
    found,
    not_found,
    storage_error
};

enum class ApiKeyLookupStatus {
    found,
    not_found,
    storage_error
};

enum class AccountCreateStatus {
    created,
    email_exists,
    storage_error
};

enum class JobDeleteStatus {
    deleted,
    not_found,
    running,
    storage_error
};

enum class RateLimitStatus {
    allowed,
    limited,
    storage_error
};

enum class EmailTokenCreateStatus {
    created,
    rate_limited,
    storage_error
};

enum class EmailTokenConsumeStatus {
    consumed,
    invalid_or_expired,
    storage_error
};

enum class JobLifecycleStatus {
    succeeded,
    not_found,
    invalid_state,
    storage_error
};

class MetadataStore {
public:
    explicit MetadataStore(const std::string& database_path);
    ~MetadataStore();

    MetadataStore(const MetadataStore&) = delete;
    MetadataStore& operator=(const MetadataStore&) = delete;

    bool initialize(std::string& error);

    AccountCreateStatus create_account(
        const std::string& email,
        const std::string& display_name,
        const std::string& password_salt,
        const std::string& password_hash,
        const std::string& terms_version,
        AccountRecord& account,
        std::string& error
    );

    RecordLookupStatus find_account_by_email(
        const std::string& email,
        AccountRecord& account,
        std::string& error
    );

    bool create_session(
        const AccountRecord& account,
        const std::string& session_id,
        const std::string& token_hash,
        std::string& error
    );

    RecordLookupStatus find_session(
        const std::string& token_hash,
        ApiKeyIdentity& identity,
        AccountRecord& account,
        std::string& error
    );

    bool delete_session(
        const std::string& token_hash,
        std::string& error
    );

    bool delete_sessions_for_user(
        const std::string& user_id,
        std::string& error
    );

    RateLimitStatus record_email_send_attempt(
        const std::string& purpose,
        const std::string& email_hash,
        int max_attempts,
        int window_minutes,
        std::string& error
    );

    RateLimitStatus record_email_token_submission(
        const std::string& purpose,
        const std::string& token_hash,
        int max_attempts,
        int window_minutes,
        std::string& error
    );

    EmailTokenCreateStatus create_email_token(
        const std::string& user_id,
        const std::string& purpose,
        const std::string& token_hash,
        const std::string& recipient_email,
        int ttl_minutes,
        std::string& error
    );

    EmailTokenConsumeStatus verify_email_token(
        const std::string& token_hash,
        AccountRecord& account,
        std::string& error
    );

    EmailTokenConsumeStatus consume_password_reset_token(
        const std::string& token_hash,
        AccountRecord& account,
        std::string& error
    );

    bool update_account_password(
        const std::string& user_id,
        const std::string& password_salt,
        const std::string& password_hash,
        std::string& error
    );

    bool create_personal_api_key(
        const ApiKeyIdentity& owner,
        const std::string& api_key_id,
        const std::string& key_hash,
        const std::string& label,
        std::string& error
    );

    bool list_personal_api_keys(
        const ApiKeyIdentity& owner,
        std::vector<ApiKeyRecord>& api_keys,
        std::string& error
    );

    RecordLookupStatus revoke_personal_api_key(
        const ApiKeyIdentity& owner,
        const std::string& api_key_id,
        std::string& error
    );

    bool create_api_key(
        const ApiKeyIdentity& identity,
        const std::string& key_hash,
        const std::string& label,
        std::string& error
    );

    ApiKeyLookupStatus find_active_api_key(
        const std::string& key_hash,
        ApiKeyIdentity& identity,
        std::string& error
    );

    bool record_api_key_use(
        const ApiKeyIdentity& identity,
        std::string& error
    );

    QuotaStatus check_request_quota(
        const ApiKeyIdentity& identity,
        UsageSummary& usage,
        std::string& error
    );

    QuotaStatus check_job_quota(
        const ApiKeyIdentity& identity,
        UsageSummary& usage,
        std::string& error
    );

    bool usage_summary(
        const ApiKeyIdentity& identity,
        UsageSummary& usage,
        std::string& error
    );

    bool record_worker_heartbeat(
        const std::string& status,
        std::string& error
    );

    bool list_projects(
        const ApiKeyIdentity& identity,
        std::vector<ProjectRecord>& projects,
        std::string& error
    );

    bool create_project(
        const ApiKeyIdentity& identity,
        const std::string& display_name,
        ProjectRecord& project,
        std::string& error
    );

    RecordLookupStatus find_project(
        const std::string& project_id,
        const ApiKeyIdentity& identity,
        ProjectRecord& project,
        std::string& error
    );

    bool create_job(
        const ApiKeyIdentity& owner,
        const std::string& project_id,
        const std::string& request_json,
        const std::string& pack_id,
        const std::string& source_pack_path,
        const std::string& pack_fingerprint,
        std::string& job_id,
        std::string& error
    );

    RecordLookupStatus find_job(
        const std::string& job_id,
        const ApiKeyIdentity& owner,
        JobRecord& job,
        std::string& error
    );

    bool list_jobs(
        const ApiKeyIdentity& owner,
        int limit,
        int offset,
        std::vector<JobRecord>& jobs,
        std::string& error
    );

    JobLifecycleStatus cancel_job(
        const std::string& job_id,
        const ApiKeyIdentity& owner,
        std::string& error
    );

    JobLifecycleStatus retry_job(
        const std::string& job_id,
        const ApiKeyIdentity& owner,
        std::string& new_job_id,
        std::string& error
    );

    JobDeleteStatus delete_job(
        const std::string& job_id,
        const ApiKeyIdentity& owner,
        std::string& error
    );

    RecordLookupStatus claim_next_job(
        JobRecord& job,
        std::string& error
    );

    bool complete_job(
        const std::string& job_id,
        const std::string& package_fingerprint,
        const std::string& generator_version,
        const std::string& generator_build_identity,
        const std::string& normalized_cli_command,
        const std::string& artifact_storage_key,
        std::string& error
    );

    bool complete_job_with_package(
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
    );

    bool fail_job(
        const std::string& job_id,
        const std::string& error_code,
        const std::string& error_message,
        std::string& error
    );

    RecordLookupStatus find_package(
        const std::string& package_id,
        const ApiKeyIdentity& owner,
        PackageRecord& package,
        std::string& error
    );

    bool list_api_keys(
        const std::string& organization_id,
        std::vector<ApiKeyRecord>& api_keys,
        std::string& error
    );

    bool revoke_api_key(
        const std::string& api_key_id,
        std::string& error
    );

    bool list_retention_candidates(
        int retention_days,
        std::vector<RetentionCandidate>& candidates,
        std::string& error
    );

    bool mark_package_expired(
        const std::string& package_id,
        std::string& error
    );

    bool backup_database(
        const std::string& destination_path,
        std::string& error
    );

    bool create_scenario_draft(
        const ApiKeyIdentity& owner,
        const std::string& name,
        const std::string& status,
        const std::string& document_json,
        const std::string& document_fingerprint,
        const std::string& validation_errors_json,
        ScenarioDraftRecord& draft,
        std::string& error
    );

    bool list_scenario_drafts(
        const ApiKeyIdentity& owner,
        std::vector<ScenarioDraftRecord>& drafts,
        std::string& error
    );

    RecordLookupStatus find_scenario_draft(
        const std::string& scenario_id,
        const ApiKeyIdentity& owner,
        ScenarioDraftRecord& draft,
        std::string& error
    );

    RecordLookupStatus update_scenario_draft(
        const std::string& scenario_id,
        const ApiKeyIdentity& owner,
        const std::string& name,
        const std::string& status,
        const std::string& document_json,
        const std::string& document_fingerprint,
        const std::string& validation_errors_json,
        ScenarioDraftRecord& draft,
        std::string& error
    );

    RecordLookupStatus delete_scenario_draft(
        const std::string& scenario_id,
        const ApiKeyIdentity& owner,
        std::string& error
    );

    bool create_custom_pack(
        const ApiKeyIdentity& owner,
        const CustomPackRecord& input,
        CustomPackRecord& pack,
        std::string& error
    );

    bool list_custom_packs(
        const ApiKeyIdentity& owner,
        std::vector<CustomPackRecord>& packs,
        std::string& error
    );

    RecordLookupStatus find_custom_pack(
        const std::string& pack_id,
        const ApiKeyIdentity& owner,
        CustomPackRecord& pack,
        std::string& error
    );

    RecordLookupStatus delete_custom_pack(
        const std::string& pack_id,
        const ApiKeyIdentity& owner,
        std::string& error
    );

private:
    bool open(std::string& error);
    bool execute(const char* sql, std::string& error);

    std::string database_path_;
    sqlite3* database_;
    bool initialized_;
};

}  // namespace syn_sig_ra

#endif
