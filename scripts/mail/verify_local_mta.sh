#!/bin/sh
set -eu
DOMAIN=${DOMAIN:-timeonion.com}
MAIL_HOST=${MAIL_HOST:-www.timeonion.com}
SELECTOR=${SELECTOR:-synsigra}
RECIPIENT=${1:-}

fail() { echo "mail verify: ERROR: $*" >&2; exit 1; }
command -v postconf >/dev/null 2>&1 || fail "postfix is not installed"
command -v opendkim-testkey >/dev/null 2>&1 || fail "opendkim-tools is not installed"
systemctl is-active --quiet postfix || fail "postfix is not active"
systemctl is-active --quiet opendkim || fail "opendkim is not active"
if [ "$(id -u)" -eq 0 ]; then
  postfix check
else
  sudo postfix check
fi
ss -ltn | grep -Eq '127\.0\.0\.1:25|\[::1\]:25' || fail "Postfix is not listening on loopback SMTP"
if ss -ltn | grep -Eq '0\.0\.0\.0:25|\*:25|:::25'; then
  fail "Postfix is exposed beyond loopback"
fi
printf 'MX: '; dig +short MX "$DOMAIN" || true
printf 'A: '; dig +short A "$MAIL_HOST" || true
printf 'PTR: '; dig +short -x "$(hostname -I | awk '{print $1}')" || true
printf 'SPF: '; dig +short TXT "$DOMAIN" || true
printf 'DMARC: '; dig +short TXT "_dmarc.$DOMAIN" || true
if [ "$(id -u)" -eq 0 ]; then
  opendkim-testkey -d "$DOMAIN" -s "$SELECTOR" -vvv
else
  sudo opendkim-testkey -d "$DOMAIN" -s "$SELECTOR" -vvv
fi ||
  echo "mail verify: warning: DKIM DNS record is not published or not propagated" >&2
if [ -n "$RECIPIENT" ]; then
  token="synsigra-mail-test-$(date +%s)"
  printf 'From: noreply@%s\nTo: %s\nSubject: Synsigra local MTA test %s\n\n%s\n' \
    "$DOMAIN" "$RECIPIENT" "$token" "$token" | sendmail -t
  echo "Submitted test message: $token"
fi
postqueue -p || true
