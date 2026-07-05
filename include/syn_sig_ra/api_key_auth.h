#ifndef SYN_SIG_RA_API_KEY_AUTH_H
#define SYN_SIG_RA_API_KEY_AUTH_H

#include "syn_sig_ra/metadata_store.h"

#include <string>

namespace syn_sig_ra {

enum class AuthenticationStatus {
    authenticated,
    missing_credentials,
    malformed_credentials,
    invalid_credentials,
    storage_error
};

struct AuthenticationResult {
    AuthenticationStatus status;
    ApiKeyIdentity identity;
    std::string internal_error;
};

AuthenticationResult authenticate_bearer(
    const std::string& authorization_header,
    MetadataStore& store
);

}  // namespace syn_sig_ra

#endif
