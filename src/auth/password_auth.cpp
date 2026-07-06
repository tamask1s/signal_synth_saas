#include "syn_sig_ra/password_auth.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

const int kIterations = 310000;
const std::size_t kSaltBytes = 16;
const std::size_t kHashBytes = 32;

std::string hex_encode(const unsigned char* bytes, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return output.str();
}

bool hex_decode(const std::string& text, std::vector<unsigned char>& bytes) {
    if (text.empty() || text.size() % 2 != 0) return false;
    bytes.clear();
    bytes.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2) {
        unsigned int value = 0;
        std::istringstream input(text.substr(index, 2));
        input >> std::hex >> value;
        if (!input || !input.eof()) return false;
        bytes.push_back(static_cast<unsigned char>(value));
    }
    return true;
}

bool derive(
    const std::string& password,
    const std::vector<unsigned char>& salt,
    unsigned char* hash,
    std::string& error
) {
    if (PKCS5_PBKDF2_HMAC(
            password.data(),
            static_cast<int>(password.size()),
            salt.data(),
            static_cast<int>(salt.size()),
            kIterations,
            EVP_sha256(),
            static_cast<int>(kHashBytes),
            hash
        ) != 1) {
        error = "password derivation failed";
        return false;
    }
    return true;
}

}  // namespace

namespace syn_sig_ra {

bool normalize_email(const std::string& input, std::string& email) {
    std::string::size_type begin = 0;
    while (begin < input.size() &&
           std::isspace(static_cast<unsigned char>(input[begin]))) ++begin;
    std::string::size_type end = input.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
    email = input.substr(begin, end - begin);
    for (std::size_t index = 0; index < email.size(); ++index) {
        const unsigned char value = static_cast<unsigned char>(email[index]);
        if (value >= 128 || std::isspace(value)) return false;
        email[index] = static_cast<char>(std::tolower(value));
    }
    const std::string::size_type at = email.find('@');
    return email.size() >= 3 && email.size() <= 254 &&
           at != std::string::npos && at > 0 &&
           at == email.rfind('@') && at + 1 < email.size() &&
           email.find('.', at + 1) != std::string::npos;
}

bool hash_password(
    const std::string& password,
    std::string& salt_hex,
    std::string& hash_hex,
    std::string& error
) {
    if (password.size() < 12 || password.size() > 128) {
        error = "password must contain 12-128 characters";
        return false;
    }
    unsigned char salt[kSaltBytes];
    unsigned char hash[kHashBytes];
    if (RAND_bytes(salt, sizeof(salt)) != 1) {
        error = "password salt generation failed";
        return false;
    }
    const std::vector<unsigned char> salt_vector(salt, salt + sizeof(salt));
    if (!derive(password, salt_vector, hash, error)) return false;
    salt_hex = hex_encode(salt, sizeof(salt));
    hash_hex = hex_encode(hash, sizeof(hash));
    OPENSSL_cleanse(hash, sizeof(hash));
    return true;
}

bool verify_password(
    const std::string& password,
    const std::string& salt_hex,
    const std::string& expected_hash_hex,
    bool& matches,
    std::string& error
) {
    matches = false;
    std::vector<unsigned char> salt;
    std::vector<unsigned char> expected;
    if (!hex_decode(salt_hex, salt) || salt.size() != kSaltBytes ||
        !hex_decode(expected_hash_hex, expected) ||
        expected.size() != kHashBytes) {
        error = "stored password credential is invalid";
        return false;
    }
    unsigned char actual[kHashBytes];
    if (!derive(password, salt, actual, error)) return false;
    matches = CRYPTO_memcmp(actual, expected.data(), kHashBytes) == 0;
    OPENSSL_cleanse(actual, sizeof(actual));
    return true;
}

}  // namespace syn_sig_ra
