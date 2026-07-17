#include "syn_sig_ra/core_contract.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::string replaced(
    std::string value,
    const std::string& needle,
    const std::string& replacement
) {
    const std::string::size_type at = value.find(needle);
    require(at != std::string::npos, "contract mutation fixture must exist");
    value.replace(at, needle.size(), replacement);
    return value;
}

}  // namespace

int main() {
    syn_sig_ra::CoreIntegrationContract linked;
    std::string error;
    require(
        syn_sig_ra::linked_core_integration_contract(linked, error),
        "linked integration contract should be accepted: " + error
    );
    syn_sig_ra::CoreIntegrationContract parsed;
    require(
        syn_sig_ra::parse_core_integration_contract(
            linked.canonical_json, parsed, error) &&
            parsed.generator_git_commit == linked.generator_git_commit,
        "canonical contract should round trip"
    );
    require(
        !syn_sig_ra::parse_core_integration_contract(
            linked.canonical_json + " trailing", parsed, error),
        "trailing non-JSON output must be rejected"
    );
    require(
        !syn_sig_ra::parse_core_integration_contract(
            replaced(
                linked.canonical_json,
                "\"contract\":\"synsigra_core_integration_v1\"",
                "\"contract\":\"synsigra_core_integration_v1\","
                "\"contract\":\"synsigra_core_integration_v1\""),
            parsed, error),
        "duplicate keys must be rejected"
    );
    require(
        !syn_sig_ra::parse_core_integration_contract(
            replaced(
                linked.canonical_json,
                "synsigra_core_integration_v1",
                "synsigra_core_integration_v0"),
            parsed, error),
        "unsupported integration versions must be rejected"
    );
    require(
        !syn_sig_ra::parse_core_integration_contract(
            replaced(linked.canonical_json, "r_peak", "rpeaks"),
            parsed, error),
        "removed scoring target aliases must be rejected"
    );
    return EXIT_SUCCESS;
}
