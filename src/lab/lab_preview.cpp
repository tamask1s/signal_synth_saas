#include "syn_sig_ra/lab_preview.h"

#include "syn_sig_ra/random_id.h"
#include "syn_sig_ra/signal_viewer.h"
#include "ecg_export.h"
#include "ecg_render.h"
#include "ecg_scenario_json.h"
#include "ecg_wfdb_export.h"

#include <jansson.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

const unsigned int kTtlSeconds = 30u * 60u;
const unsigned int kMaximumDurationSeconds = 5u * 60u;
const unsigned long long kMaximumSamples = 300000u;
const unsigned int kMaximumRetainedPerUser = 3u;
const unsigned int kRenderCooldownSeconds = 1u;

bool safe_segment(const std::string& value, const std::string& prefix = "") {
    if (value.empty() || value.size() > 128u ||
        (!prefix.empty() && value.compare(0, prefix.size(), prefix) != 0)) {
        return false;
    }
    for (std::string::const_iterator it = value.begin(); it != value.end(); ++it) {
        const unsigned char byte = static_cast<unsigned char>(*it);
        if (!(std::isalnum(byte) || *it == '_' || *it == '-')) return false;
    }
    return true;
}

bool directory(const std::string& path) {
    struct stat information;
    return lstat(path.c_str(), &information) == 0 && S_ISDIR(information.st_mode);
}

bool ensure_directory(const std::string& path, std::string& error) {
    if (mkdir(path.c_str(), 0750) == 0 || (errno == EEXIST && directory(path))) {
        return true;
    }
    error = "unable to create Lab preview storage";
    return false;
}

bool remove_tree(const std::string& path) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0) return errno == ENOENT;
    if (S_ISLNK(information.st_mode)) return false;
    if (S_ISDIR(information.st_mode)) {
        DIR* handle = opendir(path.c_str());
        if (handle == nullptr) return false;
        bool succeeded = true;
        for (dirent* entry = readdir(handle); entry != nullptr; entry = readdir(handle)) {
            const std::string name(entry->d_name);
            if (name != "." && name != ".." && !remove_tree(path + "/" + name)) {
                succeeded = false;
                break;
            }
        }
        closedir(handle);
        return succeeded && rmdir(path.c_str()) == 0;
    }
    return S_ISREG(information.st_mode) && unlink(path.c_str()) == 0;
}

bool write_file(
    const std::string& path,
    const std::string& content,
    mode_t mode,
    std::string& error
) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        error = "unable to write Lab preview data";
        return false;
    }
    if (chmod(path.c_str(), mode) != 0) {
        error = "unable to protect Lab preview data";
        return false;
    }
    return true;
}

std::string iso_time(std::time_t value) {
    std::tm utc;
    if (gmtime_r(&value, &utc) == nullptr) return std::string();
    char encoded[32];
    return std::strftime(encoded, sizeof(encoded), "%Y-%m-%dT%H:%M:%SZ", &utc)
        ? std::string(encoded) : std::string();
}

std::string user_root(
    const std::string& data_root,
    const syn_sig_ra::ApiKeyIdentity& owner,
    std::string& error
) {
    if (data_root.empty() || !safe_segment(owner.organization_id, "org_") ||
        !safe_segment(owner.user_id)) {
        error = "invalid Lab preview owner";
        return std::string();
    }
    const std::string lab = data_root + "/lab-previews";
    const std::string organization = lab + "/" + owner.organization_id;
    const std::string user = organization + "/" + owner.user_id;
    if (!ensure_directory(lab, error) || !ensure_directory(organization, error) ||
        !ensure_directory(user, error)) {
        return std::string();
    }
    return user;
}

struct PreviewEntry {
    std::string path;
    std::time_t modified;
};

bool preview_entries(
    const std::string& root,
    std::vector<PreviewEntry>& entries,
    std::string& error
) {
    DIR* handle = opendir(root.c_str());
    if (handle == nullptr) {
        error = "unable to inspect Lab preview storage";
        return false;
    }
    for (dirent* item = readdir(handle); item != nullptr; item = readdir(handle)) {
        const std::string name(item->d_name);
        if (!safe_segment(name, "preview_")) continue;
        const std::string path = root + "/" + name;
        struct stat information;
        if (lstat(path.c_str(), &information) == 0 && S_ISDIR(information.st_mode)) {
            PreviewEntry entry;
            entry.path = path;
            entry.modified = information.st_mtime;
            entries.push_back(entry);
        }
    }
    closedir(handle);
    return true;
}

bool prune_user(
    const std::string& root,
    std::time_t now,
    unsigned int keep,
    std::string& error
) {
    std::vector<PreviewEntry> entries;
    if (!preview_entries(root, entries, error)) return false;
    std::sort(entries.begin(), entries.end(), [](const PreviewEntry& left, const PreviewEntry& right) {
        return left.modified > right.modified;
    });
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const bool expired = now >= entries[index].modified &&
            static_cast<unsigned long long>(now - entries[index].modified) >= kTtlSeconds;
        if ((expired || index >= keep) && !remove_tree(entries[index].path)) {
            error = "unable to remove expired Lab preview";
            return false;
        }
    }
    return true;
}

class RenderLock {
public:
    explicit RenderLock(const std::string& path) : path_(path), held_(false) {}
    ~RenderLock() { if (held_) rmdir(path_.c_str()); }
    bool acquire() {
        held_ = mkdir(path_.c_str(), 0750) == 0;
        if (!held_ && errno == EEXIST) {
            struct stat information;
            const std::time_t now = std::time(nullptr);
            if (lstat(path_.c_str(), &information) == 0 &&
                S_ISDIR(information.st_mode) && now >= information.st_mtime &&
                now - information.st_mtime > 120 && rmdir(path_.c_str()) == 0) {
                held_ = mkdir(path_.c_str(), 0750) == 0;
            }
        }
        return held_;
    }
private:
    std::string path_;
    bool held_;
};

std::string joined_messages(const std::vector<std::string>& messages) {
    std::ostringstream output;
    for (std::size_t index = 0; index < messages.size(); ++index) {
        if (index) output << "; ";
        output << messages[index];
    }
    return output.str();
}

json_t* preview_json(const syn_sig_ra::LabPreview& preview) {
    json_t* root = json_object();
    json_object_set_new(root, "schema_version", json_integer(1));
    json_object_set_new(root, "preview_id", json_string(preview.preview_id.c_str()));
    json_object_set_new(root, "case_id", json_string(preview.case_id.c_str()));
    json_object_set_new(root, "canonical_scenario_json", json_string(preview.canonical_scenario_json.c_str()));
    json_object_set_new(root, "resolved_scenario_json", json_string(preview.resolved_scenario_json.c_str()));
    json_object_set_new(root, "document_fingerprint", json_string(preview.document_fingerprint.c_str()));
    json_object_set_new(root, "resolved_document_fingerprint", json_string(preview.resolved_document_fingerprint.c_str()));
    json_object_set_new(root, "render_identity", json_string(preview.render_identity.c_str()));
    json_object_set_new(root, "generator_version", json_string(preview.generator_version.c_str()));
    json_object_set_new(root, "generator_git_commit", json_string(preview.generator_git_commit.c_str()));
    json_object_set_new(root, "generator_build_identity", json_string(preview.generator_build_identity.c_str()));
    json_object_set_new(root, "created_at", json_string(preview.created_at.c_str()));
    json_object_set_new(root, "expires_at", json_string(preview.expires_at.c_str()));
    json_object_set_new(root, "duration_seconds", json_real(preview.duration_seconds));
    json_object_set_new(root, "sample_rate_hz", json_integer(preview.sample_rate_hz));
    json_object_set_new(root, "sample_count", json_integer(static_cast<json_int_t>(preview.sample_count)));
    return root;
}

bool json_string_field(json_t* root, const char* name, std::string& output) {
    json_t* value = json_object_get(root, name);
    if (!json_is_string(value)) return false;
    output = json_string_value(value);
    return true;
}

bool read_preview_metadata(
    const std::string& path,
    syn_sig_ra::LabPreview& preview,
    std::string& error
) {
    json_error_t parse_error;
    json_t* root = json_load_file(path.c_str(), JSON_REJECT_DUPLICATES, &parse_error);
    if (!json_is_object(root)) {
        if (root != nullptr) json_decref(root);
        error = "Lab preview metadata is invalid";
        return false;
    }
    json_t* duration = json_object_get(root, "duration_seconds");
    json_t* rate = json_object_get(root, "sample_rate_hz");
    json_t* count = json_object_get(root, "sample_count");
    const bool valid =
        json_string_field(root, "preview_id", preview.preview_id) &&
        json_string_field(root, "case_id", preview.case_id) &&
        json_string_field(root, "canonical_scenario_json", preview.canonical_scenario_json) &&
        json_string_field(root, "resolved_scenario_json", preview.resolved_scenario_json) &&
        json_string_field(root, "document_fingerprint", preview.document_fingerprint) &&
        json_string_field(root, "resolved_document_fingerprint", preview.resolved_document_fingerprint) &&
        json_string_field(root, "render_identity", preview.render_identity) &&
        json_string_field(root, "generator_version", preview.generator_version) &&
        json_string_field(root, "generator_git_commit", preview.generator_git_commit) &&
        json_string_field(root, "generator_build_identity", preview.generator_build_identity) &&
        json_string_field(root, "created_at", preview.created_at) &&
        json_string_field(root, "expires_at", preview.expires_at) &&
        json_is_number(duration) && json_is_integer(rate) && json_is_integer(count) &&
        json_integer_value(rate) > 0 && json_integer_value(count) > 0;
    if (valid) {
        preview.duration_seconds = json_number_value(duration);
        preview.sample_rate_hz = static_cast<unsigned int>(json_integer_value(rate));
        preview.sample_count = static_cast<unsigned long long>(json_integer_value(count));
    }
    json_decref(root);
    if (!valid) error = "Lab preview metadata is incomplete";
    return valid;
}

bool cleanup_children(
    const std::string& lab_root,
    std::time_t now,
    std::string& error
) {
    DIR* organizations = opendir(lab_root.c_str());
    if (organizations == nullptr) return errno == ENOENT;
    bool succeeded = true;
    for (dirent* org = readdir(organizations); succeeded && org != nullptr; org = readdir(organizations)) {
        const std::string org_name(org->d_name);
        if (!safe_segment(org_name, "org_")) continue;
        const std::string org_path = lab_root + "/" + org_name;
        if (!directory(org_path)) continue;
        DIR* users = opendir(org_path.c_str());
        if (users == nullptr) { succeeded = false; break; }
        for (dirent* user = readdir(users); succeeded && user != nullptr; user = readdir(users)) {
            const std::string user_name(user->d_name);
            if (!safe_segment(user_name)) continue;
            const std::string user_path = org_path + "/" + user_name;
            if (directory(user_path) && !prune_user(user_path, now, kMaximumRetainedPerUser, error)) {
                succeeded = false;
            }
        }
        closedir(users);
    }
    closedir(organizations);
    if (!succeeded && error.empty()) error = "unable to inspect Lab preview storage";
    return succeeded;
}

}  // namespace

namespace syn_sig_ra {

LabPreviewStatus create_lab_preview(
    const std::string& data_root,
    const ApiKeyIdentity& owner,
    const std::string& scenario_json,
    LabPreview& preview,
    std::vector<std::string>& validation_messages,
    std::string& error
) {
    error.clear();
    validation_messages.clear();
    const std::string root = user_root(data_root, owner, error);
    if (root.empty()) return LabPreviewStatus::io_error;
    RenderLock lock(root + "/.render-lock");
    if (!lock.acquire()) {
        error = "another Lab preview is still rendering";
        return LabPreviewStatus::busy;
    }
    const std::time_t now = std::time(nullptr);
    if (!prune_user(root, now, kMaximumRetainedPerUser, error)) {
        return LabPreviewStatus::io_error;
    }
    const std::string cooldown = root + "/.last-render";
    struct stat last_render;
    if (stat(cooldown.c_str(), &last_render) == 0 && now >= last_render.st_mtime &&
        static_cast<unsigned long long>(now - last_render.st_mtime) < kRenderCooldownSeconds) {
        error = "wait a moment before rendering another Lab preview";
        return LabPreviewStatus::busy;
    }
    if (!write_file(cooldown, "", 0640, error)) return LabPreviewStatus::io_error;

    signal_synth::ecg_scenario_document document;
    signal_synth::ecg_scenario_json_result parsed;
    if (!signal_synth::parse_ecg_scenario_json(scenario_json, document, parsed)) {
        for (std::vector<signal_synth::ecg_scenario_json_message>::const_iterator it =
                 parsed.messages.begin(); it != parsed.messages.end(); ++it) {
            validation_messages.push_back(it->path + ": " + it->message);
        }
        return LabPreviewStatus::invalid_scenario;
    }
    const unsigned int sample_rate = document.ecg.sampling_rate_hz();
    const long double requested_samples = static_cast<long double>(document.duration_seconds) * sample_rate;
    if (document.duration_seconds > kMaximumDurationSeconds ||
        requested_samples > kMaximumSamples) {
        std::ostringstream message;
        message << "Lab previews allow at most " << kMaximumDurationSeconds
                << " seconds and " << kMaximumSamples
                << " samples; use a generation job for larger data";
        error = message.str();
        return LabPreviewStatus::limit_exceeded;
    }

    signal_synth::ecg_render_bundle render;
    signal_synth::ecg_document_render_result render_result;
    if (!signal_synth::render_ecg_document(document, render, render_result)) {
        error = joined_messages(render_result.messages);
        if (error.empty()) error = "the generator rejected this Lab preview";
        return LabPreviewStatus::invalid_scenario;
    }
    if (render.record.sample_count() == 0u || render.record.sample_count() > kMaximumSamples) {
        error = "rendered Lab preview exceeds the sample limit";
        return LabPreviewStatus::limit_exceeded;
    }
    signal_synth::wfdb_export_bundle wfdb;
    signal_synth::ecg_export_result export_result;
    if (!signal_synth::build_wfdb_export_bundle(render, "synsigra", wfdb, export_result)) {
        error = joined_messages(export_result.messages);
        if (error.empty()) error = "unable to encode Lab preview waveform";
        return LabPreviewStatus::io_error;
    }

    std::string preview_id;
    if (!random_id("preview_", preview_id, error)) return LabPreviewStatus::io_error;
    const std::string preview_root = root + "/" + preview_id;
    const std::string source = preview_root + "/source";
    const std::string cases = source + "/cases";
    const std::string case_root = cases + "/preview";
    if (!ensure_directory(preview_root, error) || !ensure_directory(source, error) ||
        !ensure_directory(cases, error) || !ensure_directory(case_root, error)) {
        remove_tree(preview_root);
        return LabPreviewStatus::io_error;
    }
    bool wrote_header = false;
    bool wrote_data = false;
    for (std::vector<signal_synth::wfdb_export_artifact>::const_iterator it =
             wfdb.artifacts.begin(); it != wfdb.artifacts.end(); ++it) {
        if (it->name == "synsigra.hea") {
            wrote_header = write_file(case_root + "/synsigra.hea", it->content, 0440, error);
        } else if (it->name == "synsigra.dat") {
            wrote_data = write_file(case_root + "/synsigra.dat", it->content, 0440, error);
        }
    }
    if (!wrote_header || !wrote_data) {
        if (error.empty()) error = "generator omitted the Lab waveform artifacts";
        remove_tree(preview_root);
        return LabPreviewStatus::io_error;
    }
    const SignalViewerStatus prepared = prepare_signal_viewer_source(
        source, preview_root + "/viewer", error);
    if (prepared != SignalViewerStatus::ok) {
        remove_tree(preview_root);
        return prepared == SignalViewerStatus::invalid_source
            ? LabPreviewStatus::invalid_scenario : LabPreviewStatus::io_error;
    }
    remove_tree(source);

    preview.preview_id = preview_id;
    preview.case_id = "preview";
    preview.canonical_scenario_json = render.document_identity.canonical_json;
    preview.resolved_scenario_json = render.resolved_document_identity.canonical_json;
    preview.document_fingerprint = render.document_identity.document_fingerprint;
    preview.resolved_document_fingerprint = render.resolved_document_identity.document_fingerprint;
    preview.render_identity = render.render_identity;
    preview.generator_version = signal_synth::signal_synth_generator_version();
    preview.generator_git_commit = signal_synth::signal_synth_generator_git_commit();
    preview.generator_build_identity = signal_synth::signal_synth_build_identity();
    preview.created_at = iso_time(now);
    preview.expires_at = iso_time(now + kTtlSeconds);
    preview.duration_seconds = document.duration_seconds;
    preview.sample_rate_hz = sample_rate;
    preview.sample_count = render.record.sample_count();
    json_t* metadata = preview_json(preview);
    char* encoded = json_dumps(metadata, JSON_COMPACT | JSON_SORT_KEYS);
    json_decref(metadata);
    const bool stored = encoded != nullptr &&
        write_file(preview_root + "/metadata.json", encoded, 0440, error);
    if (encoded != nullptr) free(encoded);
    if (!stored) {
        if (error.empty()) error = "unable to encode Lab preview metadata";
        remove_tree(preview_root);
        return LabPreviewStatus::io_error;
    }
    if (!prune_user(root, now, kMaximumRetainedPerUser, error)) {
        remove_tree(preview_root);
        return LabPreviewStatus::io_error;
    }
    return LabPreviewStatus::ok;
}

LabPreviewStatus load_lab_preview(
    const std::string& data_root,
    const ApiKeyIdentity& owner,
    const std::string& preview_id,
    LabPreview& preview,
    std::string& viewer_root,
    std::string& error
) {
    error.clear();
    if (!safe_segment(preview_id, "preview_")) return LabPreviewStatus::invalid_request;
    const std::string root = user_root(data_root, owner, error);
    if (root.empty()) return LabPreviewStatus::io_error;
    const std::string path = root + "/" + preview_id;
    struct stat information;
    if (lstat(path.c_str(), &information) != 0 || !S_ISDIR(information.st_mode)) {
        return LabPreviewStatus::not_found;
    }
    const std::time_t now = std::time(nullptr);
    if (now >= information.st_mtime &&
        static_cast<unsigned long long>(now - information.st_mtime) >= kTtlSeconds) {
        remove_tree(path);
        return LabPreviewStatus::not_found;
    }
    if (!read_preview_metadata(path + "/metadata.json", preview, error) ||
        preview.preview_id != preview_id || !directory(path + "/viewer")) {
        if (error.empty()) error = "Lab preview storage is incomplete";
        return LabPreviewStatus::io_error;
    }
    viewer_root = path + "/viewer";
    return LabPreviewStatus::ok;
}

LabPreviewStatus discard_lab_preview(
    const std::string& data_root,
    const ApiKeyIdentity& owner,
    const std::string& preview_id,
    std::string& error
) {
    error.clear();
    if (!safe_segment(preview_id, "preview_")) return LabPreviewStatus::invalid_request;
    const std::string root = user_root(data_root, owner, error);
    if (root.empty()) return LabPreviewStatus::io_error;
    const std::string path = root + "/" + preview_id;
    if (!directory(path)) return LabPreviewStatus::not_found;
    if (!remove_tree(path)) {
        error = "unable to discard Lab preview";
        return LabPreviewStatus::io_error;
    }
    return LabPreviewStatus::ok;
}

bool discard_lab_previews_for_user(
    const std::string& data_root,
    const ApiKeyIdentity& owner,
    std::string& error
) {
    error.clear();
    if (!directory(data_root)) return true;
    const std::string root = user_root(data_root, owner, error);
    if (root.empty()) return false;
    if (!remove_tree(root)) {
        error = "unable to remove account Lab previews";
        return false;
    }
    return true;
}

bool cleanup_expired_lab_previews(const std::string& data_root, std::string& error) {
    error.clear();
    if (data_root.empty()) return true;
    return cleanup_children(data_root + "/lab-previews", std::time(nullptr), error);
}

unsigned int lab_preview_ttl_seconds() { return kTtlSeconds; }
unsigned int lab_preview_maximum_duration_seconds() { return kMaximumDurationSeconds; }
unsigned long long lab_preview_maximum_samples() { return kMaximumSamples; }

}  // namespace syn_sig_ra
