#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "Run as root: sudo $0" >&2
  exit 2
fi

DOMAIN=${DOMAIN:-timeonion.com}
MAIL_HOST=${MAIL_HOST:-www.timeonion.com}
SELECTOR=${SELECTOR:-synsigra}
PUBLIC_IPV4=${PUBLIC_IPV4:-45.148.30.6}
APACHE_CONF=${APACHE_CONF:-/usr/local/apache2/conf/extra/syn_sig_ra.conf}
FROM_EMAIL=${FROM_EMAIL:-noreply@$DOMAIN}
KEY_DIR=/etc/opendkim/keys/$DOMAIN

printf 'postfix postfix/mailname string %s\n' "$DOMAIN" | debconf-set-selections
printf 'postfix postfix/main_mailer_type select Internet Site\n' | debconf-set-selections
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y postfix opendkim opendkim-tools

postconf -e "myhostname = $MAIL_HOST"
postconf -e "mydomain = $DOMAIN"
postconf -e 'myorigin = $mydomain'
postconf -e 'inet_interfaces = loopback-only'
postconf -e 'inet_protocols = ipv4'
postconf -e 'mydestination ='
postconf -e 'relay_domains ='
postconf -e 'mynetworks = 127.0.0.0/8'
postconf -e 'smtpd_relay_restrictions = permit_mynetworks, reject_unauth_destination'
postconf -e 'disable_vrfy_command = yes'
postconf -e 'smtp_tls_security_level = may'
postconf -e 'smtp_tls_loglevel = 1'
postconf -e "smtp_helo_name = $MAIL_HOST"
postconf -e 'milter_protocol = 6'
postconf -e 'milter_default_action = accept'
postconf -e 'smtpd_milters = inet:127.0.0.1:8891'
postconf -e 'non_smtpd_milters = inet:127.0.0.1:8891'

install -d -m 0750 -o opendkim -g opendkim "$KEY_DIR"
if [ ! -f "$KEY_DIR/$SELECTOR.private" ]; then
  opendkim-genkey -b 2048 -d "$DOMAIN" -D "$KEY_DIR" -s "$SELECTOR"
fi
chown opendkim:opendkim "$KEY_DIR/$SELECTOR.private"
chmod 0600 "$KEY_DIR/$SELECTOR.private"
chown opendkim:opendkim "$KEY_DIR/$SELECTOR.txt"
chmod 0644 "$KEY_DIR/$SELECTOR.txt"

install -d -m 0750 -o opendkim -g opendkim /etc/opendkim
printf '%s\n'   "${SELECTOR}._domainkey.${DOMAIN} ${DOMAIN}:${SELECTOR}:$KEY_DIR/$SELECTOR.private"   | install -m 0640 -o root -g opendkim /dev/stdin /etc/opendkim/key.table
printf '%s\n'   "*@${DOMAIN} ${SELECTOR}._domainkey.${DOMAIN}"   | install -m 0640 -o root -g opendkim /dev/stdin /etc/opendkim/signing.table
printf '%s\n'   '127.0.0.1' 'localhost' "$MAIL_HOST"   | install -m 0640 -o root -g opendkim /dev/stdin /etc/opendkim/trusted.hosts

install -m 0644 -o root -g root /dev/stdin /etc/opendkim.conf <<EOF
Syslog yes
SyslogSuccess yes
LogWhy yes
UMask 002
Canonicalization relaxed/simple
Mode sv
SubDomains no
OversignHeaders From
Socket inet:8891@127.0.0.1
PidFile /run/opendkim/opendkim.pid
UserID opendkim:opendkim
KeyTable file:/etc/opendkim/key.table
SigningTable refile:/etc/opendkim/signing.table
ExternalIgnoreList file:/etc/opendkim/trusted.hosts
InternalHosts file:/etc/opendkim/trusted.hosts
EOF

if [ -f /etc/default/opendkim ]; then
  sed -i '/^SOCKET=/d; /^RUNDIR=/d; /^USER=/d; /^GROUP=/d' /etc/default/opendkim
  printf '%s\n' 'SOCKET="inet:8891@127.0.0.1"' 'RUNDIR="/run/opendkim"' 'USER="opendkim"' 'GROUP="opendkim"' >> /etc/default/opendkim
fi
systemctl enable postfix opendkim
systemctl restart opendkim
systemctl restart postfix
postfix check

if [ -f "$APACHE_CONF" ]; then
  if ! grep -q '^SynSigRaEmailTransport ' "$APACHE_CONF"; then
    install -m 0644 -o root -g root /dev/stdin "$APACHE_CONF.local-email" <<EOF
# Managed by signal_synth_saas/scripts/mail/install_local_mta.sh
SynSigRaEmailTransport smtp
SynSigRaEmailPublicOrigin https://www.timeonion.com
SynSigRaEmailFrom $FROM_EMAIL
SynSigRaEmailFromName Synsigra
SynSigRaEmailSmtpUrl smtp://127.0.0.1:25
SynSigRaEmailSmtpTls disabled
EOF
    cat "$APACHE_CONF.local-email" >> "$APACHE_CONF"
    rm -f "$APACHE_CONF.local-email"
  fi
  /usr/local/apache2/bin/httpd -t
  /usr/local/apache2/bin/apachectl graceful
fi

echo "Local Postfix/OpenDKIM is configured. Add these DNS records before relying on delivery:"
cat "$KEY_DIR/$SELECTOR.txt"
printf 'SPF suggestion (merge with any existing SPF; keep one SPF TXT record): v=spf1 ip4:%s include:secureserver.net -all\n' "$PUBLIC_IPV4"
printf 'DMARC suggestion: _dmarc.%s TXT v=DMARC1; p=none; adkim=s; aspf=s\n' "$DOMAIN"
printf 'PTR: set %s reverse DNS to %s at the VPS provider.\n' "$PUBLIC_IPV4" "$MAIL_HOST"
