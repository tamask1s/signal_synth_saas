#include "syn_sig_ra/transactional_email.h"

#include "syn_sig_ra/random_id.h"

#include <curl/curl.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct UploadBuffer {
    std::string payload;
    std::size_t offset;
};

std::string trim_right_slash(const std::string& value) {
    if (value.size() > 1 && value[value.size() - 1] == '/') {
        return value.substr(0, value.size() - 1);
    }
    return value;
}

std::string read_file(const std::string& path, std::string& error) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        error = "unable to read SMTP password file";
        return std::string();
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string value = buffer.str();
    while (!value.empty() &&
           (value[value.size() - 1] == '\n' ||
            value[value.size() - 1] == '\r')) {
        value.erase(value.size() - 1);
    }
    return value;
}

std::string header_safe(const std::string& value) {
    std::string output;
    for (std::string::const_iterator it = value.begin(); it != value.end();
         ++it) {
        const unsigned char c = static_cast<unsigned char>(*it);
        if (c == '\r' || c == '\n') {
            output.push_back(' ');
        } else if (c >= 32 && c < 127) {
            output.push_back(static_cast<char>(c));
        }
    }
    return output;
}

std::string mail_payload(
    const syn_sig_ra::EmailConfig& config,
    const std::string& recipient_email,
    const std::string& subject,
    const std::string& text_body
) {
    std::ostringstream output;
    const std::string from_name = config.from_name.empty()
        ? "SynSigRa"
        : header_safe(config.from_name);
    output << "From: " << from_name << " <" << config.from_email << ">\r\n"
           << "To: <" << recipient_email << ">\r\n"
           << "Subject: " << header_safe(subject) << "\r\n"
           << "MIME-Version: 1.0\r\n"
           << "Content-Type: text/plain; charset=UTF-8\r\n"
           << "Content-Transfer-Encoding: 8bit\r\n"
           << "\r\n";
    for (std::string::const_iterator it = text_body.begin();
         it != text_body.end(); ++it) {
        if (*it == '\n') {
            output << "\r\n";
        } else if (*it != '\r') {
            output << *it;
        }
    }
    output << "\r\n";
    return output.str();
}

std::size_t read_callback(
    char* ptr,
    std::size_t size,
    std::size_t nmemb,
    void* userdata
) {
    UploadBuffer* upload = static_cast<UploadBuffer*>(userdata);
    const std::size_t capacity = size * nmemb;
    if (capacity == 0 || upload->offset >= upload->payload.size()) {
        return 0;
    }
    const std::size_t remaining = upload->payload.size() - upload->offset;
    const std::size_t count = remaining < capacity ? remaining : capacity;
    std::memcpy(ptr, upload->payload.data() + upload->offset, count);
    upload->offset += count;
    return count;
}

bool loopback_smtp_url(const std::string& value) {
    static const char* prefixes[] = {
        "smtp://127.0.0.1", "smtp://localhost", "smtp://[::1]"
    };
    for (const char* prefix : prefixes) {
        const std::string expected(prefix);
        if (value.compare(0, expected.size(), expected) == 0 &&
            (value.size() == expected.size() || value[expected.size()] == ':')) {
            return true;
        }
    }
    return false;
}

bool directory_writable(const std::string& path) {
    struct stat info;
    return !path.empty() &&
           stat(path.c_str(), &info) == 0 &&
           S_ISDIR(info.st_mode) &&
           access(path.c_str(), W_OK | X_OK) == 0;
}

syn_sig_ra::EmailSendStatus write_capture(
    const syn_sig_ra::EmailConfig& config,
    const std::string& recipient_email,
    const std::string& payload,
    std::string& error
) {
    if (!directory_writable(config.capture_directory)) {
        error = "email capture directory is not writable";
        return syn_sig_ra::EmailSendStatus::invalid_config;
    }
    std::string id;
    if (!syn_sig_ra::random_id("email_", id, error)) {
        return syn_sig_ra::EmailSendStatus::provider_error;
    }
    const std::string path =
        config.capture_directory + "/" + id + ".eml";
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output << "X-SynSigRa-Capture-To: " << recipient_email << "\n";
    output << payload;
    if (!output) {
        error = "unable to write captured email";
        return syn_sig_ra::EmailSendStatus::provider_error;
    }
    return syn_sig_ra::EmailSendStatus::sent;
}

}  // namespace

namespace syn_sig_ra {

EmailConfig::EmailConfig()
    : transport(EmailTransport::disabled),
      tls_mode(EmailTlsMode::required),
      connect_timeout_seconds(10),
      send_timeout_seconds(20) {}

bool validate_public_origin(const std::string& value, std::string& error) {
    const std::string origin = trim_right_slash(value);
    if (origin.compare(0, 8, "https://") != 0 &&
        origin.compare(0, 7, "http://") != 0) {
        error = "SynSigRaEmailPublicOrigin must start with http:// or https://";
        return false;
    }
    if (origin.find(' ') != std::string::npos ||
        origin.find('?') != std::string::npos ||
        origin.find('#') != std::string::npos ||
        origin.find("//", origin.find("://") + 3) != std::string::npos) {
        error = "SynSigRaEmailPublicOrigin must be a normalized origin";
        return false;
    }
    return true;
}

bool validate_email_address(const std::string& value, std::string& error) {
    const std::string::size_type at = value.find('@');
    if (value.empty() || at == std::string::npos || at == 0 ||
        at + 1 >= value.size() ||
        value.find('@', at + 1) != std::string::npos ||
        value.find_first_of(" \t\r\n<>") != std::string::npos ||
        value.find('.', at + 1) == std::string::npos) {
        error = "email address must be a simple addr-spec";
        return false;
    }
    return true;
}

bool email_delivery_configured(const EmailConfig& config) {
    if (config.transport == EmailTransport::disabled) {
        return false;
    }
    std::string error;
    if (!validate_public_origin(config.public_origin, error) ||
        !validate_email_address(config.from_email, error)) {
        return false;
    }
    if (config.transport == EmailTransport::capture_file) {
        return directory_writable(config.capture_directory);
    }
    if (config.smtp_url.empty()) return false;
    return config.tls_mode != EmailTlsMode::disabled ||
        loopback_smtp_url(config.smtp_url);
}

EmailSendStatus send_transactional_email(
    const EmailConfig& config,
    const std::string& recipient_email,
    const std::string& subject,
    const std::string& text_body,
    std::string& error
) {
    if (config.transport == EmailTransport::disabled) {
        error = "email delivery is disabled";
        return EmailSendStatus::disabled;
    }
    if (!validate_public_origin(config.public_origin, error) ||
        !validate_email_address(config.from_email, error) ||
        !validate_email_address(recipient_email, error)) {
        return EmailSendStatus::invalid_config;
    }

    const std::string payload =
        mail_payload(config, recipient_email, subject, text_body);
    if (config.transport == EmailTransport::capture_file) {
        return write_capture(config, recipient_email, payload, error);
    }
    if (config.transport != EmailTransport::smtp || config.smtp_url.empty()) {
        error = "SMTP URL is not configured";
        return EmailSendStatus::invalid_config;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        error = "unable to initialize libcurl";
        return EmailSendStatus::provider_error;
    }

    UploadBuffer upload;
    upload.payload = payload;
    upload.offset = 0;

    const std::string mail_from = "<" + config.from_email + ">";
    const std::string rcpt = "<" + recipient_email + ">";
    struct curl_slist* recipients = nullptr;
    recipients = curl_slist_append(recipients, rcpt.c_str());

    std::string password = config.smtp_password;
    if (password.empty() && !config.smtp_password_file.empty()) {
        password = read_file(config.smtp_password_file, error);
        if (!error.empty()) {
            curl_slist_free_all(recipients);
            curl_easy_cleanup(curl);
            return EmailSendStatus::invalid_config;
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, config.smtp_url.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, mail_from.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    const curl_usessl tls_mode = config.tls_mode == EmailTlsMode::required
        ? CURLUSESSL_ALL
        : (config.tls_mode == EmailTlsMode::opportunistic
            ? CURLUSESSL_TRY : CURLUSESSL_NONE);
    if (config.tls_mode == EmailTlsMode::disabled &&
        !loopback_smtp_url(config.smtp_url)) {
        error = "plaintext SMTP is permitted only for a loopback URL";
        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);
        return EmailSendStatus::invalid_config;
    }
    curl_easy_setopt(curl, CURLOPT_USE_SSL, tls_mode);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, config.connect_timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config.send_timeout_seconds);
    if (!config.smtp_username.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, config.smtp_username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
    }

    const CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        error = curl_easy_strerror(result);
    }
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    return result == CURLE_OK
        ? EmailSendStatus::sent
        : EmailSendStatus::provider_error;
}

}  // namespace syn_sig_ra
