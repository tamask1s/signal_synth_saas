#ifndef SYN_SIG_RA_PACK_CATALOG_H
#define SYN_SIG_RA_PACK_CATALOG_H

#include <string>
#include <vector>

namespace syn_sig_ra {

struct PackScenarioSummary {
    std::string scenario_id;
    std::vector<std::string> targets;
    std::vector<std::string> scoreable_targets;
    std::vector<std::string> reference_only_targets;
    int duration_seconds = 0;
    int sampling_rate_hz = 0;
    int channel_count = 0;
    long long sample_count = 0;
    long long estimated_package_bytes = 0;
};

struct PackChangelogEntry {
    std::string version;
    std::string date;
    std::string summary;
};

struct PackTargetSummary {
    std::string target;
    std::string support;
    bool scoreable = false;
    std::string score_type;
    std::string description;
    std::string primary_metric;
    int case_count = 0;
    std::vector<std::string> case_ids;
    std::vector<std::string> accepted_formats;
    std::vector<std::string> reference_artifacts;
    bool has_default_tolerance_seconds = false;
    double default_tolerance_seconds = 0.0;
};

struct PackSummary {
    std::string pack_id;
    std::string display_name;
    std::string version;
    std::string description;
    std::vector<std::string> targets;
    std::vector<PackScenarioSummary> scenarios;
    std::string pack_fingerprint;
    std::string release_status;
    std::string released_at;
    std::string generator_contract;
    std::vector<std::string> compatible_generator_versions;
    std::string deprecation_message;
    std::vector<PackChangelogEntry> changelog;
    std::string scoring_mode;
    std::vector<PackTargetSummary> scoreable_targets;
    std::vector<PackTargetSummary> reference_only_targets;
    std::vector<std::string> detector_output_schemas;
    std::string recommended_profile;
    std::vector<std::string> supported_threshold_profiles;
    std::vector<std::string> recommended_for;
    std::vector<std::string> not_recommended_for;
    std::vector<std::string> difficulty;
    std::vector<std::string> feature_tags;
    std::vector<std::string> modality;
    int minimum_case_seconds = 0;
    int maximum_case_seconds = 0;
    int total_seconds = 0;
    std::vector<int> sampling_rates_hz;
    int minimum_channel_count = 0;
    int maximum_channel_count = 0;
    long long estimated_package_bytes = 0;
    long long peak_memory_bytes = 0;
    std::string package_size_class;
    std::string local_verifier_min_version;
    std::string challenge_package_contract;
    std::string scoring_manifest_contract;
    std::vector<std::string> output_artifact_roles;
    std::vector<std::string> primary_badges;
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
