#include "syn_sig_ra/random_id.h"

#include <openssl/rand.h>

#include <iomanip>
#include <sstream>
#include <string>

namespace syn_sig_ra {

bool random_id(
    const std::string& prefix,
    std::string& identifier,
    std::string& error
) {
    unsigned char random_bytes[16];
    if (RAND_bytes(random_bytes, sizeof(random_bytes)) != 1) {
        error = "secure random identifier generation failed";
        return false;
    }
    std::ostringstream output;
    output << prefix << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < sizeof(random_bytes); ++index) {
        output << std::setw(2)
               << static_cast<unsigned int>(random_bytes[index]);
    }
    identifier = output.str();
    return true;
}

}  // namespace syn_sig_ra
