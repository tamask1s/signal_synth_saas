#include "syn_sig_ra/core_contract.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

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
                "\"contract\":\"synsigra_core_integration_v7\"",
                "\"contract\":\"synsigra_core_integration_v7\","
                "\"contract\":\"synsigra_core_integration_v7\""),
            parsed, error),
        "duplicate keys must be rejected"
    );
    std::vector<std::pair<std::string, std::string> > tuple_mutations;
    tuple_mutations.push_back(std::make_pair(
        "synsigra_core_integration_v7", "synsigra_core_integration_v8"));
    tuple_mutations.push_back(std::make_pair(
        "\"name\":\"signal_synth\"", "\"name\":\"other\""));
    tuple_mutations.push_back(std::make_pair(
        "\"version\":\"0.10.0-dev\"", "\"version\":\"0.10.1-dev\""));
    tuple_mutations.push_back(std::make_pair(
        linked.generator_git_commit, std::string(40, '0')));
    tuple_mutations.push_back(std::make_pair(
        linked.generator_build_identity, "signal_synth/unpinned"));
    tuple_mutations.push_back(std::make_pair("\"cpp_facade\":\"1.5.0\"", "\"cpp_facade\":\"1.6.0\""));
    tuple_mutations.push_back(std::make_pair("\"pack_schema_version\":2", "\"pack_schema_version\":3"));
    tuple_mutations.push_back(std::make_pair("synsigra_challenge_package_v3", "synsigra_challenge_package_v4"));
    tuple_mutations.push_back(std::make_pair("synsigra_scoring_manifest_v3", "synsigra_scoring_manifest_v4"));
    tuple_mutations.push_back(std::make_pair("synsigra_verification_protocol_v2", "synsigra_verification_protocol_v3"));
    tuple_mutations.push_back(std::make_pair("synsigra_submission_v1", "synsigra_submission_v2"));
    tuple_mutations.push_back(std::make_pair("synsigra_submission_formats_v2", "synsigra_submission_formats_v3"));
    tuple_mutations.push_back(std::make_pair("synsigra_measurement_values_v2", "synsigra_measurement_values_v3"));
    tuple_mutations.push_back(std::make_pair("synsigra_measurement_truth_v2", "synsigra_measurement_truth_v3"));
    tuple_mutations.push_back(std::make_pair("synsigra_measurement_score_v2", "synsigra_measurement_score_v3"));
    tuple_mutations.push_back(std::make_pair("synsigra_local_verification_v2", "synsigra_local_verification_v3"));
    tuple_mutations.push_back(std::make_pair("synsigra_authoring_v18", "synsigra_authoring_v19"));
    tuple_mutations.push_back(std::make_pair("synsigra_templates_v5", "synsigra_templates_v6"));
    tuple_mutations.push_back(std::make_pair("\"python_verifier\":\"0.10.0\"", "\"python_verifier\":\"0.10.1\""));
    tuple_mutations.push_back(std::make_pair("synsigra_external_noise_truth_v1", "synsigra_external_noise_truth_v2"));
    for (std::vector<std::pair<std::string, std::string> >::const_iterator it =
             tuple_mutations.begin(); it != tuple_mutations.end(); ++it) {
        require(
            !syn_sig_ra::parse_core_integration_contract(
                replaced(linked.canonical_json, it->first, it->second),
                parsed, error),
            "every changed v7 tuple member must be rejected: " + it->first
        );
    }
    require(
        !syn_sig_ra::parse_core_integration_contract(
            replaced(linked.canonical_json, "r_peak", "rpeaks"),
            parsed, error),
        "removed scoring target aliases must be rejected"
    );
    return EXIT_SUCCESS;
}
