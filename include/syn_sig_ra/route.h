#ifndef SYN_SIG_RA_ROUTE_H
#define SYN_SIG_RA_ROUTE_H

#include <string>

namespace syn_sig_ra {

class MetadataStore;

enum class RouteDisposition {
    declined,
    handled
};

struct RouteResponse {
    RouteDisposition disposition;
    int status;
    std::string content_type;
    std::string body;
    std::string www_authenticate;
    std::string internal_error;
    std::string file_path;
    std::string content_disposition;
    std::string set_cookie;
    std::string cache_control;
};

RouteResponse route_request(const std::string& method, const std::string& uri);
RouteResponse route_request(
    const std::string& method,
    const std::string& uri,
    const std::string& public_base_path
);
RouteResponse route_request(
    const std::string& method,
    const std::string& uri,
    const std::string& public_base_path,
    const std::string& authorization_header,
    MetadataStore* metadata_store,
    const std::string& pack_root = "",
    const std::string& content_type = "",
    const std::string& request_body = "",
    const std::string& data_root = "",
    const std::string& query_string = "",
    const std::string& signal_synth_cli = "",
    const std::string& cookie_header = ""
);

bool route_requires_authentication(
    const std::string& uri,
    const std::string& public_base_path
);

}  // namespace syn_sig_ra

#endif
