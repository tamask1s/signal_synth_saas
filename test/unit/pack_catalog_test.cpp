#include "syn_sig_ra/pack_catalog.h"
#include "syn_sig_ra/runtime_config.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

const syn_sig_ra::PackSummary* find_pack(
    const std::vector<syn_sig_ra::PackSummary>& packs,
    const std::string& pack_id
) {
    for (std::vector<syn_sig_ra::PackSummary>::const_iterator it =
             packs.begin();
         it != packs.end();
         ++it) {
        if (it->pack_id == pack_id) {
            return &(*it);
        }
    }
    return 0;
}

bool has_target(
    const std::vector<syn_sig_ra::PackTargetSummary>& targets,
    const std::string& target,
    bool scoreable
) {
    for (std::vector<syn_sig_ra::PackTargetSummary>::const_iterator it =
             targets.begin();
         it != targets.end();
         ++it) {
        if (it->target == target && it->scoreable == scoreable) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    const syn_sig_ra::RuntimeConfig config =
        syn_sig_ra::default_runtime_config();
    const syn_sig_ra::PackCatalog catalog(config.pack_root);
    std::string error;
    std::vector<syn_sig_ra::PackSummary> packs;

    require(catalog.list(packs, error), "catalog list should succeed: " + error);
    require(packs.size() == 18, "catalog should contain the exact v7 release set");
    const syn_sig_ra::PackSummary* r_peak =
        find_pack(packs, "r_peak_stress_v1");
    require(r_peak != 0, "catalog should expose the R-peak pack");
    require(
        find_pack(packs, "hrv_robustness_v2") != 0 &&
            find_pack(packs, "ppg_benchmark_v1") != 0 &&
            find_pack(packs, "wearable_timebase_v2") != 0,
        "catalog should expose HRV, PPG and wearable release packs"
    );
    require(
        r_peak->pack_id == "r_peak_stress_v1",
        "catalog should expose the filename-matched pack ID"
    );
    require(
        r_peak->display_name == "R-peak Stress Pack v1",
        "catalog should expose the authoritative display name"
    );
    require(
        r_peak->pack_fingerprint.compare(0, 7, "sha256:") == 0,
        "catalog should expose the authoritative fingerprint"
    );
    require(
        r_peak->version == "1.0" && r_peak->scenarios.size() == 4,
        "catalog should expose pack version and scenario count"
    );
    require(
        r_peak->scenarios[0].scenario_id == "clean_70",
        "catalog should expose scenario IDs without source paths"
    );
    require(
        r_peak->release_status == "beta" &&
            r_peak->integration_contract_version ==
                "synsigra_core_integration_v7" &&
            r_peak->catalog_version == "3.0" &&
            r_peak->catalog_source_sha256 ==
                "sha256:2ab03e48ed533636d2abb5bc5a6f90590f1d9abbb4ed8664ed9efd0dac06892e" &&
            r_peak->changelog.size() == 1,
        "catalog should expose validated release and compatibility metadata"
    );
    require(
        r_peak->scoring_mode == "local" &&
            has_target(r_peak->scoreable_targets, "r_peak", true) &&
            has_target(r_peak->scoreable_targets, "signal_quality", true) &&
            r_peak->reference_only_targets.empty(),
        "catalog should expose the v7 scoreable target set"
    );
    require(
        r_peak->recommended_profile == "stress" &&
            r_peak->submission_output_schemas.size() >= 1 &&
            r_peak->estimated_package_bytes > 0 &&
            r_peak->total_seconds == 100 &&
            r_peak->minimum_channel_count == 12 &&
            r_peak->maximum_channel_count == 12 &&
            r_peak->sampling_rates_hz.size() == 1,
        "catalog should expose discovery metadata for duration, channels, package size and verifier profile"
    );
    require(
        r_peak->scenarios[0].duration_seconds > 0 &&
            r_peak->scenarios[0].sampling_rate_hz == 500 &&
            has_target(r_peak->scoreable_targets, r_peak->scenarios[0].scoreable_targets[0], true),
        "catalog should expose per-case scoring metadata"
    );

    const syn_sig_ra::PackSummary* morphology =
        find_pack(packs, "ecg_morphology_stress_v1");
    require(
        morphology != 0 &&
            morphology->version == "1.1" &&
            morphology->scoring_mode == "local" &&
            has_target(morphology->scoreable_targets, "morphology_assertions", true) &&
            morphology->reference_only_targets.empty(),
        "morphology measurements should be locally scoreable in v7"
    );
    const syn_sig_ra::PackSummary* ppg_benchmark =
        find_pack(packs, "ppg_benchmark_v1");
    require(
        ppg_benchmark != 0 &&
            has_target(ppg_benchmark->scoreable_targets, "ppg_pulse_onset", true),
        "catalog should expose non-R-peak scoreable targets from the curated release set"
    );

    syn_sig_ra::PackSummary detail;
    require(
        catalog.find("r_peak_stress_v1", detail, error) ==
            syn_sig_ra::PackLookupStatus::found,
        "known pack lookup should succeed: " + error
    );
    require(
        detail.description.find("R-peak detector") != std::string::npos,
        "pack detail should include its description"
    );
    require(
        syn_sig_ra::pack_summary_json(detail).find("\"scenarios\"") !=
            std::string::npos &&
            syn_sig_ra::pack_summary_json(detail).find("\"changelog\"") !=
                std::string::npos &&
            syn_sig_ra::pack_summary_json(detail).find("\"scoreable_targets\"") !=
                std::string::npos &&
            syn_sig_ra::pack_summary_json(detail).find("\"reference_only_targets\"") !=
                std::string::npos &&
            syn_sig_ra::pack_summary_json(detail).find("\"recommended_profile\":\"stress\"") !=
                std::string::npos &&
            syn_sig_ra::pack_summary_json(detail).find("\"submission_output_schemas\"") !=
                std::string::npos &&
            syn_sig_ra::pack_summary_json(detail).find("detector_output_schemas") ==
                std::string::npos,
        "pack JSON should include scenario, release and rich discovery summaries"
    );

    const syn_sig_ra::PackSummary* protocol_pack =
        find_pack(packs, "r_peak_rr_noise_v1");
    require(
        protocol_pack != 0 && protocol_pack->verification_protocol_available &&
            protocol_pack->verification_protocol_contract ==
                "synsigra_verification_protocol_v2" &&
            protocol_pack->verification_protocol_sha256.compare(0, 7, "sha256:") == 0 &&
            protocol_pack->external_noise_asset_ids.size() == 1,
        "catalog should expose normalized protocol and approved external-noise metadata"
    );

    require(
        catalog.find("../secret", detail, error) ==
            syn_sig_ra::PackLookupStatus::invalid_id,
        "path traversal should be rejected"
    );
    require(
        catalog.find("/etc/passwd", detail, error) ==
            syn_sig_ra::PackLookupStatus::invalid_id,
        "absolute paths should be rejected"
    );
    require(
        catalog.find("missing_pack", detail, error) ==
            syn_sig_ra::PackLookupStatus::not_found,
        "unknown safe IDs should be reported as missing"
    );
    require(
        syn_sig_ra::pack_summary_json(detail).find("../") ==
            std::string::npos &&
            syn_sig_ra::pack_summary_json(detail).find("/signal_synth/") ==
                std::string::npos,
        "catalog JSON must not expose source paths"
    );
    return EXIT_SUCCESS;
}
