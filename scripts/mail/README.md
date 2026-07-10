# Local outbound mail (Postfix + OpenDKIM)

This is an optional send-only MTA for the SynSigRa VPS. Postfix accepts SMTP
only from loopback and delivers outbound mail directly to the recipient domain's
MX. OpenDKIM signs `From: noreply@timeonion.com` messages. The SaaS uses
`smtp://127.0.0.1:25` with `SynSigRaEmailSmtpTls disabled`; plaintext is accepted
only for a loopback URL by the application.

This does not create an inbox and does not replace the existing GoDaddy MX.
It also cannot fix DNS/reputation automatically. Google requires senders to
have SPF or DKIM, valid forward/reverse DNS and TLS for delivery to Gmail;
DKIM, SPF and a correct PTR are therefore part of the deployment, not optional
polish.

## Install or migrate

Run on the VPS as root:

```sh
cd /home/graphyt2/git/signal_synth_saas
sudo DOMAIN=timeonion.com MAIL_HOST=www.timeonion.com \
  SELECTOR=synsigra scripts/mail/install_local_mta.sh
```

The script is idempotent. It preserves an existing DKIM private key, configures
Postfix as loopback-only, enables/restarts both services, and adds the local
SMTP directives to `/usr/local/apache2/conf/extra/syn_sig_ra.conf` once. It does
not touch the SaaS SQLite database, so a database migration/reset is unrelated
to mail migration.

For a fresh VPS, copy this directory with the repository and run the script
again after installing the same Apache module. For a migration, preserve:

- `/etc/postfix/main.cf` (or re-run the script and review local overrides);
- `/etc/opendkim/keys/timeonion.com/synsigra.private`;
- `/etc/opendkim/key.table`, `signing.table`, and `trusted.hosts`;
- the DNS records listed below.

Never commit or paste the private DKIM key or SMTP queue contents.

## DNS prerequisites

The script prints the exact DKIM TXT value. Publish it as:

- `synsigra._domainkey.timeonion.com TXT <printed value>`;
- one merged SPF TXT record at `timeonion.com`. Because the current MX is
  GoDaddy Professional Email, merge the VPS IP with its existing include, for
  example `v=spf1 ip4:45.148.30.6 include:secureserver.net -all`;
- `_dmarc.timeonion.com TXT v=DMARC1; p=none; adkim=s; aspf=s` initially;
- set the VPS reverse DNS/PTR for `45.148.30.6` to `www.timeonion.com`, and
  keep `A www.timeonion.com` pointing to `45.148.30.6`.

Do not add a second SPF record. If other services already send as
`@timeonion.com`, merge their mechanisms into the existing single record.
After DNS propagation, verify:

```sh
scripts/mail/verify_local_mta.sh
```

## Sending test mail

After DNS is configured, send a test to an address you control:

```sh
scripts/mail/verify_local_mta.sh you@example.net
```

Inspect `/var/log/mail.log`, `mailq`, and the recipient's authentication headers
for SPF, DKIM and DMARC results. A queue acceptance is not proof of inbox
placement; direct-to-MX delivery can still be spam-filtered.

If the VPS provider blocks outbound port 25 or the PTR cannot be changed, use
the SaaS's existing authenticated SMTP configuration instead:
`SynSigRaEmailTransport smtp`, `SynSigRaEmailSmtpUrl smtps://...`, username,
and a root-owned password file, with TLS `required`.
