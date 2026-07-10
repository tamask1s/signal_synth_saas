#!/bin/sh
set -eu

sender=${1:-}
if [ -z "$sender" ]; then
    echo "usage: $0 <configured-gmail-address>" >&2
    exit 2
fi

email_conf=${SYN_SIG_RA_EMAIL_CONF:-/usr/local/apache2/conf/extra/syn_sig_ra_email.conf}
secret_file=${SYN_SIG_RA_GMAIL_SECRET_FILE:-/etc/syn_sig_ra/gmail-app-password}
httpd=${SYN_SIG_RA_HTTPD:-/usr/local/apache2/bin/httpd}
base_url=${SYN_SIG_RA_BASE_URL:-https://www.timeonion.com/syn_sig_ra}

test -s "$secret_file"
test -f "$email_conf"
grep -Fqx "SynSigRaEmailFrom $sender" "$email_conf"
grep -Fqx "SynSigRaEmailSmtpUrl smtp://smtp.gmail.com:587" "$email_conf"
grep -Fqx "SynSigRaEmailSmtpTls required" "$email_conf"
"$httpd" -t >/dev/null

smtp_banner=$(timeout 15 openssl s_client \
    -quiet \
    -starttls smtp \
    -connect smtp.gmail.com:587 \
    -servername smtp.gmail.com </dev/null 2>/dev/null | head -n 1 || true)
case "$smtp_banner" in
    250*|220*) ;;
    *)
        echo "Gmail STARTTLS connectivity check failed" >&2
        exit 1
        ;;
esac

response=$(curl -fsS \
    -H 'Content-Type: application/json' \
    -d "{\"email\":\"$sender\"}" \
    "$base_url/v1/auth/password-reset/request")
printf '%s\n' "$response" | grep -q '"status":"accepted"'

echo "Gmail SMTP configuration and SaaS reset-email submission succeeded."
echo "Confirm that the reset message arrived in Inbox or Spam."
