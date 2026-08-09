#include "syn_sig_ra/scenario_schema.h"

#include "ecg_scenario_json.h"

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
    const std::string legacy =
        "{\"schema_version\":2,\"scenario_id\":\"schema_test\","
        "\"name\":\"Schema test\",\"description\":\"\","
        "\"author\":\"Synsigra\",\"tags\":[\"test\"],"
        "\"duration_seconds\":2,\"sample_rate_hz\":100,\"seed\":12345,"
        "\"ecg\":{\"heart_rate_bpm\":70,\"rr_variability_seconds\":0,"
        "\"ectopic_every_n_beats\":0,"
        "\"second_degree_av_pattern\":\"unspecified\","
        "\"q_wave_territory\":\"unspecified\",\"rhythm_episodes\":[],"
        "\"flutter_conduction_pattern\":\"fixed\","
        "\"pacing_mode\":\"ventricular\","
        "\"pacing_non_capture_every_n_beats\":0,"
        "\"fidelity_policy\":\"allow_parameterized\","
        "\"conditions\":[{\"code\":\"NORM\",\"severity\":1}]},"
        "\"ppg\":{\"enabled\":false,\"pulse_delay_ms\":180,"
        "\"rise_time_ms\":120,\"decay_time_ms\":300,\"amplitude_au\":1,"
        "\"baseline_au\":0,\"dicrotic_delay_ms\":180,"
        "\"dicrotic_width_ms\":80,\"dicrotic_amplitude_ratio\":0.15}}";
    std::string current;
    std::vector<std::string> messages;
    require(
        syn_sig_ra::normalize_current_scenario_json(
            legacy, current, messages),
        "valid legacy fixture should normalize"
    );
    require(
        current.find("{\"schema_version\":9") == 0,
        "normalization should write schema version 9, got: " +
            current.substr(0, 64)
    );
    require(
        current.find("\"randomization\"") != std::string::npos &&
            current.find("\"physiology\"") != std::string::npos &&
            current.find("\"optical\"") != std::string::npos &&
            current.find("\"output\"") != std::string::npos,
        "normalization should materialize current-schema sections"
    );
    require(
        syn_sig_ra::scenario_json_uses_current_schema(current),
        "normalized output should parse as the current schema"
    );
    std::string repeated;
    require(
        syn_sig_ra::normalize_current_scenario_json(
            current, repeated, messages) && repeated == current,
        "current-schema normalization should be idempotent"
    );
    signal_synth::ecg_scenario_document parsed;
    signal_synth::ecg_scenario_json_result result;
    require(
        signal_synth::parse_ecg_scenario_json(current, parsed, result) &&
            parsed.schema_version == 9u && parsed.sample_count() == 200u,
        "normalized scenario should remain valid in the pinned core"
    );
    require(
        !syn_sig_ra::normalize_current_scenario_json(
            "{\"schema_version\":9}", repeated, messages) &&
            !messages.empty(),
        "invalid scenarios should not be normalized"
    );
    std::cout << "scenario schema tests passed\n";
    return EXIT_SUCCESS;
}
