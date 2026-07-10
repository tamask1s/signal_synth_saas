# Gmail SMTP for transactional account email

Synsigra can use a real Gmail account as its authenticated transactional SMTP
sender. The SaaS already contains a libcurl SMTP client. Postfix/OpenDKIM are installed
as an optional loopback/direct-delivery experiment, but the production path
should use authenticated Gmail SMTP because the VPS IP is currently blocked by
recipient MX policy.

## Google account preparation

1. Enable 2-Step Verification on the sender Google account.
2. Open <https://myaccount.google.com/apppasswords>.
3. Create a dedicated app password named `Synsigra VPS`.
4. Keep the generated 16-character value open until the server script asks for
   it. Do not use or store the normal Google account password.

Google can hide App Passwords for organization-managed, security-key-only, or
Advanced Protection accounts. In that case use a dedicated transactional mail
provider or implement OAuth2 instead of weakening the account.

## First installation

From the checked-out SaaS repository on the VPS:

```sh
sudo ops/mail/configure_gmail_smtp.sh your-address@gmail.com
```

The script prompts without echo for the app password. It stores the normalized
value at `/etc/syn_sig_ra/gmail-app-password` with mode `0640`, readable only by
root and the Apache group. The Apache configuration contains only the secret
file path, never the credential itself.

Verify the TLS connection and submit a real password-reset email through the
live SaaS:

```sh
scripts/task3_mail_dolgok.py gmail-verify your-address@gmail.com
```

The final delivery check is the arrival of that message in Gmail Inbox or
Spam. The reset link can be ignored after the test.

## Migration to another VPS

1. Deploy the repository and Apache module normally.
2. Create a new Google App Password. Do not copy the old secret from backups.
3. Run `configure_gmail_smtp.sh` on the new VPS.
4. Run `verify_gmail_smtp.sh` and confirm delivery.
5. Revoke the old VPS app password in the Google account.

The configuration script is idempotent. It reuses an existing secret by
default. To rotate it, create a new Google App Password and run:

```sh
sudo ROTATE_SECRET=1 ops/mail/configure_gmail_smtp.sh your-address@gmail.com
```

Google revokes existing App Passwords when the main account password changes;
run the same rotation command afterward.

## Removal or rollback

Remove the `Include conf/extra/syn_sig_ra_email.conf` line from
`/usr/local/apache2/conf/extra/syn_sig_ra.conf`, validate Apache, and reload it.
Then revoke the `Synsigra VPS` App Password in the Google account and delete
`/etc/syn_sig_ra/gmail-app-password`.

Gmail authenticates and sends the message as the configured Gmail address.
This avoids depending on the VPS public IP's mail reputation, PTR, SPF, DKIM,
or DMARC records. Gmail sending limits still apply, so this setup is suitable
for the current low-volume beta rather than bulk mail.
