#ifndef SYN_SIG_RA_CORE_CONTRACT_H
#define SYN_SIG_RA_CORE_CONTRACT_H

#include <string>
#include <vector>

namespace syn_sig_ra {

struct CoreIntegrationContract {
    int schema_version = 0;
    std::string integration_contract;
    std::string generator_name;
    std::string generator_version;
    std::string generator_git_commit;
    std::string generator_build_identity;
    std::string cpp_facade;
    int pack_schema_version = 0;
    std::string challenge_package;
    std::string scoring_manifest;
    std::string verification_protocol;
    std::string submission;
    std::string submission_formats;
    std::string measurement_values;
    std::string measurement_truth;
    std::string measurement_scoring;
    std::string local_verification;
    std::string scenario_authoring;
    std::string scenario_templates;
    std::string python_verifier;
    std::string external_noise_truth;
    std::string challenge_command;
    std::string challenge_success_media_type;
    std::string customer_verification_command;
    std::vector<std::string> comparison_targets;
    std::vector<std::string> interval_targets;
    std::vector<std::string> interval_output_schemas;
    std::vector<std::string> delineation_targets;
    std::vector<std::string> delineation_output_schemas;
    std::vector<std::string> measurement_targets;
    std::vector<std::string> customer_output_schemas;
    std::string canonical_json;
};

bool parse_core_integration_contract(
    const std::string& json,
    CoreIntegrationContract& contract,
    std::string& error
);

bool linked_core_integration_contract(
    CoreIntegrationContract& contract,
    std::string& error
);

bool cli_core_integration_contract(
    const std::string& signal_synth_cli,
    CoreIntegrationContract& contract,
    std::string& error
);

bool validate_core_integration(
    const std::string& signal_synth_cli,
    CoreIntegrationContract& accepted,
    std::string& error
);

}  // namespace syn_sig_ra

#endif
