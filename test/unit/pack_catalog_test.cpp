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

}  // namespace

int main() {
    const syn_sig_ra::RuntimeConfig config =
        syn_sig_ra::default_runtime_config();
    const syn_sig_ra::PackCatalog catalog(config.pack_root);
    std::string error;
    std::vector<syn_sig_ra::PackSummary> packs;

    require(catalog.list(packs, error), "catalog list should succeed: " + error);
    require(packs.size() >= 10, "catalog should contain the imported release set");
    const syn_sig_ra::PackSummary* r_peak =
        find_pack(packs, "r_peak_stress_v1");
    require(r_peak != 0, "catalog should expose the R-peak pack");
    require(
        find_pack(packs, "hrv_v1") != 0 &&
            find_pack(packs, "ppg_benchmark_v1") != 0 &&
            find_pack(packs, "wearable_stress_v1") != 0,
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
            r_peak->generator_contract ==
                "signal-synth-cli/pack-challenge-v1" &&
            r_peak->compatible_generator_versions.size() >= 1 &&
            r_peak->changelog.size() == 1,
        "catalog should expose validated release and compatibility metadata"
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
                std::string::npos,
        "pack JSON should include scenario and release summaries"
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
