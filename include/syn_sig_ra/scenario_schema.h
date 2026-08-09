#ifndef SYN_SIG_RA_SCENARIO_SCHEMA_H
#define SYN_SIG_RA_SCENARIO_SCHEMA_H

#include <string>
#include <vector>

namespace syn_sig_ra {

const unsigned int kCurrentScenarioSchemaVersion = 9u;

// Parses any scenario version understood by the pinned core, then writes the
// same scenario using the single schema version shipped by this SaaS.
bool normalize_current_scenario_json(
    const std::string& input,
    std::string& canonical_json,
    std::vector<std::string>& messages
);

bool scenario_json_uses_current_schema(const std::string& input);

}  // namespace syn_sig_ra

#endif
