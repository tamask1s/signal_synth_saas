#ifndef SYN_SIG_RA_MCP_SERVER_H
#define SYN_SIG_RA_MCP_SERVER_H

#include "syn_sig_ra/route.h"

#include <string>

namespace syn_sig_ra {

class MetadataStore;

RouteResponse handle_mcp_request(
    const std::string& method,
    const std::string& public_base_path,
    const std::string& authorization_header,
    MetadataStore* metadata_store,
    const std::string& pack_root,
    const std::string& data_root,
    const std::string& signal_synth_cli,
    const EmailConfig& email_config,
    const std::string& content_type,
    const std::string& request_body,
    const std::string& accept_header,
    const std::string& origin_header,
    const std::string& protocol_version_header
);

}  // namespace syn_sig_ra

#endif
