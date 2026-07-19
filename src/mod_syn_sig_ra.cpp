#include "syn_sig_ra/route.h"
#include "syn_sig_ra/core_contract.h"
#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/runtime_config.h"
#include "syn_sig_ra/transactional_email.h"

extern "C" {
#include <apr_strings.h>
#include <apr_time.h>
#include <httpd.h>
#include <http_config.h>
#include <http_log.h>
#include <http_protocol.h>
}

#include <string>

extern "C" {
extern module AP_MODULE_DECLARE_DATA syn_sig_ra_module;
}

#ifdef APLOG_USE_MODULE
APLOG_USE_MODULE(syn_sig_ra);
#endif

namespace {

bool read_request_body(request_rec* request, std::string& body) {
    if (request->method == nullptr) {
        return true;
    }
    const std::string method(request->method);
    if (method != "POST" && method != "PUT" && method != "PATCH" &&
        method != "DELETE") {
        return true;
    }
    if (ap_setup_client_block(request, REQUEST_CHUNKED_ERROR) != OK) {
        return false;
    }
    if (!ap_should_client_block(request)) {
        return true;
    }
    char buffer[8192];
    long read_count = 0;
    while ((read_count = ap_get_client_block(
                request,
                buffer,
                sizeof(buffer)
            )) > 0) {
        if (body.size() + static_cast<std::size_t>(read_count) >
            64u * 1024u) {
            return false;
        }
        body.append(buffer, static_cast<std::size_t>(read_count));
    }
    return read_count == 0;
}

struct ApacheServerConfig {
    const char* data_root;
    const char* signal_synth_cli;
    const char* pack_root;
    const char* public_base_path;
    const char* email_transport;
    const char* email_public_origin;
    const char* email_from;
    const char* email_from_name;
    const char* email_smtp_url;
    const char* email_smtp_username;
    const char* email_smtp_password_file;
    const char* email_smtp_tls_mode;
    const char* email_capture_directory;
    bool data_root_set;
    bool signal_synth_cli_set;
    bool pack_root_set;
    bool public_base_path_set;
    bool email_transport_set;
    bool email_public_origin_set;
    bool email_from_set;
    bool email_from_name_set;
    bool email_smtp_url_set;
    bool email_smtp_username_set;
    bool email_smtp_password_file_set;
    bool email_smtp_tls_mode_set;
    bool email_capture_directory_set;
};

void* create_server_config(apr_pool_t* pool, server_rec*) {
    const syn_sig_ra::RuntimeConfig defaults =
        syn_sig_ra::default_runtime_config();
    ApacheServerConfig* config = static_cast<ApacheServerConfig*>(
        apr_pcalloc(pool, sizeof(ApacheServerConfig))
    );
    config->data_root = apr_pstrdup(pool, defaults.data_root.c_str());
    config->signal_synth_cli =
        apr_pstrdup(pool, defaults.signal_synth_cli.c_str());
    config->pack_root = apr_pstrdup(pool, defaults.pack_root.c_str());
    config->public_base_path =
        apr_pstrdup(pool, defaults.public_base_path.c_str());
    config->email_transport = "disabled";
    config->email_public_origin = "";
    config->email_from = "";
    config->email_from_name = "Synsigra";
    config->email_smtp_url = "";
    config->email_smtp_username = "";
    config->email_smtp_password_file = "";
    config->email_smtp_tls_mode = "required";
    config->email_capture_directory = "";
    return config;
}

void* merge_server_config(apr_pool_t* pool, void* base_value, void* add_value) {
    const ApacheServerConfig* base =
        static_cast<const ApacheServerConfig*>(base_value);
    const ApacheServerConfig* add =
        static_cast<const ApacheServerConfig*>(add_value);
    ApacheServerConfig* merged = static_cast<ApacheServerConfig*>(
        apr_pcalloc(pool, sizeof(ApacheServerConfig))
    );

    merged->data_root = add->data_root_set ? add->data_root : base->data_root;
    merged->signal_synth_cli = add->signal_synth_cli_set
        ? add->signal_synth_cli
        : base->signal_synth_cli;
    merged->pack_root = add->pack_root_set ? add->pack_root : base->pack_root;
    merged->public_base_path = add->public_base_path_set
        ? add->public_base_path
        : base->public_base_path;
    merged->email_transport = add->email_transport_set
        ? add->email_transport : base->email_transport;
    merged->email_public_origin = add->email_public_origin_set
        ? add->email_public_origin : base->email_public_origin;
    merged->email_from = add->email_from_set
        ? add->email_from : base->email_from;
    merged->email_from_name = add->email_from_name_set
        ? add->email_from_name : base->email_from_name;
    merged->email_smtp_url = add->email_smtp_url_set
        ? add->email_smtp_url : base->email_smtp_url;
    merged->email_smtp_username = add->email_smtp_username_set
        ? add->email_smtp_username : base->email_smtp_username;
    merged->email_smtp_password_file = add->email_smtp_password_file_set
        ? add->email_smtp_password_file : base->email_smtp_password_file;
    merged->email_smtp_tls_mode = add->email_smtp_tls_mode_set
        ? add->email_smtp_tls_mode : base->email_smtp_tls_mode;
    merged->email_capture_directory = add->email_capture_directory_set
        ? add->email_capture_directory : base->email_capture_directory;
    merged->data_root_set = add->data_root_set || base->data_root_set;
    merged->signal_synth_cli_set =
        add->signal_synth_cli_set || base->signal_synth_cli_set;
    merged->pack_root_set = add->pack_root_set || base->pack_root_set;
    merged->public_base_path_set =
        add->public_base_path_set || base->public_base_path_set;
    merged->email_transport_set =
        add->email_transport_set || base->email_transport_set;
    merged->email_public_origin_set =
        add->email_public_origin_set || base->email_public_origin_set;
    merged->email_from_set = add->email_from_set || base->email_from_set;
    merged->email_from_name_set =
        add->email_from_name_set || base->email_from_name_set;
    merged->email_smtp_url_set =
        add->email_smtp_url_set || base->email_smtp_url_set;
    merged->email_smtp_username_set =
        add->email_smtp_username_set || base->email_smtp_username_set;
    merged->email_smtp_password_file_set =
        add->email_smtp_password_file_set || base->email_smtp_password_file_set;
    merged->email_smtp_tls_mode_set =
        add->email_smtp_tls_mode_set || base->email_smtp_tls_mode_set;
    merged->email_capture_directory_set =
        add->email_capture_directory_set || base->email_capture_directory_set;
    return merged;
}

const char* set_data_root(cmd_parms* command, void*, const char* value) {
    std::string error;
    if (!syn_sig_ra::validate_data_root(value, error)) {
        return apr_pstrdup(command->pool, error.c_str());
    }
    ApacheServerConfig* config = static_cast<ApacheServerConfig*>(
        ap_get_module_config(
            command->server->module_config,
            &syn_sig_ra_module
        )
    );
    config->data_root = apr_pstrdup(command->pool, value);
    config->data_root_set = true;
    return nullptr;
}

const char* set_signal_synth_cli(
    cmd_parms* command,
    void*,
    const char* value
) {
    std::string error;
    if (!syn_sig_ra::validate_signal_synth_cli(value, error)) {
        return apr_pstrdup(command->pool, error.c_str());
    }
    ApacheServerConfig* config = static_cast<ApacheServerConfig*>(
        ap_get_module_config(
            command->server->module_config,
            &syn_sig_ra_module
        )
    );
    config->signal_synth_cli = apr_pstrdup(command->pool, value);
    config->signal_synth_cli_set = true;
    return nullptr;
}

const char* set_pack_root(cmd_parms* command, void*, const char* value) {
    std::string error;
    if (!syn_sig_ra::validate_pack_root(value, error)) {
        return apr_pstrdup(command->pool, error.c_str());
    }
    ApacheServerConfig* config = static_cast<ApacheServerConfig*>(
        ap_get_module_config(
            command->server->module_config,
            &syn_sig_ra_module
        )
    );
    config->pack_root = apr_pstrdup(command->pool, value);
    config->pack_root_set = true;
    return nullptr;
}

const char* set_public_base_path(
    cmd_parms* command,
    void*,
    const char* value
) {
    std::string error;
    if (!syn_sig_ra::validate_public_base_path(value, error)) {
        return apr_pstrdup(command->pool, error.c_str());
    }
    ApacheServerConfig* config = static_cast<ApacheServerConfig*>(
        ap_get_module_config(
            command->server->module_config,
            &syn_sig_ra_module
        )
    );
    config->public_base_path = apr_pstrdup(command->pool, value);
    config->public_base_path_set = true;
    return nullptr;
}

ApacheServerConfig* current_server_config(cmd_parms* command) {
    return static_cast<ApacheServerConfig*>(ap_get_module_config(
        command->server->module_config, &syn_sig_ra_module));
}

const char* set_email_transport(cmd_parms* command, void*, const char* value) {
    const std::string transport(value);
    if (transport != "disabled" && transport != "smtp" &&
        transport != "capture_file") {
        return "SynSigRaEmailTransport must be disabled, smtp, or capture_file";
    }
    ApacheServerConfig* config = current_server_config(command);
    config->email_transport = apr_pstrdup(command->pool, value);
    config->email_transport_set = true;
    return nullptr;
}

const char* set_email_public_origin(cmd_parms* command, void*, const char* value) {
    std::string error;
    if (!syn_sig_ra::validate_public_origin(value, error)) {
        return apr_pstrdup(command->pool, error.c_str());
    }
    ApacheServerConfig* config = current_server_config(command);
    config->email_public_origin = apr_pstrdup(command->pool, value);
    config->email_public_origin_set = true;
    return nullptr;
}

const char* set_email_from(cmd_parms* command, void*, const char* value) {
    std::string error;
    if (!syn_sig_ra::validate_email_address(value, error)) {
        return apr_pstrdup(command->pool, error.c_str());
    }
    ApacheServerConfig* config = current_server_config(command);
    config->email_from = apr_pstrdup(command->pool, value);
    config->email_from_set = true;
    return nullptr;
}

const char* set_email_smtp_tls_mode(cmd_parms* command, void*, const char* value) {
    const std::string mode(value);
    if (mode != "required" && mode != "opportunistic" && mode != "disabled") {
        return "SynSigRaEmailSmtpTls must be required, opportunistic, or disabled";
    }
    ApacheServerConfig* config = current_server_config(command);
    config->email_smtp_tls_mode = apr_pstrdup(command->pool, value);
    config->email_smtp_tls_mode_set = true;
    return nullptr;
}

#define SYN_SIG_RA_EMAIL_STRING_SETTER(function_name, field, flag) \
const char* function_name(cmd_parms* command, void*, const char* value) { \
    ApacheServerConfig* config = current_server_config(command); \
    config->field = apr_pstrdup(command->pool, value); \
    config->flag = true; \
    return nullptr; \
}

SYN_SIG_RA_EMAIL_STRING_SETTER(
    set_email_from_name, email_from_name, email_from_name_set)
SYN_SIG_RA_EMAIL_STRING_SETTER(
    set_email_smtp_url, email_smtp_url, email_smtp_url_set)
SYN_SIG_RA_EMAIL_STRING_SETTER(
    set_email_smtp_username, email_smtp_username, email_smtp_username_set)
SYN_SIG_RA_EMAIL_STRING_SETTER(
    set_email_smtp_password_file,
    email_smtp_password_file,
    email_smtp_password_file_set)
SYN_SIG_RA_EMAIL_STRING_SETTER(
    set_email_capture_directory,
    email_capture_directory,
    email_capture_directory_set)

#undef SYN_SIG_RA_EMAIL_STRING_SETTER

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

const command_rec syn_sig_ra_directives[] = {
    AP_INIT_TAKE1(
        "SynSigRaDataRoot",
        reinterpret_cast<cmd_func>(set_data_root),
        nullptr,
        RSRC_CONF,
        "Existing writable directory for SynSigra runtime data"
    ),
    AP_INIT_TAKE1(
        "SynSigRaSignalSynthCli",
        reinterpret_cast<cmd_func>(set_signal_synth_cli),
        nullptr,
        RSRC_CONF,
        "Absolute path to the executable signal-synth CLI"
    ),
    AP_INIT_TAKE1(
        "SynSigRaPackRoot",
        reinterpret_cast<cmd_func>(set_pack_root),
        nullptr,
        RSRC_CONF,
        "Existing readable directory containing built-in packs"
    ),
    AP_INIT_TAKE1(
        "SynSigRaPublicBasePath",
        reinterpret_cast<cmd_func>(set_public_base_path),
        nullptr,
        RSRC_CONF,
        "Public URL base path at or below /syn_sig_ra"
    ),
    AP_INIT_TAKE1(
        "SynSigRaEmailTransport",
        reinterpret_cast<cmd_func>(set_email_transport),
        nullptr, RSRC_CONF,
        "Transactional email transport: disabled, smtp, or capture_file"
    ),
    AP_INIT_TAKE1(
        "SynSigRaEmailPublicOrigin",
        reinterpret_cast<cmd_func>(set_email_public_origin),
        nullptr, RSRC_CONF,
        "Public origin used in verification and reset links"
    ),
    AP_INIT_TAKE1(
        "SynSigRaEmailFrom",
        reinterpret_cast<cmd_func>(set_email_from),
        nullptr, RSRC_CONF,
        "Verified sender email address"
    ),
    AP_INIT_TAKE1(
        "SynSigRaEmailFromName",
        reinterpret_cast<cmd_func>(set_email_from_name),
        nullptr, RSRC_CONF,
        "Sender display name"
    ),
    AP_INIT_TAKE1(
        "SynSigRaEmailSmtpUrl",
        reinterpret_cast<cmd_func>(set_email_smtp_url),
        nullptr, RSRC_CONF,
        "SMTP URL, for example smtps://smtp.provider.example:465"
    ),
    AP_INIT_TAKE1(
        "SynSigRaEmailSmtpUsername",
        reinterpret_cast<cmd_func>(set_email_smtp_username),
        nullptr, RSRC_CONF,
        "SMTP username"
    ),
    AP_INIT_TAKE1(
        "SynSigRaEmailSmtpTls",
        reinterpret_cast<cmd_func>(set_email_smtp_tls_mode),
        nullptr, RSRC_CONF,
        "SMTP TLS mode: required, opportunistic, or disabled (loopback only)"
    ),
    AP_INIT_TAKE1(
        "SynSigRaEmailSmtpPasswordFile",
        reinterpret_cast<cmd_func>(set_email_smtp_password_file),
        nullptr, RSRC_CONF,
        "File containing the SMTP password"
    ),
    AP_INIT_TAKE1(
        "SynSigRaEmailCaptureDirectory",
        reinterpret_cast<cmd_func>(set_email_capture_directory),
        nullptr, RSRC_CONF,
        "Test-only directory for captured email files"
    ),
    {nullptr, nullptr, nullptr, 0, RAW_ARGS, nullptr}
};

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

int syn_sig_ra_handler(request_rec* request) {
    const apr_time_t request_started_at = apr_time_now();
    const std::string method =
        request->method == nullptr ? std::string() : request->method;
    const std::string uri =
        request->uri == nullptr ? std::string() : request->uri;
    const std::string query_string =
        request->args == nullptr ? std::string() : request->args;
    const char* authorization_value = apr_table_get(
        request->headers_in,
        "Authorization"
    );
    const std::string authorization = authorization_value == nullptr
        ? std::string()
        : authorization_value;
    const char* cookie_value = apr_table_get(request->headers_in, "Cookie");
    const std::string cookie = cookie_value == nullptr
        ? std::string()
        : cookie_value;
    const char* content_type_value = apr_table_get(
        request->headers_in,
        "Content-Type"
    );
    const std::string content_type = content_type_value == nullptr
        ? std::string()
        : content_type_value;
    const char* range_value = apr_table_get(request->headers_in, "Range");
    const std::string range_header = range_value == nullptr
        ? std::string()
        : range_value;
    const char* accept_value = apr_table_get(request->headers_in, "Accept");
    const std::string accept_header = accept_value == nullptr
        ? std::string()
        : accept_value;
    const char* origin_value = apr_table_get(request->headers_in, "Origin");
    const std::string origin_header = origin_value == nullptr
        ? std::string()
        : origin_value;
    const char* protocol_version_value = apr_table_get(
        request->headers_in, "MCP-Protocol-Version");
    const std::string mcp_protocol_version_header =
        protocol_version_value == nullptr
            ? std::string()
            : protocol_version_value;
    std::string request_body;
    if (!read_request_body(request, request_body)) {
        request->status = HTTP_REQUEST_ENTITY_TOO_LARGE;
        ap_set_content_type(request, "application/json");
        ap_rputs(
            "{\"error\":{\"code\":\"request_too_large\","
            "\"message\":\"Request body exceeds 64 KiB.\"}}\n",
            request
        );
        return OK;
    }
    const ApacheServerConfig* config =
        static_cast<const ApacheServerConfig*>(
            ap_get_module_config(
                request->server->module_config,
                &syn_sig_ra_module
            )
        );
    syn_sig_ra::MetadataStore metadata_store(
        std::string(config->data_root) + "/db.sqlite3"
    );
    syn_sig_ra::MetadataStore* metadata_store_pointer = nullptr;
    const std::string auth_path =
        std::string(config->public_base_path) + "/v1/auth";
    const bool account_route =
        uri == auth_path ||
        (uri.size() > auth_path.size() &&
         uri.compare(0, auth_path.size(), auth_path) == 0 &&
         uri[auth_path.size()] == '/');
    if (syn_sig_ra::route_requires_authentication(
            uri, config->public_base_path) ||
        uri == std::string(config->public_base_path) + "/mcp" ||
        uri == std::string(config->public_base_path) + "/readyz" ||
        account_route) {
        std::string storage_error;
        if (metadata_store.initialize(storage_error)) {
            metadata_store_pointer = &metadata_store;
        } else {
            ap_log_rerror(
                APLOG_MARK,
                APLOG_ERR,
                0,
                request,
                "syn_sig_ra metadata initialization failed: %s",
                storage_error.c_str()
            );
        }
    }
    syn_sig_ra::EmailConfig email_config;
    const std::string email_transport(config->email_transport);
    if (email_transport == "smtp") {
        email_config.transport = syn_sig_ra::EmailTransport::smtp;
    } else if (email_transport == "capture_file") {
        email_config.transport = syn_sig_ra::EmailTransport::capture_file;
    }
    email_config.public_origin = config->email_public_origin;
    email_config.from_email = config->email_from;
    email_config.from_name = config->email_from_name;
    email_config.smtp_url = config->email_smtp_url;
    email_config.smtp_username = config->email_smtp_username;
    email_config.smtp_password_file = config->email_smtp_password_file;
    const std::string email_tls_mode(config->email_smtp_tls_mode);
    email_config.tls_mode = email_tls_mode == "disabled"
        ? syn_sig_ra::EmailTlsMode::disabled
        : (email_tls_mode == "opportunistic"
            ? syn_sig_ra::EmailTlsMode::opportunistic
            : syn_sig_ra::EmailTlsMode::required);
    email_config.capture_directory = config->email_capture_directory;
    const syn_sig_ra::RouteResponse response = syn_sig_ra::route_request(
        method,
        uri,
        config->public_base_path,
        authorization,
        metadata_store_pointer,
        config->pack_root,
        content_type,
        request_body,
        config->data_root,
        query_string,
        config->signal_synth_cli,
        cookie,
        email_config,
        range_header,
        accept_header,
        origin_header,
        mcp_protocol_version_header
    );

    if (response.disposition == syn_sig_ra::RouteDisposition::declined) {
        return DECLINED;
    }
    ap_log_rerror(
        APLOG_MARK,
        APLOG_INFO,
        0,
        request,
        "event=http_request method=%s path=%s status=%d duration_ms=%lld",
        method.c_str(),
        uri.c_str(),
        response.status,
        static_cast<long long>(
            (apr_time_now() - request_started_at) / 1000
        )
    );

    if (!response.internal_error.empty()) {
        ap_log_rerror(
            APLOG_MARK,
            APLOG_ERR,
            0,
            request,
            "syn_sig_ra request failed internally: %s",
            response.internal_error.c_str()
        );
    }
    request->status = response.status;
    ap_set_content_type(
        request,
        apr_pstrdup(request->pool, response.content_type.c_str())
    );
    if (!response.www_authenticate.empty()) {
        apr_table_set(
            request->headers_out,
            "WWW-Authenticate",
            response.www_authenticate.c_str()
        );
    }
    if (!response.content_disposition.empty()) {
        apr_table_set(
            request->headers_out,
            "Content-Disposition",
            response.content_disposition.c_str()
        );
    }
    if (!response.set_cookie.empty()) {
        apr_table_set(
            request->headers_out,
            "Set-Cookie",
            response.set_cookie.c_str()
        );
    }
    if (!response.cache_control.empty()) {
        apr_table_set(
            request->headers_out,
            "Cache-Control",
            response.cache_control.c_str()
        );
    }
    if (response.accept_ranges) {
        apr_table_set(request->headers_out, "Accept-Ranges", "bytes");
    }
    if (!response.content_range.empty()) {
        apr_table_set(
            request->headers_out,
            "Content-Range",
            response.content_range.c_str()
        );
    }
    if (!response.etag.empty()) {
        apr_table_set(request->headers_out, "ETag", response.etag.c_str());
    }
    if (!response.checksum_sha256.empty()) {
        apr_table_set(
            request->headers_out,
            "X-Checksum-SHA256",
            response.checksum_sha256.c_str()
        );
    }
    if (!response.artifact_expires_at.empty()) {
        apr_table_set(
            request->headers_out,
            "X-Artifact-Expires-At",
            response.artifact_expires_at.c_str()
        );
    }
    if (!response.file_path.empty()) {
        apr_file_t* file = nullptr;
        apr_finfo_t information;
        if (apr_file_open(
                &file,
                response.file_path.c_str(),
                APR_READ | APR_BINARY,
                APR_OS_DEFAULT,
                request->pool
            ) != APR_SUCCESS ||
            apr_file_info_get(&information, APR_FINFO_SIZE, file) !=
                APR_SUCCESS) {
            request->status = HTTP_INTERNAL_SERVER_ERROR;
            ap_set_content_type(request, "application/json");
            ap_rputs(
                "{\"error\":{\"code\":\"artifact_storage_unavailable\","
                "\"message\":\"Artifact storage is unavailable.\"}}\n",
                request
            );
            return OK;
        }
        const apr_off_t offset = static_cast<apr_off_t>(response.file_offset);
        const apr_off_t length = response.file_length >= 0
            ? static_cast<apr_off_t>(response.file_length)
            : information.size;
        const bool invalid_metadata =
            offset < 0 || length < 0 || offset > information.size ||
            length > information.size - offset ||
            (response.file_size >= 0 &&
             information.size != static_cast<apr_off_t>(response.file_size));
        if (invalid_metadata) {
            apr_file_close(file);
            request->status = HTTP_INTERNAL_SERVER_ERROR;
            ap_set_content_type(request, "application/json");
            ap_rputs(
                "{\"error\":{\"code\":\"artifact_changed\","
                "\"message\":\"The immutable artifact changed during delivery.\"}}\n",
                request
            );
            return OK;
        }
        ap_set_content_length(request, length);
        if (response.headers_only || request->header_only) {
            apr_file_close(file);
            return OK;
        }
        apr_off_t sent_total = 0;
        const apr_size_t maximum_chunk = 64u * 1024u * 1024u;
        while (sent_total < length && !request->connection->aborted) {
            const apr_off_t remaining = length - sent_total;
            const apr_size_t chunk = remaining >
                static_cast<apr_off_t>(maximum_chunk)
                ? maximum_chunk
                : static_cast<apr_size_t>(remaining);
            apr_size_t bytes_sent = 0;
            const apr_status_t send_status = ap_send_fd(
                file,
                request,
                offset + sent_total,
                chunk,
                &bytes_sent
            );
            if (send_status != APR_SUCCESS || bytes_sent == 0) break;
            sent_total += static_cast<apr_off_t>(bytes_sent);
        }
        apr_file_close(file);
        return OK;
    }
    if (request->header_only) return OK;
    ap_rwrite(
        response.body.data(),
        static_cast<int>(response.body.size()),
        request
    );
    return OK;
}

void syn_sig_ra_register_hooks(apr_pool_t*) {
    ap_hook_post_config(
        [](apr_pool_t*, apr_pool_t*, apr_pool_t*, server_rec* server) -> int {
            for (server_rec* current = server; current != nullptr;
                 current = current->next) {
                const ApacheServerConfig* config =
                    static_cast<const ApacheServerConfig*>(ap_get_module_config(
                        current->module_config, &syn_sig_ra_module));
                syn_sig_ra::CoreIntegrationContract contract;
                std::string error;
                if (config == nullptr || !syn_sig_ra::validate_core_integration(
                        config->signal_synth_cli, contract, error)) {
                    ap_log_error(
                        APLOG_MARK, APLOG_CRIT, 0, current,
                        "Synsigra core integration rejected: %s", error.c_str());
                    return HTTP_INTERNAL_SERVER_ERROR;
                }
            }
            return OK;
        },
        nullptr,
        nullptr,
        APR_HOOK_MIDDLE
    );
    ap_hook_handler(
        syn_sig_ra_handler,
        nullptr,
        nullptr,
        APR_HOOK_MIDDLE
    );
}

}  // namespace

extern "C" {

module AP_MODULE_DECLARE_DATA syn_sig_ra_module = {
    STANDARD20_MODULE_STUFF,
    nullptr,
    nullptr,
    create_server_config,
    merge_server_config,
    syn_sig_ra_directives,
    syn_sig_ra_register_hooks,
#ifdef AP_MODULE_FLAG_NONE
    AP_MODULE_FLAG_NONE
#endif
};

}
