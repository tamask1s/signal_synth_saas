#include "syn_sig_ra/core_contract.h"

#include "synsigra_api.h"

#include <jansson.h>

#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

namespace {

const char kIntegration[] = "synsigra_core_integration_v1";
const char kCppFacade[] = "1.0.0";
const char kChallengePackage[] = "synsigra_challenge_package_v1";
const char kScoringManifest[] = "synsigra_scoring_manifest_v1";
const char kAuthoring[] = "synsigra_authoring_v6";
const char kTemplates[] = "synsigra_templates_v3";

bool exact_object(json_t* value, std::size_t size) {
    return json_is_object(value) && json_object_size(value) == size;
}

bool string_field(json_t* object, const char* key, std::string& value) {
    json_t* field = json_object_get(object, key);
    if (!json_is_string(field) || json_string_length(field) == 0) return false;
    value = json_string_value(field);
    return true;
}

bool supported(const syn_sig_ra::CoreIntegrationContract& value) {
    static const char* targets[] = {
        "r_peak", "ppg_systolic_peak", "ppg_pulse_onset",
        "ecg_beat_classification"
    };
    if (value.schema_version != 1 ||
        value.integration_contract != kIntegration ||
        value.generator_name != "signal_synth" ||
        value.cpp_facade != kCppFacade ||
        value.challenge_package != kChallengePackage ||
        value.scoring_manifest != kScoringManifest ||
        value.scenario_authoring != kAuthoring ||
        value.scenario_templates != kTemplates ||
        value.challenge_command !=
            "signal-synth pack challenge <pack.json> --out <new-directory>" ||
        value.challenge_success_media_type != "application/json" ||
        value.comparison_targets.size() != 4) return false;
    for (std::size_t i = 0; i < 4; ++i) {
        if (value.comparison_targets[i] != targets[i]) return false;
    }
    return true;
}

bool same_producer(
    const syn_sig_ra::CoreIntegrationContract& left,
    const syn_sig_ra::CoreIntegrationContract& right
) {
    return left.integration_contract == right.integration_contract &&
        left.generator_name == right.generator_name &&
        left.generator_version == right.generator_version &&
        left.generator_git_commit == right.generator_git_commit &&
        left.generator_build_identity == right.generator_build_identity &&
        left.cpp_facade == right.cpp_facade &&
        left.challenge_package == right.challenge_package &&
        left.scoring_manifest == right.scoring_manifest &&
        left.scenario_authoring == right.scenario_authoring &&
        left.scenario_templates == right.scenario_templates;
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
        execl(cli.c_str(), cli.c_str(), "contract", static_cast<char*>(0));
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
    close(out_pipe[0]); close(err_pipe[0]);
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
    if (root == 0) {
        error = "invalid core integration JSON";
        return false;
    }
    json_t* schema = json_object_get(root, "schema_version");
    json_t* generator = json_object_get(root, "generator");
    json_t* contracts = json_object_get(root, "contracts");
    json_t* cli = json_object_get(root, "cli");
    CoreIntegrationContract parsed;
    const bool shape = exact_object(root, 5) && json_is_integer(schema) &&
        exact_object(generator, 4) && exact_object(contracts, 5) &&
        exact_object(cli, 3);
    parsed.schema_version = shape ? static_cast<int>(json_integer_value(schema)) : 0;
    bool valid = shape &&
        string_field(root, "contract", parsed.integration_contract) &&
        string_field(generator, "name", parsed.generator_name) &&
        string_field(generator, "version", parsed.generator_version) &&
        string_field(generator, "git_commit", parsed.generator_git_commit) &&
        string_field(generator, "build_identity", parsed.generator_build_identity) &&
        string_field(contracts, "cpp_facade", parsed.cpp_facade) &&
        string_field(contracts, "challenge_package", parsed.challenge_package) &&
        string_field(contracts, "scoring_manifest", parsed.scoring_manifest) &&
        string_field(contracts, "scenario_authoring", parsed.scenario_authoring) &&
        string_field(contracts, "scenario_templates", parsed.scenario_templates) &&
        string_field(cli, "challenge_command", parsed.challenge_command) &&
        string_field(cli, "challenge_success_media_type", parsed.challenge_success_media_type);
    json_t* targets = shape ? json_object_get(cli, "comparison_targets") : 0;
    if (!json_is_array(targets)) valid = false;
    if (valid) {
        for (std::size_t i = 0; i < json_array_size(targets); ++i) {
            json_t* target = json_array_get(targets, i);
            if (!json_is_string(target) || json_string_length(target) == 0) {
                valid = false;
                break;
            }
            parsed.comparison_targets.push_back(json_string_value(target));
        }
    }
    char* canonical = valid ? json_dumps(root, JSON_COMPACT | JSON_SORT_KEYS) : 0;
    if (canonical != 0) {
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
    if (!same_producer(linked, external)) {
        error = "linked core and signal-synth CLI producer identities differ";
        return false;
    }
    accepted = linked;
    return true;
}

}  // namespace syn_sig_ra
