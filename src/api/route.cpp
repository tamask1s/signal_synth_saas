#include "syn_sig_ra/route.h"

#include "syn_sig_ra/api_key_auth.h"
#include "syn_sig_ra/build_info.h"
#include "syn_sig_ra/job_request.h"
#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/pack_catalog.h"
#include "syn_sig_ra/password_auth.h"
#include "syn_sig_ra/random_id.h"
#include "syn_sig_ra/sha256.h"
#include "ecg_pack.h"
#include "synsigra_api.h"

#include <jansson.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace {

bool owns_uri(const std::string& uri, const std::string& prefix) {
    return uri == prefix ||
           (uri.size() > prefix.size() &&
            uri.compare(0, prefix.size(), prefix) == 0 &&
            uri[prefix.size()] == '/');
}

bool path_at_or_below(const std::string& uri, const std::string& path) {
    return uri == path ||
           (uri.size() > path.size() &&
            uri.compare(0, path.size(), path) == 0 &&
            uri[path.size()] == '/');
}

syn_sig_ra::RouteResponse json_response(
    int status,
    const std::string& body
) {
    syn_sig_ra::RouteResponse response;
    response.disposition = syn_sig_ra::RouteDisposition::handled;
    response.status = status;
    response.content_type = "application/json";
    response.body = body;
    return response;
}

bool is_json_content_type(const std::string& content_type) {
    const std::string expected("application/json");
    if (content_type.size() < expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (std::tolower(
                static_cast<unsigned char>(content_type[index])
            ) != expected[index]) {
            return false;
        }
    }
    return content_type.size() == expected.size() ||
           content_type[expected.size()] == ';';
}

std::string json_dump_line(json_t* value);

std::string account_json(const syn_sig_ra::AccountRecord& account) {
    json_t* root = json_object();
    json_object_set_new(root, "user_id", json_string(account.user_id.c_str()));
    json_object_set_new(
        root, "organization_id", json_string(account.organization_id.c_str()));
    json_object_set_new(root, "email", json_string(account.email.c_str()));
    json_object_set_new(
        root, "display_name", json_string(account.display_name.c_str()));
    json_object_set_new(root, "role", json_string(account.role.c_str()));
    const std::string encoded = json_dump_line(root);
    json_decref(root);
    return encoded;
}

bool issue_browser_session(
    syn_sig_ra::MetadataStore& store,
    const syn_sig_ra::AccountRecord& account,
    syn_sig_ra::RouteResponse& response,
    std::string& error
) {
    std::string session_id;
    std::string token;
    std::string token_hash;
    if (!syn_sig_ra::random_id("session_", session_id, error) ||
        !syn_sig_ra::random_id("ss_", token, error) ||
        !syn_sig_ra::sha256_hex(token, token_hash, error) ||
        !store.create_session(account, session_id, token_hash, error)) {
        return false;
    }
    response = json_response(200, account_json(account));
    response.cache_control = "no-store";
    response.set_cookie =
        "syn_sig_ra_session=" + token +
        "; Path=/syn_sig_ra; Max-Age=604800; Secure; HttpOnly; SameSite=Lax";
    return true;
}

std::string json_dump_line(json_t* value) {
    char* encoded = json_dumps(value, JSON_COMPACT | JSON_SORT_KEYS);
    if (encoded == nullptr) {
        return "{}\n";
    }
    std::string output(encoded);
    free(encoded);
    output += '\n';
    return output;
}

struct ZipEntry {
    std::string path;
    std::string content;
};

void append_u16(std::string& output, std::uint16_t value) {
    output.push_back(static_cast<char>(value & 0xffu));
    output.push_back(static_cast<char>((value >> 8) & 0xffu));
}

void append_u32(std::string& output, std::uint32_t value) {
    output.push_back(static_cast<char>(value & 0xffu));
    output.push_back(static_cast<char>((value >> 8) & 0xffu));
    output.push_back(static_cast<char>((value >> 16) & 0xffu));
    output.push_back(static_cast<char>((value >> 24) & 0xffu));
}

std::uint32_t crc32(const std::string& data) {
    std::uint32_t crc = 0xffffffffu;
    for (std::string::const_iterator it = data.begin(); it != data.end(); ++it) {
        crc ^= static_cast<unsigned char>(*it);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = (crc & 1u) ? 0xedb88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return crc ^ 0xffffffffu;
}

std::string zip_store_archive(const std::vector<ZipEntry>& entries) {
    std::string output;
    std::string central_directory;
    for (std::vector<ZipEntry>::const_iterator it = entries.begin();
         it != entries.end(); ++it) {
        const std::uint32_t offset = static_cast<std::uint32_t>(output.size());
        const std::uint32_t crc = crc32(it->content);
        const std::uint32_t size = static_cast<std::uint32_t>(it->content.size());
        append_u32(output, 0x04034b50u);
        append_u16(output, 20);
        append_u16(output, 0);
        append_u16(output, 0);
        append_u16(output, 0);
        append_u16(output, 0);
        append_u32(output, crc);
        append_u32(output, size);
        append_u32(output, size);
        append_u16(output, static_cast<std::uint16_t>(it->path.size()));
        append_u16(output, 0);
        output += it->path;
        output += it->content;

        append_u32(central_directory, 0x02014b50u);
        append_u16(central_directory, 20);
        append_u16(central_directory, 20);
        append_u16(central_directory, 0);
        append_u16(central_directory, 0);
        append_u16(central_directory, 0);
        append_u16(central_directory, 0);
        append_u32(central_directory, crc);
        append_u32(central_directory, size);
        append_u32(central_directory, size);
        append_u16(central_directory, static_cast<std::uint16_t>(it->path.size()));
        append_u16(central_directory, 0);
        append_u16(central_directory, 0);
        append_u16(central_directory, 0);
        append_u16(central_directory, 0);
        append_u32(central_directory, 0);
        append_u32(central_directory, offset);
        central_directory += it->path;
    }
    const std::uint32_t central_offset = static_cast<std::uint32_t>(output.size());
    output += central_directory;
    append_u32(output, 0x06054b50u);
    append_u16(output, 0);
    append_u16(output, 0);
    append_u16(output, static_cast<std::uint16_t>(entries.size()));
    append_u16(output, static_cast<std::uint16_t>(entries.size()));
    append_u32(output, static_cast<std::uint32_t>(central_directory.size()));
    append_u32(output, central_offset);
    append_u16(output, 0);
    return output;
}

std::string detection_template_extension(const syn_sig_ra::PackTargetSummary& target) {
    for (std::vector<std::string>::const_iterator it =
             target.accepted_formats.begin();
         it != target.accepted_formats.end(); ++it) {
        if (*it == "hrv_json_v1") {
            return ".json";
        }
    }
    return ".csv";
}

std::string detection_template_content(
    const syn_sig_ra::PackTargetSummary& target,
    const std::string& case_id
) {
    if (detection_template_extension(target) == ".json") {
        std::ostringstream output;
        output << "{\n"
               << "  \"schema_version\": 1,\n"
               << "  \"case_id\": \"" << case_id << "\",\n"
               << "  \"target\": \"" << target.target << "\",\n"
               << "  \"metrics\": {\n"
               << "    \"sdnn_ms\": 0.0,\n"
               << "    \"rmssd_ms\": 0.0,\n"
               << "    \"mean_rr_ms\": 0.0,\n"
               << "    \"lf_hf_ratio\": 0.0\n"
               << "  }\n"
               << "}\n";
        return output.str();
    }
    if (target.target == "ecg_beat_classification") {
        return "time_seconds,sample_index,label,confidence\n"
               "0.000,0,N,1.0\n";
    }
    return "time_seconds,sample_index,channel,label,confidence\n"
           "0.000,0,,event,1.0\n";
}

std::string detection_template_readme(
    const syn_sig_ra::JobRecord& job,
    const syn_sig_ra::PackSummary& pack,
    const std::vector<ZipEntry>& template_entries
) {
    const std::string package_file = job.package_id + "-package.zip";
    const std::string output_dir = "verification-" + job.package_id;
    const std::string profile = pack.recommended_profile.empty()
        ? "regression" : pack.recommended_profile;
    std::ostringstream output;
    output << "# Detection templates for " << job.job_id << "\n\n"
           << "Pack: `" << pack.pack_id << "` " << pack.version << "\n\n"
           << "Copy this `detections/` folder next to the downloaded package, "
           << "replace example rows with your algorithm output, then run:\n\n"
           << "```sh\n"
           << "synsigra-verify \"" << package_file << "\" detections/ \""
           << output_dir << "\" --profile " << profile << " --force\n"
           << "```\n\n"
           << "The verifier writes `verification_summary.json`, "
           << "`verification_summary.csv`, and `verification_report.html` "
           << "under `" << output_dir << "/`.\n\n"
           << "Only locally scoreable targets are included. Reference-only "
           << "targets are intentionally not detector-output requirements.\n\n"
           << "## Files\n\n";
    for (std::vector<ZipEntry>::const_iterator it = template_entries.begin();
         it != template_entries.end(); ++it) {
        output << "- `" << it->path << "`\n";
    }
    output << "\n## Column notes\n\n"
           << "- Event-detection CSV required column: `time_seconds`, in seconds from case start.\n"
           << "- Event-detection CSV optional columns: `sample_index`, `channel`, `label`, `confidence`.\n"
           << "- Beat-classification CSV required columns: `time_seconds`, `label`.\n"
           << "- HRV JSON templates provide a minimal `metrics` object; replace values with algorithm output.\n";
    return output.str();
}

bool build_detection_template_zip(
    const syn_sig_ra::JobRecord& job,
    const syn_sig_ra::PackSummary& pack,
    std::string& zip,
    std::string& error
) {
    std::vector<ZipEntry> templates;
    for (std::vector<syn_sig_ra::PackTargetSummary>::const_iterator target =
             pack.scoreable_targets.begin();
         target != pack.scoreable_targets.end(); ++target) {
        const std::string extension = detection_template_extension(*target);
        for (std::vector<std::string>::const_iterator case_id =
                 target->case_ids.begin();
             case_id != target->case_ids.end(); ++case_id) {
            ZipEntry entry;
            entry.path = "detections/" + *case_id + "_" + target->target + extension;
            entry.content = detection_template_content(*target, *case_id);
            templates.push_back(entry);
        }
    }
    if (templates.empty()) {
        error = "pack has no locally scoreable detector-output templates";
        return false;
    }
    std::vector<ZipEntry> entries;
    ZipEntry readme;
    readme.path = "README.md";
    readme.content = detection_template_readme(job, pack, templates);
    entries.push_back(readme);
    entries.insert(entries.end(), templates.begin(), templates.end());
    zip = zip_store_archive(entries);
    return true;
}

json_t* job_json_object(
    const syn_sig_ra::JobRecord& job,
    const std::string& public_base_path
) {
    json_t* root = json_object();
    json_object_set_new(root, "job_id", json_string(job.job_id.c_str()));
    json_object_set_new(root, "project_id", json_string(job.project_id.c_str()));
    json_object_set_new(root, "status", json_string(job.status.c_str()));
    json_object_set_new(
        root,
        "pack_id",
        json_string(job.selected_pack_id.c_str())
    );
    json_object_set_new(
        root,
        "created_at",
        json_string(job.created_at.c_str())
    );
    if (!job.started_at.empty()) {
        json_object_set_new(
            root,
            "started_at",
            json_string(job.started_at.c_str())
        );
    }
    if (!job.completed_at.empty()) {
        json_object_set_new(
            root,
            "completed_at",
            json_string(job.completed_at.c_str())
        );
    }
    if (job.status == "succeeded") {
        if (job.package_id.empty()) {
            json_object_set_new(
                root, "artifact_status", json_string("expired")
            );
            return root;
        }
        json_object_set_new(
            root, "artifact_status", json_string("available")
        );
        json_object_set_new(
            root,
            "package_id",
            json_string(job.package_id.c_str())
        );
        json_object_set_new(
            root,
            "package_fingerprint",
            json_string(job.package_fingerprint.c_str())
        );
        json_object_set_new(
            root,
            "generator_version",
            json_string(job.generator_version.c_str())
        );
        json_object_set_new(
            root,
            "generator_build_identity",
            json_string(job.generator_build_identity.c_str())
        );
        json_object_set_new(
            root,
            "manifest_url",
            json_string(
                (
                    public_base_path + "/v1/artifacts/" + job.package_id +
                    "/manifest.json"
                ).c_str()
            )
        );
        json_object_set_new(
            root,
            "archive_url",
            json_string(
                (
                    public_base_path + "/v1/artifacts/" + job.package_id +
                    "/package.zip"
                ).c_str()
            )
        );
        json_object_set_new(
            root,
            "detection_templates_url",
            json_string(
                (
                    public_base_path + "/v1/jobs/" + job.job_id +
                    "/detection-templates.zip"
                ).c_str()
            )
        );
    } else if (job.status == "failed") {
        json_t* error = json_object();
        json_object_set_new(
            error,
            "code",
            json_string(job.error_code.c_str())
        );
        json_object_set_new(
            error,
            "message",
            json_string(job.error_message.c_str())
        );
        json_object_set_new(root, "error", error);
    }
    return root;
}

std::string job_json(
    const syn_sig_ra::JobRecord& job,
    const std::string& public_base_path
) {
    json_t* root = job_json_object(job, public_base_path);
    const std::string output = json_dump_line(root);
    json_decref(root);
    return output;
}

std::string job_list_json(
    const std::vector<syn_sig_ra::JobRecord>& jobs,
    const std::string& public_base_path,
    int limit,
    int offset
) {
    json_t* root = json_object();
    json_t* array = json_array();
    for (std::vector<syn_sig_ra::JobRecord>::const_iterator it = jobs.begin();
         it != jobs.end();
         ++it) {
        json_array_append_new(array, job_json_object(*it, public_base_path));
    }
    json_object_set_new(root, "jobs", array);
    json_object_set_new(
        root,
        "limit",
        json_integer(static_cast<json_int_t>(limit))
    );
    json_object_set_new(
        root,
        "count",
        json_integer(static_cast<json_int_t>(jobs.size()))
    );
    json_object_set_new(root, "offset", json_integer(offset));
    if (jobs.size() == static_cast<std::size_t>(limit)) {
        json_object_set_new(root, "next_offset", json_integer(offset + limit));
    }
    const std::string output = json_dump_line(root);
    json_decref(root);
    return output;
}

bool query_integer(
    const std::string& query,
    const std::string& name,
    int default_value,
    int& value
) {
    value = default_value;
    if (query.empty()) return true;
    const std::string needle = name + "=";
    const std::string::size_type start = query.find(needle);
    if (start == std::string::npos ||
        (start != 0 && query[start - 1] != '&')) return true;
    const std::string::size_type value_start = start + needle.size();
    const std::string::size_type end = query.find('&', value_start);
    const std::string encoded = query.substr(value_start, end - value_start);
    if (encoded.empty()) return false;
    char* parsed_end = nullptr;
    const long parsed = std::strtol(encoded.c_str(), &parsed_end, 10);
    if (*parsed_end != '\0' || parsed < 0 || parsed > 1000000) return false;
    value = static_cast<int>(parsed);
    return true;
}

json_t* project_json_object(const syn_sig_ra::ProjectRecord& project) {
    json_t* root = json_object();
    json_object_set_new(
        root, "project_id", json_string(project.project_id.c_str())
    );
    json_object_set_new(
        root, "display_name", json_string(project.display_name.c_str())
    );
    json_object_set_new(
        root, "created_at", json_string(project.created_at.c_str())
    );
    return root;
}

std::string usage_json(const syn_sig_ra::UsageSummary& usage) {
    json_t* root = json_object();
    json_object_set_new(root, "requests_last_minute",
                        json_integer(usage.requests_last_minute));
    json_object_set_new(root, "active_jobs", json_integer(usage.active_jobs));
    json_object_set_new(root, "jobs_this_month",
                        json_integer(usage.jobs_this_month));
    json_object_set_new(root, "packages_this_month",
                        json_integer(usage.packages_this_month));
    json_object_set_new(root, "package_bytes_this_month",
                        json_integer(usage.package_bytes_this_month));
    json_object_set_new(root, "queued_jobs", json_integer(usage.queued_jobs));
    json_object_set_new(root, "running_jobs", json_integer(usage.running_jobs));
    json_object_set_new(root, "failed_jobs_this_month",
                        json_integer(usage.failed_jobs_this_month));
    json_object_set_new(root, "quota_rejections_this_month",
                        json_integer(usage.quota_rejections_this_month));
    json_object_set_new(root, "worker_last_seen_at",
                        json_string(usage.worker_last_seen_at.c_str()));
    json_object_set_new(root, "worker_last_status",
                        json_string(usage.worker_last_status.c_str()));
    json_t* limits = json_object();
    json_object_set_new(limits, "requests_per_minute",
                        json_integer(usage.request_limit_per_minute));
    json_object_set_new(limits, "concurrent_jobs",
                        json_integer(usage.concurrent_job_limit));
    json_object_set_new(limits, "jobs_per_month",
                        json_integer(usage.monthly_job_limit));
    json_object_set_new(root, "limits", limits);
    const std::string output = json_dump_line(root);
    json_decref(root);
    return output;
}

json_t* scenario_draft_json_object(
    const syn_sig_ra::ScenarioDraftRecord& draft
) {
    json_t* root = json_object();
    json_object_set_new(root, "scenario_id", json_string(draft.scenario_id.c_str()));
    json_object_set_new(root, "name", json_string(draft.name.c_str()));
    json_object_set_new(root, "status", json_string(draft.status.c_str()));
    json_object_set_new(
        root, "document_fingerprint",
        draft.document_fingerprint.empty()
            ? json_null()
            : json_string(draft.document_fingerprint.c_str())
    );
    json_error_t parse_error;
    json_t* document = json_loads(
        draft.document_json.c_str(), JSON_REJECT_DUPLICATES, &parse_error
    );
    json_t* validation_errors = json_loads(
        draft.validation_errors_json.c_str(), JSON_REJECT_DUPLICATES, &parse_error
    );
    json_object_set_new(root, "scenario", document == nullptr ? json_null() : document);
    json_object_set_new(
        root, "validation_errors",
        validation_errors == nullptr ? json_array() : validation_errors
    );
    json_object_set_new(root, "created_at", json_string(draft.created_at.c_str()));
    json_object_set_new(root, "updated_at", json_string(draft.updated_at.c_str()));
    return root;
}

json_t* custom_pack_json_object(const syn_sig_ra::CustomPackRecord& pack) {
    json_t* root = json_object();
    json_object_set_new(root, "pack_id", json_string(pack.pack_id.c_str()));
    json_object_set_new(root, "display_name", json_string(pack.name.c_str()));
    json_object_set_new(root, "version", json_string(pack.version.c_str()));
    json_object_set_new(root, "description", json_string(pack.description.c_str()));
    json_object_set_new(
        root, "pack_fingerprint", json_string(pack.pack_fingerprint.c_str()));
    json_object_set_new(root, "source", json_string("custom"));
    json_error_t parse_error;
    json_t* targets = json_loads(
        pack.targets_json.c_str(), JSON_REJECT_DUPLICATES, &parse_error);
    json_t* scenarios = json_loads(
        pack.scenario_ids_json.c_str(), JSON_REJECT_DUPLICATES, &parse_error);
    json_object_set_new(root, "targets", targets == nullptr ? json_array() : targets);
    json_object_set_new(
        root, "scenario_ids", scenarios == nullptr ? json_array() : scenarios);
    json_object_set_new(root, "created_at", json_string(pack.created_at.c_str()));
    return root;
}

bool ensure_directory(const std::string& path, std::string& error) {
    if (mkdir(path.c_str(), 0750) == 0 || errno == EEXIST) return true;
    error = "unable to create custom pack directory";
    return false;
}

bool write_private_file(
    const std::string& path,
    const std::string& content,
    std::string& error
) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        error = "unable to write custom pack snapshot";
        return false;
    }
    chmod(path.c_str(), 0440);
    return true;
}

bool validate_scenario_document(
    json_t* scenario,
    std::string& status,
    std::string& canonical_json,
    std::string& fingerprint,
    std::string& errors_json
) {
    char* submitted = json_dumps(scenario, JSON_COMPACT | JSON_SORT_KEYS);
    if (submitted == nullptr) return false;
    const std::string submitted_json(submitted);
    free(submitted);
    signal_synth::synsigra_validation_result result;
    const bool valid =
        signal_synth::synsigra_validate_scenario_json(submitted_json, result);
    status = valid ? "valid" : "invalid";
    canonical_json = valid ? result.canonical_scenario_json : submitted_json;
    fingerprint = valid ? result.identity.document_fingerprint : "";
    json_t* errors = json_array();
    for (std::vector<signal_synth::synsigra_message>::const_iterator it =
             result.messages.begin(); it != result.messages.end(); ++it) {
        json_t* item = json_object();
        json_object_set_new(item, "code", json_string(it->code.c_str()));
        json_object_set_new(item, "path", json_string(it->path.c_str()));
        json_object_set_new(item, "message", json_string(it->message.c_str()));
        json_array_append_new(errors, item);
    }
    errors_json = json_dump_line(errors);
    if (!errors_json.empty() && errors_json[errors_json.size() - 1] == '\n') {
        errors_json.erase(errors_json.size() - 1);
    }
    json_decref(errors);
    return valid;
}

const char kUiHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SynSigRa SaaS</title>
  <link rel="stylesheet" href="/syn_sig_ra/ui/style.css">
</head>
<body>
  <main class="shell">
    <section id="overview" class="hero">
      <div>
        <p class="eyebrow">SynSigRa private beta</p>
        <h1>Challenge package generator</h1>
        <p class="lede">Generate deterministic synthetic biosignal QA packages from curated packs, then download the manifest and archive.</p>
      </div>
      <div class="status-card">
        <div class="label">Service</div>
        <div id="health-status" class="status">checking…</div>
        <div id="readiness-status" class="muted">checking components…</div>
      </div>
    </section>

    <nav class="quick-nav" aria-label="Page sections">
      <a href="#generate">Generate</a>
      <a href="#jobs-section">Jobs</a>
      <a href="#scenario-workbench">Scenarios</a>
      <a href="#custom-pack-workbench">Custom packs</a>
      <a href="#account">Account</a>
      <a href="#usage-section">Usage</a>
      <a href="#documentation">Documentation</a>
    </nav>

    <aside class="workflow" aria-label="Recommended workflow">
      <strong>Workflow</strong>
      <span>1. Choose or draft scenarios</span>
      <span>2. Choose or compose a pack</span>
      <span>3. Generate and download</span>
      <span>4. Run and verify your algorithm locally</span>
    </aside>

    <section id="account" class="panel">
      <div id="signed-out-account">
        <h2>Sign in or create an account</h2>
        <p class="muted">Browser access uses a secure session cookie. API keys are only needed for scripts and CI.</p>
        <div class="auth-grid">
          <div>
            <h3>Sign in</h3>
            <label for="login-email">Email</label>
            <input id="login-email" type="email" autocomplete="email">
            <label for="login-password">Password</label>
            <input id="login-password" type="password" autocomplete="current-password">
            <button id="login" class="primary">Sign in</button>
          </div>
          <div>
            <h3>Create account</h3>
            <label for="register-name">Display name</label>
            <input id="register-name" type="text" maxlength="100" autocomplete="name">
            <label for="register-email">Email</label>
            <input id="register-email" type="email" autocomplete="email">
            <label for="register-password">Password (12+ characters)</label>
            <input id="register-password" type="password" minlength="12" maxlength="128" autocomplete="new-password">
            <button id="register" class="primary">Create account</button>
          </div>
        </div>
      </div>
      <div id="signed-in-account" hidden>
        <div class="panel-heading">
          <div>
            <h2 id="account-name">Account</h2>
            <p id="account-email" class="muted"></p>
          </div>
          <button id="logout" class="secondary">Sign out</button>
        </div>
        <h3>Personal API keys</h3>
        <p class="muted">Create keys for scripts or CI. A new secret is shown once.</p>
        <div class="row">
          <input id="api-key-label" type="text" maxlength="100" placeholder="CI or workstation">
          <button id="create-api-key" class="secondary">Create API key</button>
        </div>
        <pre id="api-key-secret" class="output"></pre>
        <div id="api-keys" class="jobs"></div>
      </div>
      <p id="auth-output" class="muted" aria-live="polite"></p>
    </section>

    <section id="generate" class="grid">
      <div class="panel">
        <div class="panel-heading">
          <h2>Packs</h2>
          <button id="refresh-packs" class="secondary">Refresh</button>
        </div>
        <div class="filter-row">
          <label>
            Scoreable target
            <select id="pack-target-filter"></select>
          </label>
          <label>
            Difficulty
            <select id="pack-difficulty-filter"></select>
          </label>
        </div>
        <div id="packs" class="cards"></div>
      </div>

      <div class="panel">
        <div class="panel-heading">
          <h2>Create job</h2>
        </div>
        <label for="project-select">Project</label>
        <select id="project-select"></select>
        <div class="row">
          <input id="project-name" type="text" maxlength="100" placeholder="New project name">
          <button id="create-project" class="secondary" disabled>Create project</button>
        </div>
        <label for="pack-select">Pack</label>
        <select id="pack-select"></select>
        <div id="selected-pack-summary" class="selected-pack muted"></div>
        <p class="muted compact">Generation produces the complete challenge export set. Format options are not configurable per job.</p>
        <button id="create-job" class="primary" disabled>Create challenge job</button>
        <pre id="create-output" class="output"></pre>
      </div>
    </section>

    <section id="jobs-section" class="panel">
      <div class="panel-heading">
        <h2>Jobs</h2>
        <div class="actions no-margin">
          <span id="jobs-sync-status" class="muted" aria-live="polite"></span>
          <button id="refresh-jobs" class="secondary">Refresh</button>
        </div>
      </div>
      <p class="muted">The list polls in place. Download a completed ZIP, run your algorithm locally, then score its outputs with the verification helper.</p>
      <div id="jobs" class="jobs"></div>
      <button id="load-more-jobs" class="secondary" hidden>Load older jobs</button>
    </section>
    <section id="usage-section" class="panel">
      <div class="panel-heading">
        <h2>Usage</h2>
        <button id="refresh-usage" class="secondary">Refresh</button>
      </div>
      <div id="usage" class="muted">Sign in to inspect usage.</div>
    </section>
    <section id="metrics-panel" class="panel" hidden>
      <div class="panel-heading">
        <h2>Operational metrics</h2>
        <button id="refresh-metrics" class="secondary">Refresh</button>
      </div>
      <div id="metrics" class="muted"></div>
    </section>
    <section id="scenario-workbench" class="panel">
      <div class="panel-heading">
        <h2>Scenario drafts</h2>
        <button id="new-scenario" class="secondary">New draft</button>
      </div>
      <p class="muted">Start from a working ECG example or edit raw JSON. Invalid drafts remain editable and show validation details.</p>
      <p class="verify-note"><strong>No PHI:</strong> use synthetic engineering scenarios only. Do not enter patient identifiers, clinical notes, personal data, or diagnostic claims.</p>
      <label for="scenario-name">Name</label>
      <input id="scenario-name" type="text" maxlength="100" placeholder="Scenario name">
      <label for="scenario-json">Scenario JSON</label>
      <textarea id="scenario-json" rows="16" spellcheck="false">{}</textarea>
      <div class="actions">
        <button id="load-scenario-template" class="secondary">Load clean ECG example</button>
        <button id="format-scenario-json" class="secondary">Format JSON</button>
        <button id="save-scenario" class="primary" disabled>Validate and save</button>
      </div>
      <pre id="scenario-output" class="output"></pre>
      <div id="scenarios" class="jobs"></div>
    </section>
    <section id="custom-pack-workbench" class="panel">
      <div class="panel-heading">
        <h2>Custom pack composer</h2>
        <button id="refresh-custom-packs" class="secondary">Refresh</button>
      </div>
      <p class="verify-note"><strong>No PHI:</strong> pack names and descriptions must not contain patient data, personal identifiers, clinical notes, or diagnostic-use claims.</p>
      <label for="custom-pack-name">Pack name</label>
      <input id="custom-pack-name" type="text" maxlength="100" placeholder="My validation pack">
      <label for="custom-pack-description">Description</label>
      <input id="custom-pack-description" type="text" placeholder="What this pack tests">
      <label for="custom-pack-targets">Targets (comma-separated)</label>
      <input id="custom-pack-targets" type="text" value="r_peak" list="target-suggestions">
      <datalist id="target-suggestions">
        <option value="r_peak">
        <option value="ppg_systolic_peak">
        <option value="beat_classification">
        <option value="signal_quality">
        <option value="hrv">
      </datalist>
      <p class="muted compact">Select at least one valid draft. The resulting pack snapshots its scenarios; later draft edits do not alter it.</p>
      <div id="pack-scenario-options" class="cards"></div>
      <button id="create-custom-pack" class="primary" disabled>Create immutable custom pack</button>
      <pre id="custom-pack-output" class="output"></pre>
      <div id="custom-packs" class="jobs"></div>
    </section>
    <section id="documentation" class="panel">
      <h2>Documentation</h2>
      <p><a href="/syn_sig_ra/docs/quickstart">One-page quickstart</a></p>
      <p><a href="/syn_sig_ra/docs/api">Rendered API reference</a></p>
      <p><a href="/syn_sig_ra/docs/troubleshooting">Troubleshooting guide</a></p>
      <p><a href="https://github.com/tamask1s/signal_synth_saas/blob/master/README.md" target="_blank" rel="noopener">Full user manual</a></p>
      <p><a href="https://github.com/tamask1s/signal_synth_saas/blob/master/doc/openapi.yaml" target="_blank" rel="noopener">Raw OpenAPI YAML</a></p>
    </section>
  </main>
  <script src="/syn_sig_ra/ui/app.js"></script>
</body>
</html>
)HTML";

const char kQuickstartHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SynSigRa quickstart</title>
  <link rel="stylesheet" href="/syn_sig_ra/ui/style.css">
</head>
<body>
  <main class="shell">
    <section class="panel">
      <p class="eyebrow">SynSigRa docs</p>
      <h1>One-page quickstart</h1>
      <p class="verify-note"><strong>Synthetic engineering data only.</strong> Do not enter patient data, personal identifiers, clinical notes, PHI, or diagnostic-use claims.</p>
      <ol>
        <li>Open <a href="/syn_sig_ra/">the web UI</a>, create an account, or sign in.</li>
        <li>Use the default project or create a project if your role allows it.</li>
        <li>Choose a curated pack. Check <strong>Scoreable locally</strong>, <strong>Reference-only</strong>, duration, sampling rate, channel count, and recommended verifier profile before creating the job.</li>
        <li>Create a challenge job and wait for <code>succeeded</code>.</li>
        <li>Download <code>manifest.json</code>, <code>package.zip</code>, and <code>detection-templates.zip</code>.</li>
        <li>Unzip the detection templates and replace example rows under <code>detections/</code> with your algorithm output.</li>
        <li>Install the local verifier: <code>python -m pip install ../signal_synth</code>.</li>
        <li>Copy the exact <code>synsigra-verify</code> command from the completed job card and run it next to the downloaded package.</li>
        <li>Inspect <code>verification_summary.json</code>, <code>verification_summary.csv</code>, and <code>verification_report.html</code>.</li>
      </ol>
      <pre class="output">synsigra-verify "pkg_123-package.zip" detections/ "verification-pkg_123" --profile stress --force</pre>
      <p>Exit code <code>0</code> means pass, <code>1</code> means verification/input/scoring/threshold failure, and <code>2</code> means invalid CLI usage.</p>
      <p><a href="/syn_sig_ra/docs/api">Rendered API reference</a> · <a href="/syn_sig_ra/docs/troubleshooting">Troubleshooting</a> · <a href="/syn_sig_ra/">Back to app</a></p>
    </section>
  </main>
</body>
</html>
)HTML";

const char kApiDocsHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SynSigRa API reference</title>
  <link rel="stylesheet" href="/syn_sig_ra/ui/style.css">
</head>
<body>
  <main class="shell">
    <section class="panel">
      <p class="eyebrow">SynSigRa docs</p>
      <h1>Rendered API reference</h1>
      <p>Base URL: <code>https://www.timeonion.com/syn_sig_ra</code>. Browser calls use the secure session cookie. Scripts and CI use <code>Authorization: Bearer &lt;api-key&gt;</code>.</p>
      <p class="verify-note"><strong>No PHI:</strong> API requests, project names, labels, scenario drafts, and custom-pack text must contain synthetic engineering data only.</p>
      <table>
        <thead><tr><th>Method</th><th>Path</th><th>Purpose</th><th>Auth</th></tr></thead>
        <tbody>
          <tr><td>GET</td><td><code>/healthz</code></td><td>Liveness/build</td><td>Public</td></tr>
          <tr><td>GET</td><td><code>/readyz</code></td><td>Readiness and disk</td><td>Public</td></tr>
          <tr><td>GET</td><td><code>/v1/packs</code></td><td>Rich curated pack catalog</td><td>Public</td></tr>
          <tr><td>GET</td><td><code>/v1/packs/{pack_id}</code></td><td>Pack detail including scoreable/reference-only targets</td><td>Public</td></tr>
          <tr><td>POST</td><td><code>/v1/auth/register</code></td><td>Create account and session</td><td>Public</td></tr>
          <tr><td>POST</td><td><code>/v1/auth/login</code></td><td>Start browser session</td><td>Public</td></tr>
          <tr><td>GET</td><td><code>/v1/auth/me</code></td><td>Current account</td><td>Session</td></tr>
          <tr><td>POST</td><td><code>/v1/auth/logout</code></td><td>End browser session</td><td>Session</td></tr>
          <tr><td>GET/POST</td><td><code>/v1/projects</code></td><td>List/create projects</td><td>Authenticated</td></tr>
          <tr><td>GET/POST/DELETE</td><td><code>/v1/api-keys</code></td><td>Manage personal API keys</td><td>Authenticated</td></tr>
          <tr><td>GET/POST/PUT/DELETE</td><td><code>/v1/scenarios</code></td><td>Scenario draft lifecycle</td><td>Authenticated</td></tr>
          <tr><td>GET/POST/DELETE</td><td><code>/v1/custom-packs</code></td><td>Compose/list/hide custom packs</td><td>Authenticated</td></tr>
          <tr><td>GET/POST</td><td><code>/v1/jobs</code></td><td>List/create jobs</td><td>Authenticated</td></tr>
          <tr><td>GET/DELETE</td><td><code>/v1/jobs/{job_id}</code></td><td>Read or soft-delete job</td><td>Organization</td></tr>
          <tr><td>POST</td><td><code>/v1/jobs/{job_id}/cancel</code></td><td>Cancel queued job</td><td>Developer+</td></tr>
          <tr><td>POST</td><td><code>/v1/jobs/{job_id}/retry</code></td><td>Retry failed/cancelled job</td><td>Developer+</td></tr>
          <tr><td>GET</td><td><code>/v1/jobs/{job_id}/detection-templates.zip</code></td><td>Detector-output templates for completed curated jobs</td><td>Organization</td></tr>
          <tr><td>GET</td><td><code>/v1/artifacts/{package_id}/manifest.json</code></td><td>Download manifest</td><td>Organization</td></tr>
          <tr><td>GET</td><td><code>/v1/artifacts/{package_id}/package.zip</code></td><td>Download package ZIP</td><td>Organization</td></tr>
          <tr><td>GET</td><td><code>/v1/usage</code></td><td>Caller usage and limits</td><td>Authenticated</td></tr>
          <tr><td>GET</td><td><code>/v1/metrics</code></td><td>Operational metrics</td><td>Owner/admin</td></tr>
        </tbody>
      </table>
      <h2>Minimal curl client</h2>
      <pre class="output">read -r -s SYN_SIG_RA_API_KEY
BASE=https://www.timeonion.com/syn_sig_ra
curl -fsS "$BASE/v1/packs"
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" "$BASE/v1/projects"
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" -H "Content-Type: application/json" \
  -d '{"project_id":"org_live_default","pack_id":"r_peak_stress_v1"}' "$BASE/v1/jobs"</pre>
      <h2>Minimal Python client</h2>
      <pre class="output">import json, os, urllib.request
base = "https://www.timeonion.com/syn_sig_ra"
key = os.environ["SYN_SIG_RA_API_KEY"]
req = urllib.request.Request(base + "/v1/jobs", data=json.dumps({
    "project_id": "org_live_default",
    "pack_id": "r_peak_stress_v1",
}).encode(), headers={
    "Authorization": "Bearer " + key,
    "Content-Type": "application/json",
})
print(urllib.request.urlopen(req).read().decode())</pre>
      <p>Raw machine-readable contract: <a href="https://github.com/tamask1s/signal_synth_saas/blob/master/doc/openapi.yaml" target="_blank" rel="noopener">OpenAPI YAML</a>.</p>
      <p><a href="/syn_sig_ra/docs/quickstart">Quickstart</a> · <a href="/syn_sig_ra/docs/troubleshooting">Troubleshooting</a> · <a href="/syn_sig_ra/">Back to app</a></p>
    </section>
  </main>
</body>
</html>
)HTML";

const char kTroubleshootingHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SynSigRa troubleshooting</title>
  <link rel="stylesheet" href="/syn_sig_ra/ui/style.css">
</head>
<body>
  <main class="shell">
    <section class="panel">
      <p class="eyebrow">SynSigRa docs</p>
      <h1>Troubleshooting</h1>
      <table>
        <thead><tr><th>Symptom / code</th><th>Action</th></tr></thead>
        <tbody>
          <tr><td><code>401 unauthorized</code></td><td>Sign in again in the browser, or create/use a valid personal API key for scripts.</td></tr>
          <tr><td><code>403 forbidden</code></td><td>Your role cannot perform the operation. Ask an owner/admin or use a developer/owner account.</td></tr>
          <tr><td><code>404 *_not_found</code></td><td>Check the ID. Cross-organization resources intentionally return 404.</td></tr>
          <tr><td><code>409 job_*_invalid_state</code></td><td>Cancel only queued jobs; retry only failed/cancelled jobs; delete only non-running jobs.</td></tr>
          <tr><td><code>409 pack_generator_incompatible</code></td><td>Select a current curated pack. Deprecated/incompatible packs cannot be generated.</td></tr>
          <tr><td><code>409 job_templates_unavailable</code></td><td>Wait until the job succeeds. Template ZIPs are only available for completed curated jobs with scoreable targets.</td></tr>
          <tr><td><code>429 request_rate_limit</code></td><td>Slow the client down. The private-beta limit is 120 requests/minute per key.</td></tr>
          <tr><td><code>429 concurrent_job_limit</code></td><td>Wait for queued/running jobs to finish. Current limit is 2 active jobs per organization.</td></tr>
          <tr><td><code>429 monthly_job_limit</code></td><td>Monthly job quota is exhausted. Use existing packages or contact the operator.</td></tr>
          <tr><td>Failed job</td><td>Open the job card and read the error. If it is a generator/catalog issue, keep the job ID and report it.</td></tr>
          <tr><td>Expired artifact</td><td>Regenerate the job from the same pack version if the retained package is gone. Archive downloaded packages locally for audit evidence.</td></tr>
          <tr><td>Verifier exit <code>1</code></td><td>Check package path, detection filenames, required columns, units, selected profile, and per-case report.</td></tr>
          <tr><td>Verifier exit <code>2</code></td><td>Fix CLI arguments. Start from the command in the completed-job recipe panel.</td></tr>
        </tbody>
      </table>
      <p class="verify-note"><strong>Boundary:</strong> SynSigRa is synthetic engineering QA tooling, not clinical validation, diagnosis, patient monitoring, or PHI storage.</p>
      <p><a href="/syn_sig_ra/docs/quickstart">Quickstart</a> · <a href="/syn_sig_ra/docs/api">Rendered API reference</a> · <a href="/syn_sig_ra/">Back to app</a></p>
    </section>
  </main>
</body>
</html>
)HTML";

const char kUiCss[] = R"CSS(:root {
  color-scheme: light;
  --bg: #f6f7fb;
  --panel: #ffffff;
  --text: #172033;
  --muted: #667085;
  --border: #d9deea;
  --primary: #2258e8;
  --primary-dark: #1644bb;
  --danger: #b42318;
  --ok: #067647;
  --warn: #b54708;
}

* { box-sizing: border-box; }
body {
  margin: 0;
  background: var(--bg);
  color: var(--text);
  font: 15px/1.5 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
}

.shell {
  max-width: 1180px;
  margin: 0 auto;
  padding: 32px 20px 64px;
}

.hero {
  display: grid;
  grid-template-columns: 1fr minmax(220px, 280px);
  gap: 24px;
  align-items: stretch;
  margin-bottom: 24px;
}

.quick-nav {
  position: sticky;
  top: 0;
  z-index: 10;
  display: flex;
  gap: 8px;
  overflow-x: auto;
  margin: 0 0 16px;
  padding: 10px;
  background: rgba(246, 247, 251, .94);
  border: 1px solid var(--border);
  border-radius: 14px;
  backdrop-filter: blur(8px);
}
.quick-nav a {
  flex: 0 0 auto;
  color: var(--primary);
  padding: 6px 10px;
  font-weight: 700;
  text-decoration: none;
}
.quick-nav a:hover { text-decoration: underline; }

.workflow {
  display: flex;
  flex-wrap: wrap;
  gap: 8px 18px;
  align-items: center;
  margin-bottom: 20px;
  padding: 14px 16px;
  background: #eef4ff;
  border: 1px solid #c7d7fe;
  border-radius: 14px;
  color: #344054;
}

.eyebrow {
  margin: 0 0 8px;
  color: var(--primary);
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: .08em;
  font-size: 12px;
}

h1, h2, h3, p { margin-top: 0; }
h1 { font-size: clamp(32px, 5vw, 56px); line-height: 1; margin-bottom: 16px; }
h2 { font-size: 20px; margin-bottom: 14px; }
h3 { font-size: 16px; margin-bottom: 8px; }
.lede { max-width: 720px; color: var(--muted); font-size: 18px; }
.muted { color: var(--muted); }
.compact { margin-bottom: 8px; }

.panel, .status-card {
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: 18px;
  padding: 20px;
  box-shadow: 0 8px 24px rgba(16, 24, 40, .06);
}

.status-card {
  display: flex;
  flex-direction: column;
  justify-content: center;
}

.label {
  color: var(--muted);
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: .08em;
}

.status {
  margin-top: 8px;
  font-weight: 700;
  font-size: 20px;
}

.grid {
  display: grid;
  grid-template-columns: minmax(0, 1.3fr) minmax(320px, .7fr);
  gap: 20px;
  margin: 20px 0;
}

.auth-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 28px;
}

.row, .panel-heading {
  display: flex;
  gap: 10px;
  align-items: center;
}

.panel-heading {
  justify-content: space-between;
  margin-bottom: 12px;
}

input, select, button {
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 11px 12px;
  font: inherit;
}

input, select {
  background: #fff;
  min-width: 0;
  width: 100%;
}
label { display: block; margin: 10px 0 5px; font-weight: 600; }
.filter-row {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 10px;
  margin-bottom: 12px;
}
textarea {
  width: 100%;
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 10px 12px;
  font: 13px/1.45 ui-monospace, SFMono-Regular, Consolas, monospace;
  resize: vertical;
}
table {
  width: 100%;
  border-collapse: collapse;
  margin: 14px 0 20px;
}
th, td {
  border-bottom: 1px solid var(--border);
  padding: 9px 8px;
  text-align: left;
  vertical-align: top;
}
th { color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: .04em; }
code {
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: .92em;
}

button {
  cursor: pointer;
  background: var(--primary);
  border-color: var(--primary);
  color: #fff;
  font-weight: 700;
  white-space: nowrap;
}

button:hover { background: var(--primary-dark); }
button.secondary {
  background: #fff;
  border-color: var(--border);
  color: var(--text);
}
button.secondary:hover { background: #eef2ff; }
button.primary { width: 100%; margin-top: 12px; }
button.danger {
  background: #fff;
  border-color: #fda29b;
  color: var(--danger);
}
button.danger:hover { background: #fee4e2; }
button:disabled { opacity: .55; cursor: not-allowed; }

.cards {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
  gap: 12px;
}

.card, .job {
  border: 1px solid var(--border);
  border-radius: 14px;
  padding: 14px;
  background: #fff;
}

.fingerprint {
  display: block;
  overflow-wrap: anywhere;
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 12px;
  color: var(--muted);
}

.jobs {
  display: grid;
  gap: 12px;
}

.job-header {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  align-items: flex-start;
}

.badge {
  border-radius: 999px;
  padding: 4px 9px;
  font-size: 12px;
  font-weight: 700;
  background: #eef2ff;
  color: var(--primary);
}

.badge.succeeded { background: #dcfae6; color: var(--ok); }
.badge.failed { background: #fee4e2; color: var(--danger); }
.badge.running { background: #fef0c7; color: var(--warn); }
.tag-list {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin: 8px 0;
}
.tag {
  display: inline-flex;
  align-items: center;
  border-radius: 999px;
  padding: 3px 8px;
  background: #f2f4f7;
  color: #344054;
  font-size: 12px;
  font-weight: 700;
}
.tag.scoreable { background: #dcfae6; color: var(--ok); }
.tag.reference { background: #fff6ed; color: var(--warn); }
.tag.mode { background: #eef2ff; color: var(--primary); }
.selected-pack {
  margin-top: 10px;
  padding: 10px 12px;
  border: 1px solid var(--border);
  border-radius: 12px;
  background: #f9fafb;
}

.actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 12px;
}
.actions.no-margin { margin-top: 0; align-items: center; }
.actions .primary { width: auto; margin-top: 0; }

details.meta {
  margin-top: 10px;
  padding: 8px 0;
}
details.meta summary { cursor: pointer; font-weight: 700; }
.meta-grid {
  display: grid;
  grid-template-columns: max-content minmax(0, 1fr);
  gap: 5px 12px;
  margin-top: 10px;
}
.meta-grid dt { color: var(--muted); }
.meta-grid dd { margin: 0; overflow-wrap: anywhere; }
.verify-note {
  margin-top: 12px;
  padding: 10px 12px;
  background: #ecfdf3;
  border: 1px solid #abefc6;
  border-radius: 10px;
}
.card input[type="checkbox"] { width: auto; margin-right: 8px; }
.output:empty { display: none; }

.output {
  min-height: 44px;
  max-height: 220px;
  overflow: auto;
  background: #101828;
  color: #f2f4f7;
  border-radius: 12px;
  padding: 12px;
  white-space: pre-wrap;
}

.error { color: var(--danger); }
.ok { color: var(--ok); }

@media (max-width: 820px) {
  .hero, .grid, .auth-grid, .filter-row { grid-template-columns: 1fr; }
  .row { align-items: stretch; flex-direction: column; }
  .meta-grid { grid-template-columns: 1fr; }
}
)CSS";

const char kUiJs[] = R"JS((() => {
  const base = "/syn_sig_ra";
  const state = {
    authenticated: false,
    account: null,
    apiKeys: [],
    role: "",
    packs: [],
    packTargetFilter: "",
    packDifficultyFilter: "",
    projects: [],
    scenarios: [],
    customPacks: [],
    selectedScenarioId: "",
    jobs: [],
    jobsFingerprint: "",
    jobsLoaded: false,
    jobPollInFlight: false,
    jobsNextOffset: null
  };

  const $ = (id) => document.getElementById(id);
  const cleanEcgTemplate = {
    schema_version: 1,
    scenario_id: "ecg_clean_001",
    name: "Clean ECG",
    description: "Deterministic clean ECG engineering scenario.",
    author: "Synsigra",
    tags: ["clean", "ecg"],
    duration_seconds: 10,
    sample_rate_hz: 500,
    seed: 12345,
    ecg: {
      heart_rate_bpm: 70,
      rr_variability_seconds: 0,
      ectopic_every_n_beats: 0,
      second_degree_av_pattern: "unspecified",
      q_wave_territory: "unspecified",
      episode_type: "none",
      episode_start_seconds: 2,
      episode_duration_seconds: 4,
      episode_rate_bpm: 170,
      fidelity_policy: "allow_parameterized",
      conditions: [{ code: "NORM", severity: 1 }]
    }
  };

  function setText(id, text, className) {
    const node = $(id);
    node.textContent = text;
    node.className = className || "";
  }

  function formatBytes(value) {
    const bytes = Number(value);
    if (!Number.isFinite(bytes) || bytes < 0) return "n/a";
    if (bytes < 1024) return `${bytes} B`;
    const units = ["KiB", "MiB", "GiB", "TiB"];
    let amount = bytes;
    let unit = -1;
    do {
      amount /= 1024;
      unit += 1;
    } while (amount >= 1024 && unit < units.length - 1);
    return `${amount.toLocaleString(undefined, { maximumFractionDigits: 1 })} ${units[unit]}`;
  }

  function formatDate(value) {
    if (!value) return "—";
    if (/^\d{4}-\d{2}-\d{2}$/.test(value)) return value;
    const date = new Date(value);
    return Number.isNaN(date.getTime())
      ? String(value)
      : new Intl.DateTimeFormat(undefined, {
          dateStyle: "medium",
          timeStyle: "medium"
        }).format(date);
  }

  function canWrite() {
    return ["owner", "admin", "developer"].includes(state.role);
  }

  function headers(json) {
    const h = {};
    if (json) h["Content-Type"] = "application/json";
    return h;
  }

  async function api(path, options = {}) {
    const response = await fetch(base + path, {
      ...options,
      credentials: "same-origin",
      headers: { ...headers(options.json), ...(options.headers || {}) },
      body: options.json ? JSON.stringify(options.json) : options.body
    });
    const text = await response.text();
    let body = null;
    try { body = text ? JSON.parse(text) : null; } catch (_) {}
    if (!response.ok) {
      const message = body && body.error ? `${body.error.code}: ${body.error.message}` : text || response.statusText;
      const error = new Error(message);
      error.body = body;
      error.status = response.status;
      if (response.status === 401 && !path.startsWith("/v1/auth/")) {
        state.account = null;
        state.authenticated = false;
        state.role = "";
        renderAuthState();
        setText("auth-output", "Your session expired. Sign in again.", "error");
      }
      throw error;
    }
    return body;
  }

  async function checkHealth() {
    try {
      const body = await api("/healthz");
      setText("health-status", `${body.status} (${body.build.version})`, "status ok");
    } catch (error) {
      setText("health-status", error.message, "status error");
    }
    try {
      const ready = await api("/readyz");
      const components = ["database", "generator", "pack_catalog", "artifact_store"]
        .map((name) => `${name}: ${ready[name] ? "ok" : "failed"}`)
        .join(" · ");
      setText(
        "readiness-status",
        `${ready.status} · ${components} · ${formatBytes(ready.disk_free_bytes)} free`,
        ready.status === "ready" ? "muted ok" : "muted error"
      );
    } catch (error) {
      setText("readiness-status", error.message, "muted error");
    }
  }

  function renderAuthState() {
    $("signed-out-account").hidden = state.authenticated;
    $("signed-in-account").hidden = !state.authenticated;
    if (state.authenticated && state.account) {
      $("account-name").textContent = state.account.display_name;
      $("account-email").textContent =
        `${state.account.email} · role: ${state.account.role}`;
    }
  }

  function renderPackOptions() {
    const select = $("pack-select");
    const selected = select.value;
    select.innerHTML = "";
    [...state.packs, ...state.customPacks].forEach((pack) => {
      const option = document.createElement("option");
      option.value = pack.pack_id;
      option.textContent = pack.source === "custom"
        ? `${pack.display_name} (${pack.version}, custom)`
        : `${pack.display_name || pack.pack_id} (${pack.version}, ${pack.release_status})`;
      select.appendChild(option);
    });
    if ([...select.options].some((option) => option.value === selected)) {
      select.value = selected;
    }
    renderSelectedPackSummary();
  }

  function uniqueSorted(values) {
    return [...new Set(values.filter(Boolean))].sort((a, b) => a.localeCompare(b));
  }

  function targetNames(targets) {
    return (targets || []).map((target) => target.target || target).filter(Boolean);
  }

  function targetTags(targets, className) {
    const items = targetNames(targets);
    return items.length
      ? `<span class="tag-list">${items.map((name) => `<span class="tag ${className}">${escapeHtml(name)}</span>`).join("")}</span>`
      : "<span class=\"muted\">none</span>";
  }

  function formatSeconds(seconds) {
    const value = Number(seconds);
    if (!Number.isFinite(value) || value <= 0) return "n/a";
    if (value < 90) return `${value}s`;
    const minutes = value / 60;
    return `${minutes.toLocaleString(undefined, { maximumFractionDigits: 1 })} min`;
  }

  function channelRange(pack) {
    const channels = pack.channels || {};
    const min = channels.minimum_channel_count;
    const max = channels.maximum_channel_count;
    if (!min && !max) return "n/a";
    return min === max ? `${min}` : `${min}-${max}`;
  }

  function renderPackFilters() {
    const targetSelect = $("pack-target-filter");
    const difficultySelect = $("pack-difficulty-filter");
    const selectedTarget = state.packTargetFilter;
    const selectedDifficulty = state.packDifficultyFilter;
    const targets = uniqueSorted(state.packs.flatMap((pack) => targetNames(pack.scoreable_targets)));
    const difficulties = uniqueSorted(state.packs.flatMap((pack) => pack.difficulty || []));
    targetSelect.innerHTML = `<option value="">All scoreable targets</option>` +
      targets.map((target) => `<option value="${escapeHtml(target)}">${escapeHtml(target)}</option>`).join("");
    difficultySelect.innerHTML = `<option value="">All difficulties</option>` +
      difficulties.map((difficulty) => `<option value="${escapeHtml(difficulty)}">${escapeHtml(difficulty)}</option>`).join("");
    if (targets.includes(selectedTarget)) targetSelect.value = selectedTarget;
    if (difficulties.includes(selectedDifficulty)) difficultySelect.value = selectedDifficulty;
  }

  function packMatchesFilters(pack) {
    const target = state.packTargetFilter;
    const difficulty = state.packDifficultyFilter;
    const scoreable = targetNames(pack.scoreable_targets);
    if (target && !scoreable.includes(target)) return false;
    if (difficulty && !(pack.difficulty || []).includes(difficulty)) return false;
    return true;
  }

  function packFacts(pack) {
    const duration = pack.duration || {};
    const estimated = pack.estimated_package || {};
    return [
      `${escapeHtml(pack.scenario_count || 0)} cases`,
      `${escapeHtml(formatSeconds(duration.total_seconds))} total`,
      `${escapeHtml((pack.sampling_rates_hz || []).join("/") || "n/a")} Hz`,
      `${escapeHtml(channelRange(pack))} channel(s)`,
      `${escapeHtml(formatBytes(estimated.bytes))}`
    ].join(" · ");
  }

  function renderPacks() {
    const visible = state.packs.filter(packMatchesFilters);
    $("packs").innerHTML = visible.map((pack) => `
      <article class="card">
        <h3>${escapeHtml(pack.display_name || pack.pack_id)}</h3>
        <p class="muted">Version ${escapeHtml(pack.version || "")} · released ${escapeHtml(formatDate(pack.released_at))}</p>
        <p>
          <span class="badge ${escapeHtml(pack.release_status || "")}">${escapeHtml(pack.release_status || "unknown")}</span>
          <span class="tag mode">${escapeHtml(pack.scoring_mode || "unknown scoring")}</span>
        </p>
        <p class="muted">${escapeHtml(pack.description || "")}</p>
        <p class="muted">${packFacts(pack)}</p>
        <p><strong>Scoreable locally</strong>${targetTags(pack.scoreable_targets, "scoreable")}</p>
        <p><strong>Reference-only</strong>${targetTags(pack.reference_only_targets, "reference")}</p>
        <p class="muted">Verifier profile: ${escapeHtml(pack.recommended_profile || "none")} · schemas: ${escapeHtml((pack.detector_output_schemas || []).join(", ") || "n/a")}</p>
        <p class="tag-list">${(pack.difficulty || []).map((item) => `<span class="tag">${escapeHtml(item)}</span>`).join("")}</p>
        <details>
          <summary>Recommended use</summary>
          <p><strong>Use for:</strong> ${escapeHtml((pack.recommended_for || []).join("; ") || "n/a")}</p>
          <p><strong>Avoid for:</strong> ${escapeHtml((pack.not_recommended_for || []).join("; ") || "n/a")}</p>
        </details>
        <details>
          <summary>Scenarios</summary>
          <ul>
            ${(pack.scenarios || []).map((scenario) => `
              <li>${escapeHtml(scenario.scenario_id)} <span class="muted">(${escapeHtml(formatSeconds(scenario.duration_seconds))}, ${escapeHtml(scenario.sampling_rate_hz || "n/a")} Hz, score: ${escapeHtml((scenario.scoreable_targets || []).join(", ") || "none")}, ref: ${escapeHtml((scenario.reference_only_targets || []).join(", ") || "none")})</span></li>
            `).join("")}
          </ul>
        </details>
        <details>
          <summary>Compatibility and changelog</summary>
          <p class="muted">Generator: ${escapeHtml(pack.generator_contract || "n/a")} · compatible: ${escapeHtml((pack.compatible_generator_versions || []).join(", "))}</p>
          <p class="muted">Local verifier min: ${escapeHtml((pack.generator_compatibility || {}).local_verifier_min_version || "n/a")}</p>
          ${pack.deprecation_message ? `<p class="error">${escapeHtml(pack.deprecation_message)}</p>` : ""}
          <ul>
            ${(pack.changelog || []).map((entry) => `
              <li><strong>${escapeHtml(entry.version)}</strong> · ${escapeHtml(entry.date)} — ${escapeHtml(entry.summary)}</li>
            `).join("")}
          </ul>
        </details>
        <span class="fingerprint">${escapeHtml(pack.pack_fingerprint || "")}</span>
      </article>
    `).join("") || "<p class=\"muted\">No packs match the current filters.</p>";
  }

  function selectedPack() {
    const id = $("pack-select").value;
    return [...state.packs, ...state.customPacks].find((pack) => pack.pack_id === id);
  }

  function renderSelectedPackSummary() {
    const pack = selectedPack();
    const node = $("selected-pack-summary");
    if (!pack) {
      node.innerHTML = "Select a pack to see scoring support.";
      return;
    }
    if (pack.source === "custom") {
      node.innerHTML = `<strong>${escapeHtml(pack.display_name || pack.pack_id)}</strong><br>Custom pack · targets: ${escapeHtml((pack.targets || []).join(", ") || "n/a")}`;
      return;
    }
    node.innerHTML = `
      <strong>${escapeHtml(pack.display_name || pack.pack_id)}</strong><br>
      ${packFacts(pack)}<br>
      Scoreable: ${targetNames(pack.scoreable_targets).map(escapeHtml).join(", ") || "none"}<br>
      Reference-only: ${targetNames(pack.reference_only_targets).map(escapeHtml).join(", ") || "none"}<br>
      Recommended verifier profile: ${escapeHtml(pack.recommended_profile || "none")}
    `;
  }

  async function loadPacks() {
    $("packs").textContent = "Loading packs…";
    try {
      const body = await api("/v1/packs");
      state.packs = body.packs || [];
      renderPackOptions();
      renderPackFilters();
      renderPacks();
    } catch (error) {
      $("packs").innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
    }
  }

  async function loadProjects() {
    const select = $("project-select");
    select.innerHTML = "";
    if (!state.authenticated) {
      state.role = "";
      state.projects = [];
      $("metrics-panel").hidden = true;
      $("create-project").disabled = true;
      $("create-job").disabled = true;
      $("save-scenario").disabled = true;
      $("create-custom-pack").disabled = true;
      return;
    }
    try {
      const body = await api("/v1/projects");
      state.projects = body.projects || [];
      state.role = body.role || "";
      $("create-project").disabled = !["owner", "admin"].includes(body.role);
      $("create-job").disabled = !canWrite();
      $("save-scenario").disabled = !canWrite();
      $("create-custom-pack").disabled = !canWrite();
      $("metrics-panel").hidden = !["owner", "admin"].includes(body.role);
      state.projects.forEach((project) => {
        const option = document.createElement("option");
        option.value = project.project_id;
        option.textContent = project.display_name;
        select.appendChild(option);
      });
      if (!($("metrics-panel").hidden)) loadMetrics();
    } catch (error) {
      state.role = "";
      $("create-output").textContent = error.message;
      $("create-project").disabled = true;
      $("create-job").disabled = true;
      $("save-scenario").disabled = true;
      $("create-custom-pack").disabled = true;
      $("metrics-panel").hidden = true;
    }
  }

  async function loadUsage() {
    if (!state.authenticated) {
      $("usage").textContent = "Sign in to inspect usage.";
      return;
    }
    try {
      const usage = await api("/v1/usage");
      $("usage").innerHTML = `
        <p>Requests/minute: ${escapeHtml(usage.requests_last_minute)} / ${escapeHtml(usage.limits.requests_per_minute)}</p>
        <p>Active jobs: ${escapeHtml(usage.active_jobs)} / ${escapeHtml(usage.limits.concurrent_jobs)}</p>
        <p>Jobs this month: ${escapeHtml(usage.jobs_this_month)} / ${escapeHtml(usage.limits.jobs_per_month)}</p>
        <p>Packages this month: ${escapeHtml(usage.packages_this_month)} · ${escapeHtml(formatBytes(usage.package_bytes_this_month))}</p>
      `;
    } catch (error) {
      $("usage").textContent = error.message;
    }
  }

  async function loadMetrics() {
    if (!state.authenticated || !["owner", "admin"].includes(state.role)) {
      $("metrics-panel").hidden = true;
      return;
    }
    $("metrics-panel").hidden = false;
    try {
      const metrics = await api("/v1/metrics");
      $("metrics").innerHTML = `
        <p>Queue: ${escapeHtml(metrics.queued_jobs)} queued · ${escapeHtml(metrics.running_jobs)} running</p>
        <p>Failures this month: ${escapeHtml(metrics.failed_jobs_this_month)} · quota rejections: ${escapeHtml(metrics.quota_rejections_this_month)}</p>
        <p>Worker: ${escapeHtml(metrics.worker_last_status || "unknown")} · last seen ${escapeHtml(formatDate(metrics.worker_last_seen_at))}</p>
        <p>Stored packages this month: ${escapeHtml(metrics.packages_this_month)} · ${escapeHtml(formatBytes(metrics.package_bytes_this_month))}</p>
      `;
    } catch (error) {
      $("metrics").textContent = error.message;
    }
  }

  async function loadScenarios() {
    if (!state.authenticated) {
      state.scenarios = [];
      renderPackScenarioOptions();
      $("scenarios").innerHTML = "<p class=\"muted\">Sign in to list drafts.</p>";
      return;
    }
    try {
      const body = await api("/v1/scenarios");
      state.scenarios = body.scenarios || [];
      renderPackScenarioOptions();
      $("scenarios").innerHTML = state.scenarios.map((draft) => `
        <article class="job">
          <div class="job-header">
            <div><h3>${escapeHtml(draft.name)}</h3><span class="fingerprint">${escapeHtml(draft.scenario_id)}</span></div>
            <span class="badge ${escapeHtml(draft.status)}">${escapeHtml(draft.status)}</span>
          </div>
          <p class="muted">Updated ${escapeHtml(formatDate(draft.updated_at))}</p>
          <span class="fingerprint">${escapeHtml(draft.document_fingerprint || "No fingerprint until valid")}</span>
          ${(draft.validation_errors || []).length ? `
            <details class="meta">
              <summary class="error">${escapeHtml(draft.validation_errors.length)} validation issue(s)</summary>
              <ul>
                ${draft.validation_errors.map((item) => `<li class="error"><strong>${escapeHtml(item.code)}</strong> ${escapeHtml(item.path)}: ${escapeHtml(item.message)}</li>`).join("")}
              </ul>
            </details>
          ` : ""}
          <div class="actions">
            <button class="secondary" data-edit-scenario="${escapeHtml(draft.scenario_id)}">${canWrite() ? "Edit" : "View"}</button>
            ${canWrite() ? `<button class="danger" data-delete-scenario="${escapeHtml(draft.scenario_id)}">Delete</button>` : ""}
          </div>
        </article>
      `).join("") || "<p class=\"muted\">No scenario drafts yet.</p>";
    } catch (error) {
      $("scenarios").innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
    }
  }

  function renderPackScenarioOptions() {
    const valid = state.scenarios.filter((draft) => draft.status === "valid");
    $("pack-scenario-options").innerHTML = valid.map((draft) => `
      <label class="card">
        <input type="checkbox" data-pack-scenario="${escapeHtml(draft.scenario_id)}">
        <strong>${escapeHtml(draft.name)}</strong>
        <span class="muted">${escapeHtml(draft.scenario && draft.scenario.scenario_id ? draft.scenario.scenario_id : "scenario")}</span>
        <span class="fingerprint">${escapeHtml(draft.document_fingerprint || "")}</span>
      </label>
    `).join("") || "<p class=\"muted\">Create at least one valid scenario draft first.</p>";
  }

  async function loadCustomPacks() {
    if (!state.authenticated) {
      state.customPacks = [];
      renderPackOptions();
      $("custom-packs").innerHTML = "<p class=\"muted\">Sign in to list custom packs.</p>";
      return;
    }
    try {
      const body = await api("/v1/custom-packs");
      state.customPacks = body.packs || [];
      renderPackOptions();
      $("custom-packs").innerHTML = state.customPacks.map((pack) => `
        <article class="job">
          <div class="job-header">
            <div><h3>${escapeHtml(pack.display_name)}</h3><span class="fingerprint">${escapeHtml(pack.pack_id)}</span></div>
            <span class="badge">custom ${escapeHtml(pack.version)}</span>
          </div>
          <p class="muted">${escapeHtml(pack.description)}</p>
          <p class="muted">Created ${escapeHtml(formatDate(pack.created_at))} · ${escapeHtml((pack.scenario_ids || []).length)} scenarios</p>
          <p class="muted">Targets: ${escapeHtml((pack.targets || []).join(", "))}</p>
          <span class="fingerprint">${escapeHtml(pack.pack_fingerprint)}</span>
          ${canWrite() ? `<div class="actions"><button class="danger" data-delete-custom-pack="${escapeHtml(pack.pack_id)}">Delete</button></div>` : ""}
        </article>
      `).join("") || "<p class=\"muted\">No custom packs yet.</p>";
    } catch (error) {
      $("custom-packs").innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
    }
  }

  async function createCustomPack() {
    const scenarioIds = [...document.querySelectorAll("[data-pack-scenario]:checked")]
      .map((input) => input.getAttribute("data-pack-scenario"));
    const targets = $("custom-pack-targets").value.split(",")
      .map((value) => value.trim()).filter(Boolean);
    const name = $("custom-pack-name").value.trim();
    const description = $("custom-pack-description").value.trim();
    const uniqueTargets = [...new Set(targets)];
    if (!name || !description) {
      $("custom-pack-output").textContent = "Enter both a pack name and a description.";
      return;
    }
    if (!uniqueTargets.length) {
      $("custom-pack-output").textContent = "Enter at least one scoring target.";
      return;
    }
    if (uniqueTargets.length !== targets.length) {
      $("custom-pack-output").textContent = "Remove duplicate scoring targets.";
      return;
    }
    if (!scenarioIds.length) {
      $("custom-pack-output").textContent = "Select at least one valid scenario draft.";
      return;
    }
    try {
      const pack = await api("/v1/custom-packs", {
        method: "POST",
        json: {
          name,
          description,
          targets: uniqueTargets,
          scenario_ids: scenarioIds
        }
      });
      $("custom-pack-output").textContent = `Created ${pack.pack_id}`;
      $("custom-pack-name").value = "";
      $("custom-pack-description").value = "";
      document.querySelectorAll("[data-pack-scenario]:checked")
        .forEach((input) => { input.checked = false; });
      await loadCustomPacks();
      $("pack-select").value = pack.pack_id;
    } catch (error) {
      $("custom-pack-output").textContent = error.message;
    }
  }

  async function deleteCustomPack(id) {
    if (!confirm("Delete this custom pack from the composer? Existing jobs remain reproducible.")) return;
    try {
      await api(`/v1/custom-packs/${encodeURIComponent(id)}`, { method: "DELETE" });
      await loadCustomPacks();
    } catch (error) {
      alert(error.message);
    }
  }

  function newScenario() {
    state.selectedScenarioId = "";
    $("scenario-name").value = "";
    $("scenario-json").value = "{}";
    $("scenario-output").textContent = "";
  }

  function loadScenarioTemplate() {
    state.selectedScenarioId = "";
    $("scenario-name").value = "Clean ECG";
    $("scenario-json").value = JSON.stringify(cleanEcgTemplate, null, 2);
    $("scenario-output").textContent = "Example loaded. Review it, then validate and save.";
  }

  function formatScenarioJson() {
    try {
      const scenario = JSON.parse($("scenario-json").value);
      $("scenario-json").value = JSON.stringify(scenario, null, 2);
      $("scenario-output").textContent = "JSON formatted.";
    } catch (error) {
      $("scenario-output").textContent = `Invalid JSON: ${error.message}`;
    }
  }

  function editScenario(id) {
    const draft = state.scenarios.find((item) => item.scenario_id === id);
    if (!draft) return;
    state.selectedScenarioId = id;
    $("scenario-name").value = draft.name;
    $("scenario-json").value = JSON.stringify(draft.scenario, null, 2);
    $("scenario-output").textContent = draft.status;
  }

  async function saveScenario() {
    let scenario;
    try {
      scenario = JSON.parse($("scenario-json").value);
    } catch (error) {
      $("scenario-output").textContent = `Invalid JSON: ${error.message}`;
      return;
    }
    const name = $("scenario-name").value.trim();
    if (!name) {
      $("scenario-output").textContent = "Enter a draft name.";
      return;
    }
    const path = state.selectedScenarioId
      ? `/v1/scenarios/${encodeURIComponent(state.selectedScenarioId)}`
      : "/v1/scenarios";
    try {
      const saved = await api(path, {
        method: state.selectedScenarioId ? "PUT" : "POST",
        json: { name, scenario }
      });
      state.selectedScenarioId = saved.scenario_id;
      $("scenario-output").textContent = `Saved: ${saved.status}`;
    } catch (error) {
      $("scenario-output").textContent = error.message;
      if (error.body && error.body.draft) {
        state.selectedScenarioId = error.body.draft.scenario_id;
      }
    }
    await loadScenarios();
    await loadCustomPacks();
  }

  async function deleteScenario(id) {
    if (!confirm("Delete this scenario draft?")) return;
    try {
      await api(`/v1/scenarios/${encodeURIComponent(id)}`, { method: "DELETE" });
      if (state.selectedScenarioId === id) newScenario();
      await loadScenarios();
    } catch (error) {
      alert(error.message);
    }
  }

  async function createProject() {
    const displayName = $("project-name").value.trim();
    if (!displayName) return;
    $("create-project").disabled = true;
    try {
      const created = await api("/v1/projects", {
        method: "POST",
        json: { display_name: displayName }
      });
      $("project-name").value = "";
      await loadProjects();
      $("project-select").value = created.project_id;
    } catch (error) {
      $("create-output").textContent = error.message;
    } finally {
      $("create-project").disabled = !["owner", "admin"].includes(state.role);
    }
  }

  async function createJob() {
    if (!state.authenticated) {
      $("create-output").textContent = "Sign in first.";
      return;
    }
    const packId = $("pack-select").value;
    const projectId = $("project-select").value;
    if (!projectId || !packId) {
      $("create-output").textContent = "Choose both a project and a pack.";
      return;
    }
    $("create-job").disabled = true;
    $("create-output").textContent = "Creating job…";
    try {
      const body = await api("/v1/jobs", { method: "POST", json: { project_id: projectId, pack_id: packId } });
      $("create-output").textContent = JSON.stringify(body, null, 2);
      await loadJobs({ force: true });
      await loadUsage();
      await loadMetrics();
    } catch (error) {
      $("create-output").textContent = error.message;
    } finally {
      $("create-job").disabled = !canWrite();
    }
  }

  function jobsFingerprint(jobs) {
    return JSON.stringify(jobs.map((job) => ({
      job_id: job.job_id,
      project_id: job.project_id,
      status: job.status,
      package_id: job.package_id || "",
      package_fingerprint: job.package_fingerprint || "",
      artifact_status: job.artifact_status || "",
      started_at: job.started_at || "",
      completed_at: job.completed_at || "",
      generator_version: job.generator_version || "",
      generator_build_identity: job.generator_build_identity || "",
      error: job.error || null
    })));
  }

  function packById(packId) {
    return [...state.packs, ...state.customPacks].find((pack) => pack.pack_id === packId);
  }

  function shellQuote(value) {
    return `"${String(value).replace(/(["\\$`])/g, "\\$1")}"`;
  }

  function firstScoreableTarget(pack) {
    return (pack && pack.scoreable_targets && pack.scoreable_targets[0]) || null;
  }

  function detectionShape(pack) {
    const target = firstScoreableTarget(pack);
    if (!target) return "detections/";
    const caseId = (target.case_ids || [])[0] || "case_id";
    const targetName = target.target || "target";
    return [
      "detections/",
      `  ${caseId}_${targetName}.csv     # accepted fallback name`,
      `  ${caseId}_${targetName}.json    # JSON is also accepted`,
      `  ${caseId}.csv                   # accepted when the case has one scoreable target`
    ].join("\n");
  }

  function verifierRecipe(job) {
    if (job.status !== "succeeded" || !job.package_id) return "";
    const pack = packById(job.pack_id);
    const packageFile = `${job.package_id}-package.zip`;
    const outputDir = `verification-${job.package_id}`;
    if (!pack || pack.source === "custom") {
      return `
        <details class="verify-note">
          <summary>Local verification recipe</summary>
          <p>Download the package ZIP, run your algorithm locally, then use the verifier contract from the generated manifest. Custom pack scoring metadata is not expanded in this UI yet.</p>
        </details>
      `;
    }
    const scoreable = targetNames(pack.scoreable_targets);
    const referenceOnly = targetNames(pack.reference_only_targets);
    if (!scoreable.length) {
      return `
        <details class="verify-note">
          <summary>Reference-only package</summary>
          <p>This pack has no local scoring policy. Use the downloaded package for reference artifact inspection and contract/manual QA.</p>
          <p><strong>Reference targets:</strong> ${escapeHtml(referenceOnly.join(", ") || "n/a")}</p>
        </details>
      `;
    }
    const profile = pack.recommended_profile || "regression";
    const command = `synsigra-verify ${shellQuote(packageFile)} detections/ ${shellQuote(outputDir)} --profile ${profile} --force`;
    const filtered = `synsigra-verify ${shellQuote(packageFile)} detections/ ${shellQuote(outputDir)} --profile ${profile} --target ${scoreable[0]} --force`;
    return `
      <details class="verify-note" open>
        <summary>First-run local verification recipe</summary>
        <p><strong>Scoreable targets:</strong> ${escapeHtml(scoreable.join(", "))}</p>
        <p><strong>Reference-only targets:</strong> ${escapeHtml(referenceOnly.join(", ") || "none")}</p>
        <p>Download the detection-template ZIP, replace the example rows with your algorithm output, and keep the <code>detections/</code> filenames. Then install the verifier from the beta checkout or wheel.</p>
        <pre class="output">python -m pip install ../signal_synth</pre>
        <p>Accepted detection file shape:</p>
        <pre class="output">${escapeHtml(detectionShape(pack))}</pre>
        <p>Run all scoreable targets with the recommended profile:</p>
        <pre class="output">${escapeHtml(command)}</pre>
        <button class="secondary" data-copy-text="${escapeHtml(command)}">Copy verify command</button>
        <p class="muted compact">Optional single-target smoke run:</p>
        <pre class="output">${escapeHtml(filtered)}</pre>
        <p>Machine-readable summaries are written to <code>${escapeHtml(outputDir)}/verification_summary.json</code> and <code>${escapeHtml(outputDir)}/verification_summary.csv</code>; the HTML report is <code>${escapeHtml(outputDir)}/verification_report.html</code>.</p>
        <p class="muted compact">CI semantics: exit 0 = pass, exit 1 = verification/input/scoring/threshold failure, exit 2 = invalid CLI usage.</p>
      </details>
    `;
  }

  async function loadJobs(options = {}) {
    const container = $("jobs");
    if (!state.authenticated) {
      state.jobs = [];
      state.jobsFingerprint = "";
      state.jobsLoaded = false;
      state.jobsNextOffset = null;
      $("load-more-jobs").hidden = true;
      setText("jobs-sync-status", "", "muted");
      container.innerHTML = "<p class=\"muted\">Sign in to list jobs.</p>";
      return;
    }
    if (state.jobPollInFlight) return;
    state.jobPollInFlight = true;
    if (!options.silent && !state.jobsLoaded) {
      container.textContent = "Loading jobs…";
    }
    try {
      const pageSize = 25;
      const offset = options.more ? state.jobsNextOffset : 0;
      const limit = options.more
        ? pageSize
        : Math.min(100, Math.max(pageSize, state.jobs.length));
      const body = await api(`/v1/jobs?limit=${limit}&offset=${offset || 0}`);
      const page = body.jobs || [];
      const jobs = options.more
        ? [...state.jobs, ...page.filter((job) => !state.jobs.some((known) => known.job_id === job.job_id))]
        : page;
      state.jobsNextOffset = body.next_offset === undefined
        ? null
        : (options.more ? body.next_offset : body.next_offset);
      $("load-more-jobs").hidden = state.jobsNextOffset === null;
      const fingerprint = jobsFingerprint(jobs);
      if (options.force || fingerprint !== state.jobsFingerprint) {
        state.jobs = jobs;
        state.jobsFingerprint = fingerprint;
        state.jobsLoaded = true;
        renderJobs();
        setText(
          "jobs-sync-status",
          `Updated ${new Intl.DateTimeFormat(undefined, { timeStyle: "medium" }).format(new Date())}`,
          "muted"
        );
      }
    } catch (error) {
      if (!options.silent) {
        container.innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
      }
      setText("jobs-sync-status", `Sync failed: ${error.message}`, "error");
    } finally {
      state.jobPollInFlight = false;
    }
  }

  function renderJobs() {
    const container = $("jobs");
    if (!state.jobs.length) {
      container.innerHTML = "<p class=\"muted\">No jobs yet.</p>";
      return;
    }
    container.innerHTML = state.jobs.map((job) => {
      const artifactActions = job.status === "succeeded" && job.package_id ? `
        <button class="secondary" data-download="${escapeHtml(job.package_id)}" data-file="manifest.json">Manifest</button>
        <button class="secondary" data-download="${escapeHtml(job.package_id)}" data-file="package.zip">Package ZIP</button>
        ${job.detection_templates_url ? `<button class="secondary" data-download-templates="${escapeHtml(job.job_id)}">Detection templates ZIP</button>` : ""}
      ` : "";
      const artifactExpired = job.artifact_status === "expired"
        ? `<p class="muted">Artifacts expired; reproducibility metadata remains.</p>`
        : "";
      const deleteAction = !canWrite() || job.status === "running" ? "" : `
        <button class="danger" data-delete-job="${escapeHtml(job.job_id)}">Delete</button>
      `;
      const cancelAction = canWrite() && job.status === "queued" ? `
        <button class="secondary" data-job-action="cancel" data-job-id="${escapeHtml(job.job_id)}">Cancel</button>
      ` : "";
      const retryAction = canWrite() && ["failed", "cancelled"].includes(job.status) ? `
        <button class="secondary" data-job-action="retry" data-job-id="${escapeHtml(job.job_id)}">Retry</button>
      ` : "";
      const error = job.error ? `<p class="error">${escapeHtml(job.error.code)}: ${escapeHtml(job.error.message)}</p>` : "";
      const verification = verifierRecipe(job);
      return `
        <article class="job">
          <div class="job-header">
            <div>
              <h3>${escapeHtml(job.pack_id || "(unknown pack)")}</h3>
              <span class="fingerprint">${escapeHtml(job.job_id)}</span>
            </div>
            <span class="badge ${escapeHtml(job.status)}">${escapeHtml(job.status)}</span>
          </div>
          <p class="muted">Created ${escapeHtml(formatDate(job.created_at))}</p>
          ${job.package_id ? `<p class="muted">Package: <span class="fingerprint">${escapeHtml(job.package_id)}</span></p>` : ""}
          <details class="meta">
            <summary>Reproducibility details</summary>
            <dl class="meta-grid">
              <dt>Project</dt><dd>${escapeHtml(job.project_id || "—")}</dd>
              <dt>Started</dt><dd>${escapeHtml(formatDate(job.started_at))}</dd>
              <dt>Completed</dt><dd>${escapeHtml(formatDate(job.completed_at))}</dd>
              <dt>Generator</dt><dd>${escapeHtml(job.generator_version || "—")}</dd>
              <dt>Build identity</dt><dd class="fingerprint">${escapeHtml(job.generator_build_identity || "—")}</dd>
              <dt>Package fingerprint</dt><dd class="fingerprint">${escapeHtml(job.package_fingerprint || "—")}</dd>
            </dl>
          </details>
          ${artifactExpired}
          ${error}
          ${verification}
          <div class="actions">
            ${artifactActions}
            ${cancelAction}
            ${retryAction}
            ${deleteAction}
          </div>
        </article>
      `;
    }).join("");
  }

  async function deleteJob(jobId) {
    if (!confirm("Delete this job from your job list? Download links for its package will stop working.")) {
      return;
    }
    try {
      await api(`/v1/jobs/${encodeURIComponent(jobId)}`, { method: "DELETE" });
      state.jobs = state.jobs.filter((job) => job.job_id !== jobId);
      state.jobsFingerprint = jobsFingerprint(state.jobs);
      state.jobsLoaded = true;
      renderJobs();
    } catch (error) {
      alert(error.message);
    }
  }

  async function runJobAction(jobId, action) {
    try {
      await api(`/v1/jobs/${encodeURIComponent(jobId)}/${action}`, {
        method: "POST"
      });
      await loadJobs({ force: true });
      await loadUsage();
      await loadMetrics();
    } catch (error) {
      alert(error.message);
    }
  }

  async function downloadArtifact(packageId, file) {
    try {
      const response = await fetch(`${base}/v1/artifacts/${encodeURIComponent(packageId)}/${file}`, {
        headers: headers(false)
      });
      if (!response.ok) {
        const text = await response.text();
        throw new Error(text || response.statusText);
      }
      const blob = await response.blob();
      const url = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = url;
      link.download = `${packageId}-${file}`;
      document.body.appendChild(link);
      link.click();
      link.remove();
      URL.revokeObjectURL(url);
    } catch (error) {
      alert(error.message);
    }
  }

  async function downloadDetectionTemplates(jobId) {
    try {
      const response = await fetch(`${base}/v1/jobs/${encodeURIComponent(jobId)}/detection-templates.zip`, {
        credentials: "same-origin",
        headers: headers(false)
      });
      if (!response.ok) {
        const text = await response.text();
        throw new Error(text || response.statusText);
      }
      const blob = await response.blob();
      const url = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = url;
      link.download = `${jobId}-detection-templates.zip`;
      document.body.appendChild(link);
      link.click();
      link.remove();
      URL.revokeObjectURL(url);
    } catch (error) {
      alert(error.message);
    }
  }

  async function copyText(text) {
    try {
      await navigator.clipboard.writeText(text);
      setText("jobs-sync-status", "Copied command", "muted ok");
    } catch (_) {
      window.prompt("Copy command", text);
    }
  }

  async function loadSession() {
    try {
      state.account = await api("/v1/auth/me");
      state.authenticated = true;
      state.role = state.account.role || "";
    } catch (error) {
      state.account = null;
      state.authenticated = false;
      state.role = "";
      if (error.status !== 401) {
        setText("auth-output", error.message, "error");
      }
    }
    renderAuthState();
  }

  async function refreshAuthenticatedData() {
    await loadProjects();
    await Promise.all([
      loadUsage(),
      loadScenarios(),
      loadCustomPacks(),
      loadJobs({ force: true }),
      loadApiKeys()
    ]);
  }

  async function submitAuth(kind) {
    const registration = kind === "register";
    const email = $(registration ? "register-email" : "login-email").value.trim();
    const password = $(registration ? "register-password" : "login-password").value;
    const payload = { email, password };
    if (registration) payload.display_name = $("register-name").value.trim();
    setText("auth-output", registration ? "Creating account…" : "Signing in…", "muted");
    try {
      const account = await api(`/v1/auth/${kind}`, {
        method: "POST",
        json: payload
      });
      state.account = account;
      state.authenticated = true;
      state.role = account.role || "";
      $("login-password").value = "";
      $("register-password").value = "";
      setText("auth-output", "", "muted");
      renderAuthState();
      await refreshAuthenticatedData();
    } catch (error) {
      setText("auth-output", error.message, "error");
    }
  }

  async function logout() {
    try {
      await api("/v1/auth/logout", { method: "POST" });
    } catch (_) {}
    state.account = null;
    state.authenticated = false;
    state.role = "";
    state.apiKeys = [];
    renderAuthState();
    setText("auth-output", "Signed out.", "muted");
    await refreshAuthenticatedData();
  }

  function renderApiKeys() {
    $("api-keys").innerHTML = state.apiKeys.map((key) => `
      <article class="job">
        <div class="job-header">
          <div>
            <h3>${escapeHtml(key.label)}</h3>
            <span class="fingerprint">${escapeHtml(key.api_key_id)}</span>
          </div>
          <span class="badge ${key.active ? "succeeded" : "failed"}">${key.active ? "active" : "revoked"}</span>
        </div>
        <p class="muted">Created ${escapeHtml(formatDate(key.created_at))}${key.last_used_at ? ` · last used ${escapeHtml(formatDate(key.last_used_at))}` : ""}</p>
        ${key.active ? `<button class="danger" data-revoke-api-key="${escapeHtml(key.api_key_id)}">Revoke</button>` : ""}
      </article>
    `).join("") || "<p class=\"muted\">No personal API keys.</p>";
  }

  async function loadApiKeys() {
    if (!state.authenticated) {
      state.apiKeys = [];
      renderApiKeys();
      return;
    }
    try {
      const body = await api("/v1/api-keys");
      state.apiKeys = body.api_keys || [];
      renderApiKeys();
    } catch (error) {
      $("api-keys").innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
    }
  }

  async function createApiKey() {
    const label = $("api-key-label").value.trim();
    if (!label) {
      $("api-key-secret").textContent = "Enter a label.";
      return;
    }
    try {
      const created = await api("/v1/api-keys", {
        method: "POST",
        json: { label }
      });
      $("api-key-label").value = "";
      $("api-key-secret").textContent =
        `Copy this key now; it will not be shown again:\n${created.api_key}`;
      await loadApiKeys();
    } catch (error) {
      $("api-key-secret").textContent = error.message;
    }
  }

  async function revokeApiKey(keyId) {
    if (!confirm("Revoke this API key? Existing integrations will stop working.")) return;
    try {
      await api(`/v1/api-keys/${encodeURIComponent(keyId)}`, {
        method: "DELETE"
      });
      await loadApiKeys();
    } catch (error) {
      alert(error.message);
    }
  }

  function escapeHtml(value) {
    return String(value)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#39;");
  }

  $("login").addEventListener("click", () => submitAuth("login"));
  $("register").addEventListener("click", () => submitAuth("register"));
  $("logout").addEventListener("click", logout);
  $("create-api-key").addEventListener("click", createApiKey);
  $("api-keys").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const keyId = target.getAttribute("data-revoke-api-key");
    if (keyId) revokeApiKey(keyId);
  });

  $("refresh-packs").addEventListener("click", loadPacks);
  $("pack-target-filter").addEventListener("change", () => {
    state.packTargetFilter = $("pack-target-filter").value;
    renderPacks();
  });
  $("pack-difficulty-filter").addEventListener("change", () => {
    state.packDifficultyFilter = $("pack-difficulty-filter").value;
    renderPacks();
  });
  $("pack-select").addEventListener("change", renderSelectedPackSummary);
  $("refresh-jobs").addEventListener("click", () => loadJobs({ force: true }));
  $("refresh-usage").addEventListener("click", loadUsage);
  $("refresh-metrics").addEventListener("click", loadMetrics);
  $("load-more-jobs").addEventListener("click", () => loadJobs({ more: true }));
  $("new-scenario").addEventListener("click", newScenario);
  $("load-scenario-template").addEventListener("click", loadScenarioTemplate);
  $("format-scenario-json").addEventListener("click", formatScenarioJson);
  $("save-scenario").addEventListener("click", saveScenario);
  $("create-custom-pack").addEventListener("click", createCustomPack);
  $("refresh-custom-packs").addEventListener("click", loadCustomPacks);
  $("custom-packs").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const packId = target.getAttribute("data-delete-custom-pack");
    if (packId) deleteCustomPack(packId);
  });
  $("scenarios").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const editId = target.getAttribute("data-edit-scenario");
    const deleteId = target.getAttribute("data-delete-scenario");
    if (editId) editScenario(editId);
    if (deleteId) deleteScenario(deleteId);
  });
  $("create-job").addEventListener("click", createJob);
  $("create-project").addEventListener("click", createProject);
  $("jobs").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const deleteJobId = target.getAttribute("data-delete-job");
    if (deleteJobId) deleteJob(deleteJobId);
    const jobAction = target.getAttribute("data-job-action");
    const actionJobId = target.getAttribute("data-job-id");
    if (jobAction && actionJobId) runJobAction(actionJobId, jobAction);
    const packageId = target.getAttribute("data-download");
    const file = target.getAttribute("data-file");
    if (packageId && file) downloadArtifact(packageId, file);
    const templateJobId = target.getAttribute("data-download-templates");
    if (templateJobId) downloadDetectionTemplates(templateJobId);
    const copyValue = target.getAttribute("data-copy-text");
    if (copyValue) copyText(copyValue);
  });

  async function initialize() {
    renderAuthState();
    checkHealth();
    loadPacks();
    await loadSession();
    await refreshAuthenticatedData();
  }

  initialize();
  setInterval(() => loadJobs({ silent: true }), 5000);
})();
)JS";

}  // namespace

namespace syn_sig_ra {

RouteResponse route_request(const std::string& method, const std::string& uri) {
    return route_request(method, uri, "/syn_sig_ra", "", nullptr, "");
}

RouteResponse route_request(
    const std::string& method,
    const std::string& uri,
    const std::string& public_base_path
) {
    return route_request(
        method,
        uri,
        public_base_path,
        "",
        nullptr,
        ""
    );
}

bool route_requires_authentication(
    const std::string& uri,
    const std::string& public_base_path
) {
    return path_at_or_below(uri, public_base_path + "/v1/jobs") ||
           path_at_or_below(uri, public_base_path + "/v1/artifacts") ||
           path_at_or_below(uri, public_base_path + "/v1/projects") ||
           path_at_or_below(uri, public_base_path + "/v1/usage") ||
           path_at_or_below(uri, public_base_path + "/v1/metrics") ||
           path_at_or_below(uri, public_base_path + "/v1/api-keys") ||
           path_at_or_below(uri, public_base_path + "/v1/scenarios") ||
           path_at_or_below(uri, public_base_path + "/v1/custom-packs");
}

RouteResponse route_request(
    const std::string& method,
    const std::string& uri,
    const std::string& public_base_path,
    const std::string& authorization_header,
    MetadataStore* metadata_store,
    const std::string& pack_root,
    const std::string& content_type,
    const std::string& request_body,
    const std::string& data_root,
    const std::string& query_string,
    const std::string& signal_synth_cli,
    const std::string& cookie_header
) {
    if (!owns_uri(uri, public_base_path)) {
        RouteResponse response;
        response.disposition = RouteDisposition::declined;
        response.status = 0;
        return response;
    }

    const std::string ready_path = public_base_path + "/readyz";
    if (uri == ready_path) {
        if (method != "GET") {
            return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Readiness only accepts GET.\"}}\n");
        }
        const bool database_ok = metadata_store != nullptr;
        const bool generator_ok =
            !signal_synth_cli.empty() && access(signal_synth_cli.c_str(), X_OK) == 0;
        std::vector<PackSummary> packs;
        std::string pack_error;
        const bool packs_ok = PackCatalog(pack_root).list(packs, pack_error) &&
            !packs.empty();
        struct statvfs disk;
        const bool storage_ok = !data_root.empty() &&
            statvfs(data_root.c_str(), &disk) == 0 &&
            access(data_root.c_str(), W_OK) == 0;
        const bool ready = database_ok && generator_ok && packs_ok && storage_ok;
        json_t* body = json_object();
        json_object_set_new(body, "status", json_string(ready ? "ready" : "not_ready"));
        json_object_set_new(body, "database", json_boolean(database_ok));
        json_object_set_new(body, "generator", json_boolean(generator_ok));
        json_object_set_new(body, "pack_catalog", json_boolean(packs_ok));
        json_object_set_new(body, "artifact_store", json_boolean(storage_ok));
        if (storage_ok) {
            json_object_set_new(body, "disk_free_bytes", json_integer(
                static_cast<json_int_t>(disk.f_bavail) * disk.f_frsize));
        }
        const std::string encoded = json_dump_line(body);
        json_decref(body);
        return json_response(ready ? 200 : 503, encoded);
    }

    const std::string auth_path = public_base_path + "/v1/auth";
    if (path_at_or_below(uri, auth_path)) {
        if (metadata_store == nullptr) {
            return json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Account storage is unavailable.\"}}\n"
            );
        }
        if (uri == auth_path + "/me") {
            if (method != "GET") {
                return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Account identity only accepts GET.\"}}\n");
            }
            AccountRecord account;
            const AuthenticationResult session = authenticate_session(
                cookie_header, *metadata_store, &account);
            if (session.status != AuthenticationStatus::authenticated) {
                return json_response(401, "{\"error\":{\"code\":\"unauthorized\","
                    "\"message\":\"Sign in to continue.\"}}\n");
            }
            RouteResponse response = json_response(200, account_json(account));
            response.cache_control = "no-store";
            return response;
        }
        if (uri == auth_path + "/logout") {
            if (method != "POST") {
                return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Logout only accepts POST.\"}}\n");
            }
            std::string token;
            std::string token_hash;
            std::string error;
            if (session_token_from_cookie(cookie_header, token) &&
                sha256_hex(token, token_hash, error) &&
                !metadata_store->delete_session(token_hash, error)) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Unable to end the session.\"}}\n");
                response.internal_error = error;
                return response;
            }
            RouteResponse response = json_response(200, "{\"status\":\"signed_out\"}\n");
            response.cache_control = "no-store";
            response.set_cookie =
                "syn_sig_ra_session=; Path=/syn_sig_ra; Max-Age=0; "
                "Secure; HttpOnly; SameSite=Lax";
            return response;
        }
        const bool registration = uri == auth_path + "/register";
        const bool login = uri == auth_path + "/login";
        if ((!registration && !login) || method != "POST") {
            return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Unknown account operation.\"}}\n");
        }
        if (!is_json_content_type(content_type)) {
            return json_response(415, "{\"error\":{\"code\":\"unsupported_media_type\","
                "\"message\":\"Content-Type must be application/json.\"}}\n");
        }
        json_error_t parse_error;
        json_t* root = json_loadb(
            request_body.data(), request_body.size(), JSON_REJECT_DUPLICATES,
            &parse_error);
        json_t* email_value = root == nullptr
            ? nullptr : json_object_get(root, "email");
        json_t* password_value = root == nullptr
            ? nullptr : json_object_get(root, "password");
        json_t* name_value = root == nullptr
            ? nullptr : json_object_get(root, "display_name");
        std::string email;
        const bool valid_shape = json_is_object(root) &&
            json_is_string(email_value) && json_is_string(password_value) &&
            (!registration || json_is_string(name_value)) &&
            json_object_size(root) == (registration ? 3u : 2u);
        if (!valid_shape ||
            !normalize_email(
                json_is_string(email_value) ? json_string_value(email_value) : "",
                email)) {
            if (root != nullptr) json_decref(root);
            return json_response(400, "{\"error\":{\"code\":\"invalid_account_request\","
                "\"message\":\"Enter a valid email and password.\"}}\n");
        }
        const std::string password = json_string_value(password_value);
        const std::string display_name = registration
            ? json_string_value(name_value) : "";
        json_decref(root);
        if (password.size() > 128) {
            return json_response(400, "{\"error\":{\"code\":\"invalid_password\","
                "\"message\":\"Password must contain at most 128 characters.\"}}\n");
        }
        AccountRecord account;
        std::string error;
        if (registration) {
            if (display_name.empty() || display_name.size() > 100) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_display_name\","
                    "\"message\":\"Display name must contain 1-100 characters.\"}}\n");
            }
            std::string salt;
            std::string password_hash;
            if (!hash_password(password, salt, password_hash, error)) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_password\","
                    "\"message\":\"Password must contain 12-128 characters.\"}}\n");
            }
            const AccountCreateStatus created = metadata_store->create_account(
                email, display_name, salt, password_hash, account, error);
            if (created == AccountCreateStatus::email_exists) {
                return json_response(409, "{\"error\":{\"code\":\"email_exists\","
                    "\"message\":\"An account already uses this email.\"}}\n");
            }
            if (created != AccountCreateStatus::created) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Unable to create the account.\"}}\n");
                response.internal_error = error;
                return response;
            }
        } else {
            bool matches = false;
            const RecordLookupStatus found =
                metadata_store->find_account_by_email(email, account, error);
            if (found == RecordLookupStatus::storage_error) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Unable to sign in.\"}}\n");
                response.internal_error = error;
                return response;
            }
            if (found == RecordLookupStatus::not_found) {
                std::string dummy_salt;
                std::string dummy_hash;
                const std::string dummy_password = password.size() >= 12
                    ? password : password + "000000000000";
                hash_password(
                    dummy_password, dummy_salt, dummy_hash, error);
            }
            if (found != RecordLookupStatus::found ||
                !verify_password(
                    password, account.password_salt, account.password_hash,
                    matches, error) ||
                !matches) {
                return json_response(401, "{\"error\":{\"code\":\"invalid_login\","
                    "\"message\":\"Email or password is incorrect.\"}}\n");
            }
        }
        RouteResponse response;
        if (!issue_browser_session(*metadata_store, account, response, error)) {
            response = json_response(
                503, "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Unable to start the session.\"}}\n");
            response.internal_error = error;
        }
        return response;
    }

    ApiKeyIdentity authenticated_identity;
    bool authenticated = false;
    if (route_requires_authentication(uri, public_base_path)) {
        if (metadata_store == nullptr) {
            return json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Authentication storage is unavailable.\"}}\n"
            );
        }
        const AuthenticationResult authentication = authorization_header.empty()
            ? authenticate_session(cookie_header, *metadata_store)
            : authenticate_bearer(authorization_header, *metadata_store);
        if (authentication.status == AuthenticationStatus::storage_error) {
            RouteResponse response = json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Authentication storage is unavailable.\"}}\n"
            );
            response.internal_error = authentication.internal_error;
            return response;
        }
        if (authentication.status != AuthenticationStatus::authenticated) {
            RouteResponse response = json_response(
                401,
                "{\"error\":{\"code\":\"unauthorized\","
                "\"message\":\"Sign in or provide a valid Bearer API key.\"}}\n"
            );
            response.www_authenticate = "Bearer realm=\"syn_sig_ra\"";
            return response;
        }
        authenticated_identity = authentication.identity;
        authenticated = true;
        UsageSummary usage;
        std::string quota_error;
        const QuotaStatus quota = metadata_store->check_request_quota(
            authenticated_identity, usage, quota_error
        );
        if (quota == QuotaStatus::storage_error) {
            RouteResponse response = json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Quota storage is unavailable.\"}}\n"
            );
            response.internal_error = quota_error;
            return response;
        }
        if (quota == QuotaStatus::rate_limited) {
            return json_response(
                429,
                "{\"error\":{\"code\":\"request_rate_limit\","
                "\"message\":\"Per-key request rate limit exceeded.\"}}\n"
            );
        }
    }

    const std::string api_keys_path = public_base_path + "/v1/api-keys";
    if (path_at_or_below(uri, api_keys_path)) {
        const bool collection = uri == api_keys_path;
        if (collection && method == "GET") {
            std::vector<ApiKeyRecord> keys;
            std::string error;
            if (!metadata_store->list_personal_api_keys(
                    authenticated_identity, keys, error)) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"API key storage is unavailable.\"}}\n");
                response.internal_error = error;
                return response;
            }
            json_t* root = json_object();
            json_t* items = json_array();
            for (std::vector<ApiKeyRecord>::const_iterator it = keys.begin();
                 it != keys.end(); ++it) {
                json_t* item = json_object();
                json_object_set_new(
                    item, "api_key_id", json_string(it->api_key_id.c_str()));
                json_object_set_new(
                    item, "label", json_string(it->label.c_str()));
                json_object_set_new(
                    item, "active", json_boolean(it->active));
                json_object_set_new(
                    item, "created_at", json_string(it->created_at.c_str()));
                if (!it->last_used_at.empty()) {
                    json_object_set_new(
                        item, "last_used_at",
                        json_string(it->last_used_at.c_str()));
                }
                json_array_append_new(items, item);
            }
            json_object_set_new(root, "api_keys", items);
            const std::string body = json_dump_line(root);
            json_decref(root);
            return json_response(200, body);
        }
        if (collection && method == "POST") {
            if (!is_json_content_type(content_type)) {
                return json_response(415, "{\"error\":{\"code\":\"unsupported_media_type\","
                    "\"message\":\"Content-Type must be application/json.\"}}\n");
            }
            json_error_t parse_error;
            json_t* root = json_loadb(
                request_body.data(), request_body.size(),
                JSON_REJECT_DUPLICATES, &parse_error);
            json_t* label_value = root == nullptr
                ? nullptr : json_object_get(root, "label");
            const std::string label = json_is_string(label_value)
                ? json_string_value(label_value) : "";
            const bool valid_shape =
                json_is_object(root) && json_object_size(root) == 1u;
            if (root != nullptr) json_decref(root);
            if (!valid_shape || label.empty() || label.size() > 100) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_label\","
                    "\"message\":\"Label must contain 1-100 characters.\"}}\n");
            }
            std::string key_id;
            std::string token;
            std::string key_hash;
            std::string error;
            if (!random_id("key_", key_id, error) ||
                !random_id("ssk_", token, error) ||
                !sha256_hex(token, key_hash, error) ||
                !metadata_store->create_personal_api_key(
                    authenticated_identity, key_id, key_hash, label, error)) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Unable to create the API key.\"}}\n");
                response.internal_error = error;
                return response;
            }
            json_t* body = json_object();
            json_object_set_new(
                body, "api_key_id", json_string(key_id.c_str()));
            json_object_set_new(body, "api_key", json_string(token.c_str()));
            json_object_set_new(body, "label", json_string(label.c_str()));
            const std::string encoded = json_dump_line(body);
            json_decref(body);
            RouteResponse response = json_response(201, encoded);
            response.cache_control = "no-store";
            return response;
        }
        if (!collection && method == "DELETE") {
            const std::string key_id = uri.substr(api_keys_path.size() + 1);
            std::string error;
            const RecordLookupStatus status =
                metadata_store->revoke_personal_api_key(
                    authenticated_identity, key_id, error);
            if (status == RecordLookupStatus::storage_error) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Unable to revoke the API key.\"}}\n");
                response.internal_error = error;
                return response;
            }
            return status == RecordLookupStatus::found
                ? json_response(200, "{\"status\":\"revoked\"}\n")
                : json_response(404, "{\"error\":{\"code\":\"api_key_not_found\","
                    "\"message\":\"API key not found.\"}}\n");
        }
        return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
            "\"message\":\"Unsupported API key operation.\"}}\n");
    }

    const std::string usage_path = public_base_path + "/v1/usage";
    if (uri == usage_path) {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Usage only accepts GET.\"}}\n"
            );
        }
        UsageSummary usage;
        std::string error;
        if (!metadata_store->usage_summary(
                authenticated_identity, usage, error)) {
            RouteResponse response = json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Usage storage is unavailable.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        return json_response(200, usage_json(usage));
    }

    const std::string custom_packs_path =
        public_base_path + "/v1/custom-packs";
    if (path_at_or_below(uri, custom_packs_path)) {
        const bool collection = uri == custom_packs_path;
        if (collection && method == "GET") {
            std::vector<CustomPackRecord> packs;
            std::string error;
            if (!metadata_store->list_custom_packs(
                    authenticated_identity, packs, error)) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Custom pack storage is unavailable.\"}}\n");
                response.internal_error = error;
                return response;
            }
            json_t* root = json_object();
            json_t* items = json_array();
            for (std::vector<CustomPackRecord>::const_iterator it =
                     packs.begin(); it != packs.end(); ++it) {
                json_array_append_new(items, custom_pack_json_object(*it));
            }
            json_object_set_new(root, "packs", items);
            const std::string body = json_dump_line(root);
            json_decref(root);
            return json_response(200, body);
        }
        if (!collection && method == "GET") {
            const std::string pack_id =
                uri.substr(custom_packs_path.size() + 1);
            CustomPackRecord pack;
            std::string error;
            const RecordLookupStatus found = metadata_store->find_custom_pack(
                pack_id, authenticated_identity, pack, error);
            if (found == RecordLookupStatus::not_found) {
                return json_response(
                    404, "{\"error\":{\"code\":\"custom_pack_not_found\","
                    "\"message\":\"The custom pack does not exist.\"}}\n");
            }
            if (found == RecordLookupStatus::storage_error) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Custom pack storage is unavailable.\"}}\n");
                response.internal_error = error;
                return response;
            }
            json_t* value = custom_pack_json_object(pack);
            const std::string body = json_dump_line(value);
            json_decref(value);
            return json_response(200, body);
        }
        if (!collection && method == "DELETE") {
            if (authenticated_identity.role == "viewer") {
                return json_response(
                    403, "{\"error\":{\"code\":\"forbidden\","
                    "\"message\":\"Viewer role cannot delete custom packs.\"}}\n");
            }
            const std::string pack_id =
                uri.substr(custom_packs_path.size() + 1);
            std::string error;
            const RecordLookupStatus deleted = metadata_store->delete_custom_pack(
                pack_id, authenticated_identity, error);
            if (deleted == RecordLookupStatus::not_found) {
                return json_response(
                    404, "{\"error\":{\"code\":\"custom_pack_not_found\","
                    "\"message\":\"The custom pack does not exist.\"}}\n");
            }
            if (deleted == RecordLookupStatus::storage_error) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Custom pack storage is unavailable.\"}}\n");
                response.internal_error = error;
                return response;
            }
            return json_response(
                200, std::string("{\"pack_id\":\"") + pack_id +
                "\",\"status\":\"deleted\"}\n");
        }
        if (!collection || method != "POST") {
            return json_response(
                405, "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Custom pack route method is not allowed.\"}}\n");
        }
        if (authenticated_identity.role == "viewer") {
            return json_response(
                403, "{\"error\":{\"code\":\"forbidden\","
                "\"message\":\"Viewer role cannot create custom packs.\"}}\n");
        }
        if (!is_json_content_type(content_type)) {
            return json_response(
                415, "{\"error\":{\"code\":\"unsupported_media_type\","
                "\"message\":\"Content-Type must be application/json.\"}}\n");
        }
        json_error_t parse_error;
        json_t* submitted = json_loadb(
            request_body.data(), request_body.size(),
            JSON_REJECT_DUPLICATES, &parse_error);
        json_t* name = submitted == nullptr
            ? nullptr : json_object_get(submitted, "name");
        json_t* description = submitted == nullptr
            ? nullptr : json_object_get(submitted, "description");
        json_t* targets = submitted == nullptr
            ? nullptr : json_object_get(submitted, "targets");
        json_t* scenario_ids = submitted == nullptr
            ? nullptr : json_object_get(submitted, "scenario_ids");
        if (!json_is_object(submitted) || json_object_size(submitted) != 4 ||
            !json_is_string(name) || !json_is_string(description) ||
            !json_is_array(targets) || json_array_size(targets) == 0 ||
            !json_is_array(scenario_ids) || json_array_size(scenario_ids) == 0 ||
            std::string(json_string_value(name)).empty() ||
            std::string(json_string_value(name)).size() > 100) {
            if (submitted != nullptr) json_decref(submitted);
            return json_response(
                400, "{\"error\":{\"code\":\"invalid_custom_pack_request\","
                "\"message\":\"name, description, targets, and scenario_ids are required.\"}}\n");
        }
        std::vector<std::string> target_values;
        std::vector<ScenarioDraftRecord> drafts;
        std::set<std::string> unique_values;
        std::size_t index = 0;
        json_t* item = nullptr;
        bool request_valid = true;
        json_array_foreach(targets, index, item) {
            if (!json_is_string(item) ||
                !unique_values.insert(json_string_value(item)).second) {
                request_valid = false;
                break;
            }
            target_values.push_back(json_string_value(item));
        }
        unique_values.clear();
        std::string error;
        json_array_foreach(scenario_ids, index, item) {
            if (!json_is_string(item) ||
                !unique_values.insert(json_string_value(item)).second) {
                request_valid = false;
                break;
            }
            ScenarioDraftRecord draft;
            if (metadata_store->find_scenario_draft(
                    json_string_value(item), authenticated_identity,
                    draft, error) != RecordLookupStatus::found ||
                draft.status != "valid") {
                request_valid = false;
                break;
            }
            drafts.push_back(draft);
        }
        if (!request_valid) {
            json_decref(submitted);
            return json_response(
                400, "{\"error\":{\"code\":\"invalid_custom_pack_request\","
                "\"message\":\"Targets must be unique and every scenario must be a valid owned draft.\"}}\n");
        }
        std::string pack_id;
        if (!random_id("custom_pack_", pack_id, error)) {
            json_decref(submitted);
            RouteResponse response = json_response(
                503, "{\"error\":{\"code\":\"id_unavailable\","
                "\"message\":\"Custom pack ID could not be allocated.\"}}\n");
            response.internal_error = error;
            return response;
        }
        const std::string custom_root = data_root + "/custom_packs";
        const std::string pack_directory = custom_root + "/" + pack_id;
        const std::string scenario_directory = pack_directory + "/scenarios";
        if (!ensure_directory(custom_root, error) ||
            !ensure_directory(pack_directory, error) ||
            !ensure_directory(scenario_directory, error)) {
            json_decref(submitted);
            RouteResponse response = json_response(
                503, "{\"error\":{\"code\":\"artifact_storage_unavailable\","
                "\"message\":\"Custom pack snapshot could not be created.\"}}\n");
            response.internal_error = error;
            return response;
        }
        json_t* pack_json = json_object();
        json_object_set_new(pack_json, "schema_version", json_integer(1));
        json_object_set_new(pack_json, "pack_id", json_string(pack_id.c_str()));
        json_object_set_new(pack_json, "name", json_string(json_string_value(name)));
        json_object_set_new(pack_json, "version", json_string("1.0"));
        json_object_set_new(
            pack_json, "description",
            json_string(json_string_value(description)));
        json_incref(targets);
        json_object_set_new(pack_json, "targets", targets);
        json_t* pack_scenarios = json_array();
        std::set<std::string> manifest_ids;
        for (std::vector<ScenarioDraftRecord>::const_iterator it = drafts.begin();
             it != drafts.end(); ++it) {
            json_error_t document_error;
            json_t* document = json_loads(
                it->document_json.c_str(), JSON_REJECT_DUPLICATES,
                &document_error);
            json_t* document_id = document == nullptr
                ? nullptr : json_object_get(document, "scenario_id");
            if (!json_is_string(document_id) ||
                !manifest_ids.insert(json_string_value(document_id)).second) {
                if (document != nullptr) json_decref(document);
                json_decref(pack_json);
                json_decref(submitted);
                return json_response(
                    400, "{\"error\":{\"code\":\"duplicate_scenario_id\","
                    "\"message\":\"Selected drafts must have unique scenario IDs.\"}}\n");
            }
            const std::string filename = it->scenario_id + ".json";
            if (!write_private_file(
                    scenario_directory + "/" + filename,
                    it->document_json, error)) {
                json_decref(document);
                json_decref(pack_json);
                json_decref(submitted);
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"artifact_storage_unavailable\","
                    "\"message\":\"Scenario snapshot could not be written.\"}}\n");
                response.internal_error = error;
                return response;
            }
            json_t* entry = json_object();
            json_object_set_new(
                entry, "id", json_string(json_string_value(document_id)));
            json_object_set_new(
                entry, "path",
                json_string(("scenarios/" + filename).c_str()));
            json_incref(targets);
            json_object_set_new(entry, "targets", targets);
            json_array_append_new(pack_scenarios, entry);
            json_decref(document);
        }
        json_object_set_new(pack_json, "scenarios", pack_scenarios);
        char* encoded = json_dumps(pack_json, JSON_COMPACT | JSON_SORT_KEYS);
        json_decref(pack_json);
        if (encoded == nullptr) {
            json_decref(submitted);
            return json_response(
                500, "{\"error\":{\"code\":\"custom_pack_encoding_failed\","
                "\"message\":\"Custom pack could not be encoded.\"}}\n");
        }
        const std::string pack_document(encoded);
        free(encoded);
        signal_synth::ecg_pack_manifest manifest;
        signal_synth::ecg_pack_json_result validation;
        if (!signal_synth::parse_ecg_pack_json(
                pack_document, manifest, validation) ||
            !write_private_file(
                pack_directory + "/pack.json",
                validation.canonical_json, error)) {
            json_decref(submitted);
            return json_response(
                400, "{\"error\":{\"code\":\"custom_pack_invalid\","
                "\"message\":\"Custom pack failed authoritative validation.\"}}\n");
        }
        chmod(scenario_directory.c_str(), 0550);
        chmod(pack_directory.c_str(), 0550);
        char* targets_text = json_dumps(targets, JSON_COMPACT | JSON_SORT_KEYS);
        char* scenario_ids_text =
            json_dumps(scenario_ids, JSON_COMPACT | JSON_SORT_KEYS);
        CustomPackRecord input;
        input.pack_id = pack_id;
        input.name = json_string_value(name);
        input.version = "1.0";
        input.description = json_string_value(description);
        input.targets_json = targets_text == nullptr ? "[]" : targets_text;
        input.scenario_ids_json =
            scenario_ids_text == nullptr ? "[]" : scenario_ids_text;
        input.pack_fingerprint = validation.pack_fingerprint;
        input.source_pack_path = pack_directory + "/pack.json";
        free(targets_text);
        free(scenario_ids_text);
        json_decref(submitted);
        CustomPackRecord created;
        if (!metadata_store->create_custom_pack(
                authenticated_identity, input, created, error)) {
            RouteResponse response = json_response(
                503, "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Custom pack metadata could not be stored.\"}}\n");
            response.internal_error = error;
            return response;
        }
        json_t* value = custom_pack_json_object(created);
        const std::string body = json_dump_line(value);
        json_decref(value);
        return json_response(201, body);
    }

    const std::string scenarios_path = public_base_path + "/v1/scenarios";
    if (path_at_or_below(uri, scenarios_path)) {
        const bool collection = uri == scenarios_path;
        if (collection && method == "GET") {
            std::vector<ScenarioDraftRecord> drafts;
            std::string error;
            if (!metadata_store->list_scenario_drafts(
                    authenticated_identity, drafts, error)) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Scenario storage is unavailable.\"}}\n");
                response.internal_error = error;
                return response;
            }
            json_t* root = json_object();
            json_t* items = json_array();
            for (std::vector<ScenarioDraftRecord>::const_iterator it =
                     drafts.begin(); it != drafts.end(); ++it) {
                json_array_append_new(items, scenario_draft_json_object(*it));
            }
            json_object_set_new(root, "scenarios", items);
            const std::string body = json_dump_line(root);
            json_decref(root);
            return json_response(200, body);
        }
        if (!collection && method == "GET") {
            const std::string scenario_id =
                uri.substr(scenarios_path.size() + 1);
            if (!is_valid_pack_id(scenario_id) ||
                scenario_id.compare(0, 9, "scenario_") != 0) {
                return json_response(
                    400, "{\"error\":{\"code\":\"invalid_scenario_id\","
                    "\"message\":\"The scenario ID is invalid.\"}}\n");
            }
            ScenarioDraftRecord draft;
            std::string error;
            const RecordLookupStatus found =
                metadata_store->find_scenario_draft(
                    scenario_id, authenticated_identity, draft, error);
            if (found == RecordLookupStatus::not_found) {
                return json_response(
                    404, "{\"error\":{\"code\":\"scenario_not_found\","
                    "\"message\":\"The scenario draft does not exist.\"}}\n");
            }
            if (found == RecordLookupStatus::storage_error) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Scenario storage is unavailable.\"}}\n");
                response.internal_error = error;
                return response;
            }
            json_t* value = scenario_draft_json_object(draft);
            const std::string body = json_dump_line(value);
            json_decref(value);
            return json_response(200, body);
        }
        if (!collection && method == "DELETE") {
            if (authenticated_identity.role == "viewer") {
                return json_response(
                    403, "{\"error\":{\"code\":\"forbidden\","
                    "\"message\":\"Viewer role cannot delete scenarios.\"}}\n");
            }
            const std::string scenario_id =
                uri.substr(scenarios_path.size() + 1);
            std::string error;
            const RecordLookupStatus deleted =
                metadata_store->delete_scenario_draft(
                    scenario_id, authenticated_identity, error);
            if (deleted == RecordLookupStatus::not_found) {
                return json_response(
                    404, "{\"error\":{\"code\":\"scenario_not_found\","
                    "\"message\":\"The scenario draft does not exist.\"}}\n");
            }
            if (deleted == RecordLookupStatus::storage_error) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Scenario storage is unavailable.\"}}\n");
                response.internal_error = error;
                return response;
            }
            return json_response(
                200, std::string("{\"scenario_id\":\"") + scenario_id +
                "\",\"status\":\"deleted\"}\n");
        }
        const bool create = collection && method == "POST";
        const bool update = !collection && method == "PUT";
        if (!create && !update) {
            return json_response(
                405, "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Scenario route method is not allowed.\"}}\n");
        }
        if (authenticated_identity.role == "viewer") {
            return json_response(
                403, "{\"error\":{\"code\":\"forbidden\","
                "\"message\":\"Viewer role cannot edit scenarios.\"}}\n");
        }
        if (!is_json_content_type(content_type)) {
            return json_response(
                415, "{\"error\":{\"code\":\"unsupported_media_type\","
                "\"message\":\"Content-Type must be application/json.\"}}\n");
        }
        json_error_t parse_error;
        json_t* submitted = json_loadb(
            request_body.data(), request_body.size(),
            JSON_REJECT_DUPLICATES, &parse_error);
        json_t* name = submitted == nullptr
            ? nullptr : json_object_get(submitted, "name");
        json_t* scenario = submitted == nullptr
            ? nullptr : json_object_get(submitted, "scenario");
        if (!json_is_object(submitted) || json_object_size(submitted) != 2 ||
            !json_is_string(name) || !json_is_object(scenario) ||
            std::string(json_string_value(name)).empty() ||
            std::string(json_string_value(name)).size() > 100) {
            if (submitted != nullptr) json_decref(submitted);
            return json_response(
                400, "{\"error\":{\"code\":\"invalid_scenario_request\","
                "\"message\":\"name and scenario object are required.\"}}\n");
        }
        std::string status;
        std::string canonical_json;
        std::string fingerprint;
        std::string errors_json;
        validate_scenario_document(
            scenario, status, canonical_json, fingerprint, errors_json);
        const std::string draft_name = json_string_value(name);
        json_decref(submitted);
        ScenarioDraftRecord draft;
        std::string error;
        bool stored = false;
        RecordLookupStatus updated = RecordLookupStatus::storage_error;
        if (create) {
            stored = metadata_store->create_scenario_draft(
                authenticated_identity, draft_name, status, canonical_json,
                fingerprint, errors_json, draft, error);
        } else {
            const std::string scenario_id =
                uri.substr(scenarios_path.size() + 1);
            updated = metadata_store->update_scenario_draft(
                scenario_id, authenticated_identity, draft_name, status,
                canonical_json, fingerprint, errors_json, draft, error);
            stored = updated == RecordLookupStatus::found;
            if (updated == RecordLookupStatus::not_found) {
                return json_response(
                    404, "{\"error\":{\"code\":\"scenario_not_found\","
                    "\"message\":\"The scenario draft does not exist.\"}}\n");
            }
        }
        if (!stored) {
            RouteResponse response = json_response(
                503, "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Scenario storage is unavailable.\"}}\n");
            response.internal_error = error;
            return response;
        }
        json_t* response_root = scenario_draft_json_object(draft);
        if (status == "invalid") {
            json_t* wrapper = json_object();
            json_t* error_value = json_object();
            json_object_set_new(
                error_value, "code", json_string("scenario_invalid"));
            json_object_set_new(
                error_value, "message",
                json_string("The draft was saved but scenario validation failed."));
            json_object_set_new(wrapper, "error", error_value);
            json_object_set_new(wrapper, "draft", response_root);
            const std::string body = json_dump_line(wrapper);
            json_decref(wrapper);
            return json_response(422, body);
        }
        const std::string body = json_dump_line(response_root);
        json_decref(response_root);
        return json_response(create ? 201 : 200, body);
    }

    const std::string metrics_path = public_base_path + "/v1/metrics";
    if (uri == metrics_path) {
        if (method != "GET") {
            return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Metrics only accepts GET.\"}}\n");
        }
        if (authenticated_identity.role != "owner" &&
            authenticated_identity.role != "admin") {
            return json_response(403, "{\"error\":{\"code\":\"forbidden\","
                "\"message\":\"Owner or admin role is required.\"}}\n");
        }
        UsageSummary usage;
        std::string error;
        if (!metadata_store->usage_summary(authenticated_identity, usage, error)) {
            RouteResponse response = json_response(503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Metrics storage is unavailable.\"}}\n");
            response.internal_error = error;
            return response;
        }
        return json_response(200, usage_json(usage));
    }

    const std::string projects_path = public_base_path + "/v1/projects";
    if (uri == projects_path) {
        if (method == "GET") {
            std::vector<ProjectRecord> projects;
            std::string error;
            if (!metadata_store->list_projects(
                    authenticated_identity, projects, error)) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Project storage is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            json_t* root = json_object();
            json_t* array = json_array();
            for (std::vector<ProjectRecord>::const_iterator it = projects.begin();
                 it != projects.end(); ++it) {
                json_array_append_new(array, project_json_object(*it));
            }
            json_object_set_new(root, "projects", array);
            json_object_set_new(
                root, "role", json_string(authenticated_identity.role.c_str())
            );
            const std::string body = json_dump_line(root);
            json_decref(root);
            return json_response(200, body);
        }
        if (method != "POST") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Projects only accept GET or POST.\"}}\n"
            );
        }
        if (authenticated_identity.role != "owner" &&
            authenticated_identity.role != "admin") {
            return json_response(
                403,
                "{\"error\":{\"code\":\"forbidden\","
                "\"message\":\"Owner or admin role is required.\"}}\n"
            );
        }
        if (!is_json_content_type(content_type)) {
            return json_response(
                415,
                "{\"error\":{\"code\":\"unsupported_media_type\","
                "\"message\":\"Content-Type must be application/json.\"}}\n"
            );
        }
        json_error_t parse_error;
        json_t* root = json_loadb(
            request_body.data(), request_body.size(),
            JSON_REJECT_DUPLICATES, &parse_error
        );
        json_t* name = root == nullptr ? nullptr :
            json_object_get(root, "display_name");
        if (!json_is_object(root) || json_object_size(root) != 1 ||
            !json_is_string(name)) {
            if (root != nullptr) json_decref(root);
            return json_response(
                400,
                "{\"error\":{\"code\":\"invalid_project_request\","
                "\"message\":\"display_name is required.\"}}\n"
            );
        }
        const std::string display_name = json_string_value(name);
        json_decref(root);
        ProjectRecord project;
        std::string error;
        if (!metadata_store->create_project(
                authenticated_identity, display_name, project, error)) {
            RouteResponse response = json_response(
                400,
                "{\"error\":{\"code\":\"invalid_project_request\","
                "\"message\":\"Project could not be created.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        json_t* created = project_json_object(project);
        const std::string body = json_dump_line(created);
        json_decref(created);
        return json_response(201, body);
    }

    const std::string jobs_path = public_base_path + "/v1/jobs";
    if (path_at_or_below(uri, jobs_path)) {
        if (!authenticated || metadata_store == nullptr) {
            return json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Job storage is unavailable.\"}}\n"
            );
        }
        if (uri == jobs_path) {
            if (method == "GET") {
                int limit = 25;
                int offset = 0;
                if (!query_integer(query_string, "limit", 25, limit) ||
                    !query_integer(query_string, "offset", 0, offset) ||
                    limit < 1 || limit > 100) {
                    return json_response(
                        400,
                        "{\"error\":{\"code\":\"invalid_pagination\","
                        "\"message\":\"limit must be 1-100 and offset non-negative.\"}}\n"
                    );
                }
                std::vector<JobRecord> jobs;
                std::string error;
                if (!metadata_store->list_jobs(
                        authenticated_identity,
                        limit,
                        offset,
                        jobs,
                        error
                    )) {
                    RouteResponse response = json_response(
                        503,
                        "{\"error\":{\"code\":\"metadata_unavailable\","
                        "\"message\":\"Job storage is unavailable.\"}}\n"
                    );
                    response.internal_error = error;
                    return response;
                }
                return json_response(
                    200,
                    job_list_json(jobs, public_base_path, limit, offset)
                );
            }
            if (method != "POST") {
                return json_response(
                    405,
                    "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Job collection only accepts GET or POST.\"}}\n"
                );
            }
            if (authenticated_identity.role == "viewer") {
                return json_response(
                    403,
                    "{\"error\":{\"code\":\"forbidden\","
                    "\"message\":\"Viewer role cannot create jobs.\"}}\n"
                );
            }
            if (!is_json_content_type(content_type)) {
                return json_response(
                    415,
                    "{\"error\":{\"code\":\"unsupported_media_type\","
                    "\"message\":\"Content-Type must be application/json.\"}}\n"
                );
            }
            JobRequest job_request;
            std::string error;
            const JobRequestStatus request_status = parse_job_request(
                request_body,
                job_request,
                error
            );
            if (request_status != JobRequestStatus::valid) {
                const char* code =
                    request_status == JobRequestStatus::unsupported_field
                    ? "unsupported_field"
                    : "invalid_job_request";
                return json_response(
                    400,
                    std::string("{\"error\":{\"code\":\"") + code +
                    "\",\"message\":\"The job request is invalid.\"}}\n"
                );
            }
            ProjectRecord selected_project;
            const RecordLookupStatus project_status =
                metadata_store->find_project(
                    job_request.project_id,
                    authenticated_identity,
                    selected_project,
                    error
                );
            if (project_status == RecordLookupStatus::not_found) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"project_not_found\","
                    "\"message\":\"The requested project does not exist.\"}}\n"
                );
            }
            if (project_status == RecordLookupStatus::storage_error) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Project storage is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            const PackCatalog catalog(pack_root);
            PackSummary pack;
            const PackLookupStatus pack_status = catalog.find(
                job_request.pack_id,
                pack,
                error
            );
            std::string source_pack_path;
            std::string selected_pack_fingerprint;
            if (pack_status == PackLookupStatus::not_found) {
                CustomPackRecord custom_pack;
                const RecordLookupStatus custom_status =
                    metadata_store->find_custom_pack(
                        job_request.pack_id,
                        authenticated_identity,
                        custom_pack,
                        error
                    );
                if (custom_status == RecordLookupStatus::not_found) {
                    return json_response(
                        404,
                        "{\"error\":{\"code\":\"pack_not_found\","
                        "\"message\":\"The requested pack does not exist.\"}}\n"
                    );
                }
                if (custom_status == RecordLookupStatus::storage_error) {
                    RouteResponse response = json_response(
                        503,
                        "{\"error\":{\"code\":\"metadata_unavailable\","
                        "\"message\":\"Custom pack storage is unavailable.\"}}\n"
                    );
                    response.internal_error = error;
                    return response;
                }
                source_pack_path = custom_pack.source_pack_path;
                selected_pack_fingerprint = custom_pack.pack_fingerprint;
            }
            if (pack_status != PackLookupStatus::found &&
                pack_status != PackLookupStatus::not_found) {
                RouteResponse response = json_response(
                    500,
                    "{\"error\":{\"code\":\"pack_catalog_invalid\","
                    "\"message\":\"The configured pack catalog is invalid.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            if (pack_status == PackLookupStatus::found) {
                bool generator_compatible = false;
                for (std::vector<std::string>::const_iterator it =
                         pack.compatible_generator_versions.begin();
                     it != pack.compatible_generator_versions.end(); ++it) {
                    if (*it == "signal_synth-cli") {
                        generator_compatible = true;
                        break;
                    }
                }
                if (!generator_compatible) {
                    return json_response(
                        409,
                        "{\"error\":{\"code\":\"pack_generator_incompatible\","
                        "\"message\":\"The pack is not compatible with this generator.\"}}\n"
                    );
                }
                source_pack_path =
                    pack_root + "/" + job_request.pack_id + ".json";
                selected_pack_fingerprint = pack.pack_fingerprint;
            }
            UsageSummary usage;
            const QuotaStatus job_quota = metadata_store->check_job_quota(
                authenticated_identity, usage, error
            );
            if (job_quota == QuotaStatus::concurrent_limit ||
                job_quota == QuotaStatus::monthly_limit) {
                const char* code = job_quota == QuotaStatus::concurrent_limit
                    ? "concurrent_job_limit"
                    : "monthly_job_limit";
                return json_response(
                    429,
                    std::string("{\"error\":{\"code\":\"") + code +
                    "\",\"message\":\"Job quota exceeded.\"}}\n"
                );
            }
            if (job_quota == QuotaStatus::storage_error) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Quota storage is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            std::string job_id;
            if (!metadata_store->create_job(
                    authenticated_identity,
                    job_request.project_id,
                    job_request.canonical_json,
                    job_request.pack_id,
                    source_pack_path,
                    selected_pack_fingerprint,
                    job_id,
                    error
                )) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Job storage is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            json_t* created = json_object();
            json_object_set_new(
                created,
                "job_id",
                json_string(job_id.c_str())
            );
            json_object_set_new(created, "status", json_string("queued"));
            const std::string body = json_dump_line(created);
            json_decref(created);
            return json_response(202, body);
        }

        const std::string job_resource = uri.substr(jobs_path.size() + 1);
        const std::string::size_type action_separator =
            job_resource.find('/');
        const std::string job_id = job_resource.substr(0, action_separator);
        const std::string action = action_separator == std::string::npos
            ? std::string()
            : job_resource.substr(action_separator + 1);
        if (!is_valid_pack_id(job_id) ||
            job_id.compare(0, 4, "job_") != 0) {
            return json_response(
                400,
                "{\"error\":{\"code\":\"invalid_job_id\","
                "\"message\":\"The job ID is invalid.\"}}\n"
                );
        }
        if (action == "detection-templates.zip") {
            if (method != "GET") {
                return json_response(
                    405,
                    "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Detection templates only accept GET.\"}}\n"
                );
            }
            JobRecord job;
            std::string error;
            const RecordLookupStatus lookup = metadata_store->find_job(
                job_id,
                authenticated_identity,
                job,
                error
            );
            if (lookup == RecordLookupStatus::not_found) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"job_not_found\","
                    "\"message\":\"The requested job does not exist.\"}}\n"
                );
            }
            if (lookup == RecordLookupStatus::storage_error) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Job storage is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            if (job.status != "succeeded" || job.package_id.empty()) {
                return json_response(
                    409,
                    "{\"error\":{\"code\":\"job_templates_unavailable\","
                    "\"message\":\"Detection templates are available after a curated job succeeds.\"}}\n"
                );
            }
            PackSummary pack;
            const PackLookupStatus pack_status =
                PackCatalog(pack_root).find(job.selected_pack_id, pack, error);
            if (pack_status == PackLookupStatus::not_found) {
                return json_response(
                    409,
                    "{\"error\":{\"code\":\"custom_pack_templates_unavailable\","
                    "\"message\":\"Detection-template ZIP generation is currently available for curated packs.\"}}\n"
                );
            }
            if (pack_status != PackLookupStatus::found) {
                RouteResponse response = json_response(
                    500,
                    "{\"error\":{\"code\":\"pack_catalog_invalid\","
                    "\"message\":\"The configured pack catalog is invalid.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            std::string zip;
            if (!build_detection_template_zip(job, pack, zip, error)) {
                return json_response(
                    409,
                    "{\"error\":{\"code\":\"pack_templates_unavailable\","
                    "\"message\":\"This pack has no locally scoreable detector-output templates.\"}}\n"
                );
            }
            RouteResponse response;
            response.disposition = RouteDisposition::handled;
            response.status = 200;
            response.content_type = "application/zip";
            response.body = zip;
            response.content_disposition =
                "attachment; filename=\"" + job.package_id +
                "-detection-templates.zip\"";
            response.cache_control = "no-store";
            return response;
        }
        if (!action.empty()) {
            if (method != "POST" || (action != "cancel" && action != "retry")) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"route_not_found\","
                    "\"message\":\"The requested job action does not exist.\"}}\n"
                );
            }
            if (authenticated_identity.role == "viewer") {
                return json_response(
                    403,
                    "{\"error\":{\"code\":\"forbidden\","
                    "\"message\":\"Viewer role cannot modify jobs.\"}}\n"
                );
            }
            std::string error;
            std::string new_job_id;
            const JobLifecycleStatus lifecycle = action == "cancel"
                ? metadata_store->cancel_job(
                    job_id, authenticated_identity, error)
                : metadata_store->retry_job(
                    job_id, authenticated_identity, new_job_id, error);
            if (lifecycle == JobLifecycleStatus::not_found) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"job_not_found\","
                    "\"message\":\"The requested job does not exist.\"}}\n"
                );
            }
            if (lifecycle == JobLifecycleStatus::invalid_state) {
                return json_response(
                    409,
                    std::string("{\"error\":{\"code\":\"job_") + action +
                    "_invalid_state\",\"message\":\"The job cannot be " +
                    action + "ed in its current state.\"}}\n"
                );
            }
            if (lifecycle == JobLifecycleStatus::storage_error) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Job storage is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            if (action == "retry") {
                return json_response(
                    202,
                    std::string("{\"job_id\":\"") + new_job_id +
                    "\",\"retry_of\":\"" + job_id +
                    "\",\"status\":\"queued\"}\n"
                );
            }
            return json_response(
                200,
                std::string("{\"job_id\":\"") + job_id +
                "\",\"status\":\"cancelled\"}\n"
            );
        }
        if (method == "DELETE") {
            if (authenticated_identity.role == "viewer") {
                return json_response(
                    403,
                    "{\"error\":{\"code\":\"forbidden\","
                    "\"message\":\"Viewer role cannot delete jobs.\"}}\n"
                );
            }
            std::string error;
            const JobDeleteStatus delete_status = metadata_store->delete_job(
                job_id,
                authenticated_identity,
                error
            );
            if (delete_status == JobDeleteStatus::deleted) {
                return json_response(
                    200,
                    std::string("{\"job_id\":\"") + job_id +
                    "\",\"status\":\"deleted\"}\n"
                );
            }
            if (delete_status == JobDeleteStatus::not_found) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"job_not_found\","
                    "\"message\":\"The requested job does not exist.\"}}\n"
                );
            }
            if (delete_status == JobDeleteStatus::running) {
                return json_response(
                    409,
                    "{\"error\":{\"code\":\"job_running\","
                    "\"message\":\"A running job cannot be deleted.\"}}\n"
                );
            }
            RouteResponse response = json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Job storage is unavailable.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Job status only accepts GET or DELETE.\"}}\n"
            );
        }
        JobRecord job;
        std::string error;
        const RecordLookupStatus lookup = metadata_store->find_job(
            job_id,
            authenticated_identity,
            job,
            error
        );
        if (lookup == RecordLookupStatus::not_found) {
            return json_response(
                404,
                "{\"error\":{\"code\":\"job_not_found\","
                "\"message\":\"The requested job does not exist.\"}}\n"
            );
        }
        if (lookup == RecordLookupStatus::storage_error) {
            RouteResponse response = json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Job storage is unavailable.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        return json_response(200, job_json(job, public_base_path));
    }

    const std::string artifacts_path =
        public_base_path + "/v1/artifacts";
    if (path_at_or_below(uri, artifacts_path)) {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Artifact endpoints only accept GET.\"}}\n"
            );
        }
        const std::string relative =
            uri.size() > artifacts_path.size()
            ? uri.substr(artifacts_path.size() + 1)
            : std::string();
        const std::string::size_type separator = relative.find('/');
        if (separator == std::string::npos ||
            relative.find('/', separator + 1) != std::string::npos) {
            return json_response(
                400,
                "{\"error\":{\"code\":\"invalid_artifact_path\","
                "\"message\":\"The artifact path is invalid.\"}}\n"
            );
        }
        const std::string package_id = relative.substr(0, separator);
        const std::string filename = relative.substr(separator + 1);
        if (!is_valid_pack_id(package_id) ||
            package_id.compare(0, 4, "pkg_") != 0 ||
            (filename != "manifest.json" && filename != "package.zip")) {
            return json_response(
                400,
                "{\"error\":{\"code\":\"invalid_artifact_path\","
                "\"message\":\"The artifact path is invalid.\"}}\n"
            );
        }
        PackageRecord package;
        std::string error;
        const RecordLookupStatus lookup = metadata_store->find_package(
            package_id,
            authenticated_identity,
            package,
            error
        );
        if (lookup == RecordLookupStatus::not_found) {
            return json_response(
                404,
                "{\"error\":{\"code\":\"artifact_not_found\","
                "\"message\":\"The requested artifact does not exist.\"}}\n"
            );
        }
        if (lookup == RecordLookupStatus::storage_error) {
            RouteResponse response = json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Artifact metadata is unavailable.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        const std::string expected_storage =
            data_root + "/packages/" + package_id;
        if (data_root.empty() ||
            package.artifact_storage_key != expected_storage) {
            RouteResponse response = json_response(
                500,
                "{\"error\":{\"code\":\"artifact_storage_invalid\","
                "\"message\":\"Artifact storage is unavailable.\"}}\n"
            );
            response.internal_error =
                "artifact storage key does not match configured data root";
            return response;
        }
        RouteResponse response;
        response.disposition = RouteDisposition::handled;
        response.status = 200;
        response.content_type = filename == "manifest.json"
            ? "application/json"
            : "application/zip";
        response.file_path = expected_storage + "/" + filename;
        response.content_disposition =
            std::string("attachment; filename=\"") + filename + "\"";
        return response;
    }

    if (uri == public_base_path ||
        uri == public_base_path + "/" ||
        uri == public_base_path + "/ui" ||
        uri == public_base_path + "/ui/") {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"The web UI only accepts GET.\"}}\n"
            );
        }
        RouteResponse response;
        response.disposition = RouteDisposition::handled;
        response.status = 200;
        response.content_type = "text/html; charset=utf-8";
        response.body = kUiHtml;
        return response;
    }

    if (uri == public_base_path + "/docs/quickstart" ||
        uri == public_base_path + "/docs/api" ||
        uri == public_base_path + "/docs/troubleshooting") {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Documentation pages only accept GET.\"}}\n"
            );
        }
        RouteResponse response;
        response.disposition = RouteDisposition::handled;
        response.status = 200;
        response.content_type = "text/html; charset=utf-8";
        if (uri == public_base_path + "/docs/quickstart") {
            response.body = kQuickstartHtml;
        } else if (uri == public_base_path + "/docs/api") {
            response.body = kApiDocsHtml;
        } else {
            response.body = kTroubleshootingHtml;
        }
        return response;
    }

    if (uri == public_base_path + "/ui/style.css" ||
        uri == public_base_path + "/ui/app.js") {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Static UI assets only accept GET.\"}}\n"
            );
        }
        RouteResponse response;
        response.disposition = RouteDisposition::handled;
        response.status = 200;
        if (uri == public_base_path + "/ui/style.css") {
            response.content_type = "text/css; charset=utf-8";
            response.body = kUiCss;
        } else {
            response.content_type = "application/javascript; charset=utf-8";
            response.body = kUiJs;
        }
        return response;
    }

    if (uri == public_base_path + "/healthz") {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"The health endpoint only accepts GET.\"}}\n"
            );
        }

        return json_response(
            200,
            "{\"service\":\"signal_synth_saas\",\"status\":\"ok\","
            "\"build\":{\"version\":\"" SYN_SIG_RA_VERSION
            "\",\"git_commit\":\"" SYN_SIG_RA_GIT_COMMIT "\"}}\n"
        );
    }

    const std::string packs_path = public_base_path + "/v1/packs";
    if (path_at_or_below(uri, packs_path)) {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Pack catalog endpoints only accept GET.\"}}\n"
            );
        }
        if (pack_root.empty()) {
            return json_response(
                503,
                "{\"error\":{\"code\":\"pack_catalog_unavailable\","
                "\"message\":\"The pack catalog is unavailable.\"}}\n"
            );
        }

        const PackCatalog catalog(pack_root);
        if (uri == packs_path) {
            std::vector<PackSummary> packs;
            std::string error;
            if (!catalog.list(packs, error)) {
                RouteResponse response = json_response(
                    500,
                    "{\"error\":{\"code\":\"pack_catalog_invalid\","
                    "\"message\":\"The configured pack catalog is invalid.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            std::string body("{\"packs\":[");
            for (std::size_t index = 0; index < packs.size(); ++index) {
                if (index != 0) {
                    body += ',';
                }
                body += pack_summary_json(packs[index]);
            }
            body += "]}\n";
            return json_response(200, body);
        }

        const std::string pack_id = uri.substr(packs_path.size() + 1);
        PackSummary pack;
        std::string error;
        const PackLookupStatus status = catalog.find(pack_id, pack, error);
        if (status == PackLookupStatus::invalid_id) {
            return json_response(
                400,
                "{\"error\":{\"code\":\"invalid_pack_id\","
                "\"message\":\"The pack ID is invalid.\"}}\n"
            );
        }
        if (status == PackLookupStatus::not_found) {
            return json_response(
                404,
                "{\"error\":{\"code\":\"pack_not_found\","
                "\"message\":\"The requested pack does not exist.\"}}\n"
            );
        }
        if (status == PackLookupStatus::catalog_error) {
            RouteResponse response = json_response(
                500,
                "{\"error\":{\"code\":\"pack_catalog_invalid\","
                "\"message\":\"The configured pack catalog is invalid.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        return json_response(200, pack_summary_json(pack) + "\n");
    }

    return json_response(
        404,
        "{\"error\":{\"code\":\"route_not_found\","
        "\"message\":\"No API route matches the requested path.\"}}\n"
    );
}

}  // namespace syn_sig_ra
