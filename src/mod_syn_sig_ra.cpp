#include "syn_sig_ra/route.h"
#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/runtime_config.h"

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
    if (request->method == nullptr ||
        std::string(request->method) != "POST") {
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
    bool data_root_set;
    bool signal_synth_cli_set;
    bool pack_root_set;
    bool public_base_path_set;
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
    merged->data_root_set = add->data_root_set || base->data_root_set;
    merged->signal_synth_cli_set =
        add->signal_synth_cli_set || base->signal_synth_cli_set;
    merged->pack_root_set = add->pack_root_set || base->pack_root_set;
    merged->public_base_path_set =
        add->public_base_path_set || base->public_base_path_set;
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
    const char* content_type_value = apr_table_get(
        request->headers_in,
        "Content-Type"
    );
    const std::string content_type = content_type_value == nullptr
        ? std::string()
        : content_type_value;
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
    if (syn_sig_ra::route_requires_authentication(
            uri, config->public_base_path) ||
        uri == std::string(config->public_base_path) + "/readyz") {
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
        config->signal_synth_cli
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
        ap_set_content_length(request, information.size);
        apr_size_t bytes_sent = 0;
        ap_send_fd(
            file,
            request,
            0,
            static_cast<apr_size_t>(information.size),
            &bytes_sent
        );
        apr_file_close(file);
        return OK;
    }
    ap_rwrite(
        response.body.data(),
        static_cast<int>(response.body.size()),
        request
    );
    return OK;
}

void syn_sig_ra_register_hooks(apr_pool_t*) {
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
