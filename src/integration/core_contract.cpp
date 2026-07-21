#include "syn_sig_ra/core_contract.h"

#include "syn_sig_ra/build_info.h"
#include "synsigra_api.h"

#include <jansson.h>

#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

namespace {

const char kIntegration[] = "synsigra_core_integration_v7";
const char kCppFacade[] = "1.5.0";
const char kChallengePackage[] = "synsigra_challenge_package_v3";
const char kScoringManifest[] = "synsigra_scoring_manifest_v3";
const char kVerificationProtocol[] = "synsigra_verification_protocol_v2";
const char kSubmission[] = "synsigra_submission_v1";
const char kSubmissionFormats[] = "synsigra_submission_formats_v2";
const char kMeasurementValues[] = "synsigra_measurement_values_v2";
const char kMeasurementTruth[] = "synsigra_measurement_truth_v2";
const char kMeasurementScoring[] = "synsigra_measurement_score_v2";
const char kLocalVerification[] = "synsigra_local_verification_v3";
const char kAuthoring[] = "synsigra_authoring_v18";
const char kTemplates[] = "synsigra_templates_v5";
const char kPythonVerifier[] = "0.11.0";
const char kExternalNoiseTruth[] = "synsigra_external_noise_truth_v1";

bool exact_object(json_t* value, std::size_t size) {
    return json_is_object(value) && json_object_size(value) == size;
}

bool string_field(json_t* object, const char* key, std::string& value) {
    json_t* field = json_object_get(object, key);
    if (!json_is_string(field) || json_string_length(field) == 0) return false;
    value = json_string_value(field);
    return true;
}

bool exact_string(json_t* object, const char* key, const char* expected) {
    json_t* field = json_object_get(object, key);
    return json_is_string(field) && json_string_value(field) == std::string(expected);
}

bool exact_integer(json_t* object, const char* key, int expected) {
    json_t* field = json_object_get(object, key);
    return json_is_integer(field) && json_integer_value(field) == expected;
}

bool exact_boolean(json_t* object, const char* key, bool expected) {
    json_t* field = json_object_get(object, key);
    return json_is_boolean(field) &&
        static_cast<bool>(json_boolean_value(field)) == expected;
}

bool exact_strings(
    json_t* object,
    const char* key,
    const char* const* expected,
    std::size_t count,
    std::vector<std::string>* output = nullptr
) {
    json_t* values = json_object_get(object, key);
    if (!json_is_array(values) || json_array_size(values) != count) return false;
    if (output != nullptr) output->clear();
    for (std::size_t index = 0; index < count; ++index) {
        json_t* value = json_array_get(values, index);
        if (!json_is_string(value) || json_string_value(value) != std::string(expected[index]))
            return false;
        if (output != nullptr) output->push_back(expected[index]);
    }
    return true;
}

bool exact_integers(
    json_t* object,
    const char* key,
    const int* expected,
    std::size_t count
) {
    json_t* values = json_object_get(object, key);
    if (!json_is_array(values) || json_array_size(values) != count) return false;
    for (std::size_t index = 0; index < count; ++index) {
        json_t* value = json_array_get(values, index);
        if (!json_is_integer(value) || json_integer_value(value) != expected[index])
            return false;
    }
    return true;
}

bool supported(const syn_sig_ra::CoreIntegrationContract& value) {
    static const char expected_build[] =
        "signal_synth/" SYN_SIG_RA_EXPECTED_SIGNAL_SYNTH_COMMIT;
    return value.schema_version == 1 &&
        value.integration_contract == kIntegration &&
        value.generator_name == "signal_synth" &&
        value.generator_version == "0.10.0-dev" &&
        value.generator_git_commit == SYN_SIG_RA_EXPECTED_SIGNAL_SYNTH_COMMIT &&
        value.generator_build_identity == expected_build &&
        value.cpp_facade == kCppFacade &&
        value.pack_schema_version == 2 &&
        value.challenge_package == kChallengePackage &&
        value.scoring_manifest == kScoringManifest &&
        value.verification_protocol == kVerificationProtocol &&
        value.submission == kSubmission &&
        value.submission_formats == kSubmissionFormats &&
        value.measurement_values == kMeasurementValues &&
        value.measurement_truth == kMeasurementTruth &&
        value.measurement_scoring == kMeasurementScoring &&
        value.local_verification == kLocalVerification &&
        value.scenario_authoring == kAuthoring &&
        value.scenario_templates == kTemplates &&
        value.python_verifier == kPythonVerifier &&
        value.external_noise_truth == kExternalNoiseTruth &&
        value.challenge_command ==
            "signal-synth pack challenge <pack.json> --out <new-directory>" &&
        value.challenge_success_media_type == "application/json" &&
        value.customer_verification_command ==
            "synsigra-verify <challenge> <submission-directory> <result-directory>";
}

bool read_capture(int descriptor, std::string& output, std::string& error) {
    char buffer[4096];
    const ssize_t count = read(descriptor, buffer, sizeof(buffer));
    if (count > 0) {
        if (output.size() + static_cast<std::size_t>(count) > 1024u * 1024u) {
            error = "core contract output exceeded 1 MiB";
            return false;
        }
        output.append(buffer, static_cast<std::size_t>(count));
    }
    return count != 0;
}

bool run_contract_command(
    const std::string& cli,
    std::string& output,
    std::string& error
) {
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        error = "unable to allocate core contract pipes";
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        error = "unable to fork core contract command";
        return false;
    }
    if (child == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        execl(cli.c_str(), cli.c_str(), "contract", static_cast<char*>(nullptr));
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    bool out_open = true;
    bool err_open = true;
    std::string stderr_text;
    while ((out_open || err_open) && error.empty()) {
        pollfd fds[2] = {
            {out_pipe[0], static_cast<short>(out_open ? POLLIN | POLLHUP : 0), 0},
            {err_pipe[0], static_cast<short>(err_open ? POLLIN | POLLHUP : 0), 0}
        };
        const int result = poll(fds, 2, 5000);
        if (result < 0 && errno != EINTR) {
            error = "unable to read core contract command";
        } else if (result == 0) {
            error = "core contract command timed out";
            kill(child, SIGKILL);
        } else {
            if (out_open && fds[0].revents) out_open = read_capture(out_pipe[0], output, error);
            if (err_open && fds[1].revents) err_open = read_capture(err_pipe[0], stderr_text, error);
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (error.empty() && (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
        error = "core contract command failed";
    if (error.empty() && !stderr_text.empty())
        error = "core contract command wrote to stderr";
    return error.empty();
}

}  // namespace

namespace syn_sig_ra {

bool parse_core_integration_contract(
    const std::string& text,
    CoreIntegrationContract& contract,
    std::string& error
) {
    error.clear();
    json_error_t json_error;
    json_t* root = json_loadb(
        text.data(), text.size(), JSON_REJECT_DUPLICATES, &json_error);
    if (root == nullptr) {
        error = "invalid core integration JSON";
        return false;
    }
    json_t* schema = json_object_get(root, "schema_version");
    json_t* generator = json_object_get(root, "generator");
    json_t* contracts = json_object_get(root, "contracts");
    json_t* external_noise = json_object_get(root, "external_noise");
    json_t* scenario = json_object_get(root, "scenario");
    json_t* hrv = json_object_get(root, "hrv");
    json_t* cli = json_object_get(root, "cli");
    CoreIntegrationContract parsed;
    const bool shape = exact_object(root, 8) && json_is_integer(schema) &&
        exact_object(generator, 4) && exact_object(contracts, 15) &&
        exact_object(external_noise, 5) && exact_object(scenario, 2) &&
        exact_object(hrv, 5) && exact_object(cli, 10);
    parsed.schema_version = shape ? static_cast<int>(json_integer_value(schema)) : 0;
    bool valid = shape &&
        string_field(root, "contract", parsed.integration_contract) &&
        string_field(generator, "name", parsed.generator_name) &&
        string_field(generator, "version", parsed.generator_version) &&
        string_field(generator, "git_commit", parsed.generator_git_commit) &&
        string_field(generator, "build_identity", parsed.generator_build_identity) &&
        string_field(contracts, "cpp_facade", parsed.cpp_facade) &&
        exact_integer(contracts, "pack_schema_version", 2) &&
        string_field(contracts, "challenge_package", parsed.challenge_package) &&
        string_field(contracts, "scoring_manifest", parsed.scoring_manifest) &&
        string_field(contracts, "verification_protocol", parsed.verification_protocol) &&
        string_field(contracts, "submission", parsed.submission) &&
        string_field(contracts, "submission_formats", parsed.submission_formats) &&
        string_field(contracts, "measurement_values", parsed.measurement_values) &&
        string_field(contracts, "measurement_truth", parsed.measurement_truth) &&
        string_field(contracts, "measurement_scoring", parsed.measurement_scoring) &&
        string_field(contracts, "local_verification", parsed.local_verification) &&
        string_field(contracts, "scenario_authoring", parsed.scenario_authoring) &&
        string_field(contracts, "scenario_templates", parsed.scenario_templates) &&
        string_field(contracts, "python_verifier", parsed.python_verifier) &&
        string_field(contracts, "external_noise_truth", parsed.external_noise_truth) &&
        string_field(cli, "challenge_command", parsed.challenge_command) &&
        string_field(cli, "challenge_success_media_type", parsed.challenge_success_media_type) &&
        string_field(cli, "customer_verification_command", parsed.customer_verification_command);
    parsed.pack_schema_version = valid ? 2 : 0;

    static const char* comparison[] = {"r_peak", "ppg_systolic_peak", "ppg_pulse_onset", "ecg_beat_classification"};
    static const char* interval[] = {"rhythm_episode", "signal_quality"};
    static const char* interval_formats[] = {"interval_json_v1", "interval_csv_v1"};
    static const char* delineation[] = {"ecg_delineation"};
    static const char* point_formats[] = {"point_events_json_v1", "point_events_csv_v1"};
    static const char* measurements[] = {"rr_interval", "qtc", "hrv", "morphology_assertions", "ecg_ppg_alignment", "ppg_optical", "prv", "respiratory_rate", "rhythm_burden"};
    static const char* customer_formats[] = {"point_events_json_v1", "point_events_csv_v1", "interval_events_json_v1", "interval_events_csv_v1", "measurement_values_json_v2", "measurement_values_csv_v2"};
    static const char* redistribution[] = {"local_only", "rendered_output", "source_and_output"};
    static const int scenario_versions[] = {2, 3, 4, 5, 6, 7, 8, 9};
    static const char* hrv_metrics[] = {"mean_rr_seconds", "mean_heart_rate_bpm", "sdnn_seconds", "rmssd_seconds", "pnn50_percent", "sd1_seconds", "sd2_seconds", "sd1_sd2_ratio", "vlf_power_seconds2", "lf_power_seconds2", "hf_power_seconds2", "lf_hf_ratio", "lf_normalized_units", "hf_normalized_units", "total_power_seconds2"};
    valid = valid &&
        exact_strings(cli, "comparison_targets", comparison, 4, &parsed.comparison_targets) &&
        exact_strings(cli, "interval_targets", interval, 2, &parsed.interval_targets) &&
        exact_strings(cli, "interval_output_schemas", interval_formats, 2, &parsed.interval_output_schemas) &&
        exact_strings(cli, "delineation_targets", delineation, 1, &parsed.delineation_targets) &&
        exact_strings(cli, "delineation_output_schemas", point_formats, 2, &parsed.delineation_output_schemas) &&
        exact_strings(cli, "measurement_targets", measurements, 9, &parsed.measurement_targets) &&
        exact_strings(cli, "customer_output_schemas", customer_formats, 6, &parsed.customer_output_schemas) &&
        exact_integer(external_noise, "scenario_schema_version", 8) &&
        exact_string(external_noise, "asset_transport", "in_memory_csv_registry") &&
        exact_boolean(external_noise, "asset_bytes_in_challenge", false) &&
        exact_string(external_noise, "release_gate", "external_noise_truth.release_allowed") &&
        exact_strings(external_noise, "redistribution_modes", redistribution, 3) &&
        exact_integer(scenario, "latest_schema_version", 9) &&
        exact_integers(scenario, "supported_schema_versions", scenario_versions, 8) &&
        exact_integer(hrv, "scenario_schema_version", 9) &&
        exact_string(hrv, "metric_definition", "synsigra_hrv_metrics_v2") &&
        exact_string(hrv, "preprocessing_policy", "synsigra_nn_exclusion_v2") &&
        exact_string(hrv, "scoring_contract", "synsigra_measurement_score_v2") &&
        exact_strings(hrv, "metrics", hrv_metrics, 15);

    char* canonical = valid ? json_dumps(root, JSON_COMPACT | JSON_SORT_KEYS) : nullptr;
    if (canonical != nullptr) {
        parsed.canonical_json = canonical;
        free(canonical);
    } else {
        valid = false;
    }
    json_decref(root);
    if (!valid || !supported(parsed)) {
        error = "unsupported core integration contract";
        return false;
    }
    contract = parsed;
    return true;
}

bool linked_core_integration_contract(
    CoreIntegrationContract& contract,
    std::string& error
) {
    return parse_core_integration_contract(
        signal_synth::synsigra_integration_contract_json(), contract, error);
}

bool cli_core_integration_contract(
    const std::string& cli,
    CoreIntegrationContract& contract,
    std::string& error
) {
    std::string output;
    return run_contract_command(cli, output, error) &&
        parse_core_integration_contract(output, contract, error);
}

bool validate_core_integration(
    const std::string& cli,
    CoreIntegrationContract& accepted,
    std::string& error
) {
    CoreIntegrationContract linked;
    CoreIntegrationContract external;
    if (!linked_core_integration_contract(linked, error) ||
        !cli_core_integration_contract(cli, external, error)) return false;
    if (linked.canonical_json != external.canonical_json) {
        error = "linked core and signal-synth CLI contracts differ";
        return false;
    }
    accepted = linked;
    return true;
}

}  // namespace syn_sig_ra
