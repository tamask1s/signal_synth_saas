#include "syn_sig_ra/route.h"

#include "syn_sig_ra/build_info.h"

namespace {

bool owns_uri(const std::string& uri, const std::string& prefix) {
    return uri == prefix ||
           (uri.size() > prefix.size() &&
            uri.compare(0, prefix.size(), prefix) == 0 &&
            uri[prefix.size()] == '/');
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

}  // namespace

namespace syn_sig_ra {

RouteResponse route_request(const std::string& method, const std::string& uri) {
    return route_request(method, uri, "/syn_sig_ra");
}

RouteResponse route_request(
    const std::string& method,
    const std::string& uri,
    const std::string& public_base_path
) {
    if (!owns_uri(uri, public_base_path)) {
        RouteResponse response;
        response.disposition = RouteDisposition::declined;
        response.status = 0;
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

    return json_response(
        404,
        "{\"error\":{\"code\":\"route_not_found\","
        "\"message\":\"No API route matches the requested path.\"}}\n"
    );
}

}  // namespace syn_sig_ra
