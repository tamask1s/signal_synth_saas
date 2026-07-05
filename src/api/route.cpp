#include "syn_sig_ra/route.h"

#include "syn_sig_ra/api_key_auth.h"
#include "syn_sig_ra/build_info.h"
#include "syn_sig_ra/job_request.h"
#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/pack_catalog.h"

#include <jansson.h>

#include <cctype>
#include <cstdlib>
#include <string>

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

std::string job_json(
    const syn_sig_ra::JobRecord& job,
    const std::string& public_base_path
) {
    json_t* root = json_object();
    json_object_set_new(root, "job_id", json_string(job.job_id.c_str()));
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
    const std::string output = json_dump_line(root);
    json_decref(root);
    return output;
}

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
           path_at_or_below(uri, public_base_path + "/v1/artifacts");
}

RouteResponse route_request(
    const std::string& method,
    const std::string& uri,
    const std::string& public_base_path,
    const std::string& authorization_header,
    MetadataStore* metadata_store,
    const std::string& pack_root,
    const std::string& content_type,
    const std::string& request_body
) {
    if (!owns_uri(uri, public_base_path)) {
        RouteResponse response;
        response.disposition = RouteDisposition::declined;
        response.status = 0;
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
        const AuthenticationResult authentication = authenticate_bearer(
            authorization_header,
            *metadata_store
        );
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
                "\"message\":\"A valid Bearer API key is required.\"}}\n"
            );
            response.www_authenticate = "Bearer realm=\"syn_sig_ra\"";
            return response;
        }
        authenticated_identity = authentication.identity;
        authenticated = true;
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
            if (method != "POST") {
                return json_response(
                    405,
                    "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Job collection only accepts POST.\"}}\n"
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
            const PackCatalog catalog(pack_root);
            PackSummary pack;
            const PackLookupStatus pack_status = catalog.find(
                job_request.pack_id,
                pack,
                error
            );
            if (pack_status == PackLookupStatus::not_found) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"pack_not_found\","
                    "\"message\":\"The requested pack does not exist.\"}}\n"
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
            std::string job_id;
            if (!metadata_store->create_job(
                    authenticated_identity,
                    job_request.canonical_json,
                    job_request.pack_id,
                    pack_root + "/" + job_request.pack_id + ".json",
                    pack.pack_fingerprint,
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

        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Job status only accepts GET.\"}}\n"
            );
        }
        const std::string job_id = uri.substr(jobs_path.size() + 1);
        if (!is_valid_pack_id(job_id) ||
            job_id.compare(0, 4, "job_") != 0) {
            return json_response(
                400,
                "{\"error\":{\"code\":\"invalid_job_id\","
                "\"message\":\"The job ID is invalid.\"}}\n"
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
