#include "syn_sig_ra/pack_catalog.h"

#include "ecg_pack.h"

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
    return std::string("{\"pack_id\":") + escape_json(pack.pack_id) +
           ",\"display_name\":" + escape_json(pack.display_name) +
           ",\"description\":" + escape_json(pack.description) +
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
    pack.description = manifest.description;
    pack.pack_fingerprint = result.pack_fingerprint;
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
