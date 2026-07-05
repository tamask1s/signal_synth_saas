#include "syn_sig_ra/sha256.h"

#include <openssl/evp.h>

#include <iomanip>
#include <sstream>
#include <string>

namespace syn_sig_ra {

bool sha256_hex(
    const std::string& input,
    std::string& output,
    std::string& error
) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        error = "unable to allocate SHA-256 context";
        return false;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    const bool succeeded =
        EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(context, input.data(), input.size()) == 1 &&
        EVP_DigestFinal_ex(context, digest, &digest_size) == 1;
    EVP_MD_CTX_free(context);

    if (!succeeded) {
        error = "unable to calculate SHA-256 digest";
        return false;
    }

    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_size; ++index) {
        encoded << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    output = encoded.str();
    return true;
}

}  // namespace syn_sig_ra
