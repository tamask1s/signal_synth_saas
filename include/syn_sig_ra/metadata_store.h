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

enum class JobDeleteStatus {
    deleted,
    not_found,
    running,
    storage_error
};

class MetadataStore {
public:
    explicit MetadataStore(const std::string& database_path);
    ~MetadataStore();

    MetadataStore(const MetadataStore&) = delete;
    MetadataStore& operator=(const MetadataStore&) = delete;

    bool initialize(std::string& error);

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
        std::vector<JobRecord>& jobs,
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

private:
    bool open(std::string& error);
    bool execute(const char* sql, std::string& error);

    std::string database_path_;
    sqlite3* database_;
    bool initialized_;
};

}  // namespace syn_sig_ra

#endif
