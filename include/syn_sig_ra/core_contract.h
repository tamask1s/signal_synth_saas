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
    std::string challenge_package;
    std::string scoring_manifest;
    std::string scenario_authoring;
    std::string scenario_templates;
    std::string challenge_command;
    std::string challenge_success_media_type;
    std::vector<std::string> comparison_targets;
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
