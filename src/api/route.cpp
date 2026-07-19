#include "syn_sig_ra/route.h"

#include "syn_sig_ra/api_key_auth.h"
#include "syn_sig_ra/artifact_store.h"
#include "syn_sig_ra/build_info.h"
#include "syn_sig_ra/core_contract.h"
#include "syn_sig_ra/job_request.h"
#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/openapi_document.h"
#include "syn_sig_ra/pack_catalog.h"
#include "syn_sig_ra/password_auth.h"
#include "syn_sig_ra/random_id.h"
#include "syn_sig_ra/sha256.h"
#include "syn_sig_ra/signal_viewer.h"
#include "syn_sig_ra/transactional_email.h"
#include "syn_sig_ra/viewer_assets.h"
#include "ecg_pack.h"
#include "ecg_scenario_json.h"
#include "scenario_authoring.h"
#include "synsigra_api.h"

#include <jansson.h>

#include <cctype>
#include <cerrno>
#include <climits>
#include <ctime>
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

const char kTermsVersion[] = "private-beta-2026-07-17-r4";
const char kSupportEmail[] = "synsigra@gmail.com";
const char kSupportUrl[] = "mailto:synsigra@gmail.com";
const char kOperatorName[] = "Kis Tamás";
const char kOperatorAddress[] =
    "2040 Budaörs, Tátra u. 6, Hungary";

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

bool regular_file_without_symlink(const std::string& path) {
    struct stat information;
    return lstat(path.c_str(), &information) == 0 &&
        S_ISREG(information.st_mode);
}

bool disk_generation_capacity(
    const std::string& data_root,
    unsigned long long& free_bytes,
    unsigned long long& reserve_bytes
) {
    struct statvfs disk;
    if (data_root.empty() || statvfs(data_root.c_str(), &disk) != 0) {
        free_bytes = 0;
        reserve_bytes = 1024ull * 1024ull * 1024ull;
        return false;
    }
    free_bytes =
        static_cast<unsigned long long>(disk.f_bavail) * disk.f_frsize;
    const unsigned long long total =
        static_cast<unsigned long long>(disk.f_blocks) * disk.f_frsize;
    reserve_bytes = total / 10ull;
    if (reserve_bytes < 1024ull * 1024ull * 1024ull) {
        reserve_bytes = 1024ull * 1024ull * 1024ull;
    }
    return free_bytes > reserve_bytes;
}

bool is_ui_page_route(const std::string& uri, const std::string& public_base_path) {
    static const char* kPages[] = {
        "",
        "/",
        "/ui",
        "/ui/",
        "/workspace",
        "/packs",
        "/generate",
        "/jobs",
        "/verify",
        "/scenarios",
        "/custom-packs",
        "/account",
        "/advanced"
    };
    for (const char* page : kPages) {
        if (uri == public_base_path + page) {
            return true;
        }
    }
    return false;
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
bool read_file_to_string(
    const std::string& path,
    std::string& content,
    std::string& error
);

json_t* account_json_object(const syn_sig_ra::AccountRecord& account) {
    json_t* root = json_object();
    json_object_set_new(root, "user_id", json_string(account.user_id.c_str()));
    json_object_set_new(
        root, "organization_id", json_string(account.organization_id.c_str()));
    json_object_set_new(root, "email", json_string(account.email.c_str()));
    json_object_set_new(
        root, "display_name", json_string(account.display_name.c_str()));
    json_object_set_new(root, "role", json_string(account.role.c_str()));
    json_object_set_new(
        root, "email_verified", json_boolean(account.email_verified));
    if (!account.email_verified_at.empty()) {
        json_object_set_new(
            root, "email_verified_at",
            json_string(account.email_verified_at.c_str()));
    }
    return root;
}

std::string account_json(const syn_sig_ra::AccountRecord& account) {
    json_t* root = account_json_object(account);
    const std::string encoded = json_dump_line(root);
    json_decref(root);
    return encoded;
}

std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm value;
    if (gmtime_r(&now, &value) == nullptr) return std::string();
    char encoded[32];
    return std::strftime(
        encoded, sizeof(encoded), "%Y-%m-%dT%H:%M:%SZ", &value) == 0
        ? std::string() : std::string(encoded);
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

enum class AccountEmailDeliveryStatus {
    delivered,
    rate_limited,
    storage_error,
    provider_error
};

AccountEmailDeliveryStatus deliver_account_email(
    syn_sig_ra::MetadataStore& store,
    const syn_sig_ra::EmailConfig& config,
    const syn_sig_ra::AccountRecord& account,
    const std::string& purpose,
    const std::string& public_base_path,
    std::string& error
) {
    std::string token;
    std::string token_hash;
    const std::string prefix = purpose == "email_verification"
        ? "verify_" : "reset_";
    if (!syn_sig_ra::random_id(prefix, token, error) ||
        !syn_sig_ra::sha256_hex(token, token_hash, error)) {
        return AccountEmailDeliveryStatus::storage_error;
    }
    const int ttl_minutes = purpose == "email_verification" ? 1440 : 30;
    const syn_sig_ra::EmailTokenCreateStatus created = store.create_email_token(
        account.user_id, purpose, token_hash, account.email, ttl_minutes, error);
    if (created == syn_sig_ra::EmailTokenCreateStatus::rate_limited) {
        return AccountEmailDeliveryStatus::rate_limited;
    }
    if (created != syn_sig_ra::EmailTokenCreateStatus::created) {
        return AccountEmailDeliveryStatus::storage_error;
    }
    const bool verification = purpose == "email_verification";
    const std::string action = verification ? "verify" : "reset";
    const std::string link = config.public_origin + public_base_path +
        "/account?" + action + "=" + token;
    const std::string subject = verification
        ? "Verify your Synsigra email"
        : "Reset your Synsigra password";
    std::ostringstream body;
    body << (verification
        ? "Verify your email to activate your Synsigra account."
        : "Use this link to choose a new Synsigra password.")
         << "\n\n" << link << "\n\n"
         << (verification
            ? "This link expires in 24 hours."
            : "This link expires in 30 minutes.")
         << " If you did not request this, you can ignore this email.\n\n"
         << "Account support: " << kSupportEmail << "\n"
         << "Never email passwords, API keys, account-action links, PHI, "
            "or proprietary outputs.\n"
         << "Operator: " << kOperatorName << ", " << kOperatorAddress;
    const syn_sig_ra::EmailSendStatus sent = syn_sig_ra::send_transactional_email(
        config, account.email, subject, body.str(), error);
    return sent == syn_sig_ra::EmailSendStatus::sent
        ? AccountEmailDeliveryStatus::delivered
        : AccountEmailDeliveryStatus::provider_error;
}

syn_sig_ra::RouteResponse generic_email_accepted_response() {
    syn_sig_ra::RouteResponse response = json_response(
        202,
        "{\"status\":\"accepted\",\"message\":\"If the account is eligible, "
        "an email will arrive shortly.\"}\n"
    );
    response.cache_control = "no-store";
    return response;
}

bool parse_email_request(
    const std::string& content_type,
    const std::string& request_body,
    std::string& email
) {
    if (!is_json_content_type(content_type)) return false;
    json_error_t parse_error;
    json_t* root = json_loadb(
        request_body.data(), request_body.size(), JSON_REJECT_DUPLICATES,
        &parse_error);
    json_t* value = root == nullptr ? nullptr : json_object_get(root, "email");
    const bool valid = json_is_object(root) && json_object_size(root) == 1u &&
        json_is_string(value) && syn_sig_ra::normalize_email(json_string_value(value), email);
    if (root != nullptr) json_decref(root);
    return valid;
}

bool parse_token_request(
    const std::string& content_type,
    const std::string& request_body,
    std::string& token
) {
    if (!is_json_content_type(content_type)) return false;
    json_error_t parse_error;
    json_t* root = json_loadb(
        request_body.data(), request_body.size(), JSON_REJECT_DUPLICATES,
        &parse_error);
    json_t* value = root == nullptr ? nullptr : json_object_get(root, "token");
    token = json_is_string(value) ? json_string_value(value) : "";
    const bool valid = json_is_object(root) && json_object_size(root) == 1u &&
        !token.empty() && token.size() <= 512;
    if (root != nullptr) json_decref(root);
    return valid;
}

bool parse_password_reset_request(
    const std::string& content_type,
    const std::string& request_body,
    std::string& token,
    std::string& password
) {
    if (!is_json_content_type(content_type)) return false;
    json_error_t parse_error;
    json_t* root = json_loadb(
        request_body.data(), request_body.size(), JSON_REJECT_DUPLICATES,
        &parse_error);
    json_t* token_value = root == nullptr
        ? nullptr : json_object_get(root, "token");
    json_t* password_value = root == nullptr
        ? nullptr : json_object_get(root, "password");
    token = json_is_string(token_value) ? json_string_value(token_value) : "";
    password = json_is_string(password_value)
        ? json_string_value(password_value) : "";
    const bool valid = json_is_object(root) && json_object_size(root) == 2u &&
        !token.empty() && token.size() <= 512 && password.size() <= 128;
    if (root != nullptr) json_decref(root);
    return valid;
}

bool parse_exact_string_fields(
    const std::string& content_type,
    const std::string& request_body,
    const std::vector<std::string>& names,
    std::vector<std::string>& values
) {
    values.clear();
    if (!is_json_content_type(content_type) || names.empty()) return false;
    json_error_t parse_error;
    json_t* root = json_loadb(
        request_body.data(), request_body.size(), JSON_REJECT_DUPLICATES,
        &parse_error);
    bool valid = json_is_object(root) && json_object_size(root) == names.size();
    for (std::vector<std::string>::const_iterator name = names.begin();
         valid && name != names.end(); ++name) {
        json_t* value = json_object_get(root, name->c_str());
        valid = json_is_string(value);
        if (valid) values.push_back(json_string_value(value));
    }
    if (root != nullptr) json_decref(root);
    if (!valid) values.clear();
    return valid;
}

syn_sig_ra::RouteResponse internal_account_error(
    const std::string& message,
    const std::string& error
) {
    syn_sig_ra::RouteResponse response = json_response(
        503, "{\"error\":{\"code\":\"account_service_unavailable\","
        "\"message\":\"" + message + "\"}}\n");
    response.internal_error = error;
    return response;
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
        root, "pack_version", json_string(job.selected_pack_version.c_str()));
    if (!job.catalog_version.empty()) {
        json_t* catalog = json_object();
        json_object_set_new(
            catalog, "version", json_string(job.catalog_version.c_str()));
        json_object_set_new(
            catalog, "source_sha256",
            json_string(job.catalog_source_sha256.c_str()));
        json_object_set_new(root, "catalog", catalog);
    }
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
        json_object_set_new(root, "integration_contract", json_string(
            job.integration_contract_version.c_str()));
        json_object_set_new(root, "generator_version", json_string(
            job.generator_version.c_str()));
        json_object_set_new(root, "generator_git_commit", json_string(
            job.generator_git_commit.c_str()));
        json_object_set_new(root, "generator_build_identity", json_string(
            job.generator_build_identity.c_str()));
        json_object_set_new(root, "generator_binary_sha256", json_string(
            job.generator_binary_sha256.c_str()));
        if (!job.challenge_metadata_json.empty()) {
            json_error_t metadata_error;
            json_t* metadata = json_loadb(
                job.challenge_metadata_json.data(),
                job.challenge_metadata_json.size(),
                JSON_REJECT_DUPLICATES,
                &metadata_error);
            if (json_is_object(metadata)) {
                json_object_set_new(root, "challenge", metadata);
            } else if (metadata != nullptr) {
                json_decref(metadata);
            }
        }
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
            "verification_kit_url",
            json_string(
                (
                    public_base_path + "/v1/jobs/" + job.job_id +
                    "/verification-kit.zip"
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

std::string csv_cell(const std::string& value) {
    bool quote = false;
    for (std::string::const_iterator it = value.begin(); it != value.end(); ++it) {
        if (*it == ',' || *it == '"' || *it == '\n' || *it == '\r') {
            quote = true;
            break;
        }
    }
    if (!quote) return value;
    std::string escaped("\"");
    for (std::string::const_iterator it = value.begin(); it != value.end(); ++it) {
        if (*it == '"') escaped.push_back('"');
        escaped.push_back(*it);
    }
    escaped.push_back('"');
    return escaped;
}

std::string audit_event_list_json(
    const std::vector<syn_sig_ra::AuditEventRecord>& events,
    int limit,
    int offset
) {
    json_t* root = json_object();
    json_t* items = json_array();
    for (std::vector<syn_sig_ra::AuditEventRecord>::const_iterator it =
             events.begin(); it != events.end(); ++it) {
        json_t* item = json_object();
        json_object_set_new(item, "id", json_integer(it->audit_event_id));
        json_object_set_new(
            item, "created_at", json_string(it->created_at.c_str()));
        json_object_set_new(
            item, "event_type", json_string(it->event_type.c_str()));
        json_object_set_new(
            item, "user_id", json_string(it->user_id.c_str()));
        json_object_set_new(
            item, "api_key_id", json_string(it->api_key_id.c_str()));
        json_object_set_new(
            item, "subject_type", json_string(it->subject_type.c_str()));
        json_object_set_new(
            item, "subject_id", json_string(it->subject_id.c_str()));
        json_error_t parse_error;
        json_t* details = json_loads(
            it->details_json.c_str(), JSON_REJECT_DUPLICATES, &parse_error);
        json_object_set_new(
            item, "details", details == nullptr ? json_object() : details);
        json_array_append_new(items, item);
    }
    json_object_set_new(root, "audit_events", items);
    json_object_set_new(root, "limit", json_integer(limit));
    json_object_set_new(root, "offset", json_integer(offset));
    json_object_set_new(root, "count", json_integer(events.size()));
    if (events.size() == static_cast<std::size_t>(limit)) {
        json_object_set_new(root, "next_offset", json_integer(offset + limit));
    }
    const std::string output = json_dump_line(root);
    json_decref(root);
    return output;
}

std::string audit_event_list_csv(
    const std::vector<syn_sig_ra::AuditEventRecord>& events
) {
    std::ostringstream output;
    output << "id,created_at,event_type,user_id,api_key_id,subject_type,subject_id,details_json\r\n";
    for (std::vector<syn_sig_ra::AuditEventRecord>::const_iterator it =
             events.begin(); it != events.end(); ++it) {
        output << it->audit_event_id << ','
               << csv_cell(it->created_at) << ','
               << csv_cell(it->event_type) << ','
               << csv_cell(it->user_id) << ','
               << csv_cell(it->api_key_id) << ','
               << csv_cell(it->subject_type) << ','
               << csv_cell(it->subject_id) << ','
               << csv_cell(it->details_json) << "\r\n";
    }
    return output.str();
}

void record_nonblocking_audit(
    syn_sig_ra::MetadataStore& store,
    const syn_sig_ra::ApiKeyIdentity& actor,
    const std::string& event_type,
    const std::string& subject_type,
    const std::string& subject_id,
    const std::string& details_json,
    syn_sig_ra::RouteResponse& response
) {
    std::string error;
    if (!store.record_audit_event(
            actor, event_type, subject_type, subject_id, details_json, error)) {
        response.internal_error = "audit write failed: " + error;
    }
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

bool query_value(
    const std::string& query,
    const std::string& name,
    std::string& value
) {
    value.clear();
    if (query.empty()) return false;
    const std::string needle = name + "=";
    std::string::size_type start = 0;
    while ((start = query.find(needle, start)) != std::string::npos) {
        if (start == 0 || query[start - 1] == '&') break;
        start += needle.size();
    }
    if (start == std::string::npos) return false;
    const std::string::size_type value_start = start + needle.size();
    const std::string::size_type end = query.find('&', value_start);
    const std::string encoded = query.substr(value_start, end - value_start);
    value.reserve(encoded.size());
    for (std::string::size_type index = 0; index < encoded.size(); ++index) {
        if (encoded[index] == '+') {
            value.push_back(' ');
            continue;
        }
        if (encoded[index] != '%') {
            value.push_back(encoded[index]);
            continue;
        }
        if (index + 2u >= encoded.size() ||
            !std::isxdigit(static_cast<unsigned char>(encoded[index + 1u])) ||
            !std::isxdigit(static_cast<unsigned char>(encoded[index + 2u]))) {
            value.clear();
            return false;
        }
        const std::string hex = encoded.substr(index + 1u, 2u);
        const char decoded = static_cast<char>(std::strtoul(hex.c_str(), nullptr, 16));
        if (decoded == '\0') {
            value.clear();
            return false;
        }
        value.push_back(decoded);
        index += 2u;
    }
    return true;
}

bool query_unsigned_long_long(
    const std::string& query,
    const std::string& name,
    unsigned long long default_value,
    bool required,
    unsigned long long& value
) {
    std::string encoded;
    if (!query_value(query, name, encoded)) {
        value = default_value;
        return !required;
    }
    if (encoded.empty() || encoded[0] == '-') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(encoded.c_str(), &end, 10);
    if (errno == ERANGE || end == encoded.c_str() || *end != '\0') return false;
    value = parsed;
    return true;
}

bool query_channel_indices(
    const std::string& query,
    std::vector<unsigned int>& channels
) {
    channels.clear();
    std::string encoded;
    if (!query_value(query, "channels", encoded) || encoded.empty()) return false;
    std::string::size_type start = 0;
    while (start <= encoded.size()) {
        const std::string::size_type end = encoded.find(',', start);
        const std::string item = encoded.substr(start, end - start);
        if (item.empty() || item[0] == '-') return false;
        char* parsed_end = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(item.c_str(), &parsed_end, 10);
        if (errno == ERANGE || parsed_end == item.c_str() ||
            *parsed_end != '\0' || parsed > UINT_MAX) return false;
        channels.push_back(static_cast<unsigned int>(parsed));
        if (end == std::string::npos) break;
        start = end + 1u;
    }
    return !channels.empty();
}

std::string replace_all(
    std::string value,
    const std::string& needle,
    const std::string& replacement
) {
    std::string::size_type offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        value.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
    return value;
}

std::string signal_viewer_source_json(
    const syn_sig_ra::SignalViewerSource& source,
    const std::string& job_id,
    const std::string& package_id
) {
    json_t* root = json_object();
    json_object_set_new(root, "schema_version", json_integer(1));
    json_object_set_new(root, "job_id", json_string(job_id.c_str()));
    json_object_set_new(root, "package_id", json_string(package_id.c_str()));
    json_t* cases = json_array();
    for (std::vector<syn_sig_ra::SignalViewerCase>::const_iterator it =
             source.cases.begin(); it != source.cases.end(); ++it) {
        json_t* item = json_object();
        json_object_set_new(item, "case_id", json_string(it->case_id.c_str()));
        json_object_set_new(item, "sample_rate_hz", json_real(it->sample_rate_hz));
        json_object_set_new(
            item, "sample_count",
            json_integer(static_cast<json_int_t>(it->sample_count)));
        json_object_set_new(
            item, "duration_seconds",
            json_real(static_cast<double>(it->sample_count) / it->sample_rate_hz));
        json_t* channels = json_array();
        for (std::vector<syn_sig_ra::SignalViewerChannel>::const_iterator channel =
                 it->channels.begin(); channel != it->channels.end(); ++channel) {
            json_t* encoded = json_object();
            json_object_set_new(encoded, "index", json_integer(channel->index));
            json_object_set_new(encoded, "name", json_string(channel->name.c_str()));
            json_object_set_new(encoded, "unit", json_string(channel->unit.c_str()));
            json_object_set_new(encoded, "gain", json_real(channel->gain));
            json_object_set_new(encoded, "adc_zero", json_integer(channel->adc_zero));
            json_array_append_new(channels, encoded);
        }
        json_object_set_new(item, "channels", channels);
        json_array_append_new(cases, item);
    }
    json_object_set_new(root, "cases", cases);
    json_t* capabilities = json_object();
    json_object_set_new(
        capabilities, "transport", json_string("binary_http_viewports"));
    json_object_set_new(
        capabilities, "window_media_type",
        json_string(syn_sig_ra::signal_viewer_binary_content_type()));
    json_object_set_new(capabilities, "maximum_points", json_integer(16384));
    json_object_set_new(
        capabilities, "aggregation", json_string("exact_min_max"));
    json_object_set_new(capabilities, "channel_selection", json_boolean(true));
    json_object_set_new(root, "capabilities", capabilities);
    const std::string output = json_dump_line(root);
    json_decref(root);
    return output;
}

std::string signal_viewer_overlay_json(
    const syn_sig_ra::SignalViewerOverlayWindow& window
) {
    json_t* root = json_object();
    json_object_set_new(root, "schema_version", json_integer(1));
    json_object_set_new(
        root, "requested_start_sample",
        json_integer(static_cast<json_int_t>(window.requested_start_sample)));
    json_object_set_new(
        root, "requested_sample_count",
        json_integer(static_cast<json_int_t>(window.requested_sample_count)));
    json_object_set_new(
        root, "source_sample_count",
        json_integer(static_cast<json_int_t>(window.source_sample_count)));
    json_object_set_new(root, "sample_rate_hz", json_real(window.sample_rate_hz));
    json_object_set_new(
        root, "total_matching_items",
        json_integer(static_cast<json_int_t>(window.total_matching_items)));
    json_object_set_new(root, "aggregated", json_boolean(window.aggregated));
    json_t* kinds = json_array();
    for (std::vector<std::string>::const_iterator kind =
             window.available_kinds.begin(); kind != window.available_kinds.end(); ++kind) {
        json_array_append_new(kinds, json_string(kind->c_str()));
    }
    json_object_set_new(root, "available_kinds", kinds);
    json_t* items = json_array();
    for (std::vector<syn_sig_ra::SignalViewerOverlayItem>::const_iterator item =
             window.items.begin(); item != window.items.end(); ++item) {
        json_t* encoded = json_object();
        json_object_set_new(encoded, "kind", json_string(item->kind.c_str()));
        json_object_set_new(encoded, "source", json_string(item->source.c_str()));
        json_object_set_new(encoded, "label", json_string(item->label.c_str()));
        json_object_set_new(encoded, "channel", json_string(item->channel.c_str()));
        json_object_set_new(
            encoded, "start_sample",
            json_integer(static_cast<json_int_t>(item->start_sample)));
        json_object_set_new(
            encoded, "end_sample",
            json_integer(static_cast<json_int_t>(item->end_sample)));
        json_object_set_new(encoded, "interval", json_boolean(item->interval));
        if (item->has_value) {
            json_object_set_new(encoded, "value", json_real(item->value));
        }
        json_object_set_new(encoded, "count", json_integer(item->count));
        json_array_append_new(items, encoded);
    }
    json_object_set_new(root, "items", items);
    const std::string output = json_dump_line(root);
    json_decref(root);
    return output;
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
    json_t* target_intent = json_loads(
        draft.target_intent_json.c_str(), JSON_REJECT_DUPLICATES, &parse_error
    );
    json_object_set_new(root, "scenario", document == nullptr ? json_null() : document);
    json_object_set_new(
        root, "target_intent",
        json_is_array(target_intent) ? target_intent : json_array()
    );
    if (target_intent != nullptr && !json_is_array(target_intent)) {
        json_decref(target_intent);
    }
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

bool build_account_export(
    syn_sig_ra::MetadataStore& store,
    const syn_sig_ra::ApiKeyIdentity& identity,
    const std::string& public_base_path,
    std::string& encoded,
    std::string& error
) {
    syn_sig_ra::AccountRecord account;
    std::vector<syn_sig_ra::ProjectRecord> projects;
    std::vector<syn_sig_ra::JobRecord> jobs;
    std::vector<syn_sig_ra::ScenarioDraftRecord> scenarios;
    std::vector<syn_sig_ra::CustomPackRecord> custom_packs;
    std::vector<syn_sig_ra::ApiKeyRecord> api_keys;
    std::vector<syn_sig_ra::LegalAcceptanceRecord> legal_acceptances;
    std::vector<syn_sig_ra::AuditEventRecord> audit_events;
    if (store.find_account(identity, account, error) !=
            syn_sig_ra::RecordLookupStatus::found ||
        !store.list_projects(identity, projects, error) ||
        !store.list_account_jobs(identity, jobs, error) ||
        !store.list_scenario_drafts(identity, scenarios, error) ||
        !store.list_custom_packs(identity, custom_packs, error) ||
        !store.list_personal_api_keys(identity, api_keys, error) ||
        !store.list_legal_acceptances(identity, legal_acceptances, error)) {
        return false;
    }
    for (int offset = 0;; offset += 1000) {
        std::vector<syn_sig_ra::AuditEventRecord> page;
        if (!store.list_audit_events(identity, 1000, offset, page, error)) {
            return false;
        }
        audit_events.insert(audit_events.end(), page.begin(), page.end());
        if (page.size() < 1000u) break;
    }

    json_t* root = json_object();
    json_object_set_new(root, "schema_version", json_integer(1));
    json_object_set_new(root, "product", json_string("Synsigra"));
    json_object_set_new(root, "exported_at", json_string(utc_now().c_str()));
    json_object_set_new(root, "account", account_json_object(account));

    json_t* project_items = json_array();
    for (std::vector<syn_sig_ra::ProjectRecord>::const_iterator item =
             projects.begin(); item != projects.end(); ++item) {
        json_array_append_new(project_items, project_json_object(*item));
    }
    json_object_set_new(root, "projects", project_items);

    json_t* job_items = json_array();
    for (std::vector<syn_sig_ra::JobRecord>::const_iterator item = jobs.begin();
         item != jobs.end(); ++item) {
        json_t* value = job_json_object(*item, public_base_path);
        json_error_t parse_error;
        json_t* request = json_loads(
            item->request_json.c_str(), JSON_REJECT_DUPLICATES, &parse_error);
        json_object_set_new(
            value, "request", request == nullptr ? json_null() : request);
        if (!item->deleted_at.empty()) {
            json_object_set_new(
                value, "deleted_at", json_string(item->deleted_at.c_str()));
        }
        json_array_append_new(job_items, value);
    }
    json_object_set_new(root, "jobs", job_items);

    json_t* scenario_items = json_array();
    for (std::vector<syn_sig_ra::ScenarioDraftRecord>::const_iterator item =
             scenarios.begin(); item != scenarios.end(); ++item) {
        json_array_append_new(scenario_items, scenario_draft_json_object(*item));
    }
    json_object_set_new(root, "scenario_drafts", scenario_items);

    json_t* pack_items = json_array();
    for (std::vector<syn_sig_ra::CustomPackRecord>::const_iterator item =
             custom_packs.begin(); item != custom_packs.end(); ++item) {
        json_array_append_new(pack_items, custom_pack_json_object(*item));
    }
    json_object_set_new(root, "custom_packs", pack_items);

    json_t* key_items = json_array();
    for (std::vector<syn_sig_ra::ApiKeyRecord>::const_iterator item =
             api_keys.begin(); item != api_keys.end(); ++item) {
        json_t* value = json_object();
        json_object_set_new(
            value, "api_key_id", json_string(item->api_key_id.c_str()));
        json_object_set_new(value, "label", json_string(item->label.c_str()));
        json_object_set_new(value, "active", json_boolean(item->active));
        json_object_set_new(
            value, "created_at", json_string(item->created_at.c_str()));
        if (!item->last_used_at.empty()) {
            json_object_set_new(
                value, "last_used_at", json_string(item->last_used_at.c_str()));
        }
        json_array_append_new(key_items, value);
    }
    json_object_set_new(root, "api_keys", key_items);

    json_t* legal_items = json_array();
    for (std::vector<syn_sig_ra::LegalAcceptanceRecord>::const_iterator item =
             legal_acceptances.begin(); item != legal_acceptances.end(); ++item) {
        json_t* value = json_object();
        json_object_set_new(
            value, "document_type", json_string(item->document_type.c_str()));
        json_object_set_new(
            value, "document_version", json_string(item->document_version.c_str()));
        json_object_set_new(
            value, "accepted_at", json_string(item->accepted_at.c_str()));
        json_array_append_new(legal_items, value);
    }
    json_object_set_new(root, "legal_acceptances", legal_items);

    json_t* audit_items = json_array();
    for (std::vector<syn_sig_ra::AuditEventRecord>::const_iterator item =
             audit_events.begin(); item != audit_events.end(); ++item) {
        json_t* value = json_object();
        json_object_set_new(value, "id", json_integer(item->audit_event_id));
        json_object_set_new(
            value, "created_at", json_string(item->created_at.c_str()));
        json_object_set_new(
            value, "event_type", json_string(item->event_type.c_str()));
        json_object_set_new(
            value, "api_key_id", json_string(item->api_key_id.c_str()));
        json_object_set_new(
            value, "subject_type", json_string(item->subject_type.c_str()));
        json_object_set_new(
            value, "subject_id", json_string(item->subject_id.c_str()));
        json_error_t parse_error;
        json_t* details = json_loads(
            item->details_json.c_str(), JSON_REJECT_DUPLICATES, &parse_error);
        json_object_set_new(
            value, "details", details == nullptr ? json_object() : details);
        json_array_append_new(audit_items, value);
    }
    json_object_set_new(root, "audit_events", audit_items);

    json_t* retention = json_object();
    json_object_set_new(retention, "server_workspace_after_deletion",
        json_string("deleted"));
    json_object_set_new(retention, "retained_security_record",
        json_string("anonymous deletion receipt without account identifiers"));
    json_object_set_new(retention, "downloaded_artifacts",
        json_string("cannot be revoked; remain synthetic and governed by their bundled terms"));
    json_object_set_new(root, "deletion_and_retention", retention);
    encoded = json_dump_line(root);
    json_decref(root);
    return true;
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

bool read_file_to_string(
    const std::string& path,
    std::string& content,
    std::string& error
) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        error = "file is not readable";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    content = buffer.str();
    if (!input.good() && !input.eof()) {
        error = "file could not be read";
        return false;
    }
    return true;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string parent_directory(const std::string& path) {
    if (path.empty()) return std::string();
    const std::string::size_type separator = path.find_last_of('/');
    if (separator == std::string::npos) return std::string();
    if (separator == 0) return "/";
    return path.substr(0, separator);
}

std::string verifier_download_root(const std::string& pack_root) {
    const std::string parent = parent_directory(pack_root);
    return parent.empty() ? std::string() : parent + "/downloads/verifier";
}

bool is_regular_file(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool safe_relative_path(const std::string& path) {
    if (path.empty() || path[0] == '/' ||
        path.find('\\') != std::string::npos ||
        path.find("//") != std::string::npos) {
        return false;
    }
    std::string::size_type start = 0;
    while (start <= path.size()) {
        const std::string::size_type end = path.find('/', start);
        const std::string part = path.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start
        );
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool safe_download_filename(const std::string& filename) {
    if (filename.empty() || filename.size() > 128) return false;
    for (std::string::const_iterator it = filename.begin();
         it != filename.end(); ++it) {
        const unsigned char ch = static_cast<unsigned char>(*it);
        if (!std::isalnum(ch) && ch != '.' && ch != '_' && ch != '-') {
            return false;
        }
    }
    return filename == "metadata.json" ||
           filename == "synsigra-verifier.zip" ||
           filename == "synsigra-wheel.whl" ||
           (starts_with(filename, "synsigra-verifier-") &&
            ends_with(filename, ".zip")) ||
           (starts_with(filename, "synsigra-") &&
            ends_with(filename, "-py3-none-any.whl"));
}

syn_sig_ra::RouteResponse immutable_artifact_response(
    const std::string& method,
    const syn_sig_ra::PreparedArtifact& artifact,
    const std::string& content_type,
    const std::string& download_filename,
    const std::string& range_header,
    const std::string& expires_at
) {
    syn_sig_ra::ByteRange range;
    const syn_sig_ra::ByteRangeStatus range_status = method == "GET"
        ? syn_sig_ra::parse_byte_range(
            range_header, artifact.size_bytes, range)
        : syn_sig_ra::ByteRangeStatus::none;
    if (range_status == syn_sig_ra::ByteRangeStatus::invalid) {
        syn_sig_ra::RouteResponse response = json_response(
            416,
            "{\"error\":{\"code\":\"range_not_satisfiable\","
            "\"message\":\"The requested artifact byte range is invalid or unavailable.\"}}\n"
        );
        response.accept_ranges = true;
        response.content_range =
            "bytes */" + std::to_string(artifact.size_bytes);
        response.etag = "\"sha256-" + artifact.sha256 + "\"";
        response.checksum_sha256 = artifact.sha256;
        response.artifact_expires_at = expires_at;
        return response;
    }
    syn_sig_ra::RouteResponse response;
    response.disposition = syn_sig_ra::RouteDisposition::handled;
    response.status = range_status == syn_sig_ra::ByteRangeStatus::valid
        ? 206 : 200;
    response.content_type = content_type;
    response.file_path = artifact.path;
    response.file_size = artifact.size_bytes;
    response.file_offset = range_status == syn_sig_ra::ByteRangeStatus::valid
        ? range.offset : 0;
    response.file_length = range_status == syn_sig_ra::ByteRangeStatus::valid
        ? range.length : artifact.size_bytes;
    response.content_disposition =
        "attachment; filename=\"" + download_filename + "\"";
    response.cache_control = "private, no-store";
    response.accept_ranges = true;
    response.headers_only = method == "HEAD";
    response.etag = "\"sha256-" + artifact.sha256 + "\"";
    response.checksum_sha256 = artifact.sha256;
    response.artifact_expires_at = expires_at;
    if (range_status == syn_sig_ra::ByteRangeStatus::valid) {
        response.content_range =
            "bytes " + std::to_string(range.offset) + "-" +
            std::to_string(range.offset + range.length - 1) + "/" +
            std::to_string(artifact.size_bytes);
    }
    return response;
}

syn_sig_ra::RouteResponse download_file_response(
    const std::string& root,
    const std::string& filename
) {
    if (root.empty() || !safe_download_filename(filename)) {
        return json_response(
            400,
            "{\"error\":{\"code\":\"invalid_download_path\","
            "\"message\":\"The download path is invalid.\"}}\n"
        );
    }
    const std::string path = root + "/" + filename;
    if (!is_regular_file(path)) {
        return json_response(
            404,
            "{\"error\":{\"code\":\"download_not_available\","
            "\"message\":\"The requested verifier download is not available.\"}}\n"
        );
    }
    syn_sig_ra::RouteResponse response;
    response.disposition = syn_sig_ra::RouteDisposition::handled;
    response.status = 200;
    response.file_path = path;
    if (filename == "metadata.json") {
        response.content_type = "application/json";
        response.cache_control = "no-store";
    } else {
        response.content_type = ends_with(filename, ".zip")
            ? "application/zip"
            : "application/octet-stream";
        response.content_disposition =
            "attachment; filename=\"" + filename + "\"";
        response.cache_control = "no-store";
    }
    return response;
}

json_t* scenario_json_messages(
    const std::vector<signal_synth::ecg_scenario_json_message>& messages
) {
    json_t* array = json_array();
    for (std::vector<signal_synth::ecg_scenario_json_message>::const_iterator it =
             messages.begin();
         it != messages.end(); ++it) {
        json_t* item = json_object();
        json_object_set_new(
            item,
            "code",
            json_string(signal_synth::ecg_scenario_json_message_code_name(it->code))
        );
        json_object_set_new(item, "path", json_string(it->path.c_str()));
        json_object_set_new(item, "message", json_string(it->message.c_str()));
        json_array_append_new(array, item);
    }
    return array;
}

bool string_array_from_json(
    json_t* array,
    std::vector<std::string>& values
) {
    if (!json_is_array(array) || json_array_size(array) == 0) return false;
    std::set<std::string> seen;
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(array, index, item) {
        if (!json_is_string(item)) return false;
        const std::string value = json_string_value(item);
        if (value.empty() || !seen.insert(value).second) return false;
        values.push_back(value);
    }
    return true;
}

syn_sig_ra::RouteResponse authoring_preview_response(json_t* submitted) {
    json_t* scenario = submitted == nullptr
        ? nullptr : json_object_get(submitted, "scenario");
    json_t* targets = submitted == nullptr
        ? nullptr : json_object_get(submitted, "targets");
    if (!json_is_object(submitted) || json_object_size(submitted) != 2 ||
        !json_is_object(scenario)) {
        return json_response(
            400,
            "{\"error\":{\"code\":\"invalid_preview_request\","
            "\"message\":\"scenario object and targets array are required.\"}}\n"
        );
    }
    std::vector<std::string> target_values;
    if (!string_array_from_json(targets, target_values)) {
        return json_response(
            400,
            "{\"error\":{\"code\":\"invalid_preview_targets\","
            "\"message\":\"targets must contain unique target names.\"}}\n"
        );
    }
    char* scenario_text = json_dumps(scenario, JSON_COMPACT | JSON_SORT_KEYS);
    if (scenario_text == nullptr) {
        return json_response(
            400,
            "{\"error\":{\"code\":\"invalid_preview_scenario\","
            "\"message\":\"scenario could not be encoded.\"}}\n"
        );
    }
    signal_synth::ecg_scenario_document document;
    signal_synth::ecg_scenario_json_result validation;
    const bool parsed = signal_synth::parse_ecg_scenario_json(
        scenario_text,
        document,
        validation
    );
    free(scenario_text);
    if (!parsed) {
        json_t* root = json_object();
        json_t* error = json_object();
        json_object_set_new(error, "code", json_string("scenario_invalid"));
        json_object_set_new(
            error,
            "message",
            json_string("Fix scenario fields before previewing package output.")
        );
        json_object_set_new(root, "error", error);
        json_object_set_new(
            root,
            "validation_errors",
            scenario_json_messages(validation.messages)
        );
        const std::string body = json_dump_line(root);
        json_decref(root);
        return json_response(422, body);
    }

    signal_synth::ecg_pack_manifest pack;
    pack.pack_id = "preview_pack";
    pack.name = "Scenario preview";
    pack.version = "preview";
    pack.description = "SaaS authoring preview.";
    pack.targets = target_values;
    signal_synth::ecg_pack_scenario item;
    item.id = document.scenario_id.empty() ? "preview_case" : document.scenario_id;
    item.path = item.id + ".json";
    item.targets = target_values;
    pack.scenarios.push_back(item);
    std::vector<signal_synth::ecg_scenario_document> scenarios;
    scenarios.push_back(document);
    signal_synth::scenario_pack_analysis analysis;
    signal_synth::analyze_scenario_pack(pack, scenarios, analysis);
    return json_response(
        200,
        signal_synth::scenario_pack_analysis_json(analysis) + "\n"
    );
}

bool read_curated_scenario(
    const std::string& pack_root,
    const std::string& pack_id,
    const std::string& case_id,
    std::string& scenario_json,
    std::string& error
) {
    if (pack_root.empty() || !syn_sig_ra::is_valid_pack_id(pack_id) ||
        !syn_sig_ra::is_valid_pack_id(case_id)) {
        error = "invalid curated scenario ID";
        return false;
    }
    const std::string pack_path = pack_root + "/" + pack_id + ".json";
    json_error_t parse_error;
    json_t* pack = json_load_file(
        pack_path.c_str(),
        JSON_REJECT_DUPLICATES,
        &parse_error
    );
    if (!json_is_object(pack)) {
        if (pack != nullptr) json_decref(pack);
        error = "curated pack JSON is unavailable";
        return false;
    }
    json_t* scenarios = json_object_get(pack, "scenarios");
    std::string relative_path;
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(scenarios, index, item) {
        json_t* id = json_object_get(item, "id");
        json_t* path = json_object_get(item, "path");
        if (json_is_string(id) && json_is_string(path) &&
            case_id == json_string_value(id)) {
            relative_path = json_string_value(path);
            break;
        }
    }
    json_decref(pack);
    if (relative_path.empty() || !safe_relative_path(relative_path)) {
        error = "curated scenario path is unavailable";
        return false;
    }
    return read_file_to_string(
        pack_root + "/" + relative_path,
        scenario_json,
        error
    );
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
  <title>Synsigra · Algorithm QA</title>
  <link rel="stylesheet" href="/syn_sig_ra/ui/style.css">
</head>
<body>
  <header class="product-bar">
    <a class="app-brand" href="/" data-no-spa>
      <span class="app-mark" aria-hidden="true">S</span>
      <span><strong>Synsigra</strong><small>Algorithm QA</small></span>
    </a>
    <nav class="primary-nav" aria-label="Primary navigation">
      <a href="/syn_sig_ra/workspace" data-nav-page="workspace">Workspace</a>
      <a href="/syn_sig_ra/packs" data-nav-page="packs">Packs</a>
      <a href="/syn_sig_ra/generate" data-nav-page="generate">Generate</a>
      <a href="/syn_sig_ra/jobs" data-nav-page="jobs">Jobs</a>
      <a href="/syn_sig_ra/viewer" data-no-spa>Lab</a>
      <a href="/syn_sig_ra/verify" data-nav-page="verify">Verify</a>
    </nav>
    <a id="header-account-link" class="profile-link" href="/syn_sig_ra/account" data-nav-page="account">
      <span id="header-account-avatar" class="profile-avatar" aria-hidden="true">?</span>
      <span id="header-account-label">Sign in</span>
    </a>
  </header>
  <main class="shell">
    <header class="app-header">
      <div>
        <p class="eyebrow">Developer workspace</p>
        <h1 id="page-heading">Algorithm QA workspace</h1>
        <p id="page-description" class="lede">Choose a synthetic biosignal challenge, generate a deterministic package, verify your algorithm locally, and archive the evidence.</p>
      </div>
      <div class="status-card">
        <div class="label"><span class="status-dot" aria-hidden="true"></span> Service status</div>
        <div id="health-status" class="status">checking…</div>
        <div id="readiness-status" class="muted">checking components…</div>
      </div>
    </header>

    <div id="app-toast" class="app-toast" role="status" aria-live="polite" hidden></div>

    <div class="app-layout">
      <aside class="side-nav" aria-label="Workspace navigation">
        <div class="side-nav-title">Workflow</div>
        <a href="/syn_sig_ra/workspace" data-nav-page="workspace">Start</a>
        <a href="/syn_sig_ra/packs" data-nav-page="packs">Choose pack</a>
        <a href="/syn_sig_ra/generate" data-nav-page="generate">Generate job</a>
        <a href="/syn_sig_ra/jobs" data-nav-page="jobs">Jobs</a>
        <a href="/syn_sig_ra/viewer" data-no-spa>View signals</a>
        <a href="/syn_sig_ra/verify" data-nav-page="verify">Verify locally</a>
        <div class="side-nav-title section-title">Build custom tests</div>
        <a href="/syn_sig_ra/scenarios" data-nav-page="scenarios">Scenario editor</a>
        <a href="/syn_sig_ra/custom-packs" data-nav-page="custom-packs">Custom packs</a>
        <div class="side-nav-title section-title">Settings</div>
        <a href="/syn_sig_ra/account" data-nav-page="account">Account</a>
        <a href="/syn_sig_ra/advanced" data-nav-page="advanced">Developer / API</a>
      </aside>

      <div class="content-stack">
        <section id="workspace" class="page active" data-page="workspace">
          <section id="overview" class="hero">
            <div>
              <h2>What do you want to do next?</h2>
              <p class="muted">For most algorithm development, start with a curated pack. Use scenario editing only when you need a custom edge case.</p>
            </div>
            <div id="workspace-next-action" class="selected-pack">Loading your next action…</div>
          </section>
          <div class="step-cards">
            <a class="step-card primary-step" href="/syn_sig_ra/packs">
              <span class="step-number">1</span>
              <strong>Use a curated challenge</strong>
              <span>Pick by algorithm target, difficulty, scoring mode, and recommended use.</span>
            </a>
            <a class="step-card" href="/syn_sig_ra/scenarios">
              <span class="step-number">2</span>
              <strong>Edit or clone a scenario</strong>
              <span>Start from a template or curated case, then validate before composing a pack.</span>
            </a>
            <a class="step-card" href="/syn_sig_ra/custom-packs">
              <span class="step-number">3</span>
              <strong>Compose a custom pack</strong>
              <span>Snapshot valid scenario drafts into an immutable challenge pack.</span>
            </a>
            <a class="step-card" href="/syn_sig_ra/verify">
              <span class="step-number">4</span>
              <strong>Verify an existing package</strong>
              <span>Open a completed job runbook, download the verifier, and copy exact commands.</span>
            </a>
          </div>
          <aside class="workflow" aria-label="Evidence workflow">
            <strong>Evidence path</strong>
            <span>Choose pack</span>
            <span>Generate job</span>
            <span>Download verification kit</span>
            <span>Run algorithm locally</span>
            <span>Run verifier</span>
            <span>Archive package + provenance</span>
          </aside>
          <section class="panel capability-overview" aria-labelledby="capability-overview-heading">
            <div class="panel-heading">
              <div>
                <p class="eyebrow">Authoring breadth</p>
                <h2 id="capability-overview-heading">A test space, not a single waveform</h2>
                <p class="muted compact">A starter pack is one validated slice. Build deterministic tests across timing, physiology, modulation, artifacts, targets, and output contracts.</p>
              </div>
              <a class="button-link secondary" href="/syn_sig_ra/scenarios">Open scenario editor</a>
            </div>
            <div class="capability-stats" aria-label="Current Synsigra authoring contract">
              <div><strong>142</strong><span>authoring fields</span></div>
              <div><strong>71</strong><span>catalog entries</span></div>
              <div><strong>20</strong><span>artifact families</span></div>
              <div><strong>16</strong><span>verification targets</span></div>
            </div>
            <div class="cards capability-cards">
              <article class="card"><h3>Time, sampling, repeatability</h3><p class="muted">0.01 seconds to 24 hours, 100 Hz to 1 MHz, standard presets, deterministic 64-bit seeds, and controlled randomization.</p></article>
              <article class="card"><h3>ECG rhythm and morphology</h3><p class="muted">Rate and RR variation, ectopy, rhythm/conduction patterns, episodes, pacing, and parameterized 12-lead morphology where supported.</p></article>
              <article class="card"><h3>HRV and physiology modulation</h3><p class="muted">SDNN and LF/HF structure, respiratory frequency/amplitude, RR bounds, and activity-linked modulation.</p></article>
              <article class="card"><h3>PPG timing and shape</h3><p class="muted">Pulse delay, rise/decay, dicrotic shape, perfusion, weak/missing pulses, jitter, amplitude modulation, and PTT variation.</p></article>
              <article class="card"><h3>Noise and device faults</h3><p class="muted">Baseline, powerline and EMG noise; dropout, saturation, motion, drift, clipping, quantization, lead/electrode and sensor faults.</p></article>
              <article class="card"><h3>Ground truth and evidence</h3><p class="muted">Point, interval, delineation, HRV, QTc, morphology, optical, variability, respiratory and burden targets with manifests, fingerprints, provenance, reports, and local scoring.</p></article>
            </div>
            <p class="muted capability-footnote">The condition catalog explicitly distinguishes native, parameterized, and catalog-only support. Unsupported combinations are rejected.</p>
          </section>
        </section>

    <section id="account" class="panel page" data-page="account">
      <div id="reset-password-panel" class="verify-note" hidden>
        <h2>Choose a new password</h2>
        <p class="muted">This reset link is single-use and expires after 30 minutes.</p>
        <label for="reset-password">New password (12+ characters)</label>
        <input id="reset-password" type="password" minlength="12" maxlength="128" autocomplete="new-password">
        <button id="complete-password-reset" class="primary">Change password and sign in</button>
      </div>
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
            <button id="request-password-reset" class="secondary">Email me a reset link</button>
          </div>
          <div>
            <h3>Create account</h3>
            <label for="register-name">Display name</label>
            <input id="register-name" type="text" maxlength="100" autocomplete="name">
            <label for="register-email">Email</label>
            <input id="register-email" type="email" autocomplete="email">
            <label for="register-password">Password (12+ characters)</label>
            <input id="register-password" type="password" minlength="12" maxlength="128" autocomplete="new-password">
            <label class="terms-consent" for="register-terms">
              <input id="register-terms" type="checkbox">
              <span>I accept the <a href="/syn_sig_ra/legal/terms" target="_blank" rel="noopener">Private Beta Terms</a> and <a href="/syn_sig_ra/legal/privacy" target="_blank" rel="noopener">Privacy &amp; No-PHI Notice</a> (version <code>private-beta-2026-07-17-r4</code>).</span>
            </label>
            <button id="register" class="primary" disabled>Create account</button>
          </div>
        </div>
        <div id="verification-pending" class="verify-note" hidden>
          <strong>Verification email sent</strong>
          <p class="muted">Open the link in your inbox. It expires after 24 hours.</p>
          <button id="resend-verification" class="secondary">Resend verification email</button>
        </div>
      </div>
      <div id="signed-in-account" hidden>
        <div class="panel-heading">
          <div>
            <h2 id="account-name">Account</h2>
            <p id="account-email" class="muted"></p>
            <p id="account-verification" class="status ok"></p>
          </div>
          <button id="logout" class="secondary">Sign out</button>
        </div>
        <div class="auth-grid account-lifecycle-grid">
          <section class="card">
            <h3>Profile</h3>
            <p class="muted compact">This name appears in the product navigation and account records.</p>
            <label for="profile-display-name">Display name</label>
            <input id="profile-display-name" type="text" maxlength="100" autocomplete="name">
            <button id="save-profile" class="secondary">Save profile</button>
          </section>
          <section class="card">
            <h3>Change password</h3>
            <p class="muted compact">Changing it signs out every other browser session.</p>
            <label for="current-password">Current password</label>
            <input id="current-password" type="password" maxlength="128" autocomplete="current-password">
            <label for="new-password">New password (12+ characters)</label>
            <input id="new-password" type="password" minlength="12" maxlength="128" autocomplete="new-password">
            <button id="change-password" class="secondary">Change password</button>
          </section>
        </div>
        <section class="card account-data-card">
          <div class="panel-heading">
            <div>
              <h3>Your data</h3>
              <p class="muted compact">Download your profile, projects, jobs, scenarios, custom packs, API-key metadata, legal acceptances, and audit history. Passwords, hashes, session tokens, and API-key secrets are never exported.</p>
            </div>
            <button id="export-account" class="secondary">Download account export</button>
          </div>
        </section>
        <h3>Personal API keys</h3>
        <p class="muted">Create keys for scripts or CI. A new secret is shown once.</p>
        <div class="row">
          <input id="api-key-label" type="text" maxlength="100" placeholder="CI or workstation">
          <button id="create-api-key" class="secondary">Create API key</button>
        </div>
        <pre id="api-key-secret" class="output"></pre>
        <div id="api-keys" class="jobs"></div>
        <section id="security-audit-panel">
          <div class="panel-heading">
            <div>
              <h3>Security &amp; audit</h3>
              <p class="muted compact">Review organization security, job, and artifact actions. Export up to 1,000 events per CSV page.</p>
            </div>
            <div class="row">
              <button id="refresh-audit" class="secondary">Refresh</button>
              <a class="button-link" href="/syn_sig_ra/v1/audit-events?format=csv&amp;limit=1000" download="synsigra-audit-events.csv" data-no-spa>Download CSV</a>
            </div>
          </div>
          <div id="audit-events" class="jobs"></div>
        </section>
        <details class="card danger-zone">
          <summary>Delete account and workspace</summary>
          <p>This permanently removes the account, projects, jobs, server-cached packages, scenarios, custom packs, sessions, and API keys.</p>
          <p class="muted">An anonymous deletion receipt is retained without your account identifiers. Files you already downloaded cannot be revoked and remain governed by their bundled Synsigra terms.</p>
          <label for="delete-account-password">Current password</label>
          <input id="delete-account-password" type="password" maxlength="128" autocomplete="current-password">
          <label for="delete-account-confirmation">Type <code>DELETE MY ACCOUNT</code></label>
          <input id="delete-account-confirmation" type="text" autocomplete="off">
          <button id="delete-account" class="danger" disabled>Permanently delete account</button>
        </details>
      </div>
      <p id="auth-output" class="muted" aria-live="polite"></p>
    </section>

    <section id="pack-chooser-page" class="page" data-page="packs">
      <div class="panel">
        <div class="panel-heading">
          <div>
            <h2>Choose a challenge pack</h2>
            <p class="muted compact">Start from the algorithm output or measurement. You do not need to know pack IDs.</p>
          </div>
          <button id="refresh-packs" class="secondary">Refresh</button>
        </div>
        <aside class="verify-note pack-scope-note">
          <strong>A pack is a validated slice, not the platform ceiling.</strong>
          <p class="muted compact">Start with a curated pack for a fast, scoreable baseline. For different duration, sampling, physiology, frequency/amplitude modulation, noise, artifacts, or target combinations, clone a case in the <a href="/syn_sig_ra/scenarios">scenario editor</a> and compose a custom pack.</p>
        </aside>
        <div class="wizard-progress compact" aria-label="Pack selection workflow">
          <span class="active"><strong>1</strong> Algorithm output</span><span><strong>2</strong> Test intent</span><span><strong>3</strong> Recommendation</span><span><strong>4</strong> Generate</span>
        </div>
        <section class="wizard-step">
          <span class="eyebrow">Step 1</span>
          <h3>What does your algorithm output?</h3>
          <p class="muted compact">Choose one primary output. Synsigra prioritizes packs with local automated scoring over reference-only coverage.</p>
          <div id="pack-target-goals" class="target-selector"></div>
        </section>
        <section class="wizard-step">
          <span class="eyebrow">Step 2</span>
          <h3>How do you want to test it?</h3>
          <div id="pack-intent-goals" class="target-selector"></div>
        </section>
        <details id="advanced-pack-filters" class="meta">
          <summary>Advanced pack filters</summary>
          <p class="muted compact">Optional technical constraints. Leave these open-ended unless you already know the scoring contract or difficulty you need.</p>
          <div class="filter-row">
            <label>Scoring mode<select id="pack-scoring-filter"></select></label>
            <label>Difficulty<select id="pack-difficulty-filter"></select></label>
          </div>
          <button id="reset-pack-filters" class="secondary">Reset all choices</button>
        </details>
        <div id="pack-recommendation" class="selected-pack">Loading recommendation…</div>
        <h3 id="alternative-packs-heading">Other matching packs</h3>
        <div id="packs" class="cards"></div>
      </div>
    </section>

    <section id="generate" class="page" data-page="generate">
      <div class="panel">
        <div class="panel-heading">
          <div>
            <h2>Generate challenge package</h2>
            <p class="muted compact">Confirm project and pack, then create a job. The next step is the Jobs page.</p>
          </div>
          <a class="button-link secondary" href="/syn_sig_ra/packs">Change pack</a>
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
        <p class="muted compact">Generation produces the complete challenge export set, including provenance and engineering claim-boundary artifacts. Format options are not configurable per job.</p>
        <button id="create-job" class="primary" disabled>Create challenge job</button>
        <pre id="create-output" class="output"></pre>
      </div>
    </section>

    <section id="jobs-section" class="panel page" data-page="jobs">
      <div class="panel-heading">
        <div>
          <h2>Jobs</h2>
          <p class="muted compact">Watch progress, then open the verification runbook for completed jobs.</p>
        </div>
        <div class="actions no-margin">
          <span id="jobs-sync-status" class="muted" aria-live="polite"></span>
          <button id="refresh-jobs" class="secondary">Refresh</button>
        </div>
      </div>
      <p class="muted">The list polls in place. Download a completed ZIP, run your algorithm locally, then score its outputs with the verification helper.</p>
      <div id="jobs" class="jobs"></div>
      <button id="load-more-jobs" class="secondary" hidden>Load older jobs</button>
    </section>
    <section id="verification-runbook-panel" class="panel page" data-page="verify">
      <div class="panel-heading">
        <div>
          <h2>Verification runbook</h2>
          <p class="muted compact">A completed-job checklist with exact downloads, filenames, commands, and archive guidance.</p>
        </div>
        <label class="inline-label" for="runbook-job-select">
          Completed job
          <select id="runbook-job-select"></select>
        </label>
      </div>
      <div id="verification-runbook" class="jobs">Sign in and complete a job to open a runbook.</div>
    </section>
    <section id="verifier-downloads" class="panel page" data-page="verify">
      <div class="panel-heading">
        <div>
          <h2>Local verifier downloads</h2>
          <p class="muted compact">Install <code>synsigra-verify</code> without cloning the generator repository.</p>
        </div>
        <button id="refresh-verifier-downloads" class="secondary">Refresh</button>
      </div>
      <p class="verify-note"><strong>Boundary:</strong> these downloads contain only the Python verifier package and helper scripts. They do not include the C++ generator or generator source.</p>
      <div id="verifier-downloads-content" class="jobs">Sign in to download the verifier.</div>
    </section>
    <section id="usage-section" class="panel page" data-page="account">
      <div class="panel-heading">
        <h2>Usage</h2>
        <button id="refresh-usage" class="secondary">Refresh</button>
      </div>
      <div id="usage" class="muted">Sign in to inspect usage.</div>
    </section>
    <section id="metrics-panel" class="panel page" data-page="account" hidden>
      <div class="panel-heading">
        <h2>Operational metrics</h2>
        <button id="refresh-metrics" class="secondary">Refresh</button>
      </div>
      <div id="metrics" class="muted"></div>
    </section>
    <section id="scenario-workbench" class="panel page" data-page="scenarios">
      <div class="panel-heading">
        <div>
          <h2>Scenario drafts</h2>
          <p class="muted compact">Template-assisted authoring remains form-first, with raw JSON available for advanced edits.</p>
        </div>
        <div class="actions no-margin">
          <button id="refresh-authoring" class="secondary">Refresh authoring</button>
          <button id="new-scenario" class="secondary">New draft</button>
        </div>
      </div>
      <div class="wizard-progress" aria-label="Scenario workflow">
        <span class="active"><strong>1</strong> Goal</span><span><strong>2</strong> Starting point</span><span><strong>3</strong> Tune &amp; validate</span><span><strong>4</strong> Compose pack</span>
      </div>
      <p class="muted">First choose what your algorithm outputs. Synsigra then narrows the starting points and form controls without removing any advanced capability.</p>
      <p class="verify-note"><strong>No PHI:</strong> use synthetic engineering scenarios only. Do not enter patient identifiers, clinical notes, personal data, or diagnostic claims.</p>
      <section class="wizard-step">
        <span class="eyebrow">Step 1</span>
        <h3>What should your algorithm detect or measure?</h3>
        <p class="muted compact">Choose every output you want this scenario to exercise. This intent is saved with the draft and inherited by its custom pack.</p>
        <div id="scenario-targets" class="target-selector"></div>
        <div id="scenario-target-guidance" class="selected-pack muted">Choose at least one target to see compatible starting points.</div>
      </section>
      <section id="scenario-source-step" class="wizard-step" hidden>
        <span class="eyebrow">Step 2</span>
        <h3>Choose a starting point</h3>
        <p class="muted compact">A template is a general-purpose recipe. A curated case is a proven example copied from a released challenge pack. Either becomes your independent editable draft.</p>
        <label class="advanced-toggle"><input id="show-all-scenario-sources" type="checkbox"> Show all core starting points, including ones that need adaptation</label>
        <div class="authoring-grid">
        <div>
          <h4>Guided template</h4>
          <p class="muted compact">Best for building a new scenario with form controls.</p>
          <label for="scenario-template-select">Compatible templates</label>
          <select id="scenario-template-select"></select>
          <button id="apply-scenario-template" class="secondary">Apply template</button>
        </div>
        <div>
          <h4>Proven curated case</h4>
          <p class="muted compact">Best for adapting a case already used in a released pack.</p>
          <label for="curated-clone-pack">Released pack</label>
          <select id="curated-clone-pack"></select>
          <label for="curated-clone-case">Compatible case</label>
          <select id="curated-clone-case"></select>
          <button id="clone-curated-scenario" class="secondary">Clone into draft</button>
        </div>
        </div>
      </section>
      <section class="wizard-step">
        <span class="eyebrow">Step 3</span>
        <h3>Tune and validate</h3>
        <div id="scenario-form" class="scenario-groups"></div>
        <div id="scenario-preview" class="selected-pack muted">Choose a target and starting point to preview package output.</div>
      </section>
      <label for="scenario-name">Name</label>
      <input id="scenario-name" type="text" maxlength="100" placeholder="Scenario name">
      <details id="scenario-json-details" class="meta">
        <summary>Advanced JSON editor</summary>
        <p class="muted compact">Use raw JSON for fields that are not yet represented by form controls. Form edits round-trip through this document.</p>
        <label for="scenario-json">Scenario JSON</label>
        <textarea id="scenario-json" rows="16" spellcheck="false">{}</textarea>
      </details>
      <div class="actions">
        <button id="load-scenario-template" class="secondary">Load clean ECG example</button>
        <button id="format-scenario-json" class="secondary">Format JSON</button>
        <button id="make-scenario-compatible" class="secondary" hidden>Apply missing target requirements</button>
        <button id="save-scenario" class="primary" disabled>Save and continue to pack</button>
      </div>
      <pre id="scenario-output" class="output"></pre>
      <div id="scenarios" class="jobs"></div>
    </section>
    <section id="custom-pack-workbench" class="panel page" data-page="custom-packs">
      <div class="panel-heading">
        <div>
          <h2>Custom pack composer</h2>
          <p class="muted compact">Select valid scenario drafts, review targets, then snapshot them into an immutable pack.</p>
        </div>
        <button id="refresh-custom-packs" class="secondary">Refresh</button>
      </div>
      <div class="wizard-progress compact" aria-label="Custom pack workflow">
        <span><strong>1</strong> Goal</span><span><strong>2</strong> Scenario</span><span class="active"><strong>3</strong> Pack</span><span><strong>4</strong> Generate</span>
      </div>
      <p class="verify-note"><strong>No PHI:</strong> pack names and descriptions must not contain patient data, personal identifiers, clinical notes, or diagnostic-use claims.</p>
      <h3>1. Choose scenario drafts</h3>
      <label for="custom-pack-scenario-search">Search scenarios</label>
      <input id="custom-pack-scenario-search" type="search" placeholder="Filter by draft name, scenario ID, or tag">
      <p class="muted compact">Select at least one valid draft. The resulting pack snapshots its scenarios; later draft edits do not alter it.</p>
      <div id="pack-scenario-options" class="cards"></div>
      <h3>2. Inherited verification goal</h3>
      <div id="inherited-pack-targets" class="selected-pack muted">Select a scenario to inherit its verification targets.</div>
      <details id="custom-pack-target-override" class="meta">
        <summary>Advanced target override</summary>
        <p class="muted compact">Normally the pack inherits the intent saved with its scenarios. Override only when you deliberately need a different combined contract.</p>
        <div id="custom-pack-targets" class="target-selector"></div>
      </details>
      <h3>3. Review and create</h3>
      <label for="custom-pack-name">Pack name</label>
      <input id="custom-pack-name" type="text" maxlength="100" placeholder="My validation pack">
      <label for="custom-pack-description">Description</label>
      <input id="custom-pack-description" type="text" placeholder="What this pack tests">
      <div id="custom-pack-review" class="selected-pack">Select scenarios and targets to preview coverage.</div>
      <button id="create-custom-pack" class="primary" disabled>Create immutable custom pack</button>
      <pre id="custom-pack-output" class="output"></pre>
      <div id="custom-packs" class="jobs"></div>
    </section>
    <section id="documentation" class="panel page" data-page="advanced">
      <h2>Documentation</h2>
      <p class="muted">Expert/API reference, raw contracts, troubleshooting, and manual workflows.</p>
      <p><a href="/syn_sig_ra/docs/quickstart">One-page quickstart</a></p>
      <p><a href="/syn_sig_ra/docs/api">Rendered API reference</a></p>
      <p><a href="/syn_sig_ra/docs/troubleshooting">Troubleshooting guide</a></p>
      <p><a href="/syn_sig_ra/legal/terms">Private Beta Terms</a></p>
      <p><a href="/syn_sig_ra/legal/privacy">Privacy &amp; No-PHI Notice</a></p>
      <p><a href="/syn_sig_ra/legal/support">Support, availability &amp; billing</a></p>
      <p><a href="mailto:synsigra@gmail.com?subject=Synsigra%20technical%20demo">Request a technical demo</a></p>
      <p><a href="https://github.com/tamask1s/signal_synth_saas/blob/master/README.md" target="_blank" rel="noopener">Full user manual</a></p>
      <p><a href="/syn_sig_ra/openapi.yaml" data-no-spa>Live OpenAPI YAML</a></p>
    </section>
      </div>
    </div>
    <footer class="legal-footer">
      <span>Synsigra private beta · operated by Kis Tamás · <a href="mailto:synsigra@gmail.com">synsigra@gmail.com</a></span>
      <nav aria-label="Legal and support">
        <a href="/syn_sig_ra/legal/terms">Terms</a>
        <a href="/syn_sig_ra/legal/privacy">Privacy &amp; No-PHI</a>
        <a href="/syn_sig_ra/legal/support">Support &amp; service</a>
      </nav>
    </footer>
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
  <title>Synsigra quickstart</title>
  <link rel="stylesheet" href="/syn_sig_ra/ui/style.css">
</head>
<body>
  <main class="shell">
    <section class="panel">
      <p class="eyebrow">Synsigra docs</p>
      <h1>One-page quickstart</h1>
      <p class="verify-note"><strong>Synthetic engineering data only.</strong> Do not enter patient data, personal identifiers, clinical notes, PHI, or diagnostic-use claims.</p>
      <ol>
        <li>Open <a href="/syn_sig_ra/workspace">the guided workspace</a>. To create an account, read and accept the current <a href="/syn_sig_ra/legal/terms">Private Beta Terms</a> and <a href="/syn_sig_ra/legal/privacy">Privacy &amp; No-PHI Notice</a>.</li>
        <li>Open <a href="/syn_sig_ra/packs">Choose pack</a>. Filter by target, workflow intent, scoring mode, and difficulty; use the recommended pack or comparison table.</li>
        <li>Open <a href="/syn_sig_ra/generate">Generate job</a>. Use the default project or create a project if your role allows it, then create the challenge job.</li>
        <li>Open <a href="/syn_sig_ra/jobs">Jobs</a> and wait for <code>succeeded</code>.</li>
        <li>Optionally open <a href="/syn_sig_ra/viewer">Synsigra Lab</a> from the completed job to inspect selected signal channels. The viewer transfers only the visible time range, not the complete waveform.</li>
        <li>Open the completed job's <a href="/syn_sig_ra/verify">verification runbook</a> and download the single <code>verification-kit.zip</code>.</li>
        <li>Unzip the kit, run your algorithm locally against <code>verification-kit/challenge/</code>, then replace the example identity and output rows under <code>verification-kit/submission/</code>. Keep declared paths, targets and formats unchanged.</li>
        <li>Download the verifier bundle or wheel from the runbook page and install it locally without cloning the generator repository.</li>
        <li>Copy the exact <code>synsigra-verify</code> command from the runbook. The extracted kit directory itself is the challenge package.</li>
        <li>Inspect <code>verification_summary.json</code>, <code>verification_summary.csv</code>, and <code>verification_report.html</code>.</li>
      </ol>
      <pre class="output">python -m pip install synsigra-wheel.whl
unzip "job_123-verification-kit.zip" -d synsigra-kit
cd synsigra-kit/verification-kit
synsigra-verify challenge submission verification-results --force</pre>
      <p>For a challenge with protocol v2, this runs the complete immutable evidence matrix and embedded acceptance policy. Do not add profile, case, or target overrides. A kit without protocol v2 shows an explicit <code>--mode diagnostic</code> command and can never produce evidence.</p>
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
  <title>Synsigra API reference</title>
  <link rel="stylesheet" href="/syn_sig_ra/ui/style.css">
</head>
<body>
  <main class="shell">
    <section class="panel">
      <p class="eyebrow">Synsigra docs</p>
      <h1>Rendered API reference</h1>
      <p>Base URL: <code>https://www.timeonion.com/syn_sig_ra</code>. Browser calls use the secure session cookie. Scripts and CI use <code>Authorization: Bearer &lt;api-key&gt;</code>.</p>
      <p class="verify-note"><strong>No PHI:</strong> API requests, project names, labels, scenario drafts, and custom-pack text must contain synthetic engineering data only.</p>
      <table>
        <thead><tr><th>Method</th><th>Path</th><th>Purpose</th><th>Auth</th></tr></thead>
        <tbody>
          <tr><td>GET</td><td><code>/healthz</code></td><td>Liveness/build</td><td>Public</td></tr>
          <tr><td>GET</td><td><code>/readyz</code></td><td>Readiness and disk</td><td>Public</td></tr>
          <tr><td>GET</td><td><code>/v1/legal</code></td><td>Current terms version, public notices, retention and billing status</td><td>Public</td></tr>
          <tr><td>GET</td><td><code>/v1/packs</code></td><td>Rich curated pack catalog</td><td>Public</td></tr>
          <tr><td>GET</td><td><code>/v1/packs/{pack_id}</code></td><td>Pack detail including scoreable/reference-only targets</td><td>Public</td></tr>
          <tr><td>POST</td><td><code>/v1/auth/register</code></td><td>Accept current terms, create account and send verification email</td><td>Public</td></tr>
          <tr><td>POST</td><td><code>/v1/auth/verify-email</code></td><td>Verify email and start browser session</td><td>Public</td></tr>
          <tr><td>POST</td><td><code>/v1/auth/resend-verification</code></td><td>Request another verification email</td><td>Public</td></tr>
          <tr><td>POST</td><td><code>/v1/auth/password-reset/request</code></td><td>Request password-reset email</td><td>Public</td></tr>
          <tr><td>POST</td><td><code>/v1/auth/password-reset/complete</code></td><td>Set new password and start session</td><td>Public</td></tr>
          <tr><td>POST</td><td><code>/v1/auth/login</code></td><td>Start browser session after verification</td><td>Public</td></tr>
          <tr><td>GET</td><td><code>/v1/auth/me</code></td><td>Current account</td><td>Session</td></tr>
          <tr><td>POST</td><td><code>/v1/auth/logout</code></td><td>End browser session</td><td>Session</td></tr>
          <tr><td>PATCH/DELETE</td><td><code>/v1/account</code></td><td>Update profile or permanently delete the owned workspace with re-authentication</td><td>Authenticated</td></tr>
          <tr><td>POST</td><td><code>/v1/account/password</code></td><td>Change password and invalidate prior browser sessions</td><td>Authenticated</td></tr>
          <tr><td>GET</td><td><code>/v1/account/export</code></td><td>Download account, workspace, legal, and audit data without secrets</td><td>Authenticated</td></tr>
          <tr><td>GET/POST</td><td><code>/v1/projects</code></td><td>List/create projects</td><td>Authenticated</td></tr>
          <tr><td>GET/POST/DELETE</td><td><code>/v1/api-keys</code></td><td>Manage personal API keys</td><td>Authenticated</td></tr>
          <tr><td>POST</td><td><code>/v1/api-keys/{api_key_id}/rotate</code></td><td>Atomically replace a key and return the replacement secret once</td><td>Authenticated</td></tr>
          <tr><td>GET</td><td><code>/v1/audit-events</code></td><td>Organization-scoped JSON/CSV audit export</td><td>Owner/admin</td></tr>
          <tr><td>GET</td><td><code>/v1/downloads/verifier</code></td><td>Verifier download metadata</td><td>Authenticated</td></tr>
          <tr><td>GET</td><td><code>/v1/downloads/verifier/{filename}</code></td><td>Download generator-free verifier bundle or wheel</td><td>Authenticated</td></tr>
          <tr><td>GET</td><td><code>/v1/authoring/schema</code></td><td>Core scenario-authoring schema metadata</td><td>Authenticated</td></tr>
          <tr><td>GET</td><td><code>/v1/authoring/templates</code></td><td>Core scenario templates</td><td>Authenticated</td></tr>
          <tr><td>POST</td><td><code>/v1/authoring/preview</code></td><td>Preview scenario package analysis</td><td>Authenticated</td></tr>
          <tr><td>GET</td><td><code>/v1/authoring/curated-scenarios/{pack_id}/{case_id}</code></td><td>Clone curated scenario JSON into a draft</td><td>Authenticated</td></tr>
          <tr><td>GET/POST/PUT/DELETE</td><td><code>/v1/scenarios</code></td><td>Scenario draft lifecycle</td><td>Authenticated</td></tr>
          <tr><td>GET/POST/DELETE</td><td><code>/v1/custom-packs</code></td><td>Compose/list/hide custom packs</td><td>Authenticated</td></tr>
          <tr><td>GET/POST</td><td><code>/v1/jobs</code></td><td>List/create jobs</td><td>Authenticated</td></tr>
          <tr><td>GET/DELETE</td><td><code>/v1/jobs/{job_id}</code></td><td>Read or soft-delete job</td><td>Organization</td></tr>
          <tr><td>POST</td><td><code>/v1/jobs/{job_id}/cancel</code></td><td>Cancel queued job</td><td>Developer+</td></tr>
          <tr><td>POST</td><td><code>/v1/jobs/{job_id}/retry</code></td><td>Retry failed/cancelled job</td><td>Developer+</td></tr>
          <tr><td>GET</td><td><code>/v1/jobs/{job_id}/viewer</code></td><td>Describe viewable signal cases and channels</td><td>Organization</td></tr>
          <tr><td>GET</td><td><code>/v1/jobs/{job_id}/viewer/window</code></td><td>Read selected channels for one bounded binary viewport</td><td>Organization</td></tr>
          <tr><td>GET</td><td><code>/v1/jobs/{job_id}/viewer/overlays</code></td><td>Read bounded ground-truth events and intervals for one viewport</td><td>Organization</td></tr>
          <tr><td>GET/HEAD</td><td><code>/v1/jobs/{job_id}/verification-kit.zip</code></td><td>Stream or resume the role-selected challenge and submission-template kit</td><td>Organization</td></tr>
          <tr><td>GET/HEAD</td><td><code>/v1/artifacts/{package_id}/manifest.json</code></td><td>Download immutable manifest with checksum metadata</td><td>Organization</td></tr>
          <tr><td>GET/HEAD</td><td><code>/v1/artifacts/{package_id}/package.zip</code></td><td>Stream or resume package ZIP with Range and checksum metadata</td><td>Organization</td></tr>
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
      <p>Raw machine-readable contract: <a href="/syn_sig_ra/openapi.yaml">live OpenAPI YAML</a>.</p>
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
  <title>Synsigra troubleshooting</title>
  <link rel="stylesheet" href="/syn_sig_ra/ui/style.css">
</head>
<body>
  <main class="shell">
    <section class="panel">
      <p class="eyebrow">Synsigra docs</p>
      <h1>Troubleshooting</h1>
      <table>
        <thead><tr><th>Symptom / code</th><th>Action</th></tr></thead>
        <tbody>
          <tr><td><code>401 unauthorized</code></td><td>Sign in again in the browser, or create/use a valid personal API key for scripts.</td></tr>
          <tr><td><code>403 forbidden</code></td><td>Your role cannot perform the operation. Ask an owner/admin or use a developer/owner account.</td></tr>
          <tr><td><code>404 *_not_found</code></td><td>Check the ID. Cross-organization resources intentionally return 404.</td></tr>
          <tr><td><code>409 job_*_invalid_state</code></td><td>Cancel only queued jobs; retry only failed/cancelled jobs; delete only non-running jobs.</td></tr>
          <tr><td><code>409 pack_generator_incompatible</code></td><td>Select a current curated pack. Deprecated/incompatible packs cannot be generated.</td></tr>
          <tr><td><code>409 job_kit_unavailable</code></td><td>Wait until the job succeeds. The role-selected submission template is delivered inside the verification kit.</td></tr>
          <tr><td><code>429 request_rate_limit</code></td><td>Slow the client down. The private-beta limit is 120 requests/minute per key.</td></tr>
          <tr><td><code>429 concurrent_job_limit</code></td><td>Wait for queued/running jobs to finish. Current limit is 2 active jobs per organization.</td></tr>
          <tr><td><code>429 monthly_job_limit</code></td><td>Monthly job quota is exhausted. Use existing packages or contact the operator.</td></tr>
          <tr><td>Failed job</td><td>Open the job card and read the error. If it is a generator/catalog issue, keep the job ID and report it.</td></tr>
          <tr><td>Expired artifact</td><td>Click <strong>Download verification kit</strong> normally. The UI prepares the exact version in the background, verifies the package fingerprint, and starts the download. Historical jobs without preserved inputs fail explicitly.</td></tr>
          <tr><td>Verifier exit <code>1</code></td><td>Check challenge integrity, declared submission paths/formats, required fields, units, protocol matrix, policy checks, and per-case report.</td></tr>
          <tr><td>Verifier exit <code>2</code></td><td>Fix CLI arguments. Start from the command in the completed-job recipe panel.</td></tr>
        </tbody>
      </table>
      <p class="verify-note"><strong>Boundary:</strong> Synsigra is synthetic engineering QA tooling, not clinical validation, diagnosis, patient monitoring, or PHI storage.</p>
      <p><a href="/syn_sig_ra/docs/quickstart">Quickstart</a> · <a href="/syn_sig_ra/docs/api">Rendered API reference</a> · <a href="/syn_sig_ra/">Back to app</a></p>
    </section>
  </main>
</body>
</html>
)HTML";

const char kTermsHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Synsigra Private Beta Terms</title>
  <link rel="stylesheet" href="/syn_sig_ra/ui/style.css">
</head>
<body>
  <main class="shell legal-document">
    <section class="panel">
      <p class="eyebrow">Private beta · version private-beta-2026-07-17-r4</p>
      <h1>Synsigra Private Beta Terms</h1>
      <p class="lede">Effective 17 July 2026. These terms define a synthetic engineering-QA evaluation service, not a medical or clinical product.</p>
      <h2>Operator</h2>
      <p>Synsigra is operated by <strong>Kis Tamás</strong>, 2040 Budaörs, Tátra u. 6, Hungary. Private contact: <a href="mailto:synsigra@gmail.com">synsigra@gmail.com</a>.</p>
      <h2>Permitted use</h2>
      <p>You may use downloaded synthetic packages and the generator-free verifier inside your organization for algorithm development, regression testing, benchmarking, reproducibility and engineering evaluation.</p>
      <h2>Not medical or clinical use</h2>
      <p>Synsigra is not intended for diagnosis, prevention, monitoring, prediction, prognosis or treatment of disease; clinical decisions; patient monitoring; clinical validation or certification; or medical-device conformity assessment. Synthetic results are engineering evidence only.</p>
      <h2>Synthetic data and no-PHI rule</h2>
      <p>Except for your own account email and display name, do not submit PHI, patient identifiers, medical records, real patient waveforms or annotations, clinical notes, another person's personal data, algorithm source code or confidential algorithm output. Algorithm code and output are intended to remain local.</p>
      <h2>Accounts and acceptable use</h2>
      <p>Protect passwords and API keys. Do not bypass limits, probe another organization, interfere with the service, distribute malware, infringe third-party rights or use the beta unlawfully. Access may be limited or suspended to protect the service and its intended-use boundary.</p>
      <h2>Package use permission</h2>
      <p>During the beta, the account holder receives a non-exclusive, non-transferable, revocable permission to use, reproduce and archive packages and reports internally for the permitted purposes. Do not sell, sublicense or publish packages as a standalone dataset, use them to identify or model a real person, or represent them as clinical evidence. Keep manifests, fingerprints, provenance and claim-boundary notices with archived evidence.</p>
      <h2>Availability, support and billing</h2>
      <p>The beta is best-effort and has no uptime or response-time SLA. Generated artifacts are normally cached for 7 days; keep local copies. Newer jobs preserve an immutable recipe and exact generator release for a verified rebuild after cache expiry. The current beta is free, collects no payment method and never converts automatically to a paid plan. Any future paid plan requires notice, pricing and explicit opt-in.</p>
      <h2>As-is beta</h2>
      <p>To the extent permitted by law, the service and generated materials are provided as-is and as-available without warranties of uninterrupted availability, fitness for a particular purpose or suitability for regulated or clinical use. Nothing excludes liability that cannot lawfully be excluded.</p>
      <h2>Support</h2>
      <p><a href="mailto:synsigra@gmail.com">Email private support</a>. Include only a safe job/package ID, UTC timestamp, browser version, and exact error code. Never send credentials, account-action links, PHI, personal data, real patient data, generated signals, proprietary algorithm output, or source code.</p>
      <p><a href="/syn_sig_ra/legal/privacy">Privacy &amp; No-PHI Notice</a> · <a href="/syn_sig_ra/legal/support">Support &amp; service expectations</a> · <a href="/syn_sig_ra/account">Back to account</a></p>
    </section>
  </main>
</body>
</html>
)HTML";

const char kPrivacyHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Synsigra Privacy and No-PHI Notice</title>
  <link rel="stylesheet" href="/syn_sig_ra/ui/style.css">
</head>
<body>
  <main class="shell legal-document">
    <section class="panel">
      <p class="eyebrow">Private beta · effective 17 July 2026</p>
      <h1>Privacy and No-PHI Notice</h1>
      <p class="lede">Synsigra minimizes account and operational data and is designed for synthetic engineering inputs only.</p>
      <h2>Controller and private contact</h2>
      <p>Data controller: <strong>Kis Tamás</strong>, 2040 Budaörs, Tátra u. 6, Hungary. For access, correction, deletion, or privacy requests email <a href="mailto:synsigra@gmail.com">synsigra@gmail.com</a>.</p>
      <h2>What is processed</h2>
      <p>Account email and display name; salted password hashes; session and API-key hashes; organization, project, job, usage and quota metadata; synthetic scenario drafts and custom-pack descriptions; generated packages; and limited method/route/status/duration and worker-event logs.</p>
      <h2>Why</h2>
      <p>To secure accounts, deliver account email, generate and retain requested packages, enforce quotas, diagnose failures, protect the service and provide beta support. Personal data is not sold and there is no advertising or patient-level automated decision-making.</p>
      <h2>No-PHI rule</h2>
      <p>Do not submit PHI, real patient data, patient identifiers, medical records, clinical notes, real-person waveforms or annotations, or another person's personal data. The beta is not offered as a HIPAA business-associate service and no BAA is provided.</p>
      <h2>Retention and infrastructure</h2>
      <p>Generated artifacts are normally cached for 7 days. Reproducibility metadata, immutable pack recipes, and exact generator releases may remain after expiry so Synsigra can prepare a fingerprint-verified rebuild when the user downloads again. Account and security records are retained while needed to operate and protect the beta. Data is processed on the service VPS and by required hosting, DNS and Gmail SMTP providers. Essential secure session cookies are used; advertising and cross-site tracking cookies are not.</p>
      <h2>Requests and support</h2>
      <p>Eligible jobs, drafts, custom packs and API keys can be deleted in the product. Email account access, correction, or deletion requests privately to <a href="mailto:synsigra@gmail.com">synsigra@gmail.com</a>. Include only the minimum account identifier needed for identity verification; never send a password, API key, action link, PHI, patient data, generated signal file, or proprietary algorithm output.</p>
      <p><a href="/syn_sig_ra/legal/terms">Private Beta Terms</a> · <a href="/syn_sig_ra/legal/support">Support &amp; service expectations</a> · <a href="/syn_sig_ra/account">Back to account</a></p>
    </section>
  </main>
</body>
</html>
)HTML";

const char kSupportHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Synsigra Private Beta Support</title>
  <link rel="stylesheet" href="/syn_sig_ra/ui/style.css">
</head>
<body>
  <main class="shell legal-document">
    <section class="panel">
      <p class="eyebrow">Private beta support</p>
      <h1>Support, availability and billing</h1>
      <h2>Get support</h2>
      <p><a class="button-link" href="mailto:synsigra@gmail.com?subject=Synsigra%20support">Email private support</a> <a href="mailto:synsigra@gmail.com?subject=Synsigra%20technical%20demo">Request a technical demo</a></p>
      <p>Include a safe job or package ID, UTC timestamp, browser version, and exact error code. Never include passwords, API keys, account-action links, PHI, personal data, real patient data, generated signals, proprietary algorithm output, or source code.</p>
      <p>Operator: Kis Tamás, 2040 Budaörs, Tátra u. 6, Hungary · <a href="mailto:synsigra@gmail.com">synsigra@gmail.com</a></p>
      <h2>Response and availability</h2>
      <p>Support is normally reviewed within three business days, as a target rather than an SLA. The service is best-effort with no guaranteed uptime, recovery time or response time. Keep local copies of packages and evidence you need.</p>
      <p><a href="/syn_sig_ra/healthz">Service health</a> · <a href="/syn_sig_ra/readyz">Component readiness</a></p>
      <h2>Billing</h2>
      <p>The current private beta is free. No payment method is collected and there is no automatic paid conversion. Future charges require new pricing, notice and explicit opt-in.</p>
      <p><a href="/syn_sig_ra/legal/terms">Private Beta Terms</a> · <a href="/syn_sig_ra/legal/privacy">Privacy &amp; No-PHI Notice</a> · <a href="/syn_sig_ra/">Back to Synsigra</a></p>
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
  max-width: 1320px;
  margin: 0 auto;
  padding: 32px 20px 64px;
}

.app-header {
  display: grid;
  grid-template-columns: 1fr minmax(220px, 280px);
  gap: 24px;
  align-items: stretch;
  margin-bottom: 24px;
}

.app-layout {
  display: grid;
  grid-template-columns: 230px minmax(0, 1fr);
  gap: 20px;
  align-items: start;
}

.content-stack {
  display: grid;
  gap: 20px;
}

.page { display: none; }
.page.active { display: block; }

.side-nav {
  position: sticky;
  top: 16px;
  display: grid;
  gap: 6px;
  padding: 14px;
  background: rgba(255, 255, 255, .94);
  border: 1px solid var(--border);
  border-radius: 18px;
  box-shadow: 0 8px 24px rgba(16, 24, 40, .06);
}

.side-nav-title {
  margin: 2px 4px 8px;
  color: var(--muted);
  font-size: 12px;
  font-weight: 800;
  letter-spacing: .08em;
  text-transform: uppercase;
}

.side-nav a {
  border-radius: 12px;
  color: var(--text);
  padding: 9px 10px;
  font-weight: 700;
  text-decoration: none;
}

.side-nav a:hover { background: #f2f4f7; }
.side-nav a.active { background: #eef4ff; color: var(--primary); }
.side-nav hr {
  width: 100%;
  border: 0;
  border-top: 1px solid var(--border);
}

.hero {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(280px, 360px);
  gap: 20px;
  align-items: stretch;
  margin-bottom: 20px;
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

.step-cards {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 14px;
  margin-bottom: 20px;
}

.step-card {
  display: grid;
  gap: 8px;
  min-height: 150px;
  padding: 18px;
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: 18px;
  color: var(--text);
  text-decoration: none;
  box-shadow: 0 8px 24px rgba(16, 24, 40, .06);
}

.step-card:hover {
  border-color: #b2c4ff;
  box-shadow: 0 12px 30px rgba(34, 88, 232, .12);
}

.step-card.primary-step {
  background: #eef4ff;
  border-color: #c7d7fe;
}

.step-number {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 32px;
  height: 32px;
  border-radius: 999px;
  background: var(--primary);
  color: #fff;
  font-weight: 800;
}

.capability-overview { margin-top: 20px; }
.capability-stats {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 10px;
  margin: 16px 0;
}
.capability-stats div {
  display: grid;
  gap: 2px;
  padding: 14px;
  border: 1px solid var(--border);
  border-radius: 14px;
  background: rgba(111, 124, 255, .08);
}
.capability-stats strong { font-size: 26px; color: var(--primary); }
.capability-stats span { color: var(--muted); font-size: 13px; }
.capability-cards .card { min-height: 142px; }
.capability-cards p { margin-bottom: 0; }
.capability-footnote { margin: 14px 0 0; font-size: 13px; }
.pack-scope-note { margin-bottom: 16px; }
.pack-scope-note a { color: var(--primary); font-weight: 700; }

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
.account-lifecycle-grid { margin: 18px 0; gap: 14px; }
.account-lifecycle-grid .card { display: flex; flex-direction: column; }
.account-lifecycle-grid button { align-self: flex-start; margin-top: 12px; }
.account-data-card { margin: 0 0 22px; }
.danger-zone { margin-top: 22px; border-color: rgba(255, 140, 145, .4); }
.danger-zone summary { cursor: pointer; color: var(--danger); font-weight: 800; }
.danger-zone[open] summary { margin-bottom: 14px; }
.danger-zone button { margin-top: 12px; }

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
  grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
  gap: 10px;
  margin-bottom: 12px;
}
.authoring-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 16px;
  margin-bottom: 12px;
}
.wizard-progress {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 8px;
  margin: 4px 0 18px;
}
.wizard-progress span {
  padding: 9px 11px;
  border: 1px solid var(--border);
  border-radius: 999px;
  color: var(--muted);
  font-size: 13px;
  text-align: center;
}
.wizard-progress span.active {
  border-color: rgba(111, 124, 255, .72);
  background: rgba(111, 124, 255, .16);
  color: var(--text);
}
.wizard-progress strong { margin-right: 5px; }
.wizard-progress.compact { margin-top: 0; }
.wizard-step {
  margin: 16px 0;
  padding: 16px;
  border: 1px solid var(--border);
  border-radius: 16px;
  background: rgba(7, 17, 31, .32);
}
.wizard-step h3 { margin: 4px 0 5px; }
.eyebrow {
  color: var(--accent);
  font-size: 12px;
  font-weight: 800;
  letter-spacing: .08em;
  text-transform: uppercase;
}
.advanced-toggle { color: var(--muted); font-weight: 500; }
.advanced-toggle input { width: auto; margin-right: 7px; }
.target-option.recommended { border-color: rgba(111, 124, 255, .62); }
.target-option.incompatible { opacity: .7; }
.form-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 12px;
  margin: 12px 0;
}
.form-field {
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 10px 12px;
  background: #fbfcff;
}
.form-field textarea {
  min-height: 88px;
}
.scenario-groups {
  display: grid;
  gap: 10px;
  margin: 12px 0;
}
.scenario-group {
  border: 1px solid var(--border);
  border-radius: 14px;
  background: #fff;
}
.scenario-group > summary {
  cursor: pointer;
  padding: 13px 15px;
  font-weight: 800;
}
.scenario-group > .form-grid {
  margin: 0;
  padding: 0 12px 12px;
}
.array-editor {
  grid-column: 1 / -1;
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 12px;
  background: #fbfcff;
}
.array-item {
  margin-top: 10px;
  padding: 12px;
  border: 1px solid #d0d5dd;
  border-radius: 12px;
  background: #fff;
}
.array-item-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
  gap: 8px 12px;
}
.array-item-grid label { margin-top: 4px; }
.array-item-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
}
.array-item-header button { padding: 7px 10px; }
.segmented {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 7px;
}
.segmented label {
  margin: 0;
  padding: 7px 10px;
  border: 1px solid var(--border);
  border-radius: 999px;
  background: #fff;
  font-size: 13px;
}
.segmented input { width: auto; margin-right: 4px; }
.slider-row {
  display: grid;
  grid-template-columns: 1fr auto;
  gap: 8px;
  align-items: center;
}
.slider-row output {
  min-width: 48px;
  color: var(--muted);
  text-align: right;
}
.tag-editor-list {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin: 8px 0;
}
.tag-editor-list button {
  padding: 4px 8px;
  border-color: #c7d7fe;
  background: #eef4ff;
  color: var(--primary);
  font-size: 12px;
}
.channel-picker {
  display: flex;
  flex-wrap: wrap;
  gap: 5px;
}
.channel-picker label {
  margin: 0;
  padding: 5px 7px;
  border: 1px solid var(--border);
  border-radius: 8px;
  font-size: 12px;
}
.channel-picker input { width: auto; margin-right: 4px; }
.target-selector {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(230px, 1fr));
  gap: 8px;
  margin: 10px 0 14px;
}
.target-option {
  display: grid;
  grid-template-columns: auto 1fr;
  gap: 8px;
  margin: 0;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 12px;
  background: #fff;
}
.target-option input { width: auto; margin-top: 3px; }
.target-option small { display: block; margin-top: 3px; color: var(--muted); font-weight: 400; }
.preflight-stats {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
  gap: 8px;
  margin: 10px 0;
}
.preflight-stat {
  padding: 9px;
  border: 1px solid var(--border);
  border-radius: 10px;
  background: #fff;
}
.preflight-stat strong { display: block; margin-top: 3px; }
.field-invalid {
  border-color: #fda29b;
  box-shadow: 0 0 0 2px #fee4e2;
}
.hint {
  display: block;
  margin-top: 4px;
  color: var(--muted);
  font-size: 12px;
}
.validation-link {
  border: 0;
  background: transparent;
  color: var(--danger);
  padding: 0;
  font-weight: 700;
  text-align: left;
  white-space: normal;
}
.validation-link:hover {
  background: transparent;
  text-decoration: underline;
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
.table-wrap {
  overflow-x: auto;
  margin: 12px 0;
}
.table-wrap table {
  min-width: 760px;
}
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
.job-header button.primary { width: auto; margin-top: 0; }
button.danger {
  background: #fff;
  border-color: #fda29b;
  color: var(--danger);
}
button.danger:hover { background: #fee4e2; }
button:disabled { opacity: .55; cursor: not-allowed; }

.button-link {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border: 1px solid var(--primary);
  border-radius: 12px;
  padding: 11px 12px;
  background: var(--primary);
  color: #fff;
  font-weight: 700;
  text-decoration: none;
  white-space: nowrap;
}

.button-link:hover { background: var(--primary-dark); }
.button-link.secondary {
  background: #fff;
  border-color: var(--border);
  color: var(--text);
}
.button-link.secondary:hover { background: #eef2ff; }

.inline-label {
  min-width: 240px;
  margin: 0;
}

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
.tag input[type="checkbox"] { width: auto; margin-right: 6px; }
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
.warning { color: var(--warn); }
.ok { color: var(--ok); }

@media (max-width: 820px) {
  .app-header, .app-layout, .hero, .grid, .auth-grid, .filter-row, .authoring-grid { grid-template-columns: 1fr; }
  .side-nav { position: static; }
  .row { align-items: stretch; flex-direction: column; }
  .meta-grid { grid-template-columns: 1fr; }
  .wizard-progress { grid-template-columns: repeat(2, minmax(0, 1fr)); }
}

/* Synsigra landing-aligned application shell */
:root {
  color-scheme: dark;
  --bg: #07111f;
  --panel: rgba(15, 29, 49, .82);
  --panel-solid: #0f1d31;
  --text: #e7edf8;
  --muted: #a7b3c9;
  --border: rgba(167, 179, 201, .18);
  --primary: #6f7cff;
  --primary-dark: #5967ed;
  --accent: #38d9ff;
  --danger: #ff8c91;
  --ok: #52e0a4;
  --warn: #ffc66d;
}
body {
  min-height: 100vh;
  background:
    radial-gradient(circle at 8% -10%, rgba(111, 124, 255, .26), transparent 34rem),
    radial-gradient(circle at 92% 4%, rgba(56, 217, 255, .13), transparent 30rem),
    var(--bg);
}
a { color: #9da7ff; }
.product-bar {
  position: sticky;
  top: 0;
  z-index: 50;
  display: grid;
  grid-template-columns: minmax(190px, 1fr) auto minmax(190px, 1fr);
  align-items: center;
  gap: 24px;
  min-height: 72px;
  padding: 10px max(20px, calc((100vw - 1280px) / 2));
  border-bottom: 1px solid var(--border);
  background: rgba(7, 17, 31, .82);
  backdrop-filter: blur(18px);
}
.app-brand, .profile-link, .primary-nav a {
  color: var(--text);
  text-decoration: none;
}
.app-brand {
  display: inline-flex;
  align-items: center;
  gap: 11px;
  width: max-content;
}
.app-brand > span:last-child { display: grid; line-height: 1.05; }
.app-brand strong { font-size: 18px; letter-spacing: -.02em; }
.app-brand small {
  margin-top: 4px;
  color: var(--muted);
  font-size: 11px;
  letter-spacing: .08em;
  text-transform: uppercase;
}
.app-mark {
  display: grid;
  place-items: center;
  width: 38px;
  height: 38px;
  border: 1px solid rgba(157, 167, 255, .5);
  border-radius: 13px;
  background: linear-gradient(135deg, #6f7cff, #9a5cff 58%, #38d9ff);
  box-shadow: 0 8px 24px rgba(111, 124, 255, .28);
  color: white;
  font-weight: 900;
}
.primary-nav { display: flex; gap: 5px; }
.primary-nav a, .profile-link {
  border: 1px solid transparent;
  border-radius: 999px;
  padding: 8px 12px;
  color: var(--muted);
  font-size: 13px;
  font-weight: 750;
}
.primary-nav a:hover, .primary-nav a.active, .profile-link:hover, .profile-link.active {
  border-color: var(--border);
  background: rgba(255, 255, 255, .06);
  color: var(--text);
}
.profile-link {
  display: inline-flex;
  justify-self: end;
  align-items: center;
  gap: 8px;
}
.profile-avatar {
  display: grid;
  place-items: center;
  width: 28px;
  height: 28px;
  border-radius: 999px;
  background: linear-gradient(135deg, rgba(111,124,255,.5), rgba(56,217,255,.28));
  color: white;
}
.shell { max-width: 1320px; padding-top: 30px; }
.app-header {
  grid-template-columns: minmax(0, 1fr) minmax(230px, 300px);
  padding: 28px;
  border: 1px solid var(--border);
  border-radius: 26px;
  background: linear-gradient(135deg, rgba(111,124,255,.12), rgba(15,29,49,.7) 52%, rgba(56,217,255,.07));
  box-shadow: 0 24px 60px rgba(0, 0, 0, .22);
}
.app-header h1 {
  max-width: 820px;
  font-size: clamp(30px, 4vw, 50px);
  letter-spacing: -.04em;
}
.lede { margin-bottom: 0; }
.status-card {
  align-self: center;
  padding: 15px 17px;
  border-radius: 17px;
  background: rgba(7, 17, 31, .55);
  box-shadow: none;
}
.status-card .status { margin-top: 4px; font-size: 15px; }
.status-card #readiness-status { font-size: 11px; line-height: 1.4; }
.status-dot {
  display: inline-block;
  width: 7px;
  height: 7px;
  margin-right: 5px;
  border-radius: 50%;
  background: var(--ok);
  box-shadow: 0 0 0 4px rgba(82, 224, 164, .12);
}
.side-nav, .panel, .step-card, .card, .job, .scenario-group {
  border-color: var(--border);
  background: var(--panel);
  box-shadow: 0 14px 40px rgba(0, 0, 0, .16);
  backdrop-filter: blur(12px);
}
.side-nav { top: 92px; border-radius: 20px; }
.side-nav-title.section-title {
  margin-top: 13px;
  padding-top: 13px;
  border-top: 1px solid var(--border);
}
.side-nav a:hover { background: rgba(255, 255, 255, .06); }
.side-nav a.active {
  background: linear-gradient(90deg, rgba(111,124,255,.22), rgba(56,217,255,.07));
  color: #c7ceff;
  box-shadow: inset 2px 0 0 var(--primary);
}
.hero { align-items: start; }
.step-card:hover, .job.focused {
  border-color: rgba(111, 124, 255, .72);
  box-shadow: 0 18px 45px rgba(37, 48, 132, .3);
  transform: translateY(-1px);
}
.step-card.primary-step {
  border-color: rgba(111, 124, 255, .4);
  background: linear-gradient(145deg, rgba(111,124,255,.18), rgba(15,29,49,.88));
}
.workflow, .selected-pack, .verify-note {
  border-color: var(--border);
  background: rgba(111, 124, 255, .09);
  color: var(--text);
}
.verify-note { background: rgba(82, 224, 164, .08); }
.form-field, .array-editor, .array-item, .segmented label, .target-option,
.preflight-stat, .scenario-group > .form-grid, input, select, textarea {
  border-color: var(--border);
  background: rgba(7, 17, 31, .72);
  color: var(--text);
}
input::placeholder, textarea::placeholder { color: #72809a; }
select { color-scheme: dark; }
button.secondary, .button-link.secondary, button.danger {
  border-color: var(--border);
  background: rgba(255, 255, 255, .045);
  color: var(--text);
}
button.secondary:hover, .button-link.secondary:hover { background: rgba(111,124,255,.14); }
button.danger { color: var(--danger); }
button.danger:hover { background: rgba(255, 140, 145, .1); }
button, .button-link {
  background: linear-gradient(135deg, var(--primary), #8b63ed);
  box-shadow: 0 8px 20px rgba(75, 83, 201, .2);
}
button:hover, .button-link:hover {
  background: linear-gradient(135deg, var(--primary-dark), #7751d8);
}
.tag { background: rgba(255,255,255,.07); color: var(--text); }
.tag.scoreable, .badge.succeeded { background: rgba(82,224,164,.14); color: var(--ok); }
.tag.reference, .badge.running { background: rgba(255,198,109,.13); color: var(--warn); }
.tag.mode, .badge { background: rgba(111,124,255,.15); color: #b5bdff; }
.badge.failed { background: rgba(255,140,145,.13); color: var(--danger); }
.output { border: 1px solid var(--border); background: rgba(2, 8, 18, .84); }
th, td { border-color: var(--border); }
.app-toast {
  position: fixed;
  top: 88px;
  right: 22px;
  z-index: 80;
  max-width: min(420px, calc(100vw - 44px));
  padding: 13px 16px;
  border: 1px solid rgba(82,224,164,.38);
  border-radius: 14px;
  background: rgba(12, 32, 37, .96);
  box-shadow: 0 18px 50px rgba(0,0,0,.35);
  color: var(--text);
  font-weight: 700;
}
.app-toast.notice {
  border-color: rgba(56,217,255,.35);
  background: rgba(10,31,47,.96);
}
.app-toast.error {
  border-color: rgba(255,140,145,.4);
  background: rgba(48,18,28,.96);
  color: #ffd3d5;
}
:focus-visible { outline: 3px solid rgba(56, 217, 255, .65); outline-offset: 3px; }
@media (max-width: 980px) {
  .product-bar { grid-template-columns: 1fr auto; gap: 8px 14px; }
  .primary-nav {
    grid-column: 1 / -1;
    grid-row: 2;
    justify-content: flex-start;
    overflow-x: auto;
    padding-bottom: 3px;
  }
}
@media (max-width: 820px) {
  .product-bar { position: static; }
  .shell { padding-top: 20px; }
  .app-header { padding: 21px; }
  .side-nav {
    display: flex;
    overflow-x: auto;
    gap: 4px;
    padding: 9px;
  }
  .side-nav-title { display: none; }
  .side-nav a { flex: 0 0 auto; white-space: nowrap; }
  .panel-heading { align-items: flex-start; flex-direction: column; }
  .inline-label { width: 100%; min-width: 0; }
  .capability-stats { grid-template-columns: repeat(2, minmax(0, 1fr)); }
}
@media (max-width: 520px) {
  .product-bar { padding-inline: 14px; }
  .profile-link { padding-inline: 7px; }
  #header-account-label { display: none; }
  .shell { padding-inline: 12px; }
  .app-header, .panel { border-radius: 18px; }
  .capability-stats { grid-template-columns: 1fr; }
}
@media (prefers-reduced-motion: reduce) {
  *, *::before, *::after {
    scroll-behavior: auto !important;
    transition: none !important;
  }
}
.terms-consent {
  display: grid;
  grid-template-columns: auto 1fr;
  gap: 9px;
  align-items: start;
  margin-top: 14px;
  padding: 12px;
  border: 1px solid var(--border);
  border-radius: 12px;
  background: rgba(111, 124, 255, .08);
  font-size: 13px;
  font-weight: 500;
}
.terms-consent input {
  width: auto;
  margin: 3px 0 0;
}
.legal-footer {
  display: flex;
  justify-content: space-between;
  gap: 16px;
  margin-top: 28px;
  padding: 18px 4px;
  border-top: 1px solid var(--border);
  color: var(--muted);
  font-size: 13px;
}
.legal-footer nav {
  display: flex;
  flex-wrap: wrap;
  gap: 8px 16px;
}
.legal-document {
  max-width: 900px;
}
.legal-document .panel {
  padding: clamp(22px, 5vw, 48px);
}
.legal-document h1 {
  font-size: clamp(34px, 6vw, 58px);
}
.legal-document h2 {
  margin-top: 30px;
}
@media (max-width: 620px) {
  .legal-footer { flex-direction: column; }
}
)CSS";

const char kUiJs[] = R"JS((() => {
  const base = "/syn_sig_ra";
  const termsVersion = "private-beta-2026-07-17-r4";
  const state = {
    currentPage: "workspace",
    authenticated: false,
    account: null,
    pendingVerificationEmail: "",
    apiKeys: [],
    auditEvents: [],
    role: "",
    packs: [],
    packTargetFilter: "",
    packIntentFilter: "smoke",
    packScoringFilter: "",
    packDifficultyFilter: "",
    runbookJobId: "",
    verifierDownloads: null,
    authoringSchema: null,
    authoringTemplates: [],
    scenarioTargets: [],
    showAllScenarioSources: false,
    scenarioPreviewTimer: null,
    scenarioPreview: null,
    projects: [],
    scenarios: [],
    customPacks: [],
    customPackAnalysis: {},
    customPackAnalysisToken: 0,
    customPackPreviewTimer: null,
    selectedScenarioId: "",
    jobs: [],
    jobsFingerprint: "",
    jobsLoaded: false,
    jobPollInFlight: false,
    jobsNextOffset: null,
    focusJobId: "",
    focusJobHandled: false,
    preparingKits: new Set(),
    toastTimer: null
  };

  const $ = (id) => document.getElementById(id);
  const cleanEcgTemplate = {
    schema_version: 2,
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
      rhythm_episodes: [],
      flutter_conduction_pattern: "fixed",
      pacing_mode: "ventricular",
      pacing_non_capture_every_n_beats: 0,
      fidelity_policy: "allow_parameterized",
      conditions: [{ code: "NORM", severity: 1 }]
    },
    ppg: {
      enabled: false,
      pulse_delay_ms: 180,
      rise_time_ms: 120,
      decay_time_ms: 300,
      amplitude_au: 1,
      baseline_au: 0,
      dicrotic_delay_ms: 180,
      dicrotic_width_ms: 80,
      dicrotic_amplitude_ratio: 0.15
    }
  };

  const pageDetails = {
    workspace: {
      title: "Algorithm QA workspace",
      description: "Choose a synthetic biosignal challenge, generate a deterministic package, verify your algorithm locally, and archive the evidence."
    },
    packs: {
      title: "Choose a challenge pack",
      description: "Start with your algorithm output or measurement, then compare scoreable targets, difficulty and intended workflow."
    },
    generate: {
      title: "Generate a challenge package",
      description: "Confirm the project and selected pack. Synsigra will open the new job as soon as it is accepted."
    },
    jobs: {
      title: "Generation jobs",
      description: "Follow package generation in place, download completed evidence, or continue into the verification runbook."
    },
    verify: {
      title: "Verify your algorithm locally",
      description: "Download the generator-free verifier and follow the exact runbook for a completed package."
    },
    scenarios: {
      title: "Scenario workbench",
      description: "Author synthetic edge cases with guided controls, validation and an advanced JSON escape hatch."
    },
    "custom-packs": {
      title: "Custom pack composer",
      description: "Combine validated scenario drafts into an immutable, reproducible challenge pack."
    },
    account: {
      title: "Account and developer access",
      description: "Manage your browser session, profile, usage and personal API keys."
    },
    advanced: {
      title: "Developer documentation",
      description: "Open the API contract, quickstart, troubleshooting and expert integration guidance."
    }
  };
  const pageTitles = Object.fromEntries(
    Object.entries(pageDetails).map(([page, details]) => [page, details.title])
  );

  function pageFromLocation() {
    const path = window.location.pathname.replace(/\/+$/, "");
    const suffix = path.startsWith(base) ? path.slice(base.length).replace(/^\/+/, "") : "";
    if (!suffix || suffix === "ui") return "workspace";
    return Object.prototype.hasOwnProperty.call(pageTitles, suffix)
      ? suffix
      : "workspace";
  }

  function queryParam(name) {
    return new URLSearchParams(window.location.search).get(name) || "";
  }

  function safeNextPage(value) {
    return Object.prototype.hasOwnProperty.call(pageTitles, value) && value !== "account"
      ? value
      : "";
  }

  function renderCurrentPage() {
    state.currentPage = pageFromLocation();
    const details = pageDetails[state.currentPage];
    document.querySelectorAll("[data-page]").forEach((node) => {
      node.classList.toggle("active", node.getAttribute("data-page") === state.currentPage);
    });
    document.querySelectorAll("[data-nav-page]").forEach((node) => {
      node.classList.toggle("active", node.getAttribute("data-nav-page") === state.currentPage);
    });
    $("page-heading").textContent = details.title;
    $("page-description").textContent = details.description;
    document.title = `Synsigra · ${details.title}`;
    const accountLink = $("header-account-link");
    const next = safeNextPage(state.currentPage);
    accountLink.href = `${base}/account${next ? "?next=" + encodeURIComponent(next) : ""}`;
    if (state.currentPage === "verify") {
      state.runbookJobId = queryParam("job_id") || state.runbookJobId;
      renderVerificationRunbook();
    }
    if (state.currentPage === "jobs") {
      const focusJobId = queryParam("job_id");
      if (focusJobId && focusJobId !== state.focusJobId) {
        state.focusJobId = focusJobId;
        state.focusJobHandled = false;
      }
      if (state.jobsLoaded) renderJobs();
    }
  }

  function navigateTo(page, params = {}, options = {}) {
    const destination = Object.prototype.hasOwnProperty.call(pageTitles, page)
      ? page
      : "workspace";
    const query = new URLSearchParams(params);
    const url = `${base}/${destination}${query.toString() ? "?" + query.toString() : ""}`;
    if (options.replace) {
      window.history.replaceState({}, "", url);
    } else {
      window.history.pushState({}, "", url);
    }
    renderCurrentPage();
    if (!options.preserveScroll) {
      window.scrollTo({
        top: 0,
        behavior: window.matchMedia("(prefers-reduced-motion: reduce)").matches ? "auto" : "smooth"
      });
    }
  }

  function showToast(message, kind = "success") {
    const node = $("app-toast");
    window.clearTimeout(state.toastTimer);
    node.textContent = message;
    node.className = `app-toast ${kind}`;
    node.hidden = false;
    state.toastTimer = window.setTimeout(() => {
      node.hidden = true;
    }, 5000);
  }

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
        const returnPage = safeNextPage(state.currentPage);
        state.account = null;
        state.authenticated = false;
        state.role = "";
        renderAuthState();
        if (state.currentPage !== "account") {
          navigateTo("account", returnPage ? { next: returnPage } : {});
        }
        setText("auth-output", "Your session expired. Sign in again.", "error");
        showToast("Your session expired. Sign in to continue.", "error");
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
    const accountLabel = state.authenticated && state.account
      ? state.account.display_name
      : "Sign in";
    $("header-account-label").textContent = accountLabel;
    $("header-account-avatar").textContent = state.authenticated && accountLabel
      ? accountLabel.trim().charAt(0).toUpperCase()
      : "?";
    const resetFlow = Boolean(queryParam("reset"));
    $("signed-out-account").hidden = state.authenticated || resetFlow;
    $("signed-in-account").hidden = !state.authenticated;
    $("reset-password-panel").hidden = state.authenticated || !resetFlow;
    $("verification-pending").hidden = !state.pendingVerificationEmail;
    if (state.authenticated && state.account) {
      $("account-name").textContent = state.account.display_name;
      $("account-email").textContent =
        `${state.account.email} · role: ${state.account.role}`;
      $("account-verification").textContent = state.account.email_verified
        ? "Email verified"
        : "Email verification required";
      $("account-verification").className = state.account.email_verified
        ? "status ok"
        : "status error";
      if (document.activeElement !== $("profile-display-name")) {
        $("profile-display-name").value = state.account.display_name;
      }
    }
    renderWorkspaceNextAction();
  }

  function renderWorkspaceNextAction() {
    const node = $("workspace-next-action");
    if (!node) return;
    if (!state.authenticated) {
      node.innerHTML = `
        <strong>Start by creating an account or signing in.</strong>
        <p class="muted compact">The browser UI uses a session cookie; API keys are only for scripts and CI.</p>
        <a class="button-link" href="/syn_sig_ra/account">Sign in / register</a>
      `;
      return;
    }
    const running = state.jobs.find((job) => ["queued", "running"].includes(job.status));
    if (running) {
      node.innerHTML = `
        <strong>Continue: job ${escapeHtml(running.status)}</strong>
        <p class="muted compact">${escapeHtml(running.pack_id)} · ${escapeHtml(running.job_id)}</p>
        <a class="button-link" href="/syn_sig_ra/jobs">Open jobs</a>
      `;
      return;
    }
    const succeeded = state.jobs.find((job) => job.status === "succeeded" && job.package_id);
    if (succeeded) {
      node.innerHTML = `
        <strong>Next: verify your latest completed package.</strong>
        <p class="muted compact">${escapeHtml(succeeded.pack_id)} · ${escapeHtml(succeeded.job_id)}</p>
        <a class="button-link" href="/syn_sig_ra/verify?job_id=${encodeURIComponent(succeeded.job_id)}">Open runbook</a>
      `;
      return;
    }
    node.innerHTML = `
      <strong>Recommended: choose a curated pack.</strong>
      <p class="muted compact">Pick by target and intent, then generate your first challenge job.</p>
      <a class="button-link" href="/syn_sig_ra/packs">Choose pack</a>
    `;
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

  function pathParts(path) {
    return String(path || "").replace(/^\$\./, "").split(".").filter(Boolean);
  }

  function getPathValue(document, path) {
    return pathParts(path).reduce((value, part) => (
      value && Object.prototype.hasOwnProperty.call(value, part) ? value[part] : undefined
    ), document);
  }

  function setPathValue(document, path, value) {
    const parts = pathParts(path);
    let cursor = document;
    parts.slice(0, -1).forEach((part) => {
      if (!cursor[part] || typeof cursor[part] !== "object" || Array.isArray(cursor[part])) {
        cursor[part] = {};
      }
      cursor = cursor[part];
    });
    cursor[parts[parts.length - 1]] = value;
  }

  function readScenarioJson(silent = false) {
    try {
      return JSON.parse($("scenario-json").value || "{}");
    } catch (error) {
      if (!silent) {
        $("scenario-json-details").open = true;
        $("scenario-output").textContent = `Invalid JSON: ${error.message}`;
      }
      return null;
    }
  }

  function writeScenarioJson(scenario) {
    $("scenario-json").value = JSON.stringify(scenario, null, 2);
  }

  function currentScenarioTargets() {
    const inputs = [...document.querySelectorAll("[data-scenario-target]")];
    const selected = inputs.filter((input) => input.checked)
      .map((input) => input.value);
    if (inputs.length) return selected;
    return [...state.scenarioTargets];
  }

  const targetCopy = {
    r_peak: ["R-peak detection", "ECG beat timing with local precision/recall scoring"],
    rr_interval: ["R–R intervals", "Observable beat-to-beat interval reconstruction with tolerance scoring"],
    ppg_systolic_peak: ["PPG systolic peaks", "Pulse-peak timing with local scoring"],
    ppg_pulse_onset: ["PPG pulse onset", "Pulse-foot timing with local scoring"],
    ecg_beat_classification: ["ECG beat classification", "Beat labels such as normal, PAC, PVC, or paced"],
    rhythm_episode: ["Rhythm episodes", "Episode interval detection and overlap scoring"],
    rhythm_burden: ["Rhythm burden", "Per-record rhythm-duration measurements and tolerance scoring"],
    ecg_delineation: ["ECG delineation", "Wave-onset, peak, and offset fiducials with local point-event scoring"],
    qtc: ["QTc verification", "QT/QTc measurement values across declared correction formulae"],
    hrv: ["Heart-rate variability (HRV)", "Frequency-, time-, and nonlinear HRV metric verification"],
    signal_quality: ["Signal quality / artifacts", "Noise and artifact intervals with local overlap scoring"],
    morphology_assertions: ["ECG morphology", "Beat-, lead-, axis-, and phenotype-level measurement scoring"],
    ecg_ppg_alignment: ["ECG–PPG alignment", "Cardiac-to-pulse timing measurements with local scoring"],
    ppg_optical: ["Optical PPG / SpO₂", "Optical-channel and oxygenation measurements under controlled stress"],
    prv: ["Pulse-rate variability (PRV)", "PPG-derived variability measurements compared with synthetic truth"],
    respiratory_rate: ["Respiratory rate", "Cardiorespiratory rate measurements from synthetic multimodal signals"]
  };

  const packIntentCopy = {
    smoke: ["First smoke test", "Fast feedback with a compact baseline pack"],
    regression: ["Regression suite", "Repeatable coverage for changes between algorithm versions"],
    stress: ["Stress / robustness", "Noise, artifacts, harder physiology, and edge cases"],
    benchmark: ["Benchmark comparison", "Broader evidence for comparing algorithms or releases"],
    reference: ["Reference inspection", "Ground-truth-rich manual or contract QA"],
    any: ["Explore all test styles", "Do not narrow the catalog by workflow intent"]
  };

  function targetTitle(name) {
    return (targetCopy[name] || [name])[0];
  }

  function targetDescription(name) {
    return (targetCopy[name] || [name, name])[1];
  }

  function sourceMatchesTargets(sourceTargets, selectedTargets) {
    const available = new Set(sourceTargets || []);
    return selectedTargets.every((target) => available.has(target));
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
    const scoringSelect = $("pack-scoring-filter");
    const difficultySelect = $("pack-difficulty-filter");
    const selectedTarget = state.packTargetFilter;
    const selectedIntent = state.packIntentFilter;
    const selectedScoring = state.packScoringFilter;
    const selectedDifficulty = state.packDifficultyFilter;
    const targets = uniqueSorted(state.packs.flatMap((pack) => [
      ...targetNames(pack.scoreable_targets),
      ...targetNames(pack.reference_only_targets)
    ]));
    const scoringModes = uniqueSorted(state.packs.map((pack) => pack.scoring_mode || ""));
    const difficulties = uniqueSorted(state.packs.flatMap((pack) => pack.difficulty || []));
    $("pack-target-goals").innerHTML = ["", ...targets].map((target) => `
      <label class="target-option ${target === selectedTarget ? "recommended" : ""}">
        <input type="radio" name="pack-target-goal" value="${escapeHtml(target)}" ${target === selectedTarget ? "checked" : ""}>
        <span><strong>${escapeHtml(target ? targetTitle(target) : "Help me choose")}</strong><small>${escapeHtml(target ? targetDescription(target) : "Show the best general-purpose starting point")}</small></span>
      </label>
    `).join("");
    $("pack-intent-goals").innerHTML = Object.entries(packIntentCopy).map(([intent, copy]) => `
      <label class="target-option ${intent === selectedIntent ? "recommended" : ""}">
        <input type="radio" name="pack-intent-goal" value="${escapeHtml(intent)}" ${intent === selectedIntent ? "checked" : ""}>
        <span><strong>${escapeHtml(copy[0])}</strong><small>${escapeHtml(copy[1])}</small></span>
      </label>
    `).join("");
    scoringSelect.innerHTML = `<option value="">Any scoring mode</option>` +
      scoringModes.map((mode) => `<option value="${escapeHtml(mode)}">${escapeHtml(mode)}</option>`).join("");
    difficultySelect.innerHTML = `<option value="">All difficulties</option>` +
      difficulties.map((difficulty) => `<option value="${escapeHtml(difficulty)}">${escapeHtml(difficulty)}</option>`).join("");
    if (scoringModes.includes(selectedScoring)) scoringSelect.value = selectedScoring;
    if (difficulties.includes(selectedDifficulty)) difficultySelect.value = selectedDifficulty;
  }

  function packMatchesIntent(pack, intent) {
    if (!intent) return true;
    const text = [
      pack.scoring_mode || "",
      ...(pack.difficulty || []),
      ...(pack.recommended_for || []),
      ...(pack.not_recommended_for || []),
      pack.description || ""
    ].join(" ").toLowerCase();
    if (intent === "reference") return targetNames(pack.scoreable_targets).length === 0 || targetNames(pack.reference_only_targets).length > 0;
    if (intent === "smoke") return text.includes("smoke") || text.includes("first") || text.includes("quick") || (pack.scenario_count || 0) <= 4;
    if (intent === "regression") return text.includes("regression") || text.includes("baseline");
    if (intent === "stress") return text.includes("stress") || text.includes("robust") || text.includes("artifact") || text.includes("hard");
    if (intent === "benchmark") return text.includes("benchmark") || text.includes("comparison") || text.includes("suite");
    return true;
  }

  function packMatchesFilters(pack) {
    const target = state.packTargetFilter;
    const scoring = state.packScoringFilter;
    const difficulty = state.packDifficultyFilter;
    const targets = [
      ...targetNames(pack.scoreable_targets),
      ...targetNames(pack.reference_only_targets)
    ];
    if (target && !targets.includes(target)) return false;
    if (scoring && pack.scoring_mode !== scoring) return false;
    if (difficulty && !(pack.difficulty || []).includes(difficulty)) return false;
    if (!packMatchesIntent(pack, state.packIntentFilter)) return false;
    return true;
  }

  function recommendedPack(packs) {
    if (!packs.length) return null;
    const target = state.packTargetFilter;
    const intent = state.packIntentFilter;
    return [...packs].sort((a, b) => {
      const score = (pack) => {
        let value = 0;
        const scoreable = targetNames(pack.scoreable_targets);
        const reference = targetNames(pack.reference_only_targets);
        if (target && scoreable.includes(target)) value += 30;
        if (target && reference.includes(target)) value += 10;
        if (scoreable.length) value += 8;
        if ((pack.release_status || "") === "stable") value += 5;
        if ((pack.release_status || "") === "beta") value += 3;
        if (intent === "smoke" && (pack.scenario_count || 0) <= 4) value += 8;
        if (intent === "stress" && (pack.difficulty || []).some((item) => /stress|hard/i.test(item))) value += 8;
        if (intent === "reference" && reference.length) value += 8;
        value -= Math.max(0, (pack.scenario_count || 0) - 8);
        return value;
      };
      return score(b) - score(a);
    })[0];
  }

  function renderPackRecommendation(visible) {
    const node = $("pack-recommendation");
    const pack = recommendedPack(visible);
    if (!pack) {
      node.innerHTML = "<p class=\"muted compact\">No matching pack. Loosen the filters or switch to custom pack authoring.</p>";
      return;
    }
    const scoreable = targetNames(pack.scoreable_targets);
    const referenceOnly = targetNames(pack.reference_only_targets);
    const targetReason = state.packTargetFilter
      ? `${targetTitle(state.packTargetFilter)} is ${scoreable.includes(state.packTargetFilter) ? "locally scoreable" : "available as reference ground truth"}.`
      : "This is the strongest compact general-purpose match.";
    const intentReason = packIntentCopy[state.packIntentFilter]
      ? packIntentCopy[state.packIntentFilter][0]
      : "Flexible evaluation";
    node.innerHTML = `
      <div class="job-header">
        <div>
          <span class="eyebrow">Recommended for ${escapeHtml(intentReason)}</span>
          <h3>${escapeHtml(pack.display_name || pack.pack_id)}</h3>
          <p class="muted compact">${escapeHtml(pack.description || "")}</p>
          <p class="muted compact">${packFacts(pack)}</p>
        </div>
        <button class="primary" data-select-pack="${escapeHtml(pack.pack_id)}">Use this pack</button>
      </div>
      <p class="compact"><strong>Why this match:</strong> ${escapeHtml(targetReason)}</p>
      <p class="compact">${scoreable.length ? "Scoreable locally: " + escapeHtml(scoreable.map(targetTitle).join(", ")) : "Reference-only / no local scoring command."}</p>
      <p>${releaseBoundaryBadges(pack)}</p>
      ${referenceOnly.length ? `<p class="muted compact">Reference-only: ${escapeHtml(referenceOnly.map(targetTitle).join(", "))}</p>` : ""}
      <details><summary>Pack scope and recommended use</summary><p><strong>Use for:</strong> ${escapeHtml((pack.recommended_for || []).join("; ") || "n/a")}</p><p><strong>Avoid for:</strong> ${escapeHtml((pack.not_recommended_for || []).join("; ") || "n/a")}</p></details>
      ${protocolDetails(pack)}
    `;
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

  function protocolDetails(pack) {
    const envelope = pack.verification_protocol || {};
    const protocol = envelope.document;
    if (!envelope.available || !protocol) return "";
    const required = protocol.required_case_targets || [];
    const targets = [...new Set(required.flatMap((item) => item.targets || []))];
    const matrixSize = required.reduce((total, item) => total + (item.targets || []).length, 0);
    const policy = protocol.acceptance_profile || {};
    return `
      <details>
        <summary>Evidence protocol v2</summary>
        <p>${escapeHtml(protocol.context_of_use || "Engineering QA protocol")}</p>
        <p><strong>Protocol:</strong> ${escapeHtml(protocol.protocol_id || "n/a")} · <strong>embedded policy:</strong> ${escapeHtml(policy.profile_id || "n/a")}</p>
        <p><strong>Required matrix:</strong> ${escapeHtml(required.length)} cases / ${escapeHtml(matrixSize)} case-target checks · ${escapeHtml(targets.map(targetTitle).join(", ") || "n/a")}</p>
        <p class="muted compact">Evidence mode runs this complete immutable matrix and embedded numeric policy. Profile, case, and target overrides are diagnostic-only and cannot produce evidence.</p>
        <pre class="output">${escapeHtml(JSON.stringify(policy.targets || {}, null, 2))}</pre>
        <span class="fingerprint">${escapeHtml(envelope.sha256 || "")}</span>
      </details>
    `;
  }

  function releaseBoundaryBadges(pack) {
    const protocol = pack.verification_protocol || {};
    const external = pack.external_noise || {};
    return [
      protocol.available ? '<span class="tag mode">evidence protocol v2</span>' : '<span class="tag warning">diagnostic only</span>',
      external.used ? '<span class="tag mode">approved external noise</span>' : ""
    ].join("");
  }

  function renderPacks() {
    const visible = state.packs.filter(packMatchesFilters);
    const recommendation = recommendedPack(visible);
    const alternatives = recommendation
      ? visible.filter((pack) => pack.pack_id !== recommendation.pack_id)
      : visible;
    renderPackRecommendation(visible);
    $("alternative-packs-heading").hidden = !alternatives.length;
    $("packs").innerHTML = alternatives.map((pack) => `
      <article class="card">
        <h3>${escapeHtml(pack.display_name || pack.pack_id)}</h3>
        <p class="muted">Version ${escapeHtml(pack.version || "")} · released ${escapeHtml(formatDate(pack.released_at))}</p>
        <p>
          <span class="badge ${escapeHtml(pack.release_status || "")}">${escapeHtml(pack.release_status || "unknown")}</span>
          <span class="tag mode">${escapeHtml(pack.scoring_mode || "unknown scoring")}</span>
          ${releaseBoundaryBadges(pack)}
        </p>
        <p class="muted">${escapeHtml(pack.description || "")}</p>
        <p class="muted">${packFacts(pack)}</p>
        <p><strong>Scoreable locally</strong>${targetTags(pack.scoreable_targets, "scoreable")}</p>
        <p><strong>Reference-only</strong>${targetTags(pack.reference_only_targets, "reference")}</p>
        <p class="muted">Verification: ${(pack.verification_protocol || {}).available ? "package-authoritative evidence" : "diagnostic only (non-evidence)"} · submission schemas: ${escapeHtml((pack.submission_output_schemas || []).join(", ") || "n/a")}</p>
        <p class="tag-list">${(pack.difficulty || []).map((item) => `<span class="tag">${escapeHtml(item)}</span>`).join("")}</p>
        <button class="primary" data-select-pack="${escapeHtml(pack.pack_id)}">Use this pack</button>
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
          <p class="muted">Core contract: ${escapeHtml(pack.integration_contract || "n/a")} · catalog ${escapeHtml(pack.catalog_version || "n/a")}</p>
          <p class="muted">Local verifier min: ${escapeHtml((pack.generator_compatibility || {}).local_verifier_min_version || "n/a")}</p>
          ${pack.deprecation_message ? `<p class="error">${escapeHtml(pack.deprecation_message)}</p>` : ""}
          <ul>
            ${(pack.changelog || []).map((entry) => `
              <li><strong>${escapeHtml(entry.version)}</strong> · ${escapeHtml(entry.date)} — ${escapeHtml(entry.summary)}</li>
            `).join("")}
          </ul>
        </details>
        ${protocolDetails(pack)}
        <span class="fingerprint">${escapeHtml(pack.pack_fingerprint || "")}</span>
      </article>
    `).join("") || (visible.length
      ? "<p class=\"muted\">The recommendation above is the only matching curated pack.</p>"
      : "<p class=\"muted\">No packs match these choices. Reset the filters or build a custom scenario.</p>");
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
      Verification: ${(pack.verification_protocol || {}).available ? "package-authoritative evidence protocol" : "diagnostic only (non-evidence)"}
    `;
  }

  function selectPackForGeneration(packId) {
    const select = $("pack-select");
    if ([...select.options].some((option) => option.value === packId)) {
      select.value = packId;
      renderSelectedPackSummary();
    }
    navigateTo("generate");
    $("create-output").textContent = "";
    $("create-job").focus();
  }

  async function loadPacks() {
    $("packs").textContent = "Loading packs…";
    try {
      const body = await api("/v1/packs");
      state.packs = body.packs || [];
      renderPackOptions();
      renderPackFilters();
      renderPacks();
      renderCuratedCloneOptions();
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
      $("metrics-panel").hidden = !["owner", "admin"].includes(body.role);
      state.projects.forEach((project) => {
        const option = document.createElement("option");
        option.value = project.project_id;
        option.textContent = project.display_name;
        select.appendChild(option);
      });
      renderCustomPackReview();
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

  async function loadVerifierDownloads() {
    const container = $("verifier-downloads-content");
    if (!state.authenticated) {
      state.verifierDownloads = null;
      container.innerHTML = "<p class=\"muted\">Sign in to download the local verifier.</p>";
      return;
    }
    container.textContent = "Loading verifier downloads…";
    try {
      state.verifierDownloads = await api("/v1/downloads/verifier");
      renderVerifierDownloads();
    } catch (error) {
      container.innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
    }
  }

  function renderVerifierDownloads() {
    const metadata = state.verifierDownloads;
    if (!metadata) return;
    $("verifier-downloads-content").innerHTML = `
      <article class="job">
        <div class="job-header">
          <div>
            <h3>${escapeHtml(metadata.package)} ${escapeHtml(metadata.version)}</h3>
            <p class="muted">Console script: <code>${escapeHtml(metadata.console_script)}</code> · generator included: ${metadata.generator_included ? "yes" : "no"}</p>
          </div>
          <span class="badge succeeded">verifier only</span>
        </div>
        <div class="actions">
          ${(metadata.files || []).map((file) => `
            <button class="secondary" data-download-verifier="${escapeHtml(file.filename)}">${escapeHtml(file.label)}</button>
          `).join("")}
        </div>
        <details class="meta">
          <summary>Install commands</summary>
          <pre class="output">${escapeHtml((metadata.install || []).join("\n"))}</pre>
          <p class="muted">${escapeHtml(metadata.verify_example || "")}</p>
        </details>
        <details class="meta">
          <summary>File fingerprints</summary>
          <dl class="meta-grid">
            ${(metadata.files || []).map((file) => `
              <dt>${escapeHtml(file.filename)}</dt>
              <dd><span class="fingerprint">${escapeHtml(file.sha256)}</span><span class="hint">${escapeHtml(file.description || "")}</span></dd>
            `).join("")}
          </dl>
        </details>
      </article>
    `;
    renderVerificationRunbook();
  }

  async function downloadVerifierFile(filename) {
    try {
      const response = await fetch(`${base}/v1/downloads/verifier/${encodeURIComponent(filename)}`, {
        credentials: "same-origin",
        headers: headers(false)
      });
      if (!response.ok) {
        const text = await response.text();
        throw new Error(text || response.statusText);
      }
      await saveResponseAsFile(response, filename);
    } catch (error) {
      showToast(error.message, "error");
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

  async function loadAuthoring() {
    if (!state.authenticated) {
      state.authoringSchema = null;
      state.authoringTemplates = [];
      $("scenario-template-select").innerHTML = "<option>Sign in first</option>";
      $("scenario-form").innerHTML = "";
      $("scenario-targets").innerHTML = "";
      $("scenario-preview").textContent = "Sign in to use template-assisted authoring.";
      return;
    }
    try {
      const [schema, templates] = await Promise.all([
        api("/v1/authoring/schema"),
        api("/v1/authoring/templates")
      ]);
      state.authoringSchema = schema;
      state.authoringTemplates = templates.templates || [];
      renderAuthoringTemplates();
      renderScenarioTargets();
      renderCustomPackTargets();
      renderCuratedCloneOptions();
      renderScenarioForm();
      scheduleScenarioPreview();
    } catch (error) {
      $("scenario-preview").innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
    }
  }

  function selectedTemplate() {
    return state.authoringTemplates.find((item) => item.template_id === $("scenario-template-select").value);
  }

  function renderAuthoringTemplates() {
    const select = $("scenario-template-select");
    const current = select.value;
    const selectedTargets = currentScenarioTargets();
    const visible = state.authoringTemplates.filter((template) =>
      state.showAllScenarioSources || sourceMatchesTargets(template.targets, selectedTargets));
    const options = visible.map((template) => `
      <option value="${escapeHtml(template.template_id)}">${escapeHtml(template.name)} · ${escapeHtml(template.difficulty)}${sourceMatchesTargets(template.targets, selectedTargets) ? " · recommended" : " · adaptation needed"}</option>
    `).join("");
    select.innerHTML = visible.length
      ? `<option value="">Choose a ${state.showAllScenarioSources ? "core" : "compatible"} template…</option>${options}`
      : "<option value=\"\">No template covers every selected target — show all starting points or choose fewer targets</option>";
    if (visible.some((template) => template.template_id === current)) select.value = current;
  }

  function renderCuratedCloneOptions() {
    const packSelect = $("curated-clone-pack");
    const current = packSelect.value;
    const selectedTargets = currentScenarioTargets();
    const packs = state.packs.filter((pack) => state.showAllScenarioSources ||
      (pack.scenarios || []).some((scenario) => sourceMatchesTargets([
        ...(scenario.scoreable_targets || []),
        ...(scenario.reference_only_targets || []),
        ...(scenario.targets || [])
      ], selectedTargets)));
    packSelect.innerHTML = packs.map((pack) => `
      <option value="${escapeHtml(pack.pack_id)}">${escapeHtml(pack.display_name || pack.pack_id)}</option>
    `).join("") || "<option value=\"\">No released pack covers every selected target</option>";
    if (packs.some((pack) => pack.pack_id === current)) packSelect.value = current;
    renderCuratedCaseOptions();
  }

  function renderCuratedCaseOptions() {
    const pack = state.packs.find((item) => item.pack_id === $("curated-clone-pack").value);
    const caseSelect = $("curated-clone-case");
    const selectedTargets = currentScenarioTargets();
    const cases = ((pack && pack.scenarios) || []).filter((scenario) =>
      state.showAllScenarioSources || sourceMatchesTargets([
        ...(scenario.scoreable_targets || []),
        ...(scenario.reference_only_targets || []),
        ...(scenario.targets || [])
      ], selectedTargets));
    caseSelect.innerHTML = cases.map((scenario) => `
      <option value="${escapeHtml(scenario.scenario_id)}">${escapeHtml(scenario.scenario_id)} · ${escapeHtml(uniqueSorted([...(scenario.scoreable_targets || []), ...(scenario.reference_only_targets || []), ...(scenario.targets || [])]).map(targetTitle).join(", ") || "general case")}</option>
    `).join("") || "<option>No cases</option>";
  }

  function renderScenarioTargets() {
    const targets = (state.authoringSchema && state.authoringSchema.targets) || [];
    const selected = new Set(state.scenarioTargets);
    $("scenario-targets").innerHTML = targets.map((target) => `
      <label class="target-option">
        <input type="checkbox" data-scenario-target value="${escapeHtml(target.name)}" ${selected.has(target.name) ? "checked" : ""}>
        <span>
          <strong>${escapeHtml(targetTitle(target.name))}</strong>
          <small>${escapeHtml(targetDescription(target.name))}</small>
          <small class="${target.support === "local_scoring" ? "ok" : "warning"}">${escapeHtml(target.support === "local_scoring" ? "Automated local scoring" : "Reference ground truth only")}</small>
          <small>${escapeHtml(targetRequirementText(target))}</small>
        </span>
      </label>
    `).join("") || "<span class=\"muted\">Targets load after sign-in.</span>";
    renderScenarioTargetGuidance();
  }

  function renderScenarioTargetGuidance() {
    const selectedTargets = currentScenarioTargets();
    const sourceStep = $("scenario-source-step");
    sourceStep.hidden = !selectedTargets.length;
    if (!selectedTargets.length) {
      $("scenario-target-guidance").textContent = "Choose at least one target to see compatible starting points.";
      $("make-scenario-compatible").hidden = true;
      return;
    }
    const scenario = readScenarioJson(true) || {};
    const metadata = (state.authoringSchema && state.authoringSchema.targets) || [];
    const missing = uniqueSorted(selectedTargets.flatMap((name) => {
      const target = metadata.find((item) => item.name === name);
      return ((target && target.requires) || []).filter((requirement) =>
        !requirementSatisfied(requirement, scenario));
    }));
    $("scenario-target-guidance").innerHTML = `
      <strong>${escapeHtml(selectedTargets.map(targetTitle).join(" + "))}</strong>
      <p class="muted compact">${missing.length ? `The current draft still needs: ${escapeHtml(missing.map((item) => targetRequirementText({ requires: [item] })).join("; "))}.` : "The current draft satisfies the visible target requirements."}</p>
    `;
    $("make-scenario-compatible").hidden = !Object.keys(scenario).length || !missing.length;
  }

  function targetRequirementText(target) {
    const requirements = (target && target.requires) || [];
    if (!requirements.length) return "No additional scenario requirement";
    return requirements.map((requirement) => {
      if (requirement === "ppg.enabled") return "Requires an enabled PPG channel";
      if (requirement === "hrv.enabled") return "Requires HRV modulation";
      if (requirement === "duration_seconds>=300") return "Requires at least 5 minutes";
      if (requirement === "artifacts.length>0") return "Requires at least one artifact interval";
      if (requirement === "ecg.conditions") return "Requires an ECG condition";
      return `Requires ${requirement}`;
    }).join(" · ");
  }

  function requirementSatisfied(requirement, scenario) {
    if (requirement === "ppg.enabled") return Boolean(getPathValue(scenario, "$.ppg.enabled"));
    if (requirement === "hrv.enabled") return Boolean(getPathValue(scenario, "$.hrv.enabled"));
    if (requirement === "duration_seconds>=300") return Number(scenario.duration_seconds || 0) >= 300;
    if (requirement === "artifacts.length>0") return Array.isArray(scenario.artifacts) && scenario.artifacts.length > 0;
    if (requirement === "ecg.conditions") {
      return Array.isArray(getPathValue(scenario, "$.ecg.conditions")) &&
        getPathValue(scenario, "$.ecg.conditions").length > 0;
    }
    return true;
  }

  function templateScenarioForRequirement(requirement) {
    return state.authoringTemplates
      .map((template) => template.scenario)
      .find((scenario) => requirementSatisfied(requirement, scenario || {}));
  }

  function applyMissingTargetRequirements() {
    const scenario = readScenarioJson();
    if (!scenario) return;
    const metadata = (state.authoringSchema && state.authoringSchema.targets) || [];
    const requirements = uniqueSorted(currentScenarioTargets().flatMap((name) => {
      const target = metadata.find((item) => item.name === name);
      return (target && target.requires) || [];
    }));
    const applied = [];
    requirements.forEach((requirement) => {
      if (requirementSatisfied(requirement, scenario)) return;
      const donor = templateScenarioForRequirement(requirement) || {};
      if (requirement === "duration_seconds>=300") {
        scenario.duration_seconds = Math.max(300, Number(scenario.duration_seconds || 0));
      } else if (requirement === "ppg.enabled") {
        if (!scenario.ppg && donor.ppg) scenario.ppg = JSON.parse(JSON.stringify(donor.ppg));
        if (!scenario.ppg) scenario.ppg = {};
        scenario.ppg.enabled = true;
      } else if (requirement === "hrv.enabled") {
        if (!scenario.hrv && donor.hrv) scenario.hrv = JSON.parse(JSON.stringify(donor.hrv));
        if (!scenario.hrv) scenario.hrv = {};
        scenario.hrv.enabled = true;
      } else if (requirement === "artifacts.length>0") {
        scenario.artifacts = donor.artifacts
          ? JSON.parse(JSON.stringify(donor.artifacts))
          : [defaultArrayItem("$.artifacts", scenario)];
      } else if (requirement === "ecg.conditions") {
        if (!scenario.ecg) scenario.ecg = {};
        scenario.ecg.conditions = [{ code: "NORM", severity: 1 }];
      }
      applied.push(requirement);
    });
    writeScenarioJson(scenario);
    renderScenarioForm();
    renderScenarioTargetGuidance();
    scheduleScenarioPreview();
    $("scenario-output").textContent = applied.length
      ? `Applied ${applied.length} target requirement${applied.length === 1 ? "" : "s"}. Review the highlighted settings and preflight.`
      : "The current draft already satisfies the selected targets.";
  }

  function editableFieldPaths() {
    const template = selectedTemplate();
    const editable = template ? (template.editable_paths || []) : [];
    if (!editable.length) {
      return new Set(((state.authoringSchema && state.authoringSchema.fields) || [])
        .map((field) => field.path));
    }
    const fields = (state.authoringSchema && state.authoringSchema.fields) || [];
    return new Set(fields
      .filter((field) => editable.some((path) => field.path === path || field.path.startsWith(path + ".")))
      .map((field) => field.path));
  }

  function visibleField(field, scenario) {
    const rule = field.visible_when;
    if (!rule) return true;
    if (rule.path) {
      const value = getPathValue(scenario, rule.path);
      if (Object.prototype.hasOwnProperty.call(rule, "equals") && value !== rule.equals) return false;
      if (Object.prototype.hasOwnProperty.call(rule, "not_equals") && value === rule.not_equals) return false;
      if (Object.prototype.hasOwnProperty.call(rule, "greater_than") && !(Number(value) > Number(rule.greater_than))) return false;
    }
    if (rule.condition_code) {
      const conditions = getPathValue(scenario, "$.ecg.conditions") || [];
      return conditions.some((item) => item && item.code === rule.condition_code);
    }
    return true;
  }

  function castFieldValue(field, raw, checked) {
    if (field.value_type === "boolean") return Boolean(checked);
    if (field.value_type === "integer") return Number.parseInt(raw, 10);
    if (field.value_type === "number") return Number(raw);
    if (field.value_type === "string_array") {
      return raw.split(",").map((item) => item.trim()).filter(Boolean);
    }
    if (field.value_type && (field.value_type.includes("_array") || field.value_type === "condition_array")) {
      return JSON.parse(raw || "[]");
    }
    if (field.value_type === "uint64_string") {
      return /^\d+$/.test(raw) ? Number(raw) : raw;
    }
    return raw;
  }

  function scalarControlHtml(field, value, attributes) {
    const attrs = attributes || "";
    const actual = value === undefined || value === null
      ? (field.default === undefined ? "" : field.default)
      : value;
    if (field.value_type === "boolean") {
      return `<input ${attrs} type="checkbox" ${actual ? "checked" : ""}>`;
    }
    if (field.control === "select" || field.control === "condition_select") {
      return `<select ${attrs}>${(field.options || []).map((option) => `
        <option value="${escapeHtml(option)}" ${String(actual) === String(option) ? "selected" : ""}>${escapeHtml(option)}</option>
      `).join("")}</select>`;
    }
    if (field.control === "slider") {
      return `<span class="slider-row">
        <input ${attrs} type="range" value="${escapeHtml(actual)}" ${field.minimum !== undefined ? `min="${escapeHtml(field.minimum)}"` : ""} ${field.maximum !== undefined ? `max="${escapeHtml(field.maximum)}"` : ""} ${field.step !== undefined ? `step="${escapeHtml(field.step)}"` : ""}>
        <output>${escapeHtml(actual)}</output>
      </span>`;
    }
    if (field.value_type === "number" || field.value_type === "integer") {
      return `<input ${attrs} type="number" value="${escapeHtml(actual)}" ${field.minimum !== undefined ? `min="${escapeHtml(field.minimum)}"` : ""} ${field.exclusive_minimum !== undefined ? `min="${escapeHtml(Number(field.exclusive_minimum) + (field.step || 0.000001))}"` : ""} ${field.maximum !== undefined ? `max="${escapeHtml(field.maximum)}"` : ""} ${field.step !== undefined ? `step="${escapeHtml(field.step)}"` : "step=\"any\""}>`;
    }
    return `<input ${attrs} type="text" value="${escapeHtml(actual)}">`;
  }

  function tagEditorHtml(field, value) {
    const tags = Array.isArray(value) ? value : [];
    return `
      <div class="form-field" data-authoring-container="${escapeHtml(field.path)}">
        <strong>${escapeHtml(field.label)}</strong>
        <div class="tag-editor-list">
          ${tags.map((tag, index) => `<button type="button" data-remove-tag="${escapeHtml(index)}" title="Remove ${escapeHtml(tag)}">${escapeHtml(tag)} ×</button>`).join("") || "<span class=\"muted\">No tags</span>"}
        </div>
        <div class="row">
          <input type="text" data-tag-input placeholder="Add a tag" aria-label="New scenario tag">
          <button type="button" class="secondary" data-add-tag>Add</button>
        </div>
        <span class="hint">${escapeHtml(field.path)} · up to ${escapeHtml(field.maximum || 64)} tags</span>
      </div>
    `;
  }

  function conditionEditorHtml(field, values) {
    const conditions = (state.authoringSchema && state.authoringSchema.conditions) || [];
    const byCode = Object.fromEntries(conditions.map((item) => [item.code, item]));
    return `
      <div class="array-editor" data-authoring-container="${escapeHtml(field.path)}">
        <div class="array-item-header"><div><strong>${escapeHtml(field.label)}</strong><span class="hint">Search the core condition catalog and set severity where supported.</span></div><button type="button" class="secondary" data-add-array="${escapeHtml(field.path)}">Add condition</button></div>
        <datalist id="condition-catalog">${conditions.map((item) => `<option value="${escapeHtml(item.code)}">${escapeHtml(item.name)} · ${escapeHtml(item.category)}</option>`).join("")}</datalist>
        ${(values || []).map((item, index) => {
          const metadata = byCode[item.code] || {};
          return `<div class="array-item">
            <div class="array-item-header"><strong>Condition ${escapeHtml(index + 1)}</strong><button type="button" class="danger" data-remove-array="${escapeHtml(field.path)}" data-array-index="${escapeHtml(index)}">Remove</button></div>
            <div class="array-item-grid">
              <label>Condition
                <input type="text" list="condition-catalog" value="${escapeHtml(item.code || "")}" data-array-path="${escapeHtml(field.path)}" data-array-index="${escapeHtml(index)}" data-array-name="code">
                <span class="hint">${escapeHtml(metadata.name || "Choose a known condition code")} · ${escapeHtml(metadata.support || "unknown support")}</span>
              </label>
              <label>Severity
                <span class="slider-row">
                  <input type="range" min="0.000001" max="1" step="0.01" value="${escapeHtml(item.severity === undefined ? 1 : item.severity)}" data-array-path="${escapeHtml(field.path)}" data-array-index="${escapeHtml(index)}" data-array-name="severity" data-array-type="number" ${metadata.variable_severity === false ? "disabled" : ""}>
                  <output>${escapeHtml(item.severity === undefined ? 1 : item.severity)}</output>
                </span>
                <span class="hint">${metadata.variable_severity === false ? "Fixed-severity condition" : "0–1 condition intensity"}</span>
              </label>
            </div>
          </div>`;
        }).join("") || "<p class=\"muted compact\">No ECG conditions. Add one to describe morphology or rhythm.</p>"}
      </div>
    `;
  }

  function genericArrayEditorHtml(field, values, itemFields, itemLabel) {
    return `
      <div class="array-editor" data-authoring-container="${escapeHtml(field.path)}">
        <div class="array-item-header"><div><strong>${escapeHtml(field.label)}</strong><span class="hint">${escapeHtml(field.path)}</span></div><button type="button" class="secondary" data-add-array="${escapeHtml(field.path)}">Add ${escapeHtml(itemLabel)}</button></div>
        ${(values || []).map((item, index) => `
          <div class="array-item">
            <div class="array-item-header"><strong>${escapeHtml(itemLabel)} ${escapeHtml(index + 1)}</strong><button type="button" class="danger" data-remove-array="${escapeHtml(field.path)}" data-array-index="${escapeHtml(index)}">Remove</button></div>
            <div class="array-item-grid">
              ${(itemFields || []).map((itemField) => `
                <label>${escapeHtml(itemField.name.replaceAll("_", " "))}
                  ${scalarControlHtml(itemField, item[itemField.name], `data-array-path="${escapeHtml(field.path)}" data-array-index="${escapeHtml(index)}" data-array-name="${escapeHtml(itemField.name)}" data-array-type="${escapeHtml(itemField.value_type || "string")}"`)}
                  <span class="hint">${escapeHtml(itemField.unit || "")}</span>
                </label>
              `).join("")}
            </div>
          </div>
        `).join("") || `<p class="muted compact">No ${escapeHtml(itemLabel)} entries.</p>`}
      </div>
    `;
  }

  function artifactEditorHtml(field, values) {
    const artifacts = (state.authoringSchema && state.authoringSchema.artifacts) || [];
    const byType = Object.fromEntries(artifacts.map((item) => [item.type, item]));
    return `
      <div class="array-editor" data-authoring-container="${escapeHtml(field.path)}">
        <div class="array-item-header"><div><strong>${escapeHtml(field.label)}</strong><span class="hint">Add bounded deterministic noise intervals and affected channels.</span></div><button type="button" class="secondary" data-add-array="${escapeHtml(field.path)}">Add artifact</button></div>
        ${(values || []).map((item, index) => {
          const metadata = byType[item.type] || artifacts[0] || { item_fields: [] };
          return `<div class="array-item">
            <div class="array-item-header"><strong>Artifact ${escapeHtml(index + 1)}</strong><button type="button" class="danger" data-remove-array="${escapeHtml(field.path)}" data-array-index="${escapeHtml(index)}">Remove</button></div>
            <div class="array-item-grid">
              <label>Artifact type
                <select data-array-path="${escapeHtml(field.path)}" data-array-index="${escapeHtml(index)}" data-array-name="type">
                  ${artifacts.map((artifact) => `<option value="${escapeHtml(artifact.type)}" ${artifact.type === item.type ? "selected" : ""}>${escapeHtml(artifact.type)} · ${escapeHtml(artifact.channel_family)}</option>`).join("")}
                </select>
              </label>
              ${(metadata.item_fields || []).filter((itemField) => itemField.name !== "channels").map((itemField) => `
                <label>${escapeHtml(itemField.name.replaceAll("_", " "))}
                  ${scalarControlHtml(itemField, item[itemField.name], `data-array-path="${escapeHtml(field.path)}" data-array-index="${escapeHtml(index)}" data-array-name="${escapeHtml(itemField.name)}" data-array-type="${escapeHtml(itemField.value_type || "string")}"`)}
                  <span class="hint">${escapeHtml(itemField.unit || "")}</span>
                </label>
              `).join("")}
            </div>
            ${(metadata.item_fields || []).filter((itemField) => itemField.name === "channels").map((itemField) => `
              <label>Channels</label>
              <div class="channel-picker">${(itemField.options || []).map((channel) => `
                <label><input type="checkbox" value="${escapeHtml(channel)}" data-array-channel="${escapeHtml(field.path)}" data-array-index="${escapeHtml(index)}" ${(item.channels || []).includes(channel) ? "checked" : ""}>${escapeHtml(channel)}</label>
              `).join("")}</div>
            `).join("")}
          </div>`;
        }).join("") || "<p class=\"muted compact\">No artifacts. Add one when testing signal-quality handling.</p>"}
      </div>
    `;
  }

  function arrayEditorHtml(field, value) {
    const values = Array.isArray(value) ? value : [];
    if (field.control === "condition_picker") return conditionEditorHtml(field, values);
    if (field.control === "artifact_editor") return artifactEditorHtml(field, values);
    if (field.control === "episode_editor") {
      return genericArrayEditorHtml(field, values, state.authoringSchema.ppg_perfusion_episode_item_fields, "perfusion episode");
    }
    if (field.control === "envelope_editor") {
      return genericArrayEditorHtml(field, values, state.authoringSchema.randomization_envelope_item_fields, "envelope");
    }
    return genericArrayEditorHtml(field, values, [], "item");
  }

  function fieldInputHtml(field, value) {
    const id = `field-${field.path.replace(/[^a-z0-9]+/gi, "-")}`;
    const hint = [
      field.minimum !== undefined || field.maximum !== undefined
        ? `range ${field.minimum !== undefined ? field.minimum : "-∞"}..${field.maximum !== undefined ? field.maximum : "∞"}`
        : "",
      field.step !== undefined ? `step ${field.step}` : "",
      field.unit || "",
      field.default !== undefined ? `default ${JSON.stringify(field.default)}` : ""
    ].filter(Boolean).join(" · ");
    const common = `id="${escapeHtml(id)}" data-authoring-field="${escapeHtml(field.path)}"`;
    if (field.control === "tag_editor") return tagEditorHtml(field, value);
    if (field.value_type && (field.value_type.includes("_array") || field.value_type === "condition_array")) {
      return arrayEditorHtml(field, value);
    }
    let control = "";
    if (field.value_type === "boolean") {
      control = `<input ${common} type="checkbox" ${value ? "checked" : ""}>`;
    } else if (field.control === "segmented") {
      control = `<span class="segmented">${(field.options || []).map((option) => `
        <label><input data-authoring-field="${escapeHtml(field.path)}" type="radio" name="${escapeHtml(id)}" value="${escapeHtml(option)}" ${String(value) === String(option) ? "checked" : ""}>${escapeHtml(option)}</label>
      `).join("")}</span>`;
    } else if (field.control === "select") {
      const options = field.options || [];
      control = `<select ${common}>${options.map((option) => `
        <option value="${escapeHtml(option)}" ${String(value) === String(option) ? "selected" : ""}>${escapeHtml(option)}</option>
      `).join("")}</select>`;
    } else if (field.control === "slider") {
      control = scalarControlHtml(field, value, common);
    } else if (field.value_type === "number" || field.value_type === "integer") {
      control = `<input ${common} type="number" value="${escapeHtml(value === undefined ? "" : value)}" ${field.minimum !== undefined ? `min="${escapeHtml(field.minimum)}"` : ""} ${field.maximum !== undefined ? `max="${escapeHtml(field.maximum)}"` : ""} ${field.step !== undefined ? `step="${escapeHtml(field.step)}"` : ""}>`;
    } else if (field.value_type === "string_array") {
      control = `<input ${common} type="text" value="${escapeHtml((value || []).join(", "))}">`;
    } else if (field.control === "textarea") {
      control = `<textarea ${common} rows="3">${escapeHtml(value === undefined ? "" : value)}</textarea>`;
    } else {
      control = `<input ${common} type="text" value="${escapeHtml(value === undefined ? "" : value)}">`;
    }
    return `
      <div class="form-field" data-authoring-container="${escapeHtml(field.path)}">
        <label for="${escapeHtml(id)}"><strong>${escapeHtml(field.label)}</strong></label>
        ${control}
        <span class="hint">${escapeHtml(field.path)}${hint ? " · " + escapeHtml(hint) : ""}</span>
      </div>
    `;
  }

  function renderScenarioForm() {
    const scenario = readScenarioJson(true);
    const fields = (state.authoringSchema && state.authoringSchema.fields) || [];
    if (!scenario || !Object.keys(scenario).length || !fields.length) {
      $("scenario-form").innerHTML = "";
      return;
    }
    const editable = editableFieldPaths();
    const visible = fields.filter((field) => editable.has(field.path) && visibleField(field, scenario));
    const previousOpen = new Set([...$("scenario-form").querySelectorAll(".scenario-group[open]")]
      .map((node) => node.getAttribute("data-group")));
    const groups = (state.authoringSchema.groups || []).filter((group) =>
      visible.some((field) => field.group === group.id));
    $("scenario-form").innerHTML = groups.map((group, index) => {
      const groupFields = visible.filter((field) => field.group === group.id);
      const open = previousOpen.has(group.id) || (!previousOpen.size && index < 2);
      return `<details class="scenario-group" data-group="${escapeHtml(group.id)}" ${open ? "open" : ""}>
        <summary>${escapeHtml(group.label)} · ${escapeHtml(groupFields.length)} control(s)</summary>
        <div class="form-grid">${groupFields.map((field) => fieldInputHtml(field, getPathValue(scenario, field.path))).join("")}</div>
      </details>`;
    }).join("") || "<p class=\"muted\">This template exposes no form fields; use JSON mode.</p>";
  }

  function updateScenarioField(fieldPath, node) {
    const scenario = readScenarioJson();
    if (!scenario) return;
    const field = ((state.authoringSchema && state.authoringSchema.fields) || [])
      .find((item) => item.path === fieldPath);
    if (!field) return;
    try {
      setPathValue(scenario, fieldPath, castFieldValue(field, node.value, node.checked));
      writeScenarioJson(scenario);
      renderScenarioForm();
      scheduleScenarioPreview();
    } catch (error) {
      $("scenario-output").textContent = `Invalid ${field.label}: ${error.message}`;
    }
  }

  function arrayFieldValue(node) {
    const type = node.getAttribute("data-array-type") || "string";
    if (type === "number") return Number(node.value);
    if (type === "integer") return Number.parseInt(node.value, 10);
    if (type === "uint64_string") return /^\d+$/.test(node.value) ? Number(node.value) : node.value;
    return node.value;
  }

  function updateScenarioArrayItem(node) {
    const path = node.getAttribute("data-array-path");
    const index = Number.parseInt(node.getAttribute("data-array-index"), 10);
    const name = node.getAttribute("data-array-name");
    const scenario = readScenarioJson();
    const values = scenario && getPathValue(scenario, path);
    if (!Array.isArray(values) || !values[index] || !name) return;
    values[index][name] = arrayFieldValue(node);
    if (path === "$.ecg.conditions" && name === "code") {
      const condition = ((state.authoringSchema && state.authoringSchema.conditions) || [])
        .find((item) => item.code === node.value);
      if (condition && !condition.variable_severity) values[index].severity = 1;
    }
    writeScenarioJson(scenario);
    renderScenarioForm();
    scheduleScenarioPreview();
  }

  function updateScenarioArrayChannels(node) {
    const path = node.getAttribute("data-array-channel");
    const index = Number.parseInt(node.getAttribute("data-array-index"), 10);
    const scenario = readScenarioJson();
    const values = scenario && getPathValue(scenario, path);
    if (!Array.isArray(values) || !values[index]) return;
    values[index].channels = [...document.querySelectorAll(
      `[data-array-channel="${CSS.escape(path)}"][data-array-index="${index}"]:checked`
    )].map((input) => input.value);
    writeScenarioJson(scenario);
    scheduleScenarioPreview();
  }

  function defaultArrayItem(path, scenario) {
    if (path === "$.ecg.conditions") return { code: "NORM", severity: 1 };
    if (path === "$.artifacts") {
      const artifact = ((state.authoringSchema && state.authoringSchema.artifacts) || [])[0];
      const type = artifact ? artifact.type : "ecg_baseline_wander";
      const channel = artifact && artifact.item_fields
        ? artifact.item_fields.find((field) => field.name === "channels")
        : null;
      return {
        type,
        start_seconds: 0,
        duration_seconds: Math.min(5, Number(scenario.duration_seconds || 5)),
        severity: 0.5,
        seed: Number(scenario.seed || 1),
        channels: channel && channel.options ? [channel.options[0]] : ["all_ecg"]
      };
    }
    if (path === "$.ppg.perfusion_episodes") {
      return {
        start_seconds: 0,
        duration_seconds: Math.min(10, Number(scenario.duration_seconds || 10)),
        amplitude_scale: 0.5,
        rise_time_scale: 1,
        decay_time_scale: 1,
        weak_pulse_every_n_beats: 0,
        weak_pulse_amplitude_scale: 0.5,
        missing_pulse_every_n_beats: 0
      };
    }
    if (path === "$.randomization.envelopes") {
      return { parameter: "ecg.heart_rate_bpm", minimum: 60, maximum: 80 };
    }
    return {};
  }

  function addScenarioArrayItem(path) {
    const scenario = readScenarioJson();
    if (!scenario) return;
    const values = getPathValue(scenario, path);
    const next = Array.isArray(values) ? values : [];
    next.push(defaultArrayItem(path, scenario));
    setPathValue(scenario, path, next);
    writeScenarioJson(scenario);
    renderScenarioForm();
    scheduleScenarioPreview();
  }

  function removeScenarioArrayItem(path, index) {
    const scenario = readScenarioJson();
    const values = scenario && getPathValue(scenario, path);
    if (!Array.isArray(values)) return;
    values.splice(index, 1);
    writeScenarioJson(scenario);
    renderScenarioForm();
    scheduleScenarioPreview();
  }

  function addScenarioTag() {
    const input = $("scenario-form").querySelector("[data-tag-input]");
    const value = input ? input.value.trim() : "";
    const scenario = readScenarioJson();
    if (!scenario || !value) return;
    const tags = Array.isArray(scenario.tags) ? scenario.tags : [];
    if (!tags.includes(value)) tags.push(value);
    scenario.tags = tags;
    writeScenarioJson(scenario);
    renderScenarioForm();
    scheduleScenarioPreview();
  }

  function removeScenarioTag(index) {
    const scenario = readScenarioJson();
    if (!scenario || !Array.isArray(scenario.tags)) return;
    scenario.tags.splice(index, 1);
    writeScenarioJson(scenario);
    renderScenarioForm();
    scheduleScenarioPreview();
  }

  function applyScenarioTemplate() {
    const template = selectedTemplate();
    if (!template) return;
    const scenario = JSON.parse(JSON.stringify(template.scenario));
    const suffix = Date.now().toString(36).slice(-6);
    scenario.scenario_id = `${template.template_id}_${suffix}`;
    scenario.name = template.name;
    state.selectedScenarioId = "";
    $("scenario-name").value = template.name;
    writeScenarioJson(scenario);
    renderScenarioTargets();
    renderScenarioForm();
    scheduleScenarioPreview();
    $("scenario-output").textContent = "Template loaded. Review preview, then save.";
  }

  async function cloneCuratedScenario() {
    const packId = $("curated-clone-pack").value;
    const caseId = $("curated-clone-case").value;
    if (!packId || !caseId) return;
    try {
      const body = await api(`/v1/authoring/curated-scenarios/${encodeURIComponent(packId)}/${encodeURIComponent(caseId)}`);
      const scenario = body.scenario;
      const suffix = Date.now().toString(36).slice(-6);
      scenario.scenario_id = `${scenario.scenario_id || caseId}_${suffix}`;
      scenario.name = `${scenario.name || caseId} clone`;
      state.selectedScenarioId = "";
      $("scenario-template-select").value = "";
      $("scenario-name").value = scenario.name;
      writeScenarioJson(scenario);
      renderScenarioTargets();
      renderScenarioForm();
      scheduleScenarioPreview();
      $("scenario-output").textContent = `Cloned ${caseId} from ${packId}.`;
    } catch (error) {
      $("scenario-output").textContent = error.message;
    }
  }

  function scheduleScenarioPreview() {
    clearTimeout(state.scenarioPreviewTimer);
    state.scenarioPreviewTimer = setTimeout(refreshScenarioPreview, 350);
  }

  async function refreshScenarioPreview() {
    if (!state.authenticated) return;
    renderScenarioTargetGuidance();
    if (!currentScenarioTargets().length) {
      state.scenarioPreview = null;
      $("save-scenario").disabled = true;
      $("scenario-preview").innerHTML = "<p class=\"warning\">Choose at least one algorithm output in Step 1.</p>";
      return;
    }
    const scenario = readScenarioJson(true);
    if (!scenario) {
      state.scenarioPreview = null;
      $("save-scenario").disabled = true;
      $("scenario-preview").innerHTML = "<p class=\"error\">JSON is not parseable yet.</p>";
      return;
    }
    if (!Object.keys(scenario).length) {
      $("save-scenario").disabled = true;
      $("scenario-preview").textContent = "Select a template, clone a curated case, or paste scenario JSON to preview package output.";
      return;
    }
    try {
      const preview = await api("/v1/authoring/preview", {
        method: "POST",
        json: { scenario, targets: currentScenarioTargets() }
      });
      state.scenarioPreview = preview;
      renderScenarioPreview(preview);
    } catch (error) {
      state.scenarioPreview = null;
      $("save-scenario").disabled = true;
      if (error.body && error.body.validation_errors) {
        const groups = groupValidationErrors(error.body.validation_errors);
        $("scenario-preview").innerHTML = `
          <h3><span class="error">Not safe to save yet</span></h3>
          <p class="muted">Fix the grouped validation issues below. Click a path to open and focus the affected form control.</p>
          ${Object.entries(groups).map(([section, items]) => `
            <details class="meta" open>
              <summary>${escapeHtml(section)} · ${escapeHtml(items.length)} issue(s)</summary>
              <ul>${items.map((item) => `
                <li><button class="validation-link" data-validation-path="${escapeHtml(item.path)}">${escapeHtml(item.path || "$")}</button>: ${escapeHtml(userValidationMessage(item))}</li>
              `).join("")}</ul>
            </details>
          `).join("")}
        `;
      } else {
        $("scenario-preview").innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
      }
    }
  }

  function validationSection(path) {
    const value = String(path || "$");
    if (value.startsWith("$.ecg")) return "ECG";
    if (value.startsWith("$.ppg")) return "PPG";
    if (value.startsWith("$.hrv")) return "HRV";
    if (value.startsWith("$.physiology")) return "Physiology";
    if (value.startsWith("$.randomization")) return "Randomization";
    if (value.startsWith("$.artifacts")) return "Artifacts";
    if (value.startsWith("$.output")) return "Output";
    return "Scenario identity";
  }

  function groupValidationErrors(errors) {
    return (errors || []).reduce((groups, item) => {
      const section = validationSection(item.path);
      if (!groups[section]) groups[section] = [];
      groups[section].push(item);
      return groups;
    }, {});
  }

  function userValidationMessage(item) {
    const message = item.message || "";
    const path = item.path || "";
    if (path.includes("hrv") && /duration/i.test(message)) {
      return `${message} HRV scoring needs enough signal duration for stable windowed metrics.`;
    }
    if (path.includes("ppg") && /enabled|required|missing/i.test(message)) {
      return `${message} PPG targets require PPG generation to be enabled and compatible with the selected target.`;
    }
    return message;
  }

  function validationMessageClass(item) {
    if (item.severity === "error" || item.code === "TARGET_INCOMPATIBLE") {
      return "error";
    }
    if (item.severity === "warning" || item.code === "REFERENCE_ONLY_TARGET") {
      return "warning";
    }
    return "muted";
  }

  function renderScenarioPreview(preview) {
    const scenario = readScenarioJson(true) || {};
    const summary = preview.summary || {};
    const cases = preview.cases || [];
    const messages = preview.messages || [];
    const scoreable = preview.scoreable_targets || [];
    const referenceOnly = preview.reference_only_targets || [];
    const selectedTargets = currentScenarioTargets();
    const targetMetadata = (state.authoringSchema && state.authoringSchema.targets) || [];
    const requirements = selectedTargets.map((name) => {
      const target = targetMetadata.find((item) => item.name === name) || { name, requires: [] };
      const missing = (target.requires || []).filter((item) => !requirementSatisfied(item, scenario));
      return `<li class="${missing.length ? "error" : "ok"}"><strong>${escapeHtml(name)}</strong> — ${missing.length ? `missing: ${escapeHtml(missing.map((item) => targetRequirementText({ requires: [item] })).join(", "))}` : "requirements satisfied"} · ${escapeHtml(target.support === "reference_only" ? "reference-only output" : "locally scoreable")}</li>`;
    }).join("");
    $("save-scenario").disabled = !(preview.success && canWrite() && selectedTargets.length);
    $("scenario-preview").innerHTML = `
      <h3>Preflight ${preview.success ? "<span class=\"ok\">safe to save</span>" : "<span class=\"error\">needs attention</span>"}</h3>
      <p>${preview.success ? "<span class=\"badge succeeded\">safe to compose pack</span>" : "<span class=\"badge failed\">not ready for pack composition</span>"}</p>
      <p class="muted">${escapeHtml(preview.scoring_mode || "unknown")} · diagnostic threshold hint (non-evidence): ${escapeHtml(preview.recommended_verifier_profile || "n/a")} · ${escapeHtml(formatBytes(summary.estimated_package_bytes || 0))} estimated package</p>
      <p><strong>Scoreable</strong>${targetTags(scoreable, "scoreable")}</p>
      <p><strong>Reference-only</strong>${targetTags(referenceOnly, "reference")}</p>
      ${!scoreable.length && referenceOnly.length ? `<p class="muted compact">This scenario can still be useful for manual/reference QA, but it will not produce a local scoring command for those targets.</p>` : ""}
      <div class="preflight-stats">
        <div class="preflight-stat">Duration<strong>${escapeHtml(formatSeconds(summary.total_duration_seconds || 0))}</strong></div>
        <div class="preflight-stat">Samples<strong>${escapeHtml(summary.total_sample_count || 0)}</strong></div>
        <div class="preflight-stat">Estimated package<strong>${escapeHtml(formatBytes(summary.estimated_package_bytes || 0))}</strong></div>
        <div class="preflight-stat">Peak memory<strong>${escapeHtml(formatBytes(summary.estimated_peak_memory_bytes || 0))}</strong></div>
        <div class="preflight-stat">Channels<strong>${escapeHtml((cases[0] && cases[0].channel_count) || "n/a")}</strong></div>
      </div>
      <details class="meta" open><summary>Target compatibility</summary><ul>${requirements}</ul></details>
      ${messages.length ? `
        <details class="meta" open>
          <summary>${escapeHtml(messages.length)} preview message(s)</summary>
          <ul>${messages.map((item) => `
            <li class="${validationMessageClass(item)}"><button class="validation-link" data-validation-path="${escapeHtml(item.path)}">${escapeHtml(item.code)}</button>: ${escapeHtml(userValidationMessage(item))}</li>
          `).join("")}</ul>
        </details>
      ` : ""}
    `;
  }

  function focusJsonPath(path) {
    $("scenario-json-details").open = true;
    const textarea = $("scenario-json");
    textarea.focus();
    const key = String(path || "").split(".").pop().replace(/\]$/, "");
    const needle = key && key !== "$" ? `"${key.replace(/^\$/, "")}"` : "";
    const index = needle ? textarea.value.indexOf(needle) : -1;
    if (index >= 0) textarea.setSelectionRange(index, index + needle.length);
  }

  function focusScenarioPath(path) {
    const normalized = String(path || "").replace(/\[\d+\]/g, "");
    const candidates = [...$("scenario-form").querySelectorAll("[data-authoring-container]")];
    const container = candidates.find((node) => {
      const fieldPath = node.getAttribute("data-authoring-container");
      return normalized === fieldPath || normalized.startsWith(fieldPath + ".");
    });
    $("scenario-form").querySelectorAll(".field-invalid").forEach((node) =>
      node.classList.remove("field-invalid"));
    if (!container) {
      focusJsonPath(path);
      return;
    }
    const group = container.closest("details");
    if (group) group.open = true;
    container.classList.add("field-invalid");
    container.scrollIntoView({ behavior: "smooth", block: "center" });
    const control = container.querySelector("input:not([disabled]), select, textarea, button");
    if (control) window.setTimeout(() => control.focus(), 250);
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
          <p><strong>Verification goal</strong>${targetTags((draft.target_intent || []).map((target) => targetTitle(target)), "mode")}</p>
          <span class="fingerprint">${escapeHtml(draft.document_fingerprint || "No fingerprint until valid")}</span>
          ${(draft.validation_errors || []).length ? `
            <details class="meta">
              <summary class="error">${escapeHtml(draft.validation_errors.length)} validation issue(s)</summary>
              <ul>
                ${draft.validation_errors.map((item) => `<li class="error"><button class="validation-link" data-validation-path="${escapeHtml(item.path)}">${escapeHtml(item.code)}</button> ${escapeHtml(item.path)}: ${escapeHtml(item.message)}</li>`).join("")}
              </ul>
            </details>
          ` : ""}
          <div class="actions">
            <button class="secondary" data-edit-scenario="${escapeHtml(draft.scenario_id)}">${canWrite() ? "Edit" : "View"}</button>
            ${canWrite() ? `<button class="danger" data-delete-scenario="${escapeHtml(draft.scenario_id)}">Delete</button>` : ""}
          </div>
        </article>
      `).join("") || `
        <article class="job">
          <h3>No scenario drafts yet</h3>
          <p class="muted">Start from a core template or load the clean ECG example above.</p>
          <button class="secondary" type="button" data-empty-new-scenario>Start a draft</button>
        </article>
      `;
    } catch (error) {
      $("scenarios").innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
    }
  }

  function renderCustomPackTargets() {
    const selected = new Set(requestedCustomPackTargets());
    const targets = (state.authoringSchema && state.authoringSchema.targets) || [];
    $("custom-pack-targets").innerHTML = targets.map((target) => `
      <label class="target-option">
        <input type="checkbox" data-custom-pack-target value="${escapeHtml(target.name)}" ${selected.has(target.name) ? "checked" : ""}>
        <span>
          <strong>${escapeHtml(targetTitle(target.name))}</strong>
          <small>${escapeHtml(target.support === "local_scoring" ? "Automated local scoring" : "Reference ground truth; no local score")}</small>
          <small>${escapeHtml(targetRequirementText(target))}</small>
        </span>
      </label>
    `).join("") || "<p class=\"muted\">Target catalog loads after sign-in.</p>";
    state.customPackAnalysis = {};
    syncInheritedPackTargets();
    renderCustomPackReview();
  }

  function renderPackScenarioOptions() {
    const selected = new Set(selectedCustomPackScenarioIds());
    $("pack-scenario-options").innerHTML = state.scenarios.map((draft) => `
      <label class="card" data-scenario-option data-search-text="${escapeHtml([
        draft.name,
        draft.scenario_id,
        draft.scenario && draft.scenario.scenario_id,
        ...((draft.scenario && draft.scenario.tags) || [])
      ].filter(Boolean).join(" ").toLowerCase())}">
        <input type="checkbox" data-pack-scenario="${escapeHtml(draft.scenario_id)}" ${selected.has(draft.scenario_id) ? "checked" : ""} ${draft.status !== "valid" ? "disabled" : ""}>
        <strong>${escapeHtml(draft.name)}</strong>
        <span class="muted">${escapeHtml(draft.scenario && draft.scenario.scenario_id ? draft.scenario.scenario_id : "scenario")}</span>
        <span class="badge ${draft.status === "valid" ? "succeeded" : "failed"}">${escapeHtml(draft.status)}</span>
        <span class="muted">Goal: ${escapeHtml((draft.target_intent || []).map(targetTitle).join(", ") || "not set")}</span>
        <span class="fingerprint">${escapeHtml(draft.document_fingerprint || "")}</span>
        ${draft.status !== "valid" ? `<span class="error">Fix this draft in the scenario editor before adding it to a pack.</span>` : ""}
      </label>
    `).join("") || "<p class=\"muted\">Create at least one valid scenario draft first.</p>";
    filterPackScenarioOptions();
    syncInheritedPackTargets();
    renderCustomPackReview();
  }

  function filterPackScenarioOptions() {
    const query = $("custom-pack-scenario-search").value.trim().toLowerCase();
    $("pack-scenario-options").querySelectorAll("[data-scenario-option]").forEach((node) => {
      node.hidden = Boolean(query) && !node.getAttribute("data-search-text").includes(query);
    });
  }

  function selectedCustomPackScenarioIds() {
    return [...document.querySelectorAll("[data-pack-scenario]:checked")]
      .map((input) => input.getAttribute("data-pack-scenario"))
      .filter(Boolean);
  }

  function inheritedTargetsForScenarioIds(scenarioIds) {
    return uniqueSorted(scenarioIds.flatMap((id) => {
      const draft = state.scenarios.find((item) => item.scenario_id === id);
      return (draft && draft.target_intent) || [];
    }));
  }

  function setCustomPackTargets(targets) {
    const selected = new Set(targets);
    document.querySelectorAll("[data-custom-pack-target]").forEach((input) => {
      input.checked = selected.has(input.value);
    });
    state.customPackAnalysis = {};
  }

  function syncInheritedPackTargets() {
    const scenarioIds = selectedCustomPackScenarioIds();
    const inherited = inheritedTargetsForScenarioIds(scenarioIds);
    const override = $("custom-pack-target-override");
    if (!override.open) setCustomPackTargets(inherited);
    $("inherited-pack-targets").innerHTML = inherited.length
      ? `<strong>Inherited from ${escapeHtml(scenarioIds.length)} scenario${scenarioIds.length === 1 ? "" : "s"}</strong>${targetTags(inherited.map(targetTitle), "mode")}<p class="muted compact">These are the goals selected during scenario authoring. Open Advanced target override only for a deliberate different contract.</p>`
      : "Select a scenario to inherit its verification targets.";
  }

  function preselectScenarioForPack(scenarioId) {
    document.querySelectorAll("[data-pack-scenario]").forEach((input) => {
      input.checked = input.getAttribute("data-pack-scenario") === scenarioId;
    });
    $("custom-pack-target-override").open = false;
    syncInheritedPackTargets();
    renderCustomPackReview();
  }

  function requestedCustomPackTargets() {
    return [...document.querySelectorAll("[data-custom-pack-target]:checked")]
      .map((input) => input.value)
      .filter(Boolean);
  }

  function renderCustomPackReview() {
    const review = $("custom-pack-review");
    if (!review) return;
    const scenarioIds = selectedCustomPackScenarioIds();
    const targets = [...new Set(requestedCustomPackTargets())];
    const selectedDrafts = scenarioIds
      .map((id) => state.scenarios.find((draft) => draft.scenario_id === id))
      .filter(Boolean);
    const name = $("custom-pack-name").value.trim();
    const description = $("custom-pack-description").value.trim();
    $("create-custom-pack").disabled = true;
    if (!selectedDrafts.length || !targets.length) {
      review.innerHTML = "Select at least one valid scenario and one target to preview pack coverage.";
      return;
    }
    const key = JSON.stringify({
      scenarios: selectedDrafts.map((draft) => [draft.scenario_id, draft.document_fingerprint]),
      targets
    });
    if (state.customPackAnalysis.key !== key) {
      scheduleCustomPackAnalysis(key, selectedDrafts, targets);
    }
    if (state.customPackAnalysis.key !== key || state.customPackAnalysis.loading) {
      review.innerHTML = `<h3>Core preflight running…</h3><p class="muted">Checking ${escapeHtml(selectedDrafts.length)} scenario(s) against ${escapeHtml(targets.length)} target(s).</p>`;
      return;
    }
    if (state.customPackAnalysis.error) {
      review.innerHTML = `<h3 class="error">Preflight unavailable</h3><p>${escapeHtml(state.customPackAnalysis.error)}</p>`;
      return;
    }
    renderCustomPackAnalysis(selectedDrafts, targets, name, description);
  }

  function scheduleCustomPackAnalysis(key, drafts, targets) {
    clearTimeout(state.customPackPreviewTimer);
    const token = ++state.customPackAnalysisToken;
    state.customPackAnalysis = { key, loading: true, results: [] };
    state.customPackPreviewTimer = setTimeout(async () => {
      try {
        const results = await Promise.all(drafts.map(async (draft) => ({
          draft,
          preview: await api("/v1/authoring/preview", {
            method: "POST",
            json: { scenario: draft.scenario, targets }
          })
        })));
        if (token !== state.customPackAnalysisToken) return;
        state.customPackAnalysis = { key, loading: false, results };
      } catch (error) {
        if (token !== state.customPackAnalysisToken) return;
        state.customPackAnalysis = { key, loading: false, error: error.message, results: [] };
      }
      renderCustomPackReview();
    }, 250);
  }

  function renderCustomPackAnalysis(selectedDrafts, targets, name, description) {
    const review = $("custom-pack-review");
    const results = state.customPackAnalysis.results || [];
    const metadata = (state.authoringSchema && state.authoringSchema.targets) || [];
    const ready = results.length === selectedDrafts.length &&
      results.every((result) => result.preview && result.preview.success);
    const summary = results.reduce((total, result) => {
      const current = result.preview.summary || {};
      total.duration += Number(current.total_duration_seconds || 0);
      total.samples += Number(current.total_sample_count || 0);
      total.bytes += Number(current.estimated_package_bytes || 0);
      total.memory = Math.max(total.memory, Number(current.estimated_peak_memory_bytes || 0));
      return total;
    }, { duration: 0, samples: 0, bytes: 0, memory: 0 });
    const scoreable = uniqueSorted(results.flatMap((result) =>
      targetNames(result.preview.scoreable_targets || [])));
    const referenceOnly = uniqueSorted(results.flatMap((result) =>
      targetNames(result.preview.reference_only_targets || [])));
    const messages = results.flatMap((result) => (result.preview.messages || [])
      .map((message) => ({ ...message, draftName: result.draft.name })));
    const rows = selectedDrafts.map((draft) => {
      const result = results.find((item) => item.draft.scenario_id === draft.scenario_id);
      const cells = targets.map((targetName) => {
        const target = metadata.find((item) => item.name === targetName) ||
          { name: targetName, support: "unsupported", requires: [] };
        const missing = (target.requires || []).filter((requirement) =>
          !requirementSatisfied(requirement, draft.scenario || {}));
        if (missing.length) {
          return `<td><span class="error">Blocked</span><br><small>${escapeHtml(missing.map((item) => targetRequirementText({ requires: [item] })).join("; "))}</small></td>`;
        }
        if (!result || !result.preview.success) {
          return "<td><span class=\"error\">Core rejected</span></td>";
        }
        return `<td><span class="ok">Compatible</span><br><small>${escapeHtml(target.support === "reference_only" ? "reference-only" : "local scoring")}</small></td>`;
      }).join("");
      return `<tr><td><strong>${escapeHtml(draft.name)}</strong><br><span class="fingerprint">${escapeHtml(draft.document_fingerprint || "")}</span></td>${cells}</tr>`;
    }).join("");
    $("create-custom-pack").disabled = !(ready && canWrite() && name && description);
    review.innerHTML = `
      <h3>Core-backed pack review ${ready ? "<span class=\"badge succeeded\">ready</span>" : "<span class=\"badge failed\">blocked</span>"}</h3>
      <p class="muted compact">${escapeHtml(selectedDrafts.length)} immutable scenario snapshot(s) · ${escapeHtml(targets.length)} target(s)</p>
      <div class="preflight-stats">
        <div class="preflight-stat">Cases<strong>${escapeHtml(selectedDrafts.length)}</strong></div>
        <div class="preflight-stat">Total duration<strong>${escapeHtml(formatSeconds(summary.duration))}</strong></div>
        <div class="preflight-stat">Total samples<strong>${escapeHtml(summary.samples)}</strong></div>
        <div class="preflight-stat">Estimated package<strong>${escapeHtml(formatBytes(summary.bytes))}</strong></div>
        <div class="preflight-stat">Peak memory<strong>${escapeHtml(formatBytes(summary.memory))}</strong></div>
      </div>
      <p><strong>Locally scoreable</strong>${targetTags(scoreable, "scoreable")}</p>
      <p><strong>Reference-only</strong>${targetTags(referenceOnly, "reference")}</p>
      ${messages.length ? `<details class="meta" open><summary>${escapeHtml(messages.length)} core message(s)</summary><ul>${messages.map((item) => `<li class="${validationMessageClass(item)}"><strong>${escapeHtml(item.draftName)}</strong>: ${escapeHtml(item.code)} — ${escapeHtml(userValidationMessage(item))}</li>`).join("")}</ul></details>` : `<p class="ok">All selected scenario–target combinations passed core analysis.</p>`}
      <div class="table-wrap"><table>
        <thead><tr><th>Scenario snapshot</th>${targets.map((target) => `<th>${escapeHtml(target)}</th>`).join("")}</tr></thead>
        <tbody>${rows}</tbody>
      </table></div>
      ${!name || !description ? "<p class=\"muted\">Enter a pack name and description to enable creation.</p>" : ""}
      <p class="muted compact"><strong>Snapshot semantics:</strong> creation copies these exact validated drafts. Later draft edits or deletion do not change the pack or its jobs.</p>
    `;
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
      `).join("") || `
        <article class="job">
          <h3>No custom packs yet</h3>
          <p class="muted">Create and validate a scenario draft, then compose your first immutable pack.</p>
          <a class="button-link secondary" href="/syn_sig_ra/scenarios">Open scenarios</a>
        </article>
      `;
    } catch (error) {
      $("custom-packs").innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
    }
  }

  async function createCustomPack() {
    const scenarioIds = selectedCustomPackScenarioIds();
    const targets = requestedCustomPackTargets();
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
      $("custom-pack-target-override").open = false;
      syncInheritedPackTargets();
      await loadCustomPacks();
      renderCustomPackReview();
      $("pack-select").value = pack.pack_id;
      navigateTo("generate");
      showToast(`Custom pack ${pack.display_name || pack.pack_id} is ready to generate.`);
    } catch (error) {
      $("custom-pack-output").textContent = error.message;
    }
  }

  async function deleteCustomPack(id) {
    if (!confirm("Delete this custom pack from the composer? Existing jobs remain reproducible.")) return;
    try {
      await api(`/v1/custom-packs/${encodeURIComponent(id)}`, { method: "DELETE" });
      await loadCustomPacks();
      showToast("Custom pack removed. Existing jobs remain reproducible.");
    } catch (error) {
      showToast(error.message, "error");
    }
  }

  function newScenario() {
    state.selectedScenarioId = "";
    state.scenarioTargets = [];
    state.showAllScenarioSources = false;
    $("show-all-scenario-sources").checked = false;
    $("scenario-template-select").value = "";
    $("scenario-name").value = "";
    $("scenario-json").value = "{}";
    $("scenario-output").textContent = "";
    renderScenarioTargets();
    renderAuthoringTemplates();
    renderCuratedCloneOptions();
    renderScenarioForm();
    scheduleScenarioPreview();
  }

  function loadScenarioTemplate() {
    state.selectedScenarioId = "";
    state.scenarioTargets = ["r_peak"];
    $("scenario-template-select").value = "";
    $("scenario-name").value = "Clean ECG";
    $("scenario-json").value = JSON.stringify(cleanEcgTemplate, null, 2);
    $("scenario-output").textContent = "Example loaded. Review it, then validate and save.";
    renderScenarioTargets();
    renderAuthoringTemplates();
    renderCuratedCloneOptions();
    renderScenarioForm();
    scheduleScenarioPreview();
  }

  function formatScenarioJson() {
    try {
      const scenario = JSON.parse($("scenario-json").value);
      $("scenario-json").value = JSON.stringify(scenario, null, 2);
      $("scenario-output").textContent = "JSON formatted.";
      renderScenarioForm();
      scheduleScenarioPreview();
    } catch (error) {
      $("scenario-output").textContent = `Invalid JSON: ${error.message}`;
    }
  }

  function editScenario(id) {
    const draft = state.scenarios.find((item) => item.scenario_id === id);
    if (!draft) return;
    state.selectedScenarioId = id;
    state.scenarioTargets = [...(draft.target_intent || [])];
    $("scenario-template-select").value = "";
    $("scenario-name").value = draft.name;
    $("scenario-json").value = JSON.stringify(draft.scenario, null, 2);
    $("scenario-output").textContent = draft.status;
    renderScenarioTargets();
    renderAuthoringTemplates();
    renderCuratedCloneOptions();
    renderScenarioForm();
    scheduleScenarioPreview();
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
    const targetIntent = currentScenarioTargets();
    if (!name) {
      $("scenario-output").textContent = "Enter a draft name.";
      return;
    }
    if (!targetIntent.length) {
      $("scenario-output").textContent = "Choose at least one algorithm output in Step 1.";
      return;
    }
    const path = state.selectedScenarioId
      ? `/v1/scenarios/${encodeURIComponent(state.selectedScenarioId)}`
      : "/v1/scenarios";
    try {
      const saved = await api(path, {
        method: state.selectedScenarioId ? "PUT" : "POST",
        json: { name, scenario, target_intent: targetIntent }
      });
      state.selectedScenarioId = saved.scenario_id;
      $("scenario-output").textContent = `Saved: ${saved.status}`;
      showToast(
        saved.status === "valid"
          ? "Scenario saved and ready for a custom pack."
          : "Scenario draft saved. Resolve validation findings before packaging.",
        saved.status === "valid" ? "success" : "notice"
      );
      if (saved.status === "valid") {
        await loadScenarios();
        await loadCustomPacks();
        preselectScenarioForPack(saved.scenario_id);
        navigateTo("custom-packs");
        showToast("Scenario saved. Its verification goal is inherited below; name the pack and review it.", "success");
        return;
      }
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
      showToast("Scenario draft deleted.", "notice");
    } catch (error) {
      showToast(error.message, "error");
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
      showToast(`Project ${created.display_name || displayName} created and selected.`);
    } catch (error) {
      $("create-output").textContent = error.message;
    } finally {
      $("create-project").disabled = !["owner", "admin"].includes(state.role);
    }
  }

  async function createJob() {
    if (!state.authenticated) {
      navigateTo("account", { next: "generate" });
      showToast("Sign in to create a challenge job.", "notice");
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
      await loadJobs({ force: true });
      await Promise.all([loadUsage(), loadMetrics()]);
      state.focusJobId = body.job_id;
      state.focusJobHandled = false;
      $("create-output").textContent = "";
      navigateTo("jobs", { job_id: body.job_id });
      showToast("Challenge job created. Its status will update automatically.");
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
      integration_contract: job.integration_contract || "",
      generator_version: job.generator_version || "",
      generator_git_commit: job.generator_git_commit || "",
      generator_build_identity: job.generator_build_identity || "",
      generator_binary_sha256: job.generator_binary_sha256 || "",
      error: job.error || null
    })));
  }

  function packById(packId) {
    return [...state.packs, ...state.customPacks].find((pack) => pack.pack_id === packId);
  }

  function shellQuote(value) {
    return `"${String(value).replace(/(["\\$`])/g, "\\$1")}"`;
  }

  function verificationContext(job) {
    const pack = packById(job.pack_id);
    const challenge = job.challenge || {};
    const targets = Array.isArray(challenge.targets) ? challenge.targets : [];
    const outputs = Array.isArray(challenge.submission_outputs)
      ? challenge.submission_outputs
      : [];
    const packageFile = `${job.package_id}-package.zip`;
    const manifestFile = `${job.package_id}-manifest.json`;
    const kitFile = `${job.job_id}-verification-kit.zip`;
    const kitDir = `${job.job_id}-verification-kit`;
    const outputDir = "verification-results";
    const scoreable = targets.filter((target) => target.supported)
      .map((target) => target.target);
    const referenceOnly = targets.filter((target) => !target.supported)
      .map((target) => target.target);
    const protocol = challenge.verification_protocol || null;
    const verification = challenge.verification || {};
    const evidenceEligible = verification.evidence_eligible === true;
    const mode = evidenceEligible ? "evidence" : "diagnostic";
    const modeArgument = evidenceEligible ? "" : " --mode diagnostic";
    const command = `unzip ${shellQuote(kitFile)} -d ${shellQuote(kitDir)}\ncd ${shellQuote(`${kitDir}/verification-kit`)}\nsynsigra-verify challenge submission ${shellQuote(outputDir)}${modeArgument} --force`;
    return {
      pack,
      challenge,
      packageFile,
      manifestFile,
      kitFile,
      kitDir,
      outputDir,
      scoreable,
      referenceOnly,
      outputs,
      protocol,
      verification,
      evidenceEligible,
      mode,
      command
    };
  }

  function submissionShape(context) {
    const paths = context.outputs.map((output) => `  ${output.path}`);
    const visible = paths.slice(0, 16);
    if (paths.length > visible.length) {
      visible.push(`  … ${paths.length - visible.length} more declared output file(s)`);
    }
    return ["submission/", "  submission.json", "  formats.json", ...visible].join("\n");
  }

  function verifierRecipe(job) {
    if (job.status !== "succeeded" || !job.package_id) return "";
    const context = verificationContext(job);
    if (context.challenge.challenge_contract !== "synsigra_challenge_package_v3" || !context.outputs.length) {
      return `
        <details class="verify-note">
          <summary>Verification metadata unavailable</summary>
          <p class="error">This job has no validated v7 submission contract. Generate a new job before attempting local verification.</p>
        </details>
      `;
    }
    const scoreable = context.scoreable;
    const referenceOnly = context.referenceOnly;
    if (!scoreable.length) {
      return `
        <details class="verify-note">
          <summary>Reference-only package</summary>
          <p>This challenge declares no locally scoreable output. Use the downloaded challenge for reference artifact inspection and contract/manual QA.</p>
          <p><strong>Reference targets:</strong> ${escapeHtml(referenceOnly.join(", ") || "n/a")}</p>
        </details>
      `;
    }
    return `
      <details class="verify-note">
        <summary>First-run local verification recipe</summary>
        <p><strong>Scoreable targets:</strong> ${escapeHtml(scoreable.join(", "))}</p>
        <p><strong>Reference-only targets:</strong> ${escapeHtml(referenceOnly.join(", ") || "none")}</p>
        <p>Download the verification kit, run your proprietary algorithm locally, then replace only the example algorithm identity and output rows under <code>submission/</code>. Keep every declared path, target and format unchanged.</p>
        <pre class="output">python -m pip install synsigra-wheel.whl</pre>
        <p>Role-selected submission layout:</p>
        <pre class="output">${escapeHtml(submissionShape(context))}</pre>
        <p><strong>${context.evidenceEligible ? "Evidence mode" : "Diagnostic mode — not evidence"}:</strong> ${context.evidenceEligible ? "the packaged protocol fixes the complete case-target matrix and numeric policy; do not add overrides." : "this package has no protocol v2, so the verifier must run explicitly in non-evidence diagnostic mode."}</p>
        <pre class="output">${escapeHtml(context.command)}</pre>
        <button class="secondary" data-copy-text="${escapeHtml(context.command)}">Copy verify command</button>
        <p>Machine-readable summaries are written to <code>${escapeHtml(context.outputDir)}/verification_summary.json</code> and <code>${escapeHtml(context.outputDir)}/verification_summary.csv</code>; the HTML report is <code>${escapeHtml(context.outputDir)}/verification_report.html</code>.</p>
        ${context.evidenceEligible ? `<p class="muted compact">Protocol ${escapeHtml((context.verification.protocol || {}).protocol_id || "n/a")} · policy ${escapeHtml((context.verification.protocol || {}).acceptance_profile_id || "n/a")} · matrix complete · outcome pending local verification.</p>` : `<p class="warning compact">Diagnostic reports are always marked evidence_eligible=false, even when their selected thresholds pass.</p>`}
        <p class="muted compact">CI semantics: exit 0 = pass, exit 1 = verification/input/scoring/threshold failure, exit 2 = invalid CLI usage.</p>
      </details>
    `;
  }

  function completedJobs() {
    return state.jobs.filter((job) => job.status === "succeeded" && job.package_id);
  }

  function renderRunbookJobOptions() {
    const select = $("runbook-job-select");
    if (!select) return;
    const jobs = completedJobs();
    const preferred = state.runbookJobId || queryParam("job_id") || (jobs[0] && jobs[0].job_id) || "";
    select.innerHTML = jobs.map((job) => `
      <option value="${escapeHtml(job.job_id)}">${escapeHtml(job.pack_id)} · ${escapeHtml(formatDate(job.created_at))}</option>
    `).join("") || "<option value=\"\">No completed jobs</option>";
    if (jobs.some((job) => job.job_id === preferred)) {
      select.value = preferred;
      state.runbookJobId = preferred;
    } else {
      state.runbookJobId = (jobs[0] && jobs[0].job_id) || "";
      select.value = state.runbookJobId;
    }
  }

  function renderVerificationRunbook() {
    const container = $("verification-runbook");
    if (!container) return;
    renderRunbookJobOptions();
    if (!state.authenticated) {
      container.innerHTML = `
        <article class="job">
          <h3>Sign in to open a job runbook</h3>
          <p class="muted">Runbooks are generated from your completed jobs and package IDs.</p>
          <a class="button-link" href="/syn_sig_ra/account">Sign in / register</a>
        </article>
      `;
      return;
    }
    const jobs = completedJobs();
    if (!jobs.length) {
      container.innerHTML = `
        <article class="job">
          <h3>No completed jobs yet</h3>
          <p class="muted">Choose a pack, generate a job, then return here for exact download and verifier instructions.</p>
          <div class="actions">
            <a class="button-link" href="/syn_sig_ra/packs">Choose pack</a>
            <a class="button-link secondary" href="/syn_sig_ra/jobs">Open jobs</a>
          </div>
        </article>
      `;
      return;
    }
    const job = jobs.find((item) => item.job_id === state.runbookJobId) || jobs[0];
    state.runbookJobId = job.job_id;
    const context = verificationContext(job);
    const expired = job.artifact_status === "expired";
    const scoreable = context.scoreable;
    const referenceOnly = context.referenceOnly;
    const installCommands = state.verifierDownloads && state.verifierDownloads.install
      ? state.verifierDownloads.install.join("\n")
      : "python -m pip install synsigra-wheel.whl\nsynsigra-verify --help";
    const scoreableSteps = scoreable.length ? `
      <li>
        <strong>Produce the declared local submission.</strong>
        <p>Unzip <code>${escapeHtml(context.kitFile)}</code>. Run your proprietary algorithm against <code>verification-kit/challenge/</code>, then replace the example algorithm identity and output rows under <code>verification-kit/submission/</code>. Do not rename paths, targets or formats.</p>
        <pre class="output">${escapeHtml(submissionShape(context))}</pre>
      </li>
      <li>
        <strong>Run the verifier locally.</strong>
        <pre class="output">${escapeHtml(context.command)}</pre>
        <button class="secondary" data-copy-text="${escapeHtml(context.command)}">Copy verify command</button>
        ${context.evidenceEligible ? `<p class="muted compact">The immutable protocol v2, complete matrix, embedded policy, protocol SHA-256 and evidence result are recorded in the local report. Keep that report with the challenge and algorithm build.</p>` : `<p class="warning compact">This is an exploratory diagnostic run. It cannot produce evidence PASS.</p>`}
      </li>
      <li>
        <strong>Interpret outputs.</strong>
        <p>Read <code>${escapeHtml(context.outputDir)}/verification_report.html</code>, <code>verification_summary.json</code>, and <code>verification_summary.csv</code>. CI exit code <code>0</code> means pass, <code>1</code> means verification/input/scoring/threshold failure, <code>2</code> means invalid CLI usage.</p>
      </li>
    ` : `
      <li>
        <strong>Reference/manual QA workflow.</strong>
        <p>This pack has no local scoring command. Inspect the package, manifest, provenance, summary files, and reports manually against your algorithm's own QA checklist.</p>
      </li>
    `;
    container.innerHTML = `
      <article class="job">
        <div class="job-header">
          <div>
            <h3>${escapeHtml(job.pack_id)}</h3>
            <span class="fingerprint">${escapeHtml(job.job_id)}</span>
          </div>
          <span class="badge succeeded">succeeded</span>
        </div>
        ${expired ? `<p class="error">The cached artifact expired. Return to Jobs and request an exact-version rebuild before using this runbook.</p>` : ""}
        <p class="muted">Package <span class="fingerprint">${escapeHtml(job.package_id)}</span></p>
        <p><strong>Scoreable locally</strong>${targetTags(scoreable, "scoreable")}</p>
        <p><strong>Reference-only</strong>${targetTags(referenceOnly, "reference")}</p>
        <div class="actions">
          <button class="primary" data-download-kit="${escapeHtml(job.job_id)}" ${expired ? "disabled" : ""}>Download verification kit</button>
        </div>
        <details class="meta">
          <summary>Advanced artifact downloads</summary>
          <p class="muted compact">The verification kit above is the normal complete download. Use these files only for a custom integration.</p>
          <div class="actions">
            <button class="secondary" data-download="${escapeHtml(job.package_id)}" data-file="manifest.json" ${expired ? "disabled" : ""}>Manifest</button>
            <button class="secondary" data-download="${escapeHtml(job.package_id)}" data-file="package.zip" ${expired ? "disabled" : ""}>Package ZIP</button>
          </div>
        </details>
        <ol>
          <li>
            <strong>Download files.</strong>
            <p>Preferred: <code>${escapeHtml(context.kitFile)}</code>. Individual challenge files for custom integrations: <code>${escapeHtml(context.manifestFile)}</code> and <code>${escapeHtml(context.packageFile)}</code>.</p>
          </li>
          <li>
            <strong>Install the generator-free verifier.</strong>
            <pre class="output">${escapeHtml(installCommands)}</pre>
            <button class="secondary" data-copy-text="${escapeHtml(installCommands)}">Copy install commands</button>
          </li>
          ${scoreableSteps}
          <li>
            <strong>Archive evidence.</strong>
            <p>Keep the extracted kit or original kit ZIP, <code>manifest.json</code>, <code>provenance.json</code>, <code>ENGINEERING_CLAIM_BOUNDARY.txt</code>, algorithm build/config, completed submission, verifier reports, and this job ID.</p>
          </li>
        </ol>
      </article>
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
        renderWorkspaceNextAction();
        renderVerificationRunbook();
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
      container.innerHTML = `
        <article class="job">
          <h3>No generation jobs yet</h3>
          <p class="muted">Choose a curated or custom challenge pack to create your first reproducible package.</p>
          <a class="button-link" href="/syn_sig_ra/packs">Choose a pack</a>
        </article>
      `;
      renderVerificationRunbook();
      return;
    }
    container.innerHTML = state.jobs.map((job) => {
      const artifactActions = job.status === "succeeded" && job.package_id ? `
        <a class="button-link" href="${base}/viewer?job_id=${encodeURIComponent(job.job_id)}" data-no-spa>View signals</a>
        <button class="primary" data-open-runbook="${escapeHtml(job.job_id)}">Open verification runbook</button>
        <button class="secondary" data-download-kit="${escapeHtml(job.job_id)}">Verification kit ZIP</button>
      ` : job.status === "succeeded" && job.artifact_status === "expired" ? `
        <button class="primary" data-prepare-kit="${escapeHtml(job.job_id)}" ${state.preparingKits.has(job.job_id) ? "disabled" : ""}>${state.preparingKits.has(job.job_id) ? "Preparing exact package…" : "Download verification kit"}</button>
      ` : "";
      const artifactExpired = job.artifact_status === "expired"
        ? `<p class="muted">Cached package expired. Synsigra can rebuild it only from the preserved recipe and exact historical generator; it will never substitute the latest generator.</p>`
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
        <article class="job${job.job_id === state.focusJobId ? " focused" : ""}" data-job-card="${escapeHtml(job.job_id)}">
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
              <dt>Pack version</dt><dd>${escapeHtml(job.pack_version || "—")}</dd>
              <dt>Catalog snapshot</dt><dd>${escapeHtml((job.catalog && job.catalog.version) || "custom authored")}</dd>
              <dt>Catalog SHA-256</dt><dd class="fingerprint">${escapeHtml((job.catalog && job.catalog.source_sha256) || "—")}</dd>
              <dt>Started</dt><dd>${escapeHtml(formatDate(job.started_at))}</dd>
              <dt>Completed</dt><dd>${escapeHtml(formatDate(job.completed_at))}</dd>
              <dt>Core contract</dt><dd class="fingerprint">${escapeHtml(job.integration_contract || "—")}</dd>
              <dt>Generator</dt><dd>${escapeHtml(job.generator_version || "—")}</dd>
              <dt>Core commit</dt><dd class="fingerprint">${escapeHtml(job.generator_git_commit || "—")}</dd>
              <dt>Build identity</dt><dd class="fingerprint">${escapeHtml(job.generator_build_identity || "—")}</dd>
              <dt>Binary SHA-256</dt><dd class="fingerprint">${escapeHtml(job.generator_binary_sha256 || "—")}</dd>
              <dt>Package fingerprint</dt><dd class="fingerprint">${escapeHtml(job.package_fingerprint || "—")}</dd>
              <dt>Challenge integrity</dt><dd>${escapeHtml(job.challenge && job.challenge.integrity && job.challenge.integrity.ok ? "verified before and after archive" : "—")}</dd>
              <dt>Verifier contract</dt><dd>${escapeHtml((job.challenge && job.challenge.verifier_version) || "—")}</dd>
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
    if (state.focusJobId && !state.focusJobHandled) {
      const focused = container.querySelector(`[data-job-card="${CSS.escape(state.focusJobId)}"]`);
      if (focused) {
        state.focusJobHandled = true;
        window.requestAnimationFrame(() => focused.scrollIntoView({ block: "center", behavior: "smooth" }));
      }
    }
    renderVerificationRunbook();
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
      showToast("Job removed from the list.", "notice");
    } catch (error) {
      showToast(error.message, "error");
    }
  }

  async function runJobAction(jobId, action) {
    try {
      const body = await api(`/v1/jobs/${encodeURIComponent(jobId)}/${action}`, {
        method: "POST"
      });
      await Promise.all([
        loadJobs({ force: true }),
        loadUsage(),
        loadMetrics()
      ]);
      if (action === "rebuild" && body.job_id) {
        showToast("Exact-version rebuild queued.", "notice");
        navigateTo("jobs", { job_id: body.job_id });
        state.focusJobId = body.job_id;
        state.focusJobHandled = false;
      } else {
        showToast(action === "retry" ? "Job queued for retry." : "Job cancelled.", "notice");
      }
    } catch (error) {
      showToast(error.message, "error");
    }
  }

  async function downloadArtifact(packageId, file) {
    try {
      if (file === "package.zip") {
        await startNativeDownload(
          `${base}/v1/artifacts/${encodeURIComponent(packageId)}/${file}`,
          `${packageId}-${file}`
        );
        return;
      }
      const response = await fetch(`${base}/v1/artifacts/${encodeURIComponent(packageId)}/${file}`, {
        credentials: "same-origin",
        headers: headers(false)
      });
      if (!response.ok) {
        const text = await response.text();
        throw new Error(text || response.statusText);
      }
      await saveResponseAsFile(response, `${packageId}-${file}`);
    } catch (error) {
      showToast(error.message, "error");
    }
  }

  async function downloadVerificationKit(jobId) {
    try {
      await startNativeDownload(
        `${base}/v1/jobs/${encodeURIComponent(jobId)}/verification-kit.zip`,
        `${jobId}-verification-kit.zip`
      );
    } catch (error) {
      showToast(error.message, "error");
    }
  }

  async function prepareVerificationKit(jobId) {
    if (state.preparingKits.has(jobId)) return;
    state.preparingKits.add(jobId);
    renderJobs();
    try {
      const queued = await api(`/v1/jobs/${encodeURIComponent(jobId)}/rebuild`, {
        method: "POST"
      });
      showToast("The cached package expired. Preparing the exact version for download.", "notice");
      let rebuilt = null;
      for (let attempt = 0; attempt < 180; attempt += 1) {
        rebuilt = await api(`/v1/jobs/${encodeURIComponent(queued.job_id)}`);
        if (rebuilt.status === "succeeded" || rebuilt.status === "failed") break;
        await new Promise((resolve) => window.setTimeout(resolve, 1000));
      }
      if (!rebuilt || rebuilt.status !== "succeeded") {
        const detail = rebuilt && rebuilt.error
          ? `${rebuilt.error.code}: ${rebuilt.error.message}`
          : "Exact package preparation timed out.";
        throw new Error(detail);
      }
      await loadJobs({ force: true });
      await downloadVerificationKit(rebuilt.job_id);
      showToast("Exact package rebuilt and download started.", "notice");
    } catch (error) {
      showToast(error.message, "error");
    } finally {
      state.preparingKits.delete(jobId);
      renderJobs();
    }
  }

  async function saveResponseAsFile(response, filename) {
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    link.setAttribute("data-no-spa", "true");
    document.body.appendChild(link);
    link.click();
    link.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 60000);
  }

  async function startNativeDownload(url, filename) {
    const response = await fetch(url, {
      method: "HEAD",
      credentials: "same-origin",
      headers: headers(false),
      cache: "no-store"
    });
    if (!response.ok) {
      throw new Error(`Download is unavailable (${response.status} ${response.statusText}).`);
    }
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    link.setAttribute("data-no-spa", "true");
    document.body.appendChild(link);
    link.click();
    link.remove();
    showToast("Browser download started. Interrupted ZIP downloads support resume.", "notice");
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
      loadVerifierDownloads(),
      loadAuthoring(),
      loadScenarios(),
      loadCustomPacks(),
      loadJobs({ force: true }),
      loadApiKeys(),
      loadAuditEvents()
    ]);
  }

  async function submitAuth(kind) {
    const registration = kind === "register";
    const email = $(registration ? "register-email" : "login-email").value.trim();
    const password = $(registration ? "register-password" : "login-password").value;
    const payload = { email, password };
    if (registration) {
      if (!$("register-terms").checked) {
        setText(
          "auth-output",
          "Accept the Private Beta Terms and Privacy & No-PHI Notice to create an account.",
          "error"
        );
        return;
      }
      payload.display_name = $("register-name").value.trim();
      payload.accept_terms = true;
      payload.terms_version = termsVersion;
    }
    setText("auth-output", registration ? "Creating account…" : "Signing in…", "muted");
    try {
      const account = await api(`/v1/auth/${kind}`, {
        method: "POST",
        json: payload
      });
      $("login-password").value = "";
      $("register-password").value = "";
      if (registration && ["verification_required", "accepted"].includes(account.status)) {
        state.pendingVerificationEmail = email;
        $("register-terms").checked = false;
        $("register").disabled = true;
        setText("auth-output", account.message || "Check your inbox to continue.", "muted ok");
        renderAuthState();
        return;
      }
      state.account = account;
      state.authenticated = true;
      state.role = account.role || "";
      state.pendingVerificationEmail = "";
      renderAuthState();
      await refreshAuthenticatedData();
      const destination = safeNextPage(queryParam("next")) || "workspace";
      navigateTo(destination);
      showToast(`Welcome back, ${account.display_name}.`);
    } catch (error) {
      setText("auth-output", error.message, "error");
    }
  }

  async function requestPasswordReset() {
    const email = $("login-email").value.trim();
    if (!email) {
      setText("auth-output", "Enter your email address first.", "error");
      return;
    }
    setText("auth-output", "Requesting reset email…", "muted");
    try {
      const result = await api("/v1/auth/password-reset/request", {
        method: "POST",
        json: { email }
      });
      setText("auth-output", result.message, "muted ok");
    } catch (error) {
      setText("auth-output", error.message, "error");
    }
  }

  async function resendVerification() {
    if (!state.pendingVerificationEmail) return;
    setText("auth-output", "Requesting another verification email…", "muted");
    try {
      const result = await api("/v1/auth/resend-verification", {
        method: "POST",
        json: { email: state.pendingVerificationEmail }
      });
      setText("auth-output", result.message, "muted ok");
    } catch (error) {
      setText("auth-output", error.message, "error");
    }
  }

  async function verifyEmailFromLink() {
    const token = queryParam("verify");
    if (!token) return;
    setText("auth-output", "Verifying email…", "muted");
    try {
      const account = await api("/v1/auth/verify-email", {
        method: "POST",
        json: { token }
      });
      state.account = account;
      state.authenticated = true;
      state.role = account.role || "";
      state.pendingVerificationEmail = "";
      renderAuthState();
      navigateTo("packs", { welcome: "1" }, { replace: true });
      showToast("Email verified. Choose your first challenge pack.");
    } catch (error) {
      setText("auth-output", error.message, "error");
    }
  }

  async function completePasswordReset() {
    const token = queryParam("reset");
    const password = $("reset-password").value;
    if (!token || password.length < 12) {
      setText("auth-output", "Enter a password containing at least 12 characters.", "error");
      return;
    }
    setText("auth-output", "Changing password…", "muted");
    try {
      const account = await api("/v1/auth/password-reset/complete", {
        method: "POST",
        json: { token, password }
      });
      $("reset-password").value = "";
      state.account = account;
      state.authenticated = true;
      state.role = account.role || "";
      renderAuthState();
      await refreshAuthenticatedData();
      navigateTo("workspace", {}, { replace: true });
      showToast("Password changed. You are signed in.");
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
    state.auditEvents = [];
    await refreshAuthenticatedData();
    renderAuthState();
    navigateTo("account");
    setText("auth-output", "Signed out.", "muted");
    showToast("You are signed out.", "notice");
  }

  async function saveProfile() {
    const displayName = $("profile-display-name").value.trim();
    if (!displayName) {
      setText("auth-output", "Enter a display name.", "error");
      return;
    }
    setText("auth-output", "Saving profile…", "muted");
    try {
      state.account = await api("/v1/account", {
        method: "PATCH",
        json: { display_name: displayName }
      });
      state.role = state.account.role || "";
      renderAuthState();
      setText("auth-output", "Profile updated.", "muted ok");
      showToast("Profile updated.");
    } catch (error) {
      setText("auth-output", error.message, "error");
    }
  }

  async function changePassword() {
    const currentPassword = $("current-password").value;
    const newPassword = $("new-password").value;
    if (!currentPassword || newPassword.length < 12) {
      setText("auth-output", "Enter your current password and a new password of at least 12 characters.", "error");
      return;
    }
    setText("auth-output", "Changing password…", "muted");
    try {
      state.account = await api("/v1/account/password", {
        method: "POST",
        json: { current_password: currentPassword, new_password: newPassword }
      });
      $("current-password").value = "";
      $("new-password").value = "";
      renderAuthState();
      setText("auth-output", "Password changed. Other browser sessions were signed out.", "muted ok");
      showToast("Password changed securely.");
    } catch (error) {
      setText("auth-output", error.message, "error");
    }
  }

  async function exportAccount() {
    setText("auth-output", "Preparing account export…", "muted");
    try {
      const body = await api("/v1/account/export");
      const blob = new Blob([JSON.stringify(body, null, 2) + "\n"], {
        type: "application/json"
      });
      const url = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = url;
      link.download = "synsigra-account-export.json";
      link.setAttribute("data-no-spa", "true");
      document.body.appendChild(link);
      link.click();
      link.remove();
      window.setTimeout(() => URL.revokeObjectURL(url), 60000);
      setText("auth-output", "Account export downloaded.", "muted ok");
      showToast("Account export downloaded.");
    } catch (error) {
      setText("auth-output", error.message, "error");
    }
  }

  async function deleteAccount() {
    const currentPassword = $("delete-account-password").value;
    const confirmation = $("delete-account-confirmation").value;
    if (!currentPassword || confirmation !== "DELETE MY ACCOUNT") {
      setText("auth-output", "Enter your password and DELETE MY ACCOUNT exactly.", "error");
      return;
    }
    if (!window.confirm("Permanently delete your Synsigra account, workspace, and server-cached packages?")) {
      return;
    }
    setText("auth-output", "Deleting account and workspace…", "muted");
    try {
      const result = await api("/v1/account", {
        method: "DELETE",
        json: { current_password: currentPassword, confirmation }
      });
      state.account = null;
      state.authenticated = false;
      state.role = "";
      state.apiKeys = [];
      state.auditEvents = [];
      state.projects = [];
      state.scenarios = [];
      state.customPacks = [];
      state.jobs = [];
      state.jobsLoaded = false;
      $("delete-account-password").value = "";
      $("delete-account-confirmation").value = "";
      $("delete-account").disabled = true;
      renderAuthState();
      navigateTo("account", {}, { replace: true });
      setText("auth-output", `Account deleted. Receipt: ${result.deletion_receipt_id}`, "muted ok");
      showToast("Your account and workspace were deleted.", "notice");
    } catch (error) {
      setText("auth-output", error.message, "error");
    }
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
        ${key.active ? `<div class="row"><button class="secondary" data-rotate-api-key="${escapeHtml(key.api_key_id)}">Rotate</button><button class="danger" data-revoke-api-key="${escapeHtml(key.api_key_id)}">Revoke</button></div>` : ""}
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
      showToast("API key created. Copy the secret now; it is shown only once.");
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
      showToast("API key revoked.", "notice");
    } catch (error) {
      showToast(error.message, "error");
    }
  }

  async function rotateApiKey(keyId) {
    if (!confirm("Rotate this API key? The current secret will stop working immediately.")) return;
    try {
      const created = await api(`/v1/api-keys/${encodeURIComponent(keyId)}/rotate`, {
        method: "POST"
      });
      $("api-key-secret").textContent =
        `Replacement key created. Copy it now; it will not be shown again:\n${created.api_key}`;
      await Promise.all([loadApiKeys(), loadAuditEvents()]);
      showToast("API key rotated. Update the integration with the replacement secret.");
    } catch (error) {
      showToast(error.message, "error");
    }
  }

  function renderAuditEvents() {
    const panel = $("security-audit-panel");
    const permitted = state.authenticated && ["owner", "admin"].includes(state.role);
    panel.hidden = !permitted;
    if (!permitted) return;
    $("audit-events").innerHTML = state.auditEvents.map((event) => `
      <article class="job">
        <div class="job-header">
          <div><h3>${escapeHtml(event.event_type)}</h3><span class="fingerprint">${escapeHtml(event.subject_type || "service")}${event.subject_id ? ` · ${escapeHtml(event.subject_id)}` : ""}</span></div>
          <span class="muted">${escapeHtml(formatDate(event.created_at))}</span>
        </div>
        <p class="muted compact">Actor ${escapeHtml(event.user_id || "system")}${event.api_key_id ? ` · key ${escapeHtml(event.api_key_id)}` : ""}</p>
      </article>
    `).join("") || "<p class=\"muted\">No audit events yet.</p>";
  }

  async function loadAuditEvents() {
    if (!state.authenticated || !["owner", "admin"].includes(state.role)) {
      state.auditEvents = [];
      renderAuditEvents();
      return;
    }
    try {
      const body = await api("/v1/audit-events?limit=25");
      state.auditEvents = body.audit_events || [];
      renderAuditEvents();
    } catch (error) {
      $("security-audit-panel").hidden = false;
      $("audit-events").innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
    }
  }

  function openRunbook(jobId) {
    state.runbookJobId = jobId;
    navigateTo("verify", { job_id: jobId });
    renderVerificationRunbook();
  }

  function handleJobArtifactAction(target) {
    const runbookJobId = target.getAttribute("data-open-runbook");
    if (runbookJobId) {
      openRunbook(runbookJobId);
      return true;
    }
    const packageId = target.getAttribute("data-download");
    const file = target.getAttribute("data-file");
    if (packageId && file) {
      downloadArtifact(packageId, file);
      return true;
    }
    const kitJobId = target.getAttribute("data-download-kit");
    if (kitJobId) {
      downloadVerificationKit(kitJobId);
      return true;
    }
    const prepareJobId = target.getAttribute("data-prepare-kit");
    if (prepareJobId) {
      prepareVerificationKit(prepareJobId);
      return true;
    }
    const copyValue = target.getAttribute("data-copy-text");
    if (copyValue) {
      copyText(copyValue);
      return true;
    }
    return false;
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
  $("register-terms").addEventListener("change", () => {
    $("register").disabled = !$("register-terms").checked;
  });
  $("request-password-reset").addEventListener("click", requestPasswordReset);
  $("resend-verification").addEventListener("click", resendVerification);
  $("complete-password-reset").addEventListener("click", completePasswordReset);
  $("logout").addEventListener("click", logout);
  $("save-profile").addEventListener("click", saveProfile);
  $("change-password").addEventListener("click", changePassword);
  $("export-account").addEventListener("click", exportAccount);
  $("delete-account-confirmation").addEventListener("input", () => {
    $("delete-account").disabled =
      $("delete-account-confirmation").value !== "DELETE MY ACCOUNT";
  });
  $("delete-account").addEventListener("click", deleteAccount);
  $("create-api-key").addEventListener("click", createApiKey);
  $("refresh-audit").addEventListener("click", loadAuditEvents);
  $("api-keys").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const keyId = target.getAttribute("data-revoke-api-key");
    if (keyId) revokeApiKey(keyId);
    const rotateKeyId = target.getAttribute("data-rotate-api-key");
    if (rotateKeyId) rotateApiKey(rotateKeyId);
  });

  $("refresh-packs").addEventListener("click", loadPacks);
  $("pack-target-goals").addEventListener("change", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLInputElement)) return;
    state.packTargetFilter = target.value;
    renderPackFilters();
    renderPacks();
  });
  $("pack-intent-goals").addEventListener("change", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLInputElement)) return;
    state.packIntentFilter = target.value;
    renderPackFilters();
    renderPacks();
  });
  $("pack-scoring-filter").addEventListener("change", () => {
    state.packScoringFilter = $("pack-scoring-filter").value;
    renderPacks();
  });
  $("pack-difficulty-filter").addEventListener("change", () => {
    state.packDifficultyFilter = $("pack-difficulty-filter").value;
    renderPacks();
  });
  $("reset-pack-filters").addEventListener("click", () => {
    state.packTargetFilter = "";
    state.packIntentFilter = "any";
    state.packScoringFilter = "";
    state.packDifficultyFilter = "";
    $("advanced-pack-filters").open = false;
    renderPackFilters();
    renderPacks();
  });
  $("packs").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const packId = target.getAttribute("data-select-pack");
    if (packId) selectPackForGeneration(packId);
  });
  $("pack-recommendation").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const packId = target.getAttribute("data-select-pack");
    if (packId) selectPackForGeneration(packId);
  });
  $("pack-select").addEventListener("change", renderSelectedPackSummary);
  $("runbook-job-select").addEventListener("change", () => {
    const jobId = $("runbook-job-select").value;
    if (jobId) openRunbook(jobId);
  });
  $("refresh-jobs").addEventListener("click", () => loadJobs({ force: true }));
  $("refresh-verifier-downloads").addEventListener("click", loadVerifierDownloads);
  $("refresh-usage").addEventListener("click", loadUsage);
  $("refresh-metrics").addEventListener("click", loadMetrics);
  $("load-more-jobs").addEventListener("click", () => loadJobs({ more: true }));
  $("new-scenario").addEventListener("click", newScenario);
  $("refresh-authoring").addEventListener("click", loadAuthoring);
  $("apply-scenario-template").addEventListener("click", applyScenarioTemplate);
  $("curated-clone-pack").addEventListener("change", renderCuratedCaseOptions);
  $("clone-curated-scenario").addEventListener("click", cloneCuratedScenario);
  $("scenario-targets").addEventListener("change", () => {
    state.scenarioTargets = currentScenarioTargets();
    renderAuthoringTemplates();
    renderCuratedCloneOptions();
    renderScenarioTargetGuidance();
    scheduleScenarioPreview();
  });
  $("show-all-scenario-sources").addEventListener("change", () => {
    state.showAllScenarioSources = $("show-all-scenario-sources").checked;
    renderAuthoringTemplates();
    renderCuratedCloneOptions();
  });
  $("scenario-form").addEventListener("change", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    if (target.hasAttribute("data-array-channel")) {
      updateScenarioArrayChannels(target);
      return;
    }
    if (target.hasAttribute("data-array-path")) {
      updateScenarioArrayItem(target);
      return;
    }
    const field = target.getAttribute("data-authoring-field");
    if (field) updateScenarioField(field, target);
  });
  $("scenario-form").addEventListener("input", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLInputElement) || target.type !== "range") return;
    const output = target.closest(".slider-row");
    if (output && output.querySelector("output")) output.querySelector("output").textContent = target.value;
  });
  $("scenario-form").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const addPath = target.getAttribute("data-add-array");
    const removePath = target.getAttribute("data-remove-array");
    const removeTag = target.getAttribute("data-remove-tag");
    if (addPath) addScenarioArrayItem(addPath);
    if (removePath) removeScenarioArrayItem(
      removePath, Number.parseInt(target.getAttribute("data-array-index"), 10));
    if (target.hasAttribute("data-add-tag")) addScenarioTag();
    if (removeTag !== null) removeScenarioTag(Number.parseInt(removeTag, 10));
  });
  $("scenario-form").addEventListener("keydown", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement) || !target.hasAttribute("data-tag-input") || event.key !== "Enter") return;
    event.preventDefault();
    addScenarioTag();
  });
  $("scenario-json").addEventListener("input", () => {
    renderScenarioForm();
    scheduleScenarioPreview();
  });
  $("load-scenario-template").addEventListener("click", loadScenarioTemplate);
  $("format-scenario-json").addEventListener("click", formatScenarioJson);
  $("make-scenario-compatible").addEventListener("click", applyMissingTargetRequirements);
  $("save-scenario").addEventListener("click", saveScenario);
  $("create-custom-pack").addEventListener("click", createCustomPack);
  $("refresh-custom-packs").addEventListener("click", loadCustomPacks);
  $("pack-scenario-options").addEventListener("change", () => {
    syncInheritedPackTargets();
    renderCustomPackReview();
  });
  $("custom-pack-scenario-search").addEventListener("input", filterPackScenarioOptions);
  $("custom-pack-name").addEventListener("input", renderCustomPackReview);
  $("custom-pack-description").addEventListener("input", renderCustomPackReview);
  $("custom-pack-targets").addEventListener("change", () => {
    state.customPackAnalysis = {};
    renderCustomPackReview();
  });
  $("custom-pack-target-override").addEventListener("toggle", () => {
    if (!$("custom-pack-target-override").open) syncInheritedPackTargets();
    renderCustomPackReview();
  });
  $("custom-packs").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const packId = target.getAttribute("data-delete-custom-pack");
    if (packId) deleteCustomPack(packId);
  });
  $("verifier-downloads-content").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const filename = target.getAttribute("data-download-verifier");
    if (filename) downloadVerifierFile(filename);
  });
  $("scenario-preview").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const path = target.getAttribute("data-validation-path");
    if (path) focusScenarioPath(path);
  });
  $("scenarios").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    if (target.hasAttribute("data-empty-new-scenario")) {
      newScenario();
      $("scenario-template-select").focus();
      return;
    }
    const editId = target.getAttribute("data-edit-scenario");
    const deleteId = target.getAttribute("data-delete-scenario");
    const validationPath = target.getAttribute("data-validation-path");
    if (validationPath) focusScenarioPath(validationPath);
    if (editId) editScenario(editId);
    if (deleteId) deleteScenario(deleteId);
  });
  $("create-job").addEventListener("click", createJob);
  $("create-project").addEventListener("click", createProject);
  $("jobs").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    if (handleJobArtifactAction(target)) return;
    const deleteJobId = target.getAttribute("data-delete-job");
    if (deleteJobId) deleteJob(deleteJobId);
    const jobAction = target.getAttribute("data-job-action");
    const actionJobId = target.getAttribute("data-job-id");
    if (jobAction && actionJobId) runJobAction(actionJobId, jobAction);
  });
  $("verification-runbook").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    handleJobArtifactAction(target);
  });
  document.addEventListener("click", (event) => {
    if (event.defaultPrevented || event.button !== 0 ||
        event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) {
      return;
    }
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const link = target.closest("a[href]");
    if (!(link instanceof HTMLAnchorElement) || link.target) return;
    if (link.hasAttribute("download") || link.hasAttribute("data-no-spa")) return;
    const url = new URL(link.href);
    if (url.protocol !== "http:" && url.protocol !== "https:") return;
    if (url.origin !== window.location.origin) return;
    const appPath = url.pathname === base || url.pathname === `${base}/` ||
      url.pathname.startsWith(`${base}/`);
    if (!appPath) return;
    const suffix = url.pathname === base || url.pathname === `${base}/`
      ? "workspace"
      : url.pathname.slice(base.length).replace(/^\/+|\/+$/g, "");
    if (!Object.prototype.hasOwnProperty.call(pageTitles, suffix)) return;
    event.preventDefault();
    navigateTo(suffix, Object.fromEntries(url.searchParams.entries()));
  });

  async function initialize() {
    renderCurrentPage();
    renderAuthState();
    checkHealth();
    loadPacks();
    if (queryParam("verify")) {
      await verifyEmailFromLink();
    }
    await loadSession();
    await refreshAuthenticatedData();
  }

  initialize();
  window.addEventListener("popstate", renderCurrentPage);
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
           path_at_or_below(uri, public_base_path + "/v1/audit-events") ||
           path_at_or_below(uri, public_base_path + "/v1/account") ||
           path_at_or_below(uri, public_base_path + "/v1/api-keys") ||
           path_at_or_below(uri, public_base_path + "/v1/downloads") ||
           path_at_or_below(uri, public_base_path + "/v1/authoring") ||
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
    const std::string& cookie_header,
    const EmailConfig& email_config,
    const std::string& range_header
) {
    if (!owns_uri(uri, public_base_path)) {
        RouteResponse response;
        response.disposition = RouteDisposition::declined;
        response.status = 0;
        return response;
    }

    if (uri == public_base_path + "/v1/legal") {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Legal metadata only accepts GET.\"}}\n"
            );
        }
        json_t* root = json_object();
        json_object_set_new(
            root, "terms_version", json_string(kTermsVersion));
        json_object_set_new(
            root, "terms_url",
            json_string((public_base_path + "/legal/terms").c_str()));
        json_object_set_new(
            root, "privacy_url",
            json_string((public_base_path + "/legal/privacy").c_str()));
        json_object_set_new(
            root, "support_url", json_string(kSupportUrl));
        json_object_set_new(
            root, "support_email", json_string(kSupportEmail));
        json_object_set_new(
            root, "privacy_email", json_string(kSupportEmail));
        json_object_set_new(
            root, "operator_name", json_string(kOperatorName));
        json_object_set_new(
            root, "operator_address", json_string(kOperatorAddress));
        json_object_set_new(
            root, "demo_url",
            json_string("mailto:synsigra@gmail.com?subject=Synsigra%20technical%20demo"));
        json_object_set_new(root, "billing_status", json_string("free_beta"));
        json_object_set_new(root, "uptime_sla", json_null());
        json_object_set_new(root, "artifact_retention_days", json_integer(7));
        const std::string encoded = json_dump_line(root);
        json_decref(root);
        RouteResponse response = json_response(200, encoded);
        response.cache_control = "public, max-age=300";
        return response;
    }

    if (uri == public_base_path + "/openapi.yaml") {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"The OpenAPI document only accepts GET.\"}}\n"
            );
        }
        RouteResponse response;
        response.disposition = RouteDisposition::handled;
        response.status = 200;
        response.content_type = "application/yaml; charset=utf-8";
        response.cache_control = "public, max-age=300";
        response.body = syn_sig_ra::kOpenApiYaml;
        return response;
    }

    const std::string viewer_path = public_base_path + "/viewer";
    if (uri == viewer_path || uri == viewer_path + "/" ||
        uri == viewer_path + "/style.css" ||
        uri == viewer_path + "/signal-viewer.js" ||
        uri == viewer_path + "/app.js") {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Signal viewer assets only accept GET.\"}}\n"
            );
        }
        RouteResponse response;
        response.disposition = RouteDisposition::handled;
        response.status = 200;
        if (uri == viewer_path || uri == viewer_path + "/") {
            response.content_type = "text/html; charset=utf-8";
            response.cache_control = "no-store";
            response.body = replace_all(
                kViewerHtml, "__SYNSIGRA_BASE__", public_base_path);
        } else if (uri == viewer_path + "/style.css") {
            response.content_type = "text/css; charset=utf-8";
            response.cache_control = "public, max-age=300";
            response.body = kViewerCss;
        } else if (uri == viewer_path + "/signal-viewer.js") {
            response.content_type = "application/javascript; charset=utf-8";
            response.cache_control = "public, max-age=300";
            response.body = kSignalViewerJs;
        } else {
            response.content_type = "application/javascript; charset=utf-8";
            response.cache_control = "public, max-age=300";
            response.body = kViewerAppJs;
        }
        return response;
    }

    const std::string ready_path = public_base_path + "/readyz";
    if (uri == ready_path) {
        if (method != "GET") {
            return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Readiness only accepts GET.\"}}\n");
        }
        std::string database_error;
        const bool database_ok = metadata_store != nullptr &&
            metadata_store->initialize(database_error);
        CoreIntegrationContract contract;
        std::string contract_error;
        const bool generator_ok = !signal_synth_cli.empty() &&
            validate_core_integration(signal_synth_cli, contract, contract_error);
        std::vector<PackSummary> packs;
        std::string pack_error;
        const bool packs_ok = PackCatalog(pack_root).list(packs, pack_error) &&
            !packs.empty();
        struct statvfs disk;
        const bool storage_ok = !data_root.empty() &&
            statvfs(data_root.c_str(), &disk) == 0 &&
            access(data_root.c_str(), W_OK) == 0;
        unsigned long long disk_free = 0;
        unsigned long long disk_reserve = 0;
        const bool generation_capacity = storage_ok &&
            disk_generation_capacity(
                data_root, disk_free, disk_reserve);
        const bool ready = database_ok && generator_ok && packs_ok &&
            storage_ok && generation_capacity;
        json_t* body = json_object();
        json_object_set_new(body, "status", json_string(ready ? "ready" : "not_ready"));
        json_object_set_new(body, "database", json_boolean(database_ok));
        json_object_set_new(body, "generator", json_boolean(generator_ok));
        if (generator_ok) {
            json_t* producer = json_object();
            json_object_set_new(producer, "integration_contract", json_string(
                contract.integration_contract.c_str()));
            json_object_set_new(producer, "version", json_string(
                contract.generator_version.c_str()));
            json_object_set_new(producer, "git_commit", json_string(
                contract.generator_git_commit.c_str()));
            json_object_set_new(producer, "build_identity", json_string(
                contract.generator_build_identity.c_str()));
            json_object_set_new(producer, "challenge_package", json_string(
                contract.challenge_package.c_str()));
            json_object_set_new(producer, "scoring_manifest", json_string(
                contract.scoring_manifest.c_str()));
            json_object_set_new(producer, "cpp_facade", json_string(
                contract.cpp_facade.c_str()));
            json_object_set_new(producer, "pack_schema_version", json_integer(
                contract.pack_schema_version));
            json_object_set_new(producer, "verification_protocol", json_string(
                contract.verification_protocol.c_str()));
            json_object_set_new(producer, "submission", json_string(
                contract.submission.c_str()));
            json_object_set_new(producer, "submission_formats", json_string(
                contract.submission_formats.c_str()));
            json_object_set_new(producer, "measurement_values", json_string(
                contract.measurement_values.c_str()));
            json_object_set_new(producer, "measurement_truth", json_string(
                contract.measurement_truth.c_str()));
            json_object_set_new(producer, "measurement_scoring", json_string(
                contract.measurement_scoring.c_str()));
            json_object_set_new(producer, "local_verification", json_string(
                contract.local_verification.c_str()));
            json_object_set_new(producer, "scenario_authoring", json_string(
                contract.scenario_authoring.c_str()));
            json_object_set_new(producer, "scenario_templates", json_string(
                contract.scenario_templates.c_str()));
            json_object_set_new(producer, "python_verifier", json_string(
                contract.python_verifier.c_str()));
            json_object_set_new(producer, "external_noise_truth", json_string(
                contract.external_noise_truth.c_str()));
            json_error_t contract_document_error;
            json_t* contract_document = json_loadb(
                contract.canonical_json.data(), contract.canonical_json.size(),
                JSON_REJECT_DUPLICATES, &contract_document_error);
            if (json_is_object(contract_document)) {
                json_object_set_new(
                    producer, "contract_document", contract_document);
            } else if (contract_document != nullptr) {
                json_decref(contract_document);
            }
            json_object_set_new(body, "accepted_core", producer);
        }
        json_object_set_new(body, "pack_catalog", json_boolean(packs_ok));
        json_object_set_new(body, "artifact_store", json_boolean(storage_ok));
        json_object_set_new(
            body, "generation_capacity", json_boolean(generation_capacity));
        if (storage_ok) {
            json_object_set_new(body, "disk_free_bytes", json_integer(
                static_cast<json_int_t>(disk_free)));
            json_object_set_new(body, "disk_reserve_bytes", json_integer(
                static_cast<json_int_t>(disk_reserve)));
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
        if (uri == auth_path + "/verify-email") {
            if (method != "POST") {
                return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Email verification only accepts POST.\"}}\n");
            }
            std::string token;
            if (!parse_token_request(content_type, request_body, token)) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_token\","
                    "\"message\":\"The verification link is invalid or expired.\"}}\n");
            }
            std::string token_hash;
            std::string error;
            if (!sha256_hex(token, token_hash, error)) {
                return internal_account_error(
                    "Email verification is temporarily unavailable.", error);
            }
            const RateLimitStatus rate = metadata_store->record_email_token_submission(
                "email_verification", token_hash, 10, 15, error);
            if (rate == RateLimitStatus::limited) {
                return json_response(429, "{\"error\":{\"code\":\"verification_rate_limit\","
                    "\"message\":\"Too many verification attempts. Try again later.\"}}\n");
            }
            if (rate == RateLimitStatus::storage_error) {
                return internal_account_error(
                    "Email verification is temporarily unavailable.", error);
            }
            AccountRecord account;
            const EmailTokenConsumeStatus consumed =
                metadata_store->verify_email_token(token_hash, account, error);
            if (consumed == EmailTokenConsumeStatus::invalid_or_expired) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_or_expired_token\","
                    "\"message\":\"The verification link is invalid or expired.\"}}\n");
            }
            if (consumed != EmailTokenConsumeStatus::consumed) {
                return internal_account_error(
                    "Email verification is temporarily unavailable.", error);
            }
            RouteResponse response;
            if (!issue_browser_session(*metadata_store, account, response, error)) {
                return internal_account_error("Unable to start the session.", error);
            }
            return response;
        }
        if (uri == auth_path + "/resend-verification" ||
            uri == auth_path + "/password-reset/request") {
            if (method != "POST") {
                return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"This email operation only accepts POST.\"}}\n");
            }
            if (!email_delivery_configured(email_config)) {
                return json_response(503, "{\"error\":{\"code\":\"email_delivery_unavailable\","
                    "\"message\":\"Email delivery is not configured yet.\"}}\n");
            }
            std::string email;
            if (!parse_email_request(content_type, request_body, email)) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_email_request\","
                    "\"message\":\"Enter a valid email address.\"}}\n");
            }
            const bool verification = uri == auth_path + "/resend-verification";
            const std::string purpose = verification
                ? "email_verification" : "password_reset";
            std::string email_hash;
            std::string error;
            if (!sha256_hex(purpose + ":" + email, email_hash, error)) {
                RouteResponse response = generic_email_accepted_response();
                response.internal_error = error;
                return response;
            }
            const RateLimitStatus rate = metadata_store->record_email_send_attempt(
                purpose, email_hash, 3, 60, error);
            if (rate == RateLimitStatus::storage_error) {
                return internal_account_error(
                    "Email delivery is temporarily unavailable.", error);
            }
            if (rate == RateLimitStatus::limited) {
                return generic_email_accepted_response();
            }
            AccountRecord account;
            const RecordLookupStatus found =
                metadata_store->find_account_by_email(email, account, error);
            if (found == RecordLookupStatus::storage_error) {
                return internal_account_error(
                    "Email delivery is temporarily unavailable.", error);
            }
            const bool eligible = found == RecordLookupStatus::found &&
                (verification ? !account.email_verified : account.email_verified);
            if (!eligible) return generic_email_accepted_response();
            const AccountEmailDeliveryStatus delivered = deliver_account_email(
                *metadata_store, email_config, account, purpose,
                public_base_path, error);
            if (delivered == AccountEmailDeliveryStatus::storage_error) {
                return internal_account_error(
                    "Email delivery is temporarily unavailable.", error);
            }
            RouteResponse response = generic_email_accepted_response();
            if (delivered == AccountEmailDeliveryStatus::provider_error) {
                response.internal_error = error;
            }
            return response;
        }
        if (uri == auth_path + "/password-reset/complete") {
            if (method != "POST") {
                return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Password reset only accepts POST.\"}}\n");
            }
            std::string token;
            std::string password;
            if (!parse_password_reset_request(
                    content_type, request_body, token, password)) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_password_reset\","
                    "\"message\":\"Enter a valid reset link and new password.\"}}\n");
            }
            std::string salt;
            std::string password_hash;
            std::string token_hash;
            std::string error;
            if (!hash_password(password, salt, password_hash, error)) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_password\","
                    "\"message\":\"Password must contain 12-128 characters.\"}}\n");
            }
            if (!sha256_hex(token, token_hash, error)) {
                return internal_account_error(
                    "Password reset is temporarily unavailable.", error);
            }
            const RateLimitStatus rate = metadata_store->record_email_token_submission(
                "password_reset", token_hash, 10, 15, error);
            if (rate == RateLimitStatus::limited) {
                return json_response(429, "{\"error\":{\"code\":\"reset_rate_limit\","
                    "\"message\":\"Too many reset attempts. Try again later.\"}}\n");
            }
            if (rate == RateLimitStatus::storage_error) {
                return internal_account_error(
                    "Password reset is temporarily unavailable.", error);
            }
            AccountRecord account;
            const EmailTokenConsumeStatus consumed =
                metadata_store->consume_password_reset_token(
                    token_hash, account, error);
            if (consumed == EmailTokenConsumeStatus::invalid_or_expired) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_or_expired_token\","
                    "\"message\":\"The reset link is invalid or expired.\"}}\n");
            }
            if (consumed != EmailTokenConsumeStatus::consumed ||
                !metadata_store->update_account_password(
                    account.user_id, salt, password_hash, error) ||
                !metadata_store->delete_sessions_for_user(account.user_id, error)) {
                return internal_account_error("Unable to reset the password.", error);
            }
            RouteResponse response;
            if (!issue_browser_session(*metadata_store, account, response, error)) {
                return internal_account_error(
                    "Password changed, but sign-in failed.", error);
            }
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
        json_t* accept_terms_value = root == nullptr
            ? nullptr : json_object_get(root, "accept_terms");
        json_t* terms_version_value = root == nullptr
            ? nullptr : json_object_get(root, "terms_version");
        const bool terms_accepted = !registration ||
            (json_is_true(accept_terms_value) &&
             json_is_string(terms_version_value) &&
             std::string(json_string_value(terms_version_value)) ==
                 kTermsVersion);
        if (registration && json_is_object(root) && !terms_accepted) {
            json_decref(root);
            return json_response(
                400,
                "{\"error\":{\"code\":\"terms_acceptance_required\","
                "\"message\":\"Accept the current Private Beta Terms and "
                "Privacy & No-PHI Notice before registering.\"}}\n"
            );
        }
        std::string email;
        const bool valid_shape = json_is_object(root) &&
            json_is_string(email_value) && json_is_string(password_value) &&
            (!registration || json_is_string(name_value)) &&
            terms_accepted &&
            json_object_size(root) == (registration ? 5u : 2u);
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
            if (!email_delivery_configured(email_config)) {
                return json_response(503, "{\"error\":{\"code\":\"email_delivery_unavailable\","
                    "\"message\":\"Registration is temporarily unavailable because email delivery is not configured.\"}}\n");
            }
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
                email, display_name, salt, password_hash, kTermsVersion,
                account, error);
            if (created == AccountCreateStatus::email_exists) {
                return generic_email_accepted_response();
            }
            if (created != AccountCreateStatus::created) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Unable to create the account.\"}}\n");
                response.internal_error = error;
                return response;
            }
            const AccountEmailDeliveryStatus delivered = deliver_account_email(
                *metadata_store, email_config, account, "email_verification",
                public_base_path, error);
            if (delivered != AccountEmailDeliveryStatus::delivered) {
                return internal_account_error(
                    "The account was created, but its verification email could not be sent. Try resending it shortly.",
                    error);
            }
            RouteResponse response = json_response(
                202, "{\"status\":\"verification_required\","
                "\"message\":\"Check your inbox and verify your email before signing in.\"}\n");
            response.cache_control = "no-store";
            return response;
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
            if (!account.email_verified) {
                return json_response(403, "{\"error\":{\"code\":\"email_verification_required\","
                    "\"message\":\"Verify your email before signing in.\"}}\n");
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
    AccountRecord authenticated_account;
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
            ? authenticate_session(
                cookie_header, *metadata_store, &authenticated_account)
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
        if (authorization_header.empty() &&
            !authenticated_account.email_verified) {
            return json_response(403, "{\"error\":{\"code\":\"email_verification_required\","
                "\"message\":\"Verify your email before continuing.\"}}\n");
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

    const std::string account_path = public_base_path + "/v1/account";
    if (path_at_or_below(uri, account_path)) {
        AccountRecord account;
        std::string error;
        const RecordLookupStatus account_status = metadata_store->find_account(
            authenticated_identity, account, error);
        if (account_status == RecordLookupStatus::not_found) {
            return json_response(404, "{\"error\":{\"code\":\"account_not_found\","
                "\"message\":\"The account no longer exists.\"}}\n");
        }
        if (account_status != RecordLookupStatus::found) {
            RouteResponse response = json_response(
                503, "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Account storage is unavailable.\"}}\n");
            response.internal_error = error;
            return response;
        }

        if (uri == account_path + "/export") {
            if (method != "GET") {
                return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Account export only accepts GET.\"}}\n");
            }
            std::string body;
            if (!build_account_export(
                    *metadata_store, authenticated_identity,
                    public_base_path, body, error)) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"account_export_unavailable\","
                    "\"message\":\"The account export could not be created.\"}}\n");
                response.internal_error = error;
                return response;
            }
            RouteResponse response = json_response(200, body);
            response.content_disposition =
                "attachment; filename=\"synsigra-account-export.json\"";
            response.cache_control = "no-store";
            return response;
        }

        if (uri == account_path + "/password") {
            if (method != "POST") {
                return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Password change only accepts POST.\"}}\n");
            }
            std::vector<std::string> fields;
            if (!parse_exact_string_fields(
                    content_type, request_body,
                    std::vector<std::string>{"current_password", "new_password"},
                    fields) || fields[0].size() > 128u ||
                fields[1].size() > 128u) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_password_change\","
                    "\"message\":\"Current and new passwords are required.\"}}\n");
            }
            bool matches = false;
            if (!verify_password(
                    fields[0], account.password_salt, account.password_hash,
                    matches, error)) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"password_check_unavailable\","
                    "\"message\":\"The password could not be checked.\"}}\n");
                response.internal_error = error;
                return response;
            }
            if (!matches) {
                return json_response(401, "{\"error\":{\"code\":\"current_password_incorrect\","
                    "\"message\":\"The current password is incorrect.\"}}\n");
            }
            if (fields[0] == fields[1]) {
                return json_response(400, "{\"error\":{\"code\":\"password_unchanged\","
                    "\"message\":\"Choose a different new password.\"}}\n");
            }
            std::string salt;
            std::string password_hash;
            if (!hash_password(fields[1], salt, password_hash, error)) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_password\","
                    "\"message\":\"Password must contain 12-128 characters.\"}}\n");
            }
            if (!metadata_store->change_account_password(
                    authenticated_identity, salt, password_hash, error)) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"password_change_failed\","
                    "\"message\":\"The password could not be changed.\"}}\n");
                response.internal_error = error;
                return response;
            }
            if (authorization_header.empty()) {
                account.password_salt = salt;
                account.password_hash = password_hash;
                RouteResponse response;
                if (!issue_browser_session(
                        *metadata_store, account, response, error)) {
                    return internal_account_error(
                        "Password changed, but sign-in failed.", error);
                }
                return response;
            }
            RouteResponse response = json_response(
                200, "{\"status\":\"password_changed\"}\n");
            response.cache_control = "no-store";
            return response;
        }

        if (uri != account_path) {
            return json_response(404, "{\"error\":{\"code\":\"account_route_not_found\","
                "\"message\":\"The account operation does not exist.\"}}\n");
        }
        if (method == "PATCH") {
            std::vector<std::string> fields;
            if (!parse_exact_string_fields(
                    content_type, request_body,
                    std::vector<std::string>{"display_name"}, fields)) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_profile\","
                    "\"message\":\"A display name is required.\"}}\n");
            }
            bool visible = false;
            for (std::string::const_iterator character = fields[0].begin();
                 character != fields[0].end(); ++character) {
                if (!std::isspace(static_cast<unsigned char>(*character))) {
                    visible = true;
                    break;
                }
            }
            if (!visible || fields[0].size() > 100u) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_display_name\","
                    "\"message\":\"Display name must contain 1-100 characters.\"}}\n");
            }
            AccountRecord updated;
            const RecordLookupStatus status =
                metadata_store->update_account_display_name(
                    authenticated_identity, fields[0], updated, error);
            if (status != RecordLookupStatus::found) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"profile_update_failed\","
                    "\"message\":\"The profile could not be updated.\"}}\n");
                response.internal_error = error;
                return response;
            }
            RouteResponse response = json_response(200, account_json(updated));
            response.cache_control = "no-store";
            return response;
        }
        if (method == "DELETE") {
            std::vector<std::string> fields;
            if (!parse_exact_string_fields(
                    content_type, request_body,
                    std::vector<std::string>{"current_password", "confirmation"},
                    fields) || fields[1] != "DELETE MY ACCOUNT") {
                return json_response(400, "{\"error\":{\"code\":\"deletion_confirmation_required\","
                    "\"message\":\"Enter DELETE MY ACCOUNT exactly.\"}}\n");
            }
            bool matches = false;
            if (!verify_password(
                    fields[0], account.password_salt, account.password_hash,
                    matches, error)) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"password_check_unavailable\","
                    "\"message\":\"The password could not be checked.\"}}\n");
                response.internal_error = error;
                return response;
            }
            if (!matches) {
                return json_response(401, "{\"error\":{\"code\":\"current_password_incorrect\","
                    "\"message\":\"The current password is incorrect.\"}}\n");
            }
            std::string receipt_id;
            std::string verification_hash;
            std::string reset_hash;
            if (!random_id("deletion_", receipt_id, error) ||
                !sha256_hex("email_verification:" + account.email,
                    verification_hash, error) ||
                !sha256_hex("password_reset:" + account.email,
                    reset_hash, error)) {
                return internal_account_error(
                    "Account deletion is temporarily unavailable.", error);
            }
            AccountDeletionResult deleted;
            const AccountDeleteStatus status = metadata_store->delete_owned_account(
                authenticated_identity, verification_hash, reset_hash,
                receipt_id, deleted, error);
            if (status == AccountDeleteStatus::forbidden) {
                return json_response(403, "{\"error\":{\"code\":\"owner_required\","
                    "\"message\":\"Only a workspace owner can delete it.\"}}\n");
            }
            if (status == AccountDeleteStatus::shared_workspace) {
                return json_response(409, "{\"error\":{\"code\":\"shared_workspace\","
                    "\"message\":\"Transfer or remove other members before deleting this workspace.\"}}\n");
            }
            if (status == AccountDeleteStatus::running_jobs) {
                return json_response(409, "{\"error\":{\"code\":\"running_jobs\","
                    "\"message\":\"Wait for running jobs to finish before deleting the workspace.\"}}\n");
            }
            if (status == AccountDeleteStatus::not_found) {
                return json_response(404, "{\"error\":{\"code\":\"account_not_found\","
                    "\"message\":\"The account no longer exists.\"}}\n");
            }
            if (status != AccountDeleteStatus::deleted) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"account_deletion_failed\","
                    "\"message\":\"The account could not be deleted.\"}}\n");
                response.internal_error = error;
                return response;
            }
            std::string cleanup_error;
            const bool cleaned = purge_account_storage(
                data_root, deleted.package_ids, deleted.job_ids,
                deleted.custom_pack_ids, cleanup_error);
            json_t* body = json_object();
            json_object_set_new(body, "status", json_string("deleted"));
            json_object_set_new(
                body, "deletion_receipt_id", json_string(receipt_id.c_str()));
            json_object_set_new(body, "server_workspace",
                json_string(cleaned ? "deleted" : "cleanup_requires_operator"));
            json_object_set_new(body, "downloaded_artifacts",
                json_string("not_revocable"));
            const std::string encoded = json_dump_line(body);
            json_decref(body);
            RouteResponse response = json_response(200, encoded);
            response.cache_control = "no-store";
            response.set_cookie =
                "syn_sig_ra_session=; Path=/syn_sig_ra; Max-Age=0; "
                "Secure; HttpOnly; SameSite=Lax";
            if (!cleaned) {
                response.internal_error =
                    "account deleted; artifact cleanup failed: " + cleanup_error;
            }
            return response;
        }
        return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
            "\"message\":\"Use PATCH to update the profile, DELETE to remove the account, GET /export, or POST /password.\"}}\n");
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
        const std::string key_action = collection
            ? std::string() : uri.substr(api_keys_path.size() + 1);
        const std::string rotate_suffix = "/rotate";
        const bool rotation = key_action.size() > rotate_suffix.size() &&
            key_action.compare(
                key_action.size() - rotate_suffix.size(),
                rotate_suffix.size(), rotate_suffix) == 0;
        if (!collection && method == "POST" && rotation) {
            const std::string key_id = key_action.substr(
                0, key_action.size() - rotate_suffix.size());
            if (key_id.empty() || key_id.find('/') != std::string::npos) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_api_key_id\","
                    "\"message\":\"The API key ID is invalid.\"}}\n");
            }
            std::string replacement_id;
            std::string token;
            std::string key_hash;
            std::string error;
            ApiKeyRecord replacement;
            if (!random_id("key_", replacement_id, error) ||
                !random_id("ssk_", token, error) ||
                !sha256_hex(token, key_hash, error)) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"key_generation_failed\","
                    "\"message\":\"Unable to generate the replacement key.\"}}\n");
                response.internal_error = error;
                return response;
            }
            const RecordLookupStatus status =
                metadata_store->rotate_personal_api_key(
                    authenticated_identity, key_id, replacement_id,
                    key_hash, replacement, error);
            if (status == RecordLookupStatus::storage_error) {
                RouteResponse response = json_response(
                    503, "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Unable to rotate the API key.\"}}\n");
                response.internal_error = error;
                return response;
            }
            if (status == RecordLookupStatus::not_found) {
                return json_response(404, "{\"error\":{\"code\":\"api_key_not_found\","
                    "\"message\":\"Active API key not found.\"}}\n");
            }
            json_t* body = json_object();
            json_object_set_new(
                body, "api_key_id", json_string(replacement_id.c_str()));
            json_object_set_new(body, "api_key", json_string(token.c_str()));
            json_object_set_new(
                body, "label", json_string(replacement.label.c_str()));
            json_object_set_new(
                body, "rotated_api_key_id", json_string(key_id.c_str()));
            const std::string encoded = json_dump_line(body);
            json_decref(body);
            RouteResponse response = json_response(201, encoded);
            response.cache_control = "no-store";
            return response;
        }
        if (!collection && method == "DELETE" && !rotation) {
            const std::string key_id = key_action;
            if (key_id.empty() || key_id.find('/') != std::string::npos) {
                return json_response(400, "{\"error\":{\"code\":\"invalid_api_key_id\","
                    "\"message\":\"The API key ID is invalid.\"}}\n");
            }
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

    const std::string audit_path = public_base_path + "/v1/audit-events";
    if (uri == audit_path) {
        if (method != "GET") {
            return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Audit export only accepts GET.\"}}\n");
        }
        if (authenticated_identity.role != "owner" &&
            authenticated_identity.role != "admin") {
            return json_response(403, "{\"error\":{\"code\":\"forbidden\","
                "\"message\":\"Owner or admin role is required for audit export.\"}}\n");
        }
        int limit = 100;
        int offset = 0;
        std::string format;
        query_value(query_string, "format", format);
        if (!query_integer(query_string, "limit", 100, limit) ||
            !query_integer(query_string, "offset", 0, offset) ||
            limit < 1 || limit > 1000 ||
            (!format.empty() && format != "json" && format != "csv")) {
            return json_response(400, "{\"error\":{\"code\":\"invalid_audit_export\","
                "\"message\":\"Use limit 1-1000, a non-negative offset, and format json or csv.\"}}\n");
        }
        std::vector<AuditEventRecord> events;
        std::string error;
        if (!metadata_store->list_audit_events(
                authenticated_identity, limit, offset, events, error)) {
            RouteResponse response = json_response(
                503, "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Audit storage is unavailable.\"}}\n");
            response.internal_error = error;
            return response;
        }
        RouteResponse response;
        if (format == "csv") {
            response.disposition = RouteDisposition::handled;
            response.status = 200;
            response.content_type = "text/csv; charset=utf-8";
            response.content_disposition =
                "attachment; filename=\"synsigra-audit-events.csv\"";
            response.body = audit_event_list_csv(events);
        } else {
            response = json_response(
                200, audit_event_list_json(events, limit, offset));
        }
        response.cache_control = "no-store";
        return response;
    }

    const std::string downloads_path = public_base_path + "/v1/downloads";
    if (path_at_or_below(uri, downloads_path)) {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Download endpoints only accept GET.\"}}\n"
            );
        }
        const std::string verifier_path = downloads_path + "/verifier";
        if (uri == verifier_path || uri == verifier_path + "/metadata.json") {
            return download_file_response(
                verifier_download_root(pack_root),
                "metadata.json"
            );
        }
        if (!path_at_or_below(uri, verifier_path)) {
            return json_response(
                404,
                "{\"error\":{\"code\":\"download_not_found\","
                "\"message\":\"The requested download collection does not exist.\"}}\n"
            );
        }
        const std::string filename = uri.substr(verifier_path.size() + 1);
        if (filename.find('/') != std::string::npos) {
            return json_response(
                400,
                "{\"error\":{\"code\":\"invalid_download_path\","
                "\"message\":\"The download path is invalid.\"}}\n"
            );
        }
        return download_file_response(verifier_download_root(pack_root), filename);
    }

    const std::string authoring_path = public_base_path + "/v1/authoring";
    if (path_at_or_below(uri, authoring_path)) {
        if (uri == authoring_path + "/schema") {
            if (method != "GET") {
                return json_response(
                    405,
                    "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Authoring schema only accepts GET.\"}}\n"
                );
            }
            return json_response(
                200,
                signal_synth::scenario_authoring_metadata_json() + "\n"
            );
        }
        if (uri == authoring_path + "/templates") {
            if (method != "GET") {
                return json_response(
                    405,
                    "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Authoring templates only accept GET.\"}}\n"
                );
            }
            return json_response(
                200,
                signal_synth::scenario_template_catalog_json() + "\n"
            );
        }
        if (uri == authoring_path + "/preview") {
            if (method != "POST") {
                return json_response(
                    405,
                    "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Scenario preview only accepts POST.\"}}\n"
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
            json_t* submitted = json_loadb(
                request_body.data(),
                request_body.size(),
                JSON_REJECT_DUPLICATES,
                &parse_error
            );
            syn_sig_ra::RouteResponse response =
                authoring_preview_response(submitted);
            if (submitted != nullptr) json_decref(submitted);
            return response;
        }
        const std::string curated_path =
            authoring_path + "/curated-scenarios";
        if (path_at_or_below(uri, curated_path)) {
            if (method != "GET") {
                return json_response(
                    405,
                    "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Curated scenario clone only accepts GET.\"}}\n"
                );
            }
            const std::string relative =
                uri.size() > curated_path.size()
                    ? uri.substr(curated_path.size() + 1)
                    : std::string();
            const std::string::size_type separator = relative.find('/');
            if (separator == std::string::npos ||
                relative.find('/', separator + 1) != std::string::npos) {
                return json_response(
                    400,
                    "{\"error\":{\"code\":\"invalid_curated_scenario_path\","
                    "\"message\":\"Use /curated-scenarios/{pack_id}/{case_id}.\"}}\n"
                );
            }
            const std::string pack_id = relative.substr(0, separator);
            const std::string case_id = relative.substr(separator + 1);
            std::string scenario_json;
            std::string error;
            if (!read_curated_scenario(
                    pack_root, pack_id, case_id, scenario_json, error)) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"curated_scenario_not_found\","
                    "\"message\":\"The curated scenario could not be cloned.\"}}\n"
                );
            }
            json_error_t parse_error;
            json_t* scenario = json_loads(
                scenario_json.c_str(),
                JSON_REJECT_DUPLICATES,
                &parse_error
            );
            if (!json_is_object(scenario)) {
                if (scenario != nullptr) json_decref(scenario);
                return json_response(
                    500,
                    "{\"error\":{\"code\":\"curated_scenario_invalid\","
                    "\"message\":\"The curated scenario JSON is invalid.\"}}\n"
                );
            }
            json_t* root = json_object();
            json_object_set_new(root, "pack_id", json_string(pack_id.c_str()));
            json_object_set_new(root, "case_id", json_string(case_id.c_str()));
            json_object_set_new(root, "scenario", scenario);
            const std::string body = json_dump_line(root);
            json_decref(root);
            return json_response(200, body);
        }
        return json_response(
            404,
            "{\"error\":{\"code\":\"authoring_route_not_found\","
            "\"message\":\"The requested authoring endpoint does not exist.\"}}\n"
        );
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
        signal_synth::ecg_pack_manifest analysis_manifest;
        analysis_manifest.pack_id = "custom_pack_preflight";
        analysis_manifest.name = json_string_value(name);
        analysis_manifest.version = "preview";
        analysis_manifest.description = json_string_value(description);
        analysis_manifest.targets = target_values;
        std::vector<signal_synth::ecg_scenario_document> analyzed_scenarios;
        std::set<std::string> analyzed_ids;
        for (std::vector<ScenarioDraftRecord>::const_iterator it = drafts.begin();
             it != drafts.end(); ++it) {
            signal_synth::ecg_scenario_document document;
            signal_synth::ecg_scenario_json_result validation;
            if (!signal_synth::parse_ecg_scenario_json(
                    it->document_json, document, validation) ||
                document.scenario_id.empty() ||
                !analyzed_ids.insert(document.scenario_id).second) {
                json_decref(submitted);
                return json_response(
                    400, "{\"error\":{\"code\":\"duplicate_or_invalid_scenario_id\","
                    "\"message\":\"Selected drafts must contain valid, unique scenario document IDs.\"}}\n");
            }
            signal_synth::ecg_pack_scenario scenario_item;
            scenario_item.id = document.scenario_id;
            scenario_item.path = document.scenario_id + ".json";
            scenario_item.targets = target_values;
            analysis_manifest.scenarios.push_back(scenario_item);
            analyzed_scenarios.push_back(document);
        }
        signal_synth::scenario_pack_analysis pack_analysis;
        if (!signal_synth::analyze_scenario_pack(
                analysis_manifest, analyzed_scenarios, pack_analysis)) {
            const std::string analysis =
                signal_synth::scenario_pack_analysis_json(pack_analysis);
            json_decref(submitted);
            return json_response(
                422,
                "{\"error\":{\"code\":\"custom_pack_incompatible\","
                "\"message\":\"Selected scenarios do not satisfy every requested target.\"},"
                "\"analysis\":" + analysis + "}\n"
            );
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
        json_object_set_new(pack_json, "schema_version", json_integer(2));
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
        json_t* target_intent = submitted == nullptr
            ? nullptr : json_object_get(submitted, "target_intent");
        if (!json_is_object(submitted) || json_object_size(submitted) != 3 ||
            !json_is_string(name) || !json_is_object(scenario) ||
            !json_is_array(target_intent) || json_array_size(target_intent) == 0 ||
            std::string(json_string_value(name)).empty() ||
            std::string(json_string_value(name)).size() > 100) {
            if (submitted != nullptr) json_decref(submitted);
            return json_response(
                400, "{\"error\":{\"code\":\"invalid_scenario_request\","
                "\"message\":\"name, scenario, and a non-empty target_intent are required.\"}}\n");
        }
        std::set<std::string> unique_target_intent;
        std::size_t target_index = 0;
        json_t* target_item = nullptr;
        bool target_intent_valid = true;
        json_array_foreach(target_intent, target_index, target_item) {
            if (!json_is_string(target_item) ||
                std::string(json_string_value(target_item)).empty() ||
                !unique_target_intent.insert(json_string_value(target_item)).second) {
                target_intent_valid = false;
                break;
            }
        }
        if (!target_intent_valid) {
            json_decref(submitted);
            return json_response(
                400, "{\"error\":{\"code\":\"invalid_scenario_request\","
                "\"message\":\"target_intent must contain unique non-empty target names.\"}}\n");
        }
        char* target_intent_text =
            json_dumps(target_intent, JSON_COMPACT | JSON_SORT_KEYS);
        const std::string target_intent_json = target_intent_text == nullptr
            ? "[]" : target_intent_text;
        free(target_intent_text);
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
                fingerprint, target_intent_json, errors_json, draft, error);
        } else {
            const std::string scenario_id =
                uri.substr(scenarios_path.size() + 1);
            updated = metadata_store->update_scenario_draft(
                scenario_id, authenticated_identity, draft_name, status,
                canonical_json, fingerprint, target_intent_json, errors_json,
                draft, error);
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
            std::string selected_pack_version;
            std::string selected_catalog_version;
            std::string selected_catalog_source_sha256;
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
                selected_pack_version = custom_pack.version;
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
                if (pack.integration_contract_version !=
                    "synsigra_core_integration_v7") {
                    return json_response(
                        409,
                        "{\"error\":{\"code\":\"pack_generator_incompatible\","
                        "\"message\":\"The pack is not compatible with this generator.\"}}\n"
                    );
                }
                CoreIntegrationContract accepted;
                if (!signal_synth_cli.empty() && !validate_core_integration(
                        signal_synth_cli, accepted, error)) {
                    RouteResponse response = json_response(
                        503,
                        "{\"error\":{\"code\":\"generator_contract_mismatch\","
                        "\"message\":\"The generator producer identity is not ready.\"}}\n"
                    );
                    response.internal_error = error;
                    return response;
                }
                source_pack_path =
                    pack_root + "/" + job_request.pack_id + ".json";
                selected_pack_fingerprint = pack.pack_fingerprint;
                selected_pack_version = pack.version;
                selected_catalog_version = pack.catalog_version;
                selected_catalog_source_sha256 = pack.catalog_source_sha256;
            }
            UsageSummary usage;
            unsigned long long disk_free = 0;
            unsigned long long disk_reserve = 0;
            if (!data_root.empty() && !disk_generation_capacity(
                    data_root, disk_free, disk_reserve)) {
                json_t* root = json_object();
                json_t* value = json_object();
                json_object_set_new(
                    value, "code", json_string("artifact_storage_pressure"));
                json_object_set_new(
                    value, "message",
                    json_string(
                        "Generation is temporarily paused to preserve server disk space. "
                        "Retry after retained artifacts are cleaned up."));
                json_object_set_new(
                    value, "disk_free_bytes",
                    json_integer(static_cast<json_int_t>(disk_free)));
                json_object_set_new(
                    value, "disk_reserve_bytes",
                    json_integer(static_cast<json_int_t>(disk_reserve)));
                json_object_set_new(root, "error", value);
                const std::string body = json_dump_line(root);
                json_decref(root);
                return json_response(507, body);
            }
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
                    selected_pack_version,
                    selected_catalog_version,
                    selected_catalog_source_sha256,
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
            RouteResponse response = json_response(202, body);
            record_nonblocking_audit(
                *metadata_store, authenticated_identity, "job.created",
                "job", job_id, "{}", response);
            return response;
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
        if (action == "viewer" || action == "viewer/window" ||
            action == "viewer/overlays") {
            if (method != "GET") {
                return json_response(
                    405,
                    "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Signal viewer endpoints only accept GET.\"}}\n"
                );
            }
            JobRecord job;
            std::string error;
            const RecordLookupStatus lookup = metadata_store->find_job(
                job_id, authenticated_identity, job, error);
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
                    "{\"error\":{\"code\":\"viewer_job_unavailable\","
                    "\"message\":\"Signals are available after the job succeeds and while its package is retained.\"}}\n"
                );
            }
            PackageRecord package;
            const RecordLookupStatus package_lookup = metadata_store->find_package(
                job.package_id, authenticated_identity, package, error);
            if (package_lookup == RecordLookupStatus::not_found) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"artifact_not_found\","
                    "\"message\":\"The retained package is unavailable.\"}}\n"
                );
            }
            if (package_lookup == RecordLookupStatus::storage_error) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Artifact metadata is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            const std::string expected_storage =
                data_root + "/packages/" + job.package_id;
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
            const std::string viewer_root = expected_storage + "/viewer";
            if (action == "viewer") {
                SignalViewerSource source;
                const SignalViewerStatus viewer_status =
                    describe_signal_viewer_source(viewer_root, source, error);
                if (viewer_status == SignalViewerStatus::ok) {
                    RouteResponse response = json_response(
                        200, signal_viewer_source_json(
                            source, job.job_id, job.package_id));
                    response.cache_control = "private, no-store";
                    return response;
                }
                if (viewer_status == SignalViewerStatus::not_found) {
                    return json_response(
                        409,
                        "{\"error\":{\"code\":\"viewer_source_unavailable\","
                        "\"message\":\"This retained package predates the signal viewer or contains no supported WFDB signal. Rebuild or regenerate it to view signals.\"}}\n"
                    );
                }
                RouteResponse response = json_response(
                    viewer_status == SignalViewerStatus::io_error ? 503 : 409,
                    viewer_status == SignalViewerStatus::io_error
                        ? "{\"error\":{\"code\":\"viewer_io_unavailable\",\"message\":\"Signal data is temporarily unavailable.\"}}\n"
                        : "{\"error\":{\"code\":\"viewer_source_invalid\",\"message\":\"The retained signal source is invalid.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }

            if (action == "viewer/overlays") {
                SignalViewerOverlayRequest overlay_request;
                std::string case_id;
                unsigned long long start_sample = 0;
                unsigned long long sample_count = 0;
                int max_items = 4000;
                if (!query_value(query_string, "case_id", case_id) ||
                    !query_unsigned_long_long(
                        query_string, "start_sample", 0, false, start_sample) ||
                    !query_unsigned_long_long(
                        query_string, "sample_count", 0, true, sample_count) ||
                    !query_integer(query_string, "max_items", 4000, max_items) ||
                    max_items <= 0 || max_items > 10000) {
                    return json_response(
                        400,
                        "{\"error\":{\"code\":\"invalid_viewer_overlays\","
                        "\"message\":\"Provide a valid case_id, positive sample_count, and max_items (1-10000).\"}}\n"
                    );
                }
                overlay_request.case_id = case_id;
                overlay_request.start_sample = start_sample;
                overlay_request.sample_count = sample_count;
                overlay_request.max_items = static_cast<unsigned int>(max_items);
                SignalViewerOverlayWindow overlay_window;
                const SignalViewerStatus overlay_status =
                    read_signal_viewer_overlays(
                        viewer_root, overlay_request, overlay_window, error);
                if (overlay_status == SignalViewerStatus::ok) {
                    RouteResponse response = json_response(
                        200, signal_viewer_overlay_json(overlay_window));
                    response.cache_control = "private, no-store";
                    return response;
                }
                if (overlay_status == SignalViewerStatus::invalid_request) {
                    return json_response(
                        400,
                        "{\"error\":{\"code\":\"invalid_viewer_overlays\","
                        "\"message\":\"The requested overlay window or case is invalid.\"}}\n"
                    );
                }
                if (overlay_status == SignalViewerStatus::not_found) {
                    return json_response(
                        409,
                        "{\"error\":{\"code\":\"viewer_overlays_unavailable\","
                        "\"message\":\"This package predates prepared ground-truth overlays. Regenerate it to enable annotations.\"}}\n"
                    );
                }
                RouteResponse response = json_response(
                    overlay_status == SignalViewerStatus::io_error ? 503 : 409,
                    overlay_status == SignalViewerStatus::io_error
                        ? "{\"error\":{\"code\":\"viewer_io_unavailable\",\"message\":\"Overlay data is temporarily unavailable.\"}}\n"
                        : "{\"error\":{\"code\":\"viewer_source_invalid\",\"message\":\"The retained overlay source is invalid.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }

            SignalViewerWindowRequest viewer_request;
            std::string case_id;
            unsigned long long start_sample = 0;
            unsigned long long sample_count = 0;
            int points = 2048;
            if (!query_value(query_string, "case_id", case_id) ||
                !query_unsigned_long_long(
                    query_string, "start_sample", 0, false, start_sample) ||
                !query_unsigned_long_long(
                    query_string, "sample_count", 0, true, sample_count) ||
                !query_integer(query_string, "points", 2048, points) ||
                points <= 0 || points > 16384 ||
                !query_channel_indices(
                    query_string, viewer_request.channel_indices)) {
                return json_response(
                    400,
                    "{\"error\":{\"code\":\"invalid_viewer_window\","
                    "\"message\":\"Provide a valid case_id, positive sample_count, points (1-16384), and comma-separated channels.\"}}\n"
                );
            }
            viewer_request.case_id = case_id;
            viewer_request.start_sample = start_sample;
            viewer_request.sample_count = sample_count;
            viewer_request.max_points = static_cast<unsigned int>(points);
            SignalViewerWindow window;
            const SignalViewerStatus viewer_status = read_signal_viewer_window(
                viewer_root, viewer_request, window, error);
            if (viewer_status == SignalViewerStatus::ok) {
                RouteResponse response;
                response.disposition = RouteDisposition::handled;
                response.status = 200;
                response.content_type = signal_viewer_binary_content_type();
                response.cache_control = "private, no-store";
                response.body.swap(window.binary);
                return response;
            }
            if (viewer_status == SignalViewerStatus::invalid_request) {
                return json_response(
                    400,
                    "{\"error\":{\"code\":\"invalid_viewer_window\","
                    "\"message\":\"The requested signal window, case, or channel selection is invalid.\"}}\n"
                );
            }
            if (viewer_status == SignalViewerStatus::not_found) {
                return json_response(
                    409,
                    "{\"error\":{\"code\":\"viewer_source_unavailable\","
                    "\"message\":\"This retained package has no prepared signal viewer source.\"}}\n"
                );
            }
            RouteResponse response = json_response(
                viewer_status == SignalViewerStatus::io_error ? 503 : 409,
                viewer_status == SignalViewerStatus::io_error
                    ? "{\"error\":{\"code\":\"viewer_io_unavailable\",\"message\":\"Signal data is temporarily unavailable.\"}}\n"
                    : "{\"error\":{\"code\":\"viewer_source_invalid\",\"message\":\"The retained signal source is invalid.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        if (action == "verification-kit.zip") {
            if (method != "GET" && method != "HEAD") {
                return json_response(
                    405,
                    "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Verification kits only accept GET or HEAD.\"}}\n"
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
                    "{\"error\":{\"code\":\"job_kit_unavailable\","
                    "\"message\":\"Verification kits are available after a job succeeds and while artifacts are retained.\"}}\n"
                );
            }
            PackageRecord package;
            const RecordLookupStatus package_lookup = metadata_store->find_package(
                job.package_id,
                authenticated_identity,
                package,
                error
            );
            if (package_lookup == RecordLookupStatus::not_found) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"artifact_not_found\","
                    "\"message\":\"The requested artifact does not exist.\"}}\n"
                );
            }
            if (package_lookup == RecordLookupStatus::storage_error) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Artifact metadata is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            const std::string expected_storage =
                data_root + "/packages/" + job.package_id;
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
            const std::string::size_type separator =
                signal_synth_cli.rfind('/');
            const std::string release_bin = separator == std::string::npos
                ? std::string() : signal_synth_cli.substr(0, separator);
            const std::string helper = release_bin + "/challenge_artifact.py";
            const std::string verifier = release_bin + "/synsigra-wheel.whl";
            PreparedArtifact artifact;
            std::string challenge_metadata;
            const DerivedArtifactStatus kit_status =
                prepare_verification_kit(
                    data_root,
                    job.package_id,
                    expected_storage + "/package.zip",
                    helper,
                    verifier,
                    artifact,
                    challenge_metadata,
                    error
                );
            if (kit_status != DerivedArtifactStatus::ok) {
                const int status = kit_status ==
                    DerivedArtifactStatus::storage_pressure ? 507 :
                    (kit_status == DerivedArtifactStatus::invalid_source
                        ? 404 : 503);
                RouteResponse response = json_response(
                    status,
                    status == 507
                        ? "{\"error\":{\"code\":\"artifact_storage_pressure\",\"message\":\"Verification kit preparation is paused to preserve server disk space.\"}}\n"
                        : "{\"error\":{\"code\":\"verification_kit_unavailable\",\"message\":\"The verification kit files are not available.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            RouteResponse response = immutable_artifact_response(
                method,
                artifact,
                "application/zip",
                job.package_id + "-verification-kit.zip",
                range_header,
                package.expires_at
            );
            record_nonblocking_audit(
                *metadata_store, authenticated_identity,
                method == "HEAD" ? "artifact.inspected" : "artifact.downloaded",
                "package", job.package_id,
                range_header.empty()
                    ? "{\"kind\":\"verification_kit\",\"range\":false}"
                    : "{\"kind\":\"verification_kit\",\"range\":true}",
                response);
            return response;
        }
        if (!action.empty()) {
            if (method != "POST" || (action != "cancel" &&
                action != "retry" && action != "rebuild")) {
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
            if (action == "rebuild") {
                JobRecord original;
                const RecordLookupStatus lookup = metadata_store->find_job(
                    job_id, authenticated_identity, original, error);
                if (lookup == RecordLookupStatus::not_found) {
                    return json_response(
                        404, "{\"error\":{\"code\":\"job_not_found\","
                        "\"message\":\"The requested job does not exist.\"}}\n");
                }
                if (lookup == RecordLookupStatus::storage_error) {
                    RouteResponse response = json_response(
                        503, "{\"error\":{\"code\":\"metadata_unavailable\","
                        "\"message\":\"Job storage is unavailable.\"}}\n");
                    response.internal_error = error;
                    return response;
                }
                if (original.status != "succeeded" ||
                    !original.package_id.empty()) {
                    return json_response(
                        409, "{\"error\":{\"code\":\"exact_rebuild_invalid_state\","
                        "\"message\":\"Exact rebuild is available only after a "
                        "succeeded job's cached artifact expires.\"}}\n");
                }
                const std::string recipes = data_root + "/recipes";
                const std::string releases =
                    data_root + "/generator_releases/";
                const bool valid_identity =
                    original.generator_binary_sha256.size() == 71 &&
                    original.generator_binary_sha256.compare(
                        0, 7, "sha256:") == 0;
                const std::string release_cli = valid_identity
                    ? releases + original.generator_binary_sha256.substr(7) +
                        "/signal-synth"
                    : std::string();
                if (original.source_pack_path.size() <= recipes.size() ||
                    original.source_pack_path.compare(
                        0, recipes.size(), recipes) != 0 ||
                    original.source_pack_path[recipes.size()] != '/' ||
                    !regular_file_without_symlink(original.source_pack_path)) {
                    return json_response(
                        409, "{\"error\":{\"code\":\"rebuild_recipe_unavailable\","
                        "\"message\":\"This historical job predates immutable "
                        "pack recipes and cannot be rebuilt safely.\"}}\n");
                }
                if (!valid_identity ||
                    !regular_file_without_symlink(release_cli)) {
                    return json_response(
                        409, "{\"error\":{\"code\":\"historical_generator_unavailable\","
                        "\"message\":\"The exact historical generator build is "
                        "not available; Synsigra will not substitute a newer build.\"}}\n");
                }
                unsigned long long disk_free = 0;
                unsigned long long disk_reserve = 0;
                if (!disk_generation_capacity(
                        data_root, disk_free, disk_reserve)) {
                    return json_response(
                        507, "{\"error\":{\"code\":\"artifact_storage_pressure\","
                        "\"message\":\"Exact rebuild is paused to preserve "
                        "server disk space.\"}}\n");
                }
            }
            const JobLifecycleStatus lifecycle = action == "cancel"
                ? metadata_store->cancel_job(
                    job_id, authenticated_identity, error)
                : (action == "retry"
                    ? metadata_store->retry_job(
                        job_id, authenticated_identity, new_job_id, error)
                    : metadata_store->rebuild_job_exact(
                        job_id, authenticated_identity, new_job_id, error));
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
            if (action == "retry" || action == "rebuild") {
                RouteResponse response = json_response(
                    202,
                    std::string("{\"job_id\":\"") + new_job_id +
                    (action == "retry"
                        ? "\",\"retry_of\":\""
                        : "\",\"exact_rebuild_of\":\"") + job_id +
                    "\",\"status\":\"queued\"}\n"
                );
                if (action == "retry") {
                    record_nonblocking_audit(
                        *metadata_store, authenticated_identity,
                        "job.retried", "job", new_job_id,
                        std::string("{\"source_job_id\":\"") + job_id + "\"}",
                        response);
                }
                return response;
            }
            RouteResponse response = json_response(
                200,
                std::string("{\"job_id\":\"") + job_id +
                "\",\"status\":\"cancelled\"}\n"
            );
            record_nonblocking_audit(
                *metadata_store, authenticated_identity,
                "job.cancelled", "job", job_id, "{}", response);
            return response;
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
        if (method != "GET" && method != "HEAD") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Artifact endpoints only accept GET or HEAD.\"}}\n"
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
        PreparedArtifact artifact;
        const DerivedArtifactStatus artifact_status =
            prepare_cached_file_metadata(
                data_root,
                package_id,
                expected_storage + "/" + filename,
                filename == "manifest.json" ? "manifest-json" : "package-zip",
                artifact,
                error
            );
        if (artifact_status != DerivedArtifactStatus::ok) {
            RouteResponse response = json_response(
                artifact_status == DerivedArtifactStatus::invalid_source
                    ? 404 : 503,
                "{\"error\":{\"code\":\"artifact_storage_unavailable\","
                "\"message\":\"The artifact file is unavailable.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        RouteResponse response = immutable_artifact_response(
            method,
            artifact,
            filename == "manifest.json" ? "application/json" : "application/zip",
            filename,
            range_header,
            package.expires_at
        );
        const std::string details = std::string("{\"kind\":\"") +
            (filename == "manifest.json" ? "manifest" : "package_zip") +
            "\",\"range\":" + (range_header.empty() ? "false" : "true") + "}";
        record_nonblocking_audit(
            *metadata_store, authenticated_identity,
            method == "HEAD" ? "artifact.inspected" : "artifact.downloaded",
            "package", package_id, details, response);
        return response;
    }

    if (is_ui_page_route(uri, public_base_path)) {
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

    if (uri == public_base_path + "/legal/terms" ||
        uri == public_base_path + "/legal/privacy" ||
        uri == public_base_path + "/legal/support") {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Legal and support pages only accept GET.\"}}\n"
            );
        }
        RouteResponse response;
        response.disposition = RouteDisposition::handled;
        response.status = 200;
        response.content_type = "text/html; charset=utf-8";
        response.cache_control = "public, max-age=300";
        if (uri == public_base_path + "/legal/terms") {
            response.body = kTermsHtml;
        } else if (uri == public_base_path + "/legal/privacy") {
            response.body = kPrivacyHtml;
        } else {
            response.body = kSupportHtml;
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
