#include "syn_sig_ra/scenario_schema.h"

#include "ecg_scenario_json.h"

namespace {

void append_messages(
    const signal_synth::ecg_scenario_json_result& result,
    std::vector<std::string>& messages
) {
    for (std::vector<signal_synth::ecg_scenario_json_message>::const_iterator it =
             result.messages.begin(); it != result.messages.end(); ++it) {
        messages.push_back(it->path + ": " + it->message);
    }
}

}  // namespace

namespace syn_sig_ra {

bool normalize_current_scenario_json(
    const std::string& input,
    std::string& canonical_json,
    std::vector<std::string>& messages
) {
    canonical_json.clear();
    messages.clear();
    signal_synth::ecg_scenario_document document;
    signal_synth::ecg_scenario_json_result parsed;
    if (!signal_synth::parse_ecg_scenario_json(input, document, parsed)) {
        append_messages(parsed, messages);
        return false;
    }
    const unsigned int source_version = document.schema_version;
    const unsigned long long seed =
        document.ecg.seed() % 9000000000000000ULL;
    if (source_version < 3u) {
        document.ppg.seed = seed + 101u;
        document.randomization.seed = seed + 102u;
        document.physiology.seed = seed + 103u;
    }
    if (source_version < 6u) {
        document.ppg.optical.red.seed = seed + 104u;
        document.ppg.optical.infrared.seed = seed + 105u;
    }
    document.schema_version = kCurrentScenarioSchemaVersion;
    signal_synth::ecg_scenario_json_result written;
    if (!signal_synth::write_ecg_scenario_json(document, written)) {
        append_messages(written, messages);
        return false;
    }
    canonical_json = written.canonical_json;
    return true;
}

bool scenario_json_uses_current_schema(const std::string& input) {
    signal_synth::ecg_scenario_document document;
    signal_synth::ecg_scenario_json_result result;
    return signal_synth::parse_ecg_scenario_json(input, document, result) &&
        document.schema_version == kCurrentScenarioSchemaVersion;
}

}  // namespace syn_sig_ra
