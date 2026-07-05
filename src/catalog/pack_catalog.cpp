#include "syn_sig_ra/pack_catalog.h"

#include "ecg_pack.h"

#include <jansson.h>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
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
        (std::string(json_string_value(status)) != "stable" &&
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
    json_array_foreach(changelog, index, item) {
        json_t* item_version = json_object_get(item, "version");
        json_t* item_date = json_object_get(item, "date");
        json_t* item_summary = json_object_get(item, "summary");
        if (!json_is_object(item) || !json_is_string(item_version) ||
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
        pack.changelog.push_back(entry);
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
    std::string scenarios("[");
    for (std::size_t index = 0; index < pack.scenarios.size(); ++index) {
        if (index != 0) {
            scenarios += ',';
        }
        scenarios += std::string("{\"scenario_id\":") +
            escape_json(pack.scenarios[index].scenario_id) +
            ",\"targets\":" +
            string_array_json(pack.scenarios[index].targets) +
            "}";
    }
    scenarios += ']';
    std::string changelog("[");
    for (std::size_t index = 0; index < pack.changelog.size(); ++index) {
        if (index != 0) changelog += ',';
        changelog += std::string("{\"version\":") +
            escape_json(pack.changelog[index].version) +
            ",\"date\":" + escape_json(pack.changelog[index].date) +
            ",\"summary\":" + escape_json(pack.changelog[index].summary) + "}";
    }
    changelog += ']';
    return std::string("{\"pack_id\":") + escape_json(pack.pack_id) +
           ",\"display_name\":" + escape_json(pack.display_name) +
           ",\"version\":" + escape_json(pack.version) +
           ",\"description\":" + escape_json(pack.description) +
           ",\"targets\":" + string_array_json(pack.targets) +
           ",\"scenarios\":" + scenarios +
           ",\"scenario_count\":" + size_to_string(pack.scenarios.size()) +
           ",\"release_status\":" + escape_json(pack.release_status) +
           ",\"released_at\":" + escape_json(pack.released_at) +
           ",\"generator_contract\":" + escape_json(pack.generator_contract) +
           ",\"compatible_generator_versions\":" +
           string_array_json(pack.compatible_generator_versions) +
           ",\"deprecation_message\":" +
           escape_json(pack.deprecation_message) +
           ",\"changelog\":" + changelog +
           ",\"pack_fingerprint\":" +
           escape_json(pack.pack_fingerprint) + "}";
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
