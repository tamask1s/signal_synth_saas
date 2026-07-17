#include "syn_sig_ra/sha256.h"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace syn_sig_ra {

namespace {

std::string hex_digest(const unsigned char* digest, unsigned int size) {
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < size; ++index) {
        encoded << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return encoded.str();
}

}  // namespace

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

    output = hex_digest(digest, SHA256_DIGEST_LENGTH);
    return true;
}

bool sha256_file_hex(
    const std::string& path,
    std::string& output,
    std::string& error
) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        error = "unable to open file for SHA-256 digest";
        return false;
    }
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr ||
        EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(context);
        error = "unable to initialize SHA-256 digest";
        return false;
    }
    char buffer[1024 * 1024];
    bool succeeded = true;
    while (input) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        if (count > 0 && EVP_DigestUpdate(
                context, buffer, static_cast<std::size_t>(count)) != 1) {
            succeeded = false;
            error = "unable to update SHA-256 digest";
            break;
        }
    }
    if (succeeded && !input.eof()) {
        succeeded = false;
        error = "unable to read complete file for SHA-256 digest";
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (succeeded &&
        EVP_DigestFinal_ex(context, digest, &digest_size) != 1) {
        succeeded = false;
        error = "unable to finalize SHA-256 digest";
    }
    EVP_MD_CTX_free(context);
    if (!succeeded || digest_size != SHA256_DIGEST_LENGTH) return false;
    output = hex_digest(digest, digest_size);
    return true;
}

}  // namespace syn_sig_ra
