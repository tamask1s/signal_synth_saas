#include "syn_sig_ra/pack_catalog.h"

#include "syn_sig_ra/build_info.h"
#include "ecg_pack.h"

#include <jansson.h>
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

bool parse_semantic_version(
    const std::string& value,
    int& major,
    int& minor,
    int& patch
) {
    char first_dot = 0;
    char second_dot = 0;
    std::istringstream input(value);
    if (!(input >> major >> first_dot >> minor >> second_dot >> patch) ||
        first_dot != '.' || second_dot != '.' ||
        major < 0 || minor < 0 || patch < 0) {
        return false;
    }
    return input.peek() == std::char_traits<char>::eof();
}

bool supported_verifier_minimum(const std::string& required) {
    int required_major = 0;
    int required_minor = 0;
    int required_patch = 0;
    int current_major = 0;
    int current_minor = 0;
    int current_patch = 0;
    if (!parse_semantic_version(
            required, required_major, required_minor, required_patch) ||
        !parse_semantic_version(
            SYN_SIG_RA_EXPECTED_PYTHON_VERIFIER,
            current_major, current_minor, current_patch)) {
        return false;
    }
    if (required_major != current_major) return required_major < current_major;
    if (required_minor != current_minor) return required_minor < current_minor;
    return required_patch <= current_patch;
}

bool exact_object(json_t* value, std::size_t size) {
    return json_is_object(value) && json_object_size(value) == size;
}

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
        if (!read_string_array(item, "accepted_formats", expected_scoreable,
                target.accepted_formats, error)) return false;
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
        json_t* external = json_object_get(case_object, "external_noise");
        json_t* release_allowed =
            json_object_get(case_object, "external_noise_release_allowed");
        if (!json_is_boolean(external) || !json_is_boolean(release_allowed) ||
            !read_string_array(case_object, "external_noise_asset_ids", true,
                it->external_noise_asset_ids, error)) {
            error = "curated catalog external-noise case metadata is invalid";
            return false;
        }
        it->uses_external_noise = json_boolean_value(external);
        it->external_noise_release_allowed = json_boolean_value(release_allowed);
        if (it->uses_external_noise && !it->external_noise_release_allowed) {
            error = "curated catalog contains a non-releasable external-noise case";
            return false;
        }
        if (it->uses_external_noise) {
            pack.uses_external_noise = true;
            for (std::vector<std::string>::const_iterator asset =
                     it->external_noise_asset_ids.begin();
                 asset != it->external_noise_asset_ids.end(); ++asset) {
                if (std::find(pack.external_noise_asset_ids.begin(),
                        pack.external_noise_asset_ids.end(), *asset) ==
                    pack.external_noise_asset_ids.end()) {
                    pack.external_noise_asset_ids.push_back(*asset);
                }
            }
        }
    }
    return true;
}

bool valid_sha256(json_t* value) {
    if (!json_is_string(value)) return false;
    const std::string text(json_string_value(value));
    if (text.size() != 71 || text.compare(0, 7, "sha256:") != 0) return false;
    return text.find_first_not_of("0123456789abcdef", 7) == std::string::npos;
}

bool curated_pack_ids(
    const std::string& path,
    std::vector<std::string>& ids,
    std::string& error
) {
    std::string content;
    if (!read_file(path, content, error)) return false;
    json_error_t parse_error;
    json_t* root = json_loadb(
        content.data(), content.size(), JSON_REJECT_DUPLICATES, &parse_error);
    json_t* packs = root == nullptr ? nullptr : json_object_get(root, "packs");
    json_t* count = root == nullptr ? nullptr : json_object_get(root, "pack_count");
    if (!exact_object(root, 13) ||
        json_string_or_empty(json_object_get(root, "catalog_version")) != "3.4" ||
        json_string_or_empty(json_object_get(root, "source_catalog_sha256")) !=
            "sha256:cb6a015cc30978662b34328dc6719cb71fc69318eeb867db7d70ad6ded983500" ||
        !json_is_array(packs) || !json_is_integer(count) ||
        json_integer_value(count) <= 0 ||
        static_cast<std::size_t>(json_integer_value(count)) !=
            json_array_size(packs)) {
        if (root != nullptr) json_decref(root);
        error = "curated pack catalog index is invalid";
        return false;
    }
    ids.clear();
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(packs, index, item) {
        json_t* id = json_object_get(item, "pack_id");
        if (!json_is_string(id) ||
            !syn_sig_ra::is_valid_pack_id(json_string_value(id)) ||
            std::find(ids.begin(), ids.end(), json_string_value(id)) != ids.end()) {
            json_decref(root);
            error = "curated pack catalog index contains an invalid pack ID";
            return false;
        }
        ids.push_back(json_string_value(id));
    }
    json_decref(root);
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
        content.data(), content.size(), JSON_REJECT_DUPLICATES, &parse_error);
    json_t* packs = root == nullptr ? nullptr : json_object_get(root, "packs");
    json_t* pack_count = root == nullptr ? nullptr : json_object_get(root, "pack_count");
    const bool header_valid = exact_object(root, 13) &&
        json_is_integer(json_object_get(root, "schema_version")) &&
        json_integer_value(json_object_get(root, "schema_version")) == 1 &&
        json_string_or_empty(json_object_get(root, "metadata_type")) ==
            "synsigra_curated_pack_catalog" &&
        json_string_or_empty(json_object_get(root, "metadata_version")) ==
            "synsigra_curated_pack_metadata_export_v1" &&
        json_string_or_empty(json_object_get(root, "catalog_id")) ==
            "synsigra_verification_packs" &&
        json_string_or_empty(json_object_get(root, "catalog_version")) == "3.4" &&
        json_string_or_empty(json_object_get(root, "release_set_status")) == "beta" &&
        json_is_string(json_object_get(root, "release_set_id")) &&
        json_string_length(json_object_get(root, "release_set_id")) > 0 &&
        valid_sha256(json_object_get(root, "source_catalog_sha256")) &&
        json_string_or_empty(json_object_get(root, "source_catalog_sha256")) ==
            "sha256:cb6a015cc30978662b34328dc6719cb71fc69318eeb867db7d70ad6ded983500" &&
        json_is_array(packs) && json_is_integer(pack_count) &&
        json_integer_value(pack_count) > 0 &&
        static_cast<std::size_t>(json_integer_value(pack_count)) ==
            json_array_size(packs);
    if (!header_valid) {
        if (root != nullptr) json_decref(root);
        error = "curated pack catalog metadata has an unsupported 3.4 header";
        return false;
    }
    pack.catalog_version = json_string_value(json_object_get(root, "catalog_version"));
    pack.catalog_source_sha256 =
        json_string_value(json_object_get(root, "source_catalog_sha256"));
    pack.release_set_id = json_string_value(json_object_get(root, "release_set_id"));

    json_t* match = nullptr;
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(packs, index, item) {
        json_t* pack_id = json_object_get(item, "pack_id");
        if (json_is_object(item) && json_is_string(pack_id) &&
            pack.pack_id == json_string_value(pack_id)) {
            if (match != nullptr) {
                json_decref(root);
                error = "curated pack catalog contains a duplicate pack ID";
                return false;
            }
            match = item;
        }
    }
    json_t* version = match == nullptr ? nullptr : json_object_get(match, "version");
    json_t* name = match == nullptr ? nullptr : json_object_get(match, "name");
    json_t* description = match == nullptr ? nullptr : json_object_get(match, "description");
    json_t* scoring_mode = match == nullptr ? nullptr : json_object_get(match, "scoring_mode");
    json_t* status = match == nullptr ? nullptr : json_object_get(match, "release_status");
    json_t* released = match == nullptr ? nullptr : json_object_get(match, "release_date");
    json_t* deprecation = match == nullptr ? nullptr : json_object_get(match, "deprecation_message");
    if (!json_is_string(version) || !json_is_string(name) ||
        !json_is_string(description) || !json_is_string(scoring_mode) ||
        !json_is_string(status) || !json_is_string(released) ||
        !json_is_string(deprecation) || !is_semantic_version(json_string_value(version)) ||
        pack.version != json_string_value(version) ||
        pack.display_name != json_string_value(name) ||
        pack.description != json_string_value(description) ||
        (json_string_or_empty(status) != "beta" &&
         json_string_or_empty(status) != "stable" &&
         json_string_or_empty(status) != "deprecated")) {
        json_decref(root);
        error = "curated catalog metadata does not match the validated pack";
        return false;
    }
    pack.integration_contract_version = "synsigra_core_integration_v7";
    pack.scoring_mode = json_string_value(scoring_mode);
    pack.release_status = json_string_value(status);
    pack.released_at = json_string_value(released);
    pack.deprecation_message = json_string_value(deprecation);
    if (pack.release_status == "deprecated" && pack.deprecation_message.empty()) {
        json_decref(root);
        error = "deprecated packs require a deprecation message";
        return false;
    }

    pack.changelog.clear();
    bool version_documented = false;
    json_t* changelog = json_object_get(match, "changelog");
    if (!json_is_array(changelog) || json_array_size(changelog) == 0) {
        json_decref(root);
        error = "curated pack changelog is missing";
        return false;
    }
    json_array_foreach(changelog, index, item) {
        json_t* entry_version = json_object_get(item, "version");
        json_t* entry_date = json_object_get(item, "date");
        std::vector<std::string> changes;
        if (!exact_object(item, 3) || !json_is_string(entry_version) ||
            !json_is_string(entry_date) ||
            !is_semantic_version(json_string_value(entry_version)) ||
            !read_string_array(item, "changes", true, changes, error) ||
            changes.empty()) {
            json_decref(root);
            error = "curated pack changelog is invalid";
            return false;
        }
        syn_sig_ra::PackChangelogEntry entry;
        entry.version = json_string_value(entry_version);
        entry.date = json_string_value(entry_date);
        for (std::size_t change = 0; change < changes.size(); ++change) {
            if (change != 0) entry.summary += " ";
            entry.summary += changes[change];
        }
        version_documented = version_documented || entry.version == pack.version;
        pack.changelog.push_back(entry);
    }
    if (!version_documented) {
        json_decref(root);
        error = "current pack version is missing from the changelog";
        return false;
    }

    json_t* recommended_profile = json_object_get(match, "recommended_profile");
    pack.recommended_profile =
        json_is_string(recommended_profile) ? json_string_value(recommended_profile) : "";
    if (!read_string_array(match, "targets", true, pack.targets, error) ||
        !read_target_array(match, "scoreable_targets", true, pack.scoreable_targets, error) ||
        !read_target_array(match, "reference_only_targets", false, pack.reference_only_targets, error) ||
        !read_string_array(match, "submission_output_schemas", true, pack.submission_output_schemas, error) ||
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
    const std::string local_verifier_min_version = json_string_or_empty(
        json_object_get(compatibility, "local_verifier_min_version"));
    std::vector<int> scenario_versions;
    if (!json_is_object(duration) || !json_is_object(estimated) ||
        !json_is_object(channels) || !exact_object(compatibility, 8) ||
        json_string_or_empty(json_object_get(compatibility, "minimum_generator_version")) != "0.10.0-dev" ||
        integer_field(compatibility, "pack_schema_version") != 2 ||
        !read_int_array(compatibility, "scenario_schema_versions", scenario_versions, error) ||
        scenario_versions.empty() ||
        json_string_or_empty(json_object_get(compatibility, "challenge_package_contract")) != "synsigra_challenge_package_v3" ||
        json_string_or_empty(json_object_get(compatibility, "scoring_manifest_contract")) != "synsigra_scoring_manifest_v3" ||
        json_string_or_empty(json_object_get(compatibility, "submission_contract")) != "synsigra_submission_v1" ||
        json_string_or_empty(json_object_get(compatibility, "verification_protocol_contract")) != "synsigra_verification_protocol_v2" ||
        !supported_verifier_minimum(local_verifier_min_version)) {
        json_decref(root);
        error = "curated pack generator compatibility is not the v7 tuple";
        return false;
    }
    for (std::size_t schema_index = 0; schema_index < scenario_versions.size(); ++schema_index) {
        if (scenario_versions[schema_index] < 2 || scenario_versions[schema_index] > 9) {
            json_decref(root);
            error = "curated pack uses an unsupported scenario schema";
            return false;
        }
    }
    pack.minimum_case_seconds = int_field(duration, "minimum_case_seconds");
    pack.maximum_case_seconds = int_field(duration, "maximum_case_seconds");
    pack.total_seconds = int_field(duration, "total_seconds");
    pack.minimum_channel_count = int_field(channels, "minimum_channel_count");
    pack.maximum_channel_count = int_field(channels, "maximum_channel_count");
    pack.estimated_package_bytes = integer_field(estimated, "bytes");
    pack.peak_memory_bytes = integer_field(estimated, "peak_memory_bytes");
    pack.package_size_class = json_string_or_empty(json_object_get(estimated, "size_class"));
    pack.local_verifier_min_version = local_verifier_min_version;
    pack.challenge_package_contract = "synsigra_challenge_package_v3";
    pack.scoring_manifest_contract = "synsigra_scoring_manifest_v3";
    pack.submission_contract = "synsigra_submission_v1";
    pack.verification_protocol_contract = "synsigra_verification_protocol_v2";

    pack.output_artifact_roles.clear();
    json_t* artifacts = json_object_get(match, "output_artifacts");
    if (!json_is_array(artifacts)) {
        json_decref(root);
        error = "curated pack output artifacts are invalid";
        return false;
    }
    json_array_foreach(artifacts, index, item) {
        json_t* role = json_object_get(item, "role");
        if (!json_is_object(item) || !json_is_string(role)) {
            json_decref(root);
            error = "curated pack output artifact role is invalid";
            return false;
        }
        pack.output_artifact_roles.push_back(json_string_value(role));
    }
    json_t* ui = json_object_get(match, "ui");
    if (!json_is_object(ui) ||
        !read_string_array(ui, "primary_badges", true, pack.primary_badges, error)) {
        json_decref(root);
        error = "curated pack UI metadata is invalid";
        return false;
    }

    json_t* protocol = json_object_get(match, "verification_protocol");
    json_t* protocol_available =
        json_is_object(protocol) ? json_object_get(protocol, "available") : nullptr;
    json_t* protocol_document =
        json_is_object(protocol) ? json_object_get(protocol, "document") : nullptr;
    if (!exact_object(protocol, 5) || !json_is_boolean(protocol_available) ||
        !json_is_string(json_object_get(protocol, "artifact_role")) ||
        !json_is_string(json_object_get(protocol, "source_content_sha256")) ||
        !json_is_string(json_object_get(protocol, "source_path"))) {
        json_decref(root);
        error = "curated pack verification protocol metadata is invalid";
        return false;
    }
    pack.verification_protocol_available = json_boolean_value(protocol_available);
    if (pack.verification_protocol_available) {
        if (json_string_or_empty(json_object_get(protocol, "artifact_role")) !=
                "verification_protocol_json" ||
            !valid_sha256(json_object_get(protocol, "source_content_sha256")) ||
            !json_is_object(protocol_document)) {
            json_decref(root);
            error = "curated pack verification protocol is incomplete";
            return false;
        }
        char* canonical = json_dumps(protocol_document, JSON_COMPACT | JSON_SORT_KEYS);
        if (canonical == nullptr) {
            json_decref(root);
            error = "curated pack verification protocol cannot be encoded";
            return false;
        }
        pack.verification_protocol_json = canonical;
        free(canonical);
        pack.verification_protocol_sha256 =
            json_string_value(json_object_get(protocol, "source_content_sha256"));
    } else if (!json_is_null(protocol_document) ||
               json_string_length(json_object_get(protocol, "artifact_role")) != 0 ||
               json_string_length(json_object_get(protocol, "source_content_sha256")) != 0) {
        json_decref(root);
        error = "unavailable verification protocol must not carry content";
        return false;
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
    json_object_set_new(root, "external_noise", json_boolean(scenario.uses_external_noise));
    json_object_set_new(root, "external_noise_release_allowed", json_boolean(scenario.external_noise_release_allowed));
    json_object_set_new(root, "external_noise_asset_ids", string_array_json_value(scenario.external_noise_asset_ids));
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
    json_object_set_new(root, "catalog_version", json_string(pack.catalog_version.c_str()));
    json_object_set_new(root, "catalog_source_sha256", json_string(pack.catalog_source_sha256.c_str()));
    json_object_set_new(root, "release_set_id", json_string(pack.release_set_id.c_str()));
    json_object_set_new(root, "release_status", json_string(pack.release_status.c_str()));
    json_object_set_new(root, "released_at", json_string(pack.released_at.c_str()));
    json_object_set_new(root, "integration_contract", json_string(pack.integration_contract_version.c_str()));
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
    json_object_set_new(root, "submission_output_schemas", string_array_json_value(pack.submission_output_schemas));
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
    json_object_set_new(compatibility, "submission_contract", json_string(pack.submission_contract.c_str()));
    json_object_set_new(compatibility, "verification_protocol_contract", json_string(pack.verification_protocol_contract.c_str()));
    json_object_set_new(compatibility, "integration_contract", json_string(pack.integration_contract_version.c_str()));
    json_object_set_new(root, "generator_compatibility", compatibility);
    json_object_set_new(root, "output_artifact_roles", string_array_json_value(pack.output_artifact_roles));
    json_object_set_new(root, "primary_badges", string_array_json_value(pack.primary_badges));
    json_t* protocol = json_object();
    json_object_set_new(protocol, "available", json_boolean(pack.verification_protocol_available));
    json_object_set_new(protocol, "contract", json_string(pack.verification_protocol_contract.c_str()));
    json_object_set_new(protocol, "sha256", json_string(pack.verification_protocol_sha256.c_str()));
    if (pack.verification_protocol_available) {
        json_error_t protocol_error;
        json_t* document = json_loadb(
            pack.verification_protocol_json.data(),
            pack.verification_protocol_json.size(),
            JSON_REJECT_DUPLICATES,
            &protocol_error);
        json_object_set_new(protocol, "document", document == nullptr ? json_null() : document);
    } else {
        json_object_set_new(protocol, "document", json_null());
    }
    json_object_set_new(root, "verification_protocol", protocol);
    json_t* external_noise = json_object();
    json_object_set_new(external_noise, "used", json_boolean(pack.uses_external_noise));
    json_object_set_new(external_noise, "release_allowed", json_true());
    json_object_set_new(external_noise, "asset_ids", string_array_json_value(pack.external_noise_asset_ids));
    json_object_set_new(root, "external_noise", external_noise);
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
    std::vector<std::string> ids;
    if (!curated_pack_ids(
            pack_root_ + "/curated_pack_metadata_v1.catalog", ids, error))
        return false;
    for (std::vector<std::string>::const_iterator id = ids.begin();
         id != ids.end(); ++id) {
        PackSummary pack;
        const PackLookupStatus status = load_file(
            *id,
            pack_root_ + "/" + *id + ".json",
            pack,
            error
        );
        if (status != PackLookupStatus::found) {
            if (error.empty()) {
                error = "unable to load curated pack: " + *id;
            }
            packs.clear();
            return false;
        }
        packs.push_back(pack);
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
