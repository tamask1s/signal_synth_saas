#include "syn_sig_ra/pack_catalog.h"

#include "ecg_pack.h"

#include <jansson.h>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

const std::size_t kMaximumPackBytes = 16u * 1024u * 1024u;

std::string escape_json(const std::string& value) {
    std::ostringstream output;
    output << '"';
    for (std::string::const_iterator it = value.begin(); it != value.end();
         ++it) {
        const unsigned char character = static_cast<unsigned char>(*it);
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20) {
                output << "\\u00"
                       << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned int>(character)
                       << std::dec;
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
    return output.str();
}

bool has_json_extension(const std::string& filename) {
    const std::string extension(".json");
    return filename.size() > extension.size() &&
           filename.compare(
               filename.size() - extension.size(),
               extension.size(),
               extension
           ) == 0;
}

bool read_file(
    const std::string& path,
    std::string& content,
    std::string& error
) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        error = "unable to open pack file";
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0 ||
        static_cast<unsigned long long>(size) > kMaximumPackBytes) {
        error = "pack file exceeds the 16 MiB limit";
        return false;
    }
    input.seekg(0, std::ios::beg);
    content.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        input.read(&content[0], size);
    }
    if (!input && size > 0) {
        error = "unable to read complete pack file";
        return false;
    }
    return true;
}

bool pack_id_less(
    const syn_sig_ra::PackSummary& left,
    const syn_sig_ra::PackSummary& right
) {
    return left.pack_id < right.pack_id;
}

std::string string_array_json(const std::vector<std::string>& values) {
    std::string output("[");
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output += ',';
        }
        output += escape_json(values[index]);
    }
    output += ']';
    return output;
}

std::string size_to_string(std::size_t value) {
    std::ostringstream output;
    output << value;
    return output.str();
}

bool is_semantic_version(const std::string& value) {
    int dots = 0;
    bool digit = false;
    for (std::string::const_iterator it = value.begin(); it != value.end(); ++it) {
        if (*it == '.') {
            if (!digit || ++dots > 2) return false;
            digit = false;
        } else if (*it >= '0' && *it <= '9') {
            digit = true;
        } else {
            return false;
        }
    }
    return digit && (dots == 1 || dots == 2);
}

bool load_product_metadata(
    const std::string& path,
    syn_sig_ra::PackSummary& pack,
    std::string& error
) {
    std::string content;
    if (!read_file(path, content, error)) return false;
    json_error_t parse_error;
    json_t* root = json_loadb(
        content.data(), content.size(), JSON_REJECT_DUPLICATES, &parse_error
    );
    if (!json_is_object(root)) {
        if (root != nullptr) json_decref(root);
        error = "pack product metadata must be a JSON object";
        return false;
    }
    json_t* schema = json_object_get(root, "schema_version");
    json_t* pack_id = json_object_get(root, "pack_id");
    json_t* version = json_object_get(root, "version");
    json_t* status = json_object_get(root, "release_status");
    json_t* released = json_object_get(root, "released_at");
    json_t* expected = json_object_get(root, "expected_pack_fingerprint");
    json_t* contract = json_object_get(root, "generator_contract");
    json_t* compatible = json_object_get(root, "compatible_generator_versions");
    json_t* deprecation = json_object_get(root, "deprecation_message");
    json_t* changelog = json_object_get(root, "changelog");
    const bool scalar_valid =
        json_object_size(root) == 10 &&
        json_is_integer(schema) && json_integer_value(schema) == 1 &&
        json_is_string(pack_id) && json_is_string(version) &&
        json_is_string(status) && json_is_string(released) &&
        json_is_string(expected) && json_is_string(contract) &&
        json_is_array(compatible) && json_array_size(compatible) > 0 &&
        json_is_string(deprecation) && json_is_array(changelog) &&
        json_array_size(changelog) > 0;
    if (!scalar_valid ||
        pack.pack_id != json_string_value(pack_id) ||
        pack.version != json_string_value(version) ||
        !is_semantic_version(pack.version) ||
        pack.pack_fingerprint != json_string_value(expected) ||
        (std::string(json_string_value(status)) != "beta" &&
         std::string(json_string_value(status)) != "stable" &&
         std::string(json_string_value(status)) != "deprecated")) {
        json_decref(root);
        error = "pack product metadata does not match the validated release";
        return false;
    }
    pack.release_status = json_string_value(status);
    pack.released_at = json_string_value(released);
    pack.generator_contract = json_string_value(contract);
    pack.deprecation_message = json_string_value(deprecation);
    pack.compatible_generator_versions.clear();
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(compatible, index, item) {
        if (!json_is_string(item)) {
            json_decref(root);
            error = "compatible generator versions must be strings";
            return false;
        }
        pack.compatible_generator_versions.push_back(json_string_value(item));
    }
    pack.changelog.clear();
    bool current_version_documented = false;
    json_array_foreach(changelog, index, item) {
        json_t* item_version = json_object_get(item, "version");
        json_t* item_date = json_object_get(item, "date");
        json_t* item_summary = json_object_get(item, "summary");
        if (!json_is_object(item) || json_object_size(item) != 3 ||
            !json_is_string(item_version) ||
            !json_is_string(item_date) || !json_is_string(item_summary) ||
            !is_semantic_version(json_string_value(item_version))) {
            json_decref(root);
            error = "pack changelog entries are invalid";
            return false;
        }
        syn_sig_ra::PackChangelogEntry entry;
        entry.version = json_string_value(item_version);
        entry.date = json_string_value(item_date);
        entry.summary = json_string_value(item_summary);
        if (entry.version == pack.version) {
            current_version_documented = true;
        }
        pack.changelog.push_back(entry);
    }
    if (!current_version_documented) {
        json_decref(root);
        error = "current pack version requires a changelog entry";
        return false;
    }
    if (pack.release_status == "deprecated" &&
        pack.deprecation_message.empty()) {
        json_decref(root);
        error = "deprecated packs require a deprecation message";
        return false;
    }
    json_decref(root);
    return true;
}

std::string json_string_or_empty(json_t* value) {
    return json_is_string(value) ? json_string_value(value) : "";
}

bool read_string_array(
    json_t* object,
    const char* key,
    bool required,
    std::vector<std::string>& output,
    std::string& error
) {
    output.clear();
    json_t* array = json_object_get(object, key);
    if (array == nullptr && !required) return true;
    if (!json_is_array(array)) {
        error = std::string("curated catalog field must be a string array: ") + key;
        return false;
    }
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(array, index, item) {
        if (!json_is_string(item)) {
            error = std::string("curated catalog array contains a non-string: ") + key;
            return false;
        }
        output.push_back(json_string_value(item));
    }
    return true;
}

bool read_int_array(
    json_t* object,
    const char* key,
    std::vector<int>& output,
    std::string& error
) {
    output.clear();
    json_t* array = json_object_get(object, key);
    if (!json_is_array(array)) {
        error = std::string("curated catalog field must be an integer array: ") + key;
        return false;
    }
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(array, index, item) {
        if (!json_is_integer(item)) {
            error = std::string("curated catalog array contains a non-integer: ") + key;
            return false;
        }
        output.push_back(static_cast<int>(json_integer_value(item)));
    }
    return true;
}

long long integer_field(json_t* object, const char* key) {
    json_t* value = json_object_get(object, key);
    return json_is_integer(value) ? static_cast<long long>(json_integer_value(value)) : 0;
}

int int_field(json_t* object, const char* key) {
    return static_cast<int>(integer_field(object, key));
}

bool read_target_array(
    json_t* pack_object,
    const char* key,
    bool expected_scoreable,
    std::vector<syn_sig_ra::PackTargetSummary>& output,
    std::string& error
) {
    output.clear();
    json_t* array = json_object_get(pack_object, key);
    if (!json_is_array(array)) {
        error = std::string("curated catalog field must be a target array: ") + key;
        return false;
    }
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(array, index, item) {
        if (!json_is_object(item)) {
            error = std::string("curated catalog target entry must be an object: ") + key;
            return false;
        }
        json_t* target_value = json_object_get(item, "target");
        json_t* scoreable_value = json_object_get(item, "scoreable");
        json_t* case_count_value = json_object_get(item, "case_count");
        if (!json_is_string(target_value) ||
            !json_is_boolean(scoreable_value) ||
            !json_is_integer(case_count_value) ||
            json_boolean_value(scoreable_value) != expected_scoreable) {
            error = std::string("curated catalog target entry is invalid: ") + key;
            return false;
        }
        syn_sig_ra::PackTargetSummary target;
        target.target = json_string_value(target_value);
        target.support = json_string_or_empty(json_object_get(item, "support"));
        target.scoreable = expected_scoreable;
        target.score_type = json_string_or_empty(json_object_get(item, "score_type"));
        target.description = json_string_or_empty(json_object_get(item, "description"));
        target.primary_metric = json_string_or_empty(json_object_get(item, "primary_metric"));
        target.case_count = static_cast<int>(json_integer_value(case_count_value));
        if (!read_string_array(item, "case_ids", true, target.case_ids, error)) {
            return false;
        }
        std::vector<std::string> formats;
        if (!read_string_array(item, "accepted_detection_formats", false, formats, error)) {
            return false;
        }
        target.accepted_formats.insert(
            target.accepted_formats.end(), formats.begin(), formats.end());
        if (!read_string_array(item, "accepted_user_output_formats", false, formats, error)) {
            return false;
        }
        target.accepted_formats.insert(
            target.accepted_formats.end(), formats.begin(), formats.end());
        if (!read_string_array(item, "reference_artifacts", false, target.reference_artifacts, error)) {
            return false;
        }
        json_t* tolerance = json_object_get(item, "default_tolerance_seconds");
        if (json_is_number(tolerance)) {
            target.has_default_tolerance_seconds = true;
            target.default_tolerance_seconds = json_number_value(tolerance);
        }
        output.push_back(target);
    }
    return true;
}

bool attach_case_metadata(
    json_t* pack_object,
    syn_sig_ra::PackSummary& pack,
    std::string& error
) {
    json_t* cases = json_object_get(pack_object, "cases");
    if (!json_is_array(cases)) {
        error = "curated catalog pack cases must be an array";
        return false;
    }
    std::map<std::string, json_t*> by_case_id;
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(cases, index, item) {
        json_t* case_id = json_object_get(item, "case_id");
        if (!json_is_object(item) || !json_is_string(case_id)) {
            error = "curated catalog case entries are invalid";
            return false;
        }
        by_case_id[json_string_value(case_id)] = item;
    }
    for (std::vector<syn_sig_ra::PackScenarioSummary>::iterator it =
             pack.scenarios.begin(); it != pack.scenarios.end(); ++it) {
        std::map<std::string, json_t*>::const_iterator found =
            by_case_id.find(it->scenario_id);
        if (found == by_case_id.end()) {
            error = "curated catalog is missing scenario case metadata";
            return false;
        }
        json_t* case_object = found->second;
        if (!read_string_array(case_object, "scoreable_targets", false, it->scoreable_targets, error) ||
            !read_string_array(case_object, "reference_only_targets", false, it->reference_only_targets, error)) {
            return false;
        }
        it->duration_seconds = int_field(case_object, "duration_seconds");
        it->sampling_rate_hz = int_field(case_object, "sampling_rate_hz");
        it->channel_count = int_field(case_object, "channel_count");
        it->sample_count = integer_field(case_object, "sample_count");
        it->estimated_package_bytes = integer_field(case_object, "estimated_package_bytes");
    }
    return true;
}

bool load_curated_catalog_metadata(
    const std::string& path,
    syn_sig_ra::PackSummary& pack,
    std::string& error
) {
    std::string content;
    if (!read_file(path, content, error)) {
        error = "curated pack catalog metadata is unavailable";
        return false;
    }
    json_error_t parse_error;
    json_t* root = json_loadb(
        content.data(), content.size(), JSON_REJECT_DUPLICATES, &parse_error
    );
    if (!json_is_object(root)) {
        if (root != nullptr) json_decref(root);
        error = "curated pack catalog metadata must be a JSON object";
        return false;
    }
    json_t* metadata_type = json_object_get(root, "metadata_type");
    json_t* packs = json_object_get(root, "packs");
    if (!json_is_string(metadata_type) ||
        std::string(json_string_value(metadata_type)) != "synsigra_curated_pack_catalog" ||
        !json_is_array(packs)) {
        json_decref(root);
        error = "curated pack catalog metadata has an invalid header";
        return false;
    }
    json_t* match = nullptr;
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(packs, index, item) {
        json_t* pack_id = json_object_get(item, "pack_id");
        if (json_is_object(item) && json_is_string(pack_id) &&
            pack.pack_id == json_string_value(pack_id)) {
            match = item;
            break;
        }
    }
    if (match == nullptr) {
        json_decref(root);
        error = "curated pack catalog does not contain pack metadata";
        return false;
    }
    json_t* version = json_object_get(match, "version");
    json_t* name = json_object_get(match, "name");
    json_t* scoring_mode = json_object_get(match, "scoring_mode");
    if (!json_is_string(version) || !json_is_string(name) ||
        !json_is_string(scoring_mode) ||
        pack.version != json_string_value(version) ||
        pack.display_name != json_string_value(name)) {
        json_decref(root);
        error = "curated pack catalog metadata does not match pack/product metadata";
        return false;
    }
    pack.scoring_mode = json_string_value(scoring_mode);
    json_t* recommended_profile = json_object_get(match, "recommended_profile");
    pack.recommended_profile =
        json_is_string(recommended_profile) ? json_string_value(recommended_profile) : "";
    if (!read_target_array(match, "scoreable_targets", true, pack.scoreable_targets, error) ||
        !read_target_array(match, "reference_only_targets", false, pack.reference_only_targets, error) ||
        !read_string_array(match, "detector_output_schemas", true, pack.detector_output_schemas, error) ||
        !read_string_array(match, "supported_threshold_profiles", true, pack.supported_threshold_profiles, error) ||
        !read_string_array(match, "recommended_for", true, pack.recommended_for, error) ||
        !read_string_array(match, "not_recommended_for", true, pack.not_recommended_for, error) ||
        !read_string_array(match, "difficulty", true, pack.difficulty, error) ||
        !read_string_array(match, "feature_tags", true, pack.feature_tags, error) ||
        !read_string_array(match, "modality", true, pack.modality, error) ||
        !read_int_array(match, "sampling_rates_hz", pack.sampling_rates_hz, error)) {
        json_decref(root);
        return false;
    }
    json_t* duration = json_object_get(match, "duration");
    json_t* estimated = json_object_get(match, "estimated_package");
    json_t* channels = json_object_get(match, "channels");
    json_t* compatibility = json_object_get(match, "generator_compatibility");
    if (!json_is_object(duration) || !json_is_object(estimated) ||
        !json_is_object(channels) || !json_is_object(compatibility)) {
        json_decref(root);
        error = "curated pack catalog numeric summaries are invalid";
        return false;
    }
    pack.minimum_case_seconds = int_field(duration, "minimum_case_seconds");
    pack.maximum_case_seconds = int_field(duration, "maximum_case_seconds");
    pack.total_seconds = int_field(duration, "total_seconds");
    pack.minimum_channel_count = int_field(channels, "minimum_channel_count");
    pack.maximum_channel_count = int_field(channels, "maximum_channel_count");
    pack.estimated_package_bytes = integer_field(estimated, "bytes");
    pack.peak_memory_bytes = integer_field(estimated, "peak_memory_bytes");
    pack.package_size_class = json_string_or_empty(json_object_get(estimated, "size_class"));
    pack.local_verifier_min_version =
        json_string_or_empty(json_object_get(compatibility, "local_verifier_min_version"));
    pack.challenge_package_contract =
        json_string_or_empty(json_object_get(compatibility, "challenge_package_contract"));
    pack.scoring_manifest_contract =
        json_string_or_empty(json_object_get(compatibility, "scoring_manifest_contract"));

    pack.output_artifact_roles.clear();
    json_t* artifacts = json_object_get(match, "output_artifacts");
    if (json_is_array(artifacts)) {
        json_array_foreach(artifacts, index, item) {
            json_t* role = json_object_get(item, "role");
            if (json_is_object(item) && json_is_string(role)) {
                pack.output_artifact_roles.push_back(json_string_value(role));
            }
        }
    }
    json_t* ui = json_object_get(match, "ui");
    if (json_is_object(ui)) {
        if (!read_string_array(ui, "primary_badges", false, pack.primary_badges, error)) {
            json_decref(root);
            return false;
        }
    }
    if (!attach_case_metadata(match, pack, error)) {
        json_decref(root);
        return false;
    }
    json_decref(root);
    return true;
}

json_t* string_array_json_value(const std::vector<std::string>& values) {
    json_t* array = json_array();
    for (std::vector<std::string>::const_iterator it = values.begin();
         it != values.end(); ++it) {
        json_array_append_new(array, json_string(it->c_str()));
    }
    return array;
}

json_t* int_array_json_value(const std::vector<int>& values) {
    json_t* array = json_array();
    for (std::vector<int>::const_iterator it = values.begin();
         it != values.end(); ++it) {
        json_array_append_new(array, json_integer(*it));
    }
    return array;
}

json_t* target_summary_json_value(const syn_sig_ra::PackTargetSummary& target) {
    json_t* root = json_object();
    json_object_set_new(root, "target", json_string(target.target.c_str()));
    json_object_set_new(root, "support", json_string(target.support.c_str()));
    json_object_set_new(root, "scoreable", json_boolean(target.scoreable));
    json_object_set_new(root, "score_type", json_string(target.score_type.c_str()));
    json_object_set_new(root, "description", json_string(target.description.c_str()));
    json_object_set_new(root, "primary_metric", json_string(target.primary_metric.c_str()));
    json_object_set_new(root, "case_count", json_integer(target.case_count));
    json_object_set_new(root, "case_ids", string_array_json_value(target.case_ids));
    json_object_set_new(root, "accepted_formats", string_array_json_value(target.accepted_formats));
    json_object_set_new(root, "reference_artifacts", string_array_json_value(target.reference_artifacts));
    if (target.has_default_tolerance_seconds) {
        json_object_set_new(root, "default_tolerance_seconds", json_real(target.default_tolerance_seconds));
    } else {
        json_object_set_new(root, "default_tolerance_seconds", json_null());
    }
    return root;
}

json_t* target_array_json_value(
    const std::vector<syn_sig_ra::PackTargetSummary>& targets
) {
    json_t* array = json_array();
    for (std::vector<syn_sig_ra::PackTargetSummary>::const_iterator it =
             targets.begin(); it != targets.end(); ++it) {
        json_array_append_new(array, target_summary_json_value(*it));
    }
    return array;
}

json_t* scenario_summary_json_value(const syn_sig_ra::PackScenarioSummary& scenario) {
    json_t* root = json_object();
    json_object_set_new(root, "scenario_id", json_string(scenario.scenario_id.c_str()));
    json_object_set_new(root, "targets", string_array_json_value(scenario.targets));
    json_object_set_new(root, "scoreable_targets", string_array_json_value(scenario.scoreable_targets));
    json_object_set_new(root, "reference_only_targets", string_array_json_value(scenario.reference_only_targets));
    json_object_set_new(root, "duration_seconds", json_integer(scenario.duration_seconds));
    json_object_set_new(root, "sampling_rate_hz", json_integer(scenario.sampling_rate_hz));
    json_object_set_new(root, "channel_count", json_integer(scenario.channel_count));
    json_object_set_new(root, "sample_count", json_integer(static_cast<json_int_t>(scenario.sample_count)));
    json_object_set_new(root, "estimated_package_bytes", json_integer(static_cast<json_int_t>(scenario.estimated_package_bytes)));
    return root;
}

}  // namespace

namespace syn_sig_ra {

bool is_valid_pack_id(const std::string& pack_id) {
    if (pack_id.empty() || pack_id.size() > 128 ||
        pack_id == "." || pack_id == ".." ||
        pack_id.find("..") != std::string::npos) {
        return false;
    }
    for (std::string::const_iterator it = pack_id.begin();
         it != pack_id.end();
         ++it) {
        const char character = *it;
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '_' ||
              character == '-' ||
              character == '.')) {
            return false;
        }
    }
    return true;
}

std::string pack_summary_json(const PackSummary& pack) {
    json_t* root = json_object();
    json_object_set_new(root, "pack_id", json_string(pack.pack_id.c_str()));
    json_object_set_new(root, "display_name", json_string(pack.display_name.c_str()));
    json_object_set_new(root, "version", json_string(pack.version.c_str()));
    json_object_set_new(root, "description", json_string(pack.description.c_str()));
    json_object_set_new(root, "targets", string_array_json_value(pack.targets));
    json_t* scenarios = json_array();
    for (std::vector<PackScenarioSummary>::const_iterator it =
             pack.scenarios.begin(); it != pack.scenarios.end(); ++it) {
        json_array_append_new(scenarios, scenario_summary_json_value(*it));
    }
    json_object_set_new(root, "scenarios", scenarios);
    json_object_set_new(root, "scenario_count", json_integer(static_cast<json_int_t>(pack.scenarios.size())));
    json_object_set_new(root, "release_status", json_string(pack.release_status.c_str()));
    json_object_set_new(root, "released_at", json_string(pack.released_at.c_str()));
    json_object_set_new(root, "generator_contract", json_string(pack.generator_contract.c_str()));
    json_object_set_new(root, "compatible_generator_versions", string_array_json_value(pack.compatible_generator_versions));
    json_object_set_new(root, "deprecation_message", json_string(pack.deprecation_message.c_str()));
    json_t* changelog = json_array();
    for (std::vector<PackChangelogEntry>::const_iterator it =
             pack.changelog.begin(); it != pack.changelog.end(); ++it) {
        json_t* entry = json_object();
        json_object_set_new(entry, "version", json_string(it->version.c_str()));
        json_object_set_new(entry, "date", json_string(it->date.c_str()));
        json_object_set_new(entry, "summary", json_string(it->summary.c_str()));
        json_array_append_new(changelog, entry);
    }
    json_object_set_new(root, "changelog", changelog);
    json_object_set_new(root, "pack_fingerprint", json_string(pack.pack_fingerprint.c_str()));
    json_object_set_new(root, "scoring_mode", json_string(pack.scoring_mode.c_str()));
    json_object_set_new(root, "scoreable_targets", target_array_json_value(pack.scoreable_targets));
    json_object_set_new(root, "reference_only_targets", target_array_json_value(pack.reference_only_targets));
    json_object_set_new(root, "detector_output_schemas", string_array_json_value(pack.detector_output_schemas));
    if (pack.recommended_profile.empty()) {
        json_object_set_new(root, "recommended_profile", json_null());
    } else {
        json_object_set_new(root, "recommended_profile", json_string(pack.recommended_profile.c_str()));
    }
    json_object_set_new(root, "supported_threshold_profiles", string_array_json_value(pack.supported_threshold_profiles));
    json_object_set_new(root, "recommended_for", string_array_json_value(pack.recommended_for));
    json_object_set_new(root, "not_recommended_for", string_array_json_value(pack.not_recommended_for));
    json_object_set_new(root, "difficulty", string_array_json_value(pack.difficulty));
    json_object_set_new(root, "feature_tags", string_array_json_value(pack.feature_tags));
    json_object_set_new(root, "modality", string_array_json_value(pack.modality));
    json_t* duration = json_object();
    json_object_set_new(duration, "minimum_case_seconds", json_integer(pack.minimum_case_seconds));
    json_object_set_new(duration, "maximum_case_seconds", json_integer(pack.maximum_case_seconds));
    json_object_set_new(duration, "total_seconds", json_integer(pack.total_seconds));
    json_object_set_new(root, "duration", duration);
    json_object_set_new(root, "sampling_rates_hz", int_array_json_value(pack.sampling_rates_hz));
    json_t* channels = json_object();
    json_object_set_new(channels, "minimum_channel_count", json_integer(pack.minimum_channel_count));
    json_object_set_new(channels, "maximum_channel_count", json_integer(pack.maximum_channel_count));
    json_object_set_new(root, "channels", channels);
    json_t* estimated = json_object();
    json_object_set_new(estimated, "bytes", json_integer(static_cast<json_int_t>(pack.estimated_package_bytes)));
    json_object_set_new(estimated, "peak_memory_bytes", json_integer(static_cast<json_int_t>(pack.peak_memory_bytes)));
    json_object_set_new(estimated, "size_class", json_string(pack.package_size_class.c_str()));
    json_object_set_new(root, "estimated_package", estimated);
    json_t* compatibility = json_object();
    json_object_set_new(compatibility, "local_verifier_min_version", json_string(pack.local_verifier_min_version.c_str()));
    json_object_set_new(compatibility, "challenge_package_contract", json_string(pack.challenge_package_contract.c_str()));
    json_object_set_new(compatibility, "scoring_manifest_contract", json_string(pack.scoring_manifest_contract.c_str()));
    json_object_set_new(root, "generator_compatibility", compatibility);
    json_object_set_new(root, "output_artifact_roles", string_array_json_value(pack.output_artifact_roles));
    json_object_set_new(root, "primary_badges", string_array_json_value(pack.primary_badges));
    char* dumped = json_dumps(root, JSON_COMPACT);
    std::string output = dumped == nullptr ? "{}" : dumped;
    if (dumped != nullptr) free(dumped);
    json_decref(root);
    return output;
}

PackCatalog::PackCatalog(const std::string& pack_root)
    : pack_root_(pack_root) {}

PackLookupStatus PackCatalog::load_file(
    const std::string& expected_pack_id,
    const std::string& path,
    PackSummary& pack,
    std::string& error
) const {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0) {
        return PackLookupStatus::not_found;
    }
    if (!S_ISREG(information.st_mode)) {
        error = "pack entries must be regular files, not links or directories";
        return PackLookupStatus::catalog_error;
    }

    std::string json;
    if (!read_file(path, json, error)) {
        return PackLookupStatus::catalog_error;
    }

    signal_synth::ecg_pack_manifest manifest;
    signal_synth::ecg_pack_json_result result;
    if (!signal_synth::parse_ecg_pack_json(json, manifest, result)) {
        error = "pack JSON failed authoritative signal_synth validation";
        if (!result.messages.empty()) {
            error += ": ";
            error += signal_synth::ecg_pack_json_message_code_name(
                result.messages[0].code
            );
            error += " at ";
            error += result.messages[0].path;
        }
        return PackLookupStatus::catalog_error;
    }
    if (manifest.pack_id != expected_pack_id) {
        error = "pack_id must match its catalog filename";
        return PackLookupStatus::catalog_error;
    }

    pack.pack_id = manifest.pack_id;
    pack.display_name = manifest.name;
    pack.version = manifest.version;
    pack.description = manifest.description;
    pack.targets = manifest.targets;
    pack.scenarios.clear();
    for (std::vector<signal_synth::ecg_pack_scenario>::const_iterator it =
             manifest.scenarios.begin();
         it != manifest.scenarios.end();
         ++it) {
        PackScenarioSummary scenario;
        scenario.scenario_id = it->id;
        scenario.targets = it->targets;
        pack.scenarios.push_back(scenario);
    }
    pack.pack_fingerprint = result.pack_fingerprint;
    if (!load_product_metadata(
            pack_root_ + "/" + expected_pack_id + ".product", pack, error)) {
        return PackLookupStatus::catalog_error;
    }
    if (!load_curated_catalog_metadata(
            pack_root_ + "/curated_pack_metadata_v1.catalog", pack, error)) {
        return PackLookupStatus::catalog_error;
    }
    return PackLookupStatus::found;
}

bool PackCatalog::list(
    std::vector<PackSummary>& packs,
    std::string& error
) const {
    packs.clear();
    DIR* directory = opendir(pack_root_.c_str());
    if (directory == nullptr) {
        error = "unable to open configured pack root";
        return false;
    }

    bool succeeded = true;
    for (dirent* entry = readdir(directory);
         entry != nullptr;
         entry = readdir(directory)) {
        const std::string filename(entry->d_name);
        if (!has_json_extension(filename)) {
            continue;
        }
        const std::string pack_id =
            filename.substr(0, filename.size() - 5);
        if (!is_valid_pack_id(pack_id)) {
            error = "pack root contains an unsafe JSON filename";
            succeeded = false;
            break;
        }

        PackSummary pack;
        const PackLookupStatus status = load_file(
            pack_id,
            pack_root_ + "/" + filename,
            pack,
            error
        );
        if (status != PackLookupStatus::found) {
            if (error.empty()) {
                error = "unable to load pack catalog file: " + filename;
            }
            succeeded = false;
            break;
        }
        packs.push_back(pack);
    }
    closedir(directory);

    if (!succeeded) {
        packs.clear();
        return false;
    }
    std::sort(packs.begin(), packs.end(), pack_id_less);
    return true;
}

PackLookupStatus PackCatalog::find(
    const std::string& pack_id,
    PackSummary& pack,
    std::string& error
) const {
    if (!is_valid_pack_id(pack_id)) {
        return PackLookupStatus::invalid_id;
    }
    return load_file(
        pack_id,
        pack_root_ + "/" + pack_id + ".json",
        pack,
        error
    );
}

}  // namespace syn_sig_ra
