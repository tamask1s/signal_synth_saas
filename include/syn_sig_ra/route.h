#ifndef SYN_SIG_RA_ROUTE_H
#define SYN_SIG_RA_ROUTE_H

#include <string>

namespace syn_sig_ra {

enum class RouteDisposition {
    declined,
    handled
};

struct RouteResponse {
    RouteDisposition disposition;
    int status;
    std::string content_type;
    std::string body;
};

RouteResponse route_request(const std::string& method, const std::string& uri);
RouteResponse route_request(
    const std::string& method,
    const std::string& uri,
    const std::string& public_base_path
);

}  // namespace syn_sig_ra

#endif
