#ifndef SYN_SIG_RA_TRANSACTIONAL_EMAIL_H
#define SYN_SIG_RA_TRANSACTIONAL_EMAIL_H

#include <string>

namespace syn_sig_ra {

enum class EmailTransport {
    disabled,
    smtp,
    capture_file
};

enum class EmailTlsMode {
    required,
    opportunistic,
    disabled
};

enum class EmailSendStatus {
    sent,
    disabled,
    invalid_config,
    provider_error
};

struct EmailConfig {
    EmailConfig();

    EmailTransport transport;
    EmailTlsMode tls_mode;
    std::string public_origin;
    std::string from_email;
    std::string from_name;
    std::string smtp_url;
    std::string smtp_username;
    std::string smtp_password;
    std::string smtp_password_file;
    std::string capture_directory;
    long connect_timeout_seconds;
    long send_timeout_seconds;
};

bool validate_public_origin(const std::string& value, std::string& error);
bool validate_email_address(const std::string& value, std::string& error);
bool email_delivery_configured(const EmailConfig& config);

EmailSendStatus send_transactional_email(
    const EmailConfig& config,
    const std::string& recipient_email,
    const std::string& subject,
    const std::string& text_body,
    std::string& error
);

}  // namespace syn_sig_ra

#endif
