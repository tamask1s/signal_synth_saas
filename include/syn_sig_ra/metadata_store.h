#ifndef SYN_SIG_RA_METADATA_STORE_H
#define SYN_SIG_RA_METADATA_STORE_H

#include <string>

struct sqlite3;

namespace syn_sig_ra {

struct ApiKeyIdentity {
    std::string api_key_id;
    std::string organization_id;
    std::string user_id;
};

enum class ApiKeyLookupStatus {
    found,
    not_found,
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

private:
    bool open(std::string& error);
    bool execute(const char* sql, std::string& error);

    std::string database_path_;
    sqlite3* database_;
    bool initialized_;
};

}  // namespace syn_sig_ra

#endif
