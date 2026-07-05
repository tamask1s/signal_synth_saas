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
    const std::string& pack_root = ""
);

bool route_requires_authentication(
    const std::string& uri,
    const std::string& public_base_path
);

}  // namespace syn_sig_ra

#endif
