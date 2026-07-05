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

}  // namespace

int main() {
    const syn_sig_ra::RuntimeConfig config =
        syn_sig_ra::default_runtime_config();
    const syn_sig_ra::PackCatalog catalog(config.pack_root);
    std::string error;
    std::vector<syn_sig_ra::PackSummary> packs;

    require(catalog.list(packs, error), "catalog list should succeed: " + error);
    require(!packs.empty(), "catalog should contain the example pack");
    require(
        packs[0].pack_id == "r_peak_stress_v1",
        "catalog should expose the filename-matched pack ID"
    );
    require(
        packs[0].display_name == "R-peak Stress Pack v1",
        "catalog should expose the authoritative display name"
    );
    require(
        packs[0].pack_fingerprint.compare(0, 7, "sha256:") == 0,
        "catalog should expose the authoritative fingerprint"
    );
    require(
        packs[0].version == "1.0" && packs[0].scenarios.size() == 4,
        "catalog should expose pack version and scenario count"
    );
    require(
        packs[0].scenarios[0].scenario_id == "clean_70",
        "catalog should expose scenario IDs without source paths"
    );
    require(
        packs[0].release_status == "stable" &&
            packs[0].generator_contract ==
                "signal-synth-cli/pack-challenge-v1" &&
            packs[0].compatible_generator_versions.size() == 1 &&
            packs[0].changelog.size() == 1,
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
