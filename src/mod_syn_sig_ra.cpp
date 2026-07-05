#include "syn_sig_ra/route.h"

extern "C" {
#include <apr_strings.h>
#include <httpd.h>
#include <http_config.h>
#include <http_protocol.h>
}

#include <string>

namespace {

int syn_sig_ra_handler(request_rec* request) {
    const std::string method =
        request->method == nullptr ? std::string() : request->method;
    const std::string uri =
        request->uri == nullptr ? std::string() : request->uri;
    const syn_sig_ra::RouteResponse response =
        syn_sig_ra::route_request(method, uri);

    if (response.disposition == syn_sig_ra::RouteDisposition::declined) {
        return DECLINED;
    }

    request->status = response.status;
    ap_set_content_type(
        request,
        apr_pstrdup(request->pool, response.content_type.c_str())
    );
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
    nullptr,
    nullptr,
    nullptr,
    syn_sig_ra_register_hooks
};

}
