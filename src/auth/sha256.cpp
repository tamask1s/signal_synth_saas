#include "syn_sig_ra/sha256.h"

#include <openssl/sha.h>

#include <iomanip>
#include <sstream>
#include <string>

namespace syn_sig_ra {

bool sha256_hex(
    const std::string& input,
    std::string& output,
    std::string& error
) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (SHA256(
            reinterpret_cast<const unsigned char*>(input.data()),
            input.size(),
            digest
        ) == nullptr) {
        error = "unable to calculate SHA-256 digest";
        return false;
    }

    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < SHA256_DIGEST_LENGTH; ++index) {
        encoded << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    output = encoded.str();
    return true;
}

}  // namespace syn_sig_ra
