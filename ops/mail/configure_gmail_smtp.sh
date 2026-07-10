#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root: sudo $0 <gmail-address>" >&2
    exit 2
fi
if [ "$#" -ne 1 ]; then
    echo "usage: sudo $0 <gmail-address>" >&2
    exit 2
fi

sender=$1
case "$sender" in
    *@gmail.com|*@googlemail.com) ;;
    *)
        echo "sender must be a Gmail address" >&2
        exit 2
        ;;
esac

apache_conf=${SYN_SIG_RA_APACHE_CONF:-/usr/local/apache2/conf/extra/syn_sig_ra.conf}
email_conf=${SYN_SIG_RA_EMAIL_CONF:-/usr/local/apache2/conf/extra/syn_sig_ra_email.conf}
httpd=${SYN_SIG_RA_HTTPD:-/usr/local/apache2/bin/httpd}
apachectl=${SYN_SIG_RA_APACHECTL:-/usr/local/apache2/bin/apachectl}
secret_dir=${SYN_SIG_RA_SECRET_DIR:-/etc/syn_sig_ra}
secret_file=${SYN_SIG_RA_GMAIL_SECRET_FILE:-$secret_dir/gmail-app-password}
source_file=${SYN_SIG_RA_GMAIL_SOURCE_FILE:-}
apache_group=${SYN_SIG_RA_APACHE_GROUP:-nogroup}

for path in "$apache_conf" "$httpd" "$apachectl"; do
    if [ ! -e "$path" ]; then
        echo "required path is missing: $path" >&2
        exit 1
    fi
done

install -d -m 0750 -o root -g "$apache_group" "$secret_dir"

if [ -n "$source_file" ]; then
    if [ ! -f "$source_file" ] || [ ! -r "$source_file" ]; then
        echo "Google App Password source file is not readable" >&2
        exit 2
    fi
    # Normalize a Google App Password without printing it or passing it as an
    # argument. The file content is never logged by this script.
    app_password=$(tr -d '[:space:]' <"$source_file")
elif [ ! -s "$secret_file" ] || [ "${ROTATE_SECRET:-0}" = "1" ]; then
    printf 'Google App Password for %s: ' "$sender" >&2
    old_stty=$(stty -g 2>/dev/null || true)
    if [ -n "$old_stty" ]; then
        stty -echo
    fi
    IFS= read -r app_password
    if [ -n "$old_stty" ]; then
        stty "$old_stty"
        printf '\n' >&2
    fi
else
    app_password=""
fi

if [ -n "$app_password" ]; then
    if [ "${#app_password}" -ne 16 ]; then
        echo "Google App Password must contain 16 characters" >&2
        exit 2
    fi
    temporary_secret=$(mktemp "$secret_dir/.gmail-app-password.XXXXXX")
    trap 'rm -f "$temporary_secret"' EXIT HUP INT TERM
    chmod 0640 "$temporary_secret"
    chown root:"$apache_group" "$temporary_secret"
    printf '%s\n' "$app_password" >"$temporary_secret"
    mv "$temporary_secret" "$secret_file"
    trap - EXIT HUP INT TERM
fi

if [ "$(stat -c '%U:%G' "$secret_file")" != "root:$apache_group" ]; then
    chown root:"$apache_group" "$secret_file"
fi
chmod 0640 "$secret_file"

temporary_config=$(mktemp "${email_conf}.XXXXXX")
trap 'rm -f "$temporary_config"' EXIT HUP INT TERM
cat >"$temporary_config" <<EOF
# Managed by ops/mail/configure_gmail_smtp.sh.
SynSigRaEmailTransport smtp
SynSigRaEmailPublicOrigin https://www.timeonion.com
SynSigRaEmailFrom $sender
SynSigRaEmailFromName "Synsigra"
SynSigRaEmailSmtpUrl smtp://smtp.gmail.com:587
SynSigRaEmailSmtpTls required
SynSigRaEmailSmtpUsername $sender
SynSigRaEmailSmtpPasswordFile $secret_file
EOF
chmod 0644 "$temporary_config"
chown root:root "$temporary_config"
mv "$temporary_config" "$email_conf"
trap - EXIT HUP INT TERM

include_line='Include conf/extra/syn_sig_ra_email.conf'
if ! grep -Fqx "$include_line" "$apache_conf"; then
    cp "$apache_conf" "$apache_conf.before-gmail-$(date -u +%Y%m%dT%H%M%SZ)"
    printf '\n%s\n' "$include_line" >>"$apache_conf"
fi

"$httpd" -t
"$apachectl" graceful

echo "Gmail SMTP configured for $sender"
echo "secret: $secret_file (root:$apache_group 0640)"
echo "config: $email_conf"
