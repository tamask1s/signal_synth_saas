#ifndef SYN_SIG_RA_PASSWORD_AUTH_H
#define SYN_SIG_RA_PASSWORD_AUTH_H

#include <string>

namespace syn_sig_ra {

bool normalize_email(
    const std::string& input,
    std::string& email
);

bool hash_password(
    const std::string& password,
    std::string& salt_hex,
    std::string& hash_hex,
    std::string& error
);

bool verify_password(
    const std::string& password,
    const std::string& salt_hex,
    const std::string& expected_hash_hex,
    bool& matches,
    std::string& error
);

}  // namespace syn_sig_ra

#endif
