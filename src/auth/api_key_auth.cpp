#include "syn_sig_ra/api_key_auth.h"

#include "syn_sig_ra/sha256.h"

#include <cctype>
#include <string>

namespace {

bool equals_bearer(const std::string& value) {
    const char expected[] = "bearer";
    if (value.size() != sizeof(expected) - 1) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index])) !=
            expected[index]) {
            return false;
        }
    }
    return true;
}

}  // namespace

namespace syn_sig_ra {

AuthenticationResult authenticate_bearer(
    const std::string& authorization_header,
    MetadataStore& store
) {
    AuthenticationResult result;
    if (authorization_header.empty()) {
        result.status = AuthenticationStatus::missing_credentials;
        return result;
    }

    const std::string::size_type separator =
        authorization_header.find(' ');
    if (separator == std::string::npos ||
        !equals_bearer(authorization_header.substr(0, separator))) {
        result.status = AuthenticationStatus::malformed_credentials;
        return result;
    }

    const std::string token = authorization_header.substr(separator + 1);
    if (token.empty() ||
        token.find_first_of(" \t\r\n") != std::string::npos) {
        result.status = AuthenticationStatus::malformed_credentials;
        return result;
    }

    std::string key_hash;
    if (!sha256_hex(token, key_hash, result.internal_error)) {
        result.status = AuthenticationStatus::storage_error;
        return result;
    }

    const ApiKeyLookupStatus lookup = store.find_active_api_key(
        key_hash,
        result.identity,
        result.internal_error
    );
    if (lookup == ApiKeyLookupStatus::not_found) {
        result.status = AuthenticationStatus::invalid_credentials;
        return result;
    }
    if (lookup == ApiKeyLookupStatus::storage_error) {
        result.status = AuthenticationStatus::storage_error;
        return result;
    }
    if (!store.record_api_key_use(result.identity, result.internal_error)) {
        result.status = AuthenticationStatus::storage_error;
        return result;
    }

    result.status = AuthenticationStatus::authenticated;
    return result;
}

bool session_token_from_cookie(
    const std::string& cookie_header,
    std::string& token
) {
    token.clear();
    const std::string name = "syn_sig_ra_session=";
    std::string::size_type begin = 0;
    while (begin < cookie_header.size()) {
        while (begin < cookie_header.size() &&
               (cookie_header[begin] == ' ' || cookie_header[begin] == ';')) {
            ++begin;
        }
        const std::string::size_type end = cookie_header.find(';', begin);
        const std::string part = cookie_header.substr(
            begin,
            end == std::string::npos ? std::string::npos : end - begin
        );
        if (part.compare(0, name.size(), name) == 0) {
            token = part.substr(name.size());
            return !token.empty() &&
                token.find_first_of(" \t\r\n;") == std::string::npos;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

AuthenticationResult authenticate_session(
    const std::string& cookie_header,
    MetadataStore& store,
    AccountRecord* account
) {
    AuthenticationResult result;
    std::string token;
    if (!session_token_from_cookie(cookie_header, token)) {
        result.status = AuthenticationStatus::missing_credentials;
        return result;
    }
    std::string token_hash;
    if (!sha256_hex(token, token_hash, result.internal_error)) {
        result.status = AuthenticationStatus::storage_error;
        return result;
    }
    AccountRecord loaded;
    const RecordLookupStatus status = store.find_session(
        token_hash, result.identity, loaded, result.internal_error
    );
    if (status == RecordLookupStatus::storage_error) {
        result.status = AuthenticationStatus::storage_error;
        return result;
    }
    if (status == RecordLookupStatus::not_found) {
        result.status = AuthenticationStatus::invalid_credentials;
        return result;
    }
    if (!store.record_api_key_use(result.identity, result.internal_error)) {
        result.status = AuthenticationStatus::storage_error;
        return result;
    }
    if (account != nullptr) *account = loaded;
    result.status = AuthenticationStatus::authenticated;
    return result;
}

}  // namespace syn_sig_ra
