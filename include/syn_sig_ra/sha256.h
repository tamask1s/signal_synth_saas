#ifndef SYN_SIG_RA_SHA256_H
#define SYN_SIG_RA_SHA256_H

#include <string>

namespace syn_sig_ra {

bool sha256_hex(
    const std::string& input,
    std::string& output,
    std::string& error
);

}  // namespace syn_sig_ra

#endif
