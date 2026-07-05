#ifndef SYN_SIG_RA_PACK_CATALOG_H
#define SYN_SIG_RA_PACK_CATALOG_H

#include <string>
#include <vector>

namespace syn_sig_ra {

struct PackSummary {
    std::string pack_id;
    std::string display_name;
    std::string description;
    std::string pack_fingerprint;
};

enum class PackLookupStatus {
    found,
    not_found,
    invalid_id,
    catalog_error
};

bool is_valid_pack_id(const std::string& pack_id);
std::string pack_summary_json(const PackSummary& pack);

class PackCatalog {
public:
    explicit PackCatalog(const std::string& pack_root);

    bool list(std::vector<PackSummary>& packs, std::string& error) const;

    PackLookupStatus find(
        const std::string& pack_id,
        PackSummary& pack,
        std::string& error
    ) const;

private:
    PackLookupStatus load_file(
        const std::string& expected_pack_id,
        const std::string& path,
        PackSummary& pack,
        std::string& error
    ) const;

    std::string pack_root_;
};

}  // namespace syn_sig_ra

#endif
