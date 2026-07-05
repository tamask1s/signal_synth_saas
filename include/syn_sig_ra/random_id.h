#ifndef SYN_SIG_RA_RANDOM_ID_H
#define SYN_SIG_RA_RANDOM_ID_H

#include <string>

namespace syn_sig_ra {

bool random_id(
    const std::string& prefix,
    std::string& identifier,
    std::string& error
);

}  // namespace syn_sig_ra

#endif
