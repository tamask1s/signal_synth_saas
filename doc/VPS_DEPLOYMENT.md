# VPS deployment runbook

This document describes the current `timeonion.com` deployment shape. The
server runs the old custom Apache 2.2 installation from `/usr/local/apache2`.
Keep using that runtime for now; do not switch traffic to the packaged Apache
2.4 service until the existing `mod_ts` site behavior is migrated and tested.

The deploy script installs `ops/apache/synsigra-apache22.logrotate` under
`/etc/logrotate.d/`. It rotates the custom Apache access/error logs daily or
at 10 MiB, keeps seven compressed revisions, and uses `copytruncate` because
this legacy Apache service does not use the distribution log paths.

The same deploy installs the versioned main landing package into
`/usr/local/apache2/htdocs/frontend/` without deleting unrelated paths such as
the preserved `/tamaskis` site.

All public SaaS routes must stay below:

```text
/syn_sig_ra/...
```

## Runtime facts

Current production shape:

```text
Public HTTP/HTTPS:     nginx.service (ports 80/443)
TLS certificate:      /etc/letsencrypt/live/timeonion.com/
TLS renewal:          certbot.timer
Apache service:       apache22.service
Apache listen address: 127.0.0.1:8080
Apache binary:        /usr/local/apache2/bin/httpd
Apache control:       /usr/local/apache2/bin/apachectl
APXS:                 /usr/local/apache2/bin/apxs
Apache config:        /usr/local/apache2/conf/httpd.conf
Apache module dir:    /usr/local/apache2/modules
SaaS config include:  /usr/local/apache2/conf/extra/syn_sig_ra.conf
Apache error log:     /usr/local/apache2/logs/error_log
Data root:            /var/lib/syn_sig_ra
Generator CLI:        /opt/signal_synth/bin/signal-synth
Pack root:            /opt/signal_synth_saas/packs
Worker service:       syn_sig_ra_worker.service
```

The packaged `apache2.service` may exist on the host, but it should remain
disabled/inactive. nginx owns the public ports and proxies HTTPS requests to
the custom Apache 2.2 backend. Apache is deliberately inaccessible from the
network.

The versioned nginx configuration is
`ops/nginx/timeonion.conf`; deployment installs it through the active
`/etc/nginx/sites-enabled/timeonion.conf` target, validates it, restores the
previous file on validation failure, and reloads nginx. HTTP serves only ACME challenges and redirects all
other requests to the canonical `https://www.timeonion.com` origin. The apex
HTTPS origin redirects there as well, which keeps future login cookies on one
origin. Unknown hostnames are rejected. The edge also limits request bodies to
64 KiB, each source IP to 30 concurrent connections, and request rates to
30/second with a 60-request burst allowance.

Certbot renews the certificate twice daily when needed;
`ops/letsencrypt/reload-nginx.sh` validates and reloads nginx after a real
renewal. Validate the renewal path after configuration changes:

```sh
sudo certbot renew --dry-run --no-random-sleep-on-renew
sudo systemctl status certbot.timer nginx apache22 --no-pager
```

## Build prerequisites

Install the generic build/runtime dependencies:

```sh
sudo apt-get update
sudo apt-get install -y \
  cmake \
  g++ \
  libssl-dev \
  libsqlite3-dev \
  libjansson-dev \
  zip \
  unzip \
  curl \
  rsync \
  openssl
```

The module must be compiled with the same Apache/APR headers as the runtime
that will load it. On this VPS that means:

```sh
/usr/local/apache2/bin/apxs -q INCLUDEDIR
/usr/local/apache2/bin/apxs -q APR_CONFIG
```

Do not build the production module with `/usr/bin/apxs` if the module will be
loaded by `/usr/local/apache2/bin/httpd`.

## Build the sibling generator

The SaaS repo intentionally uses the sibling `../signal_synth` checkout. Build
the CLI there, then install a stable copy under `/opt/signal_synth`:

```sh
cd /home/graphyt2/git/signal_synth
cmake -S . -B build \
  -DSIGNAL_SYNTH_BUILD_CLI=ON \
  -DSIGNAL_SYNTH_BUILD_TESTS=ON
cmake --build build
cd build
ctest --output-on-failure
cd ..

sudo install -d -m 0755 /opt/signal_synth/bin
sudo install -m 0755 build/signal-synth /opt/signal_synth/bin/signal-synth
sudo install -d -m 0755 /opt/signal_synth/examples
sudo rsync -a examples/scenarios /opt/signal_synth/examples/
```

The curated importer copies the complete declared scenario tree, verification
protocols, and approved noise registry into the release's
`/opt/signal_synth_saas/packs` directory. The production worker therefore does
not resolve curated pack inputs through a separately deployed core examples
tree; only the pinned `signal-synth` binary is required at runtime.

## Build the Apache module and tools

From the `signal_synth_saas` checkout:

```sh
cd /home/graphyt2/git/signal_synth_saas
cmake -S . -B build/apache22 \
  -DSIGNAL_SYNTH_ROOT=../signal_synth \
  -DAPXS_EXECUTABLE=/usr/local/apache2/bin/apxs \
  -DBUILD_TESTING=ON
cmake --build build/apache22

cd build/apache22
ctest --output-on-failure
cd ../..
```

Run the full local E2E smoke test against the same Apache runtime:

```sh
APXS_EXECUTABLE=/usr/local/apache2/bin/apxs \
APACHE_HTTPD=/usr/local/apache2/bin/httpd \
SIGNAL_SYNTH_CLI=/opt/signal_synth/bin/signal-synth \
test/integration/e2e_smoke.sh
```

Expected final output:

```text
status=e2e-succeeded
job_id=job_...
package_id=pkg_...
```

## Install files

Back up the currently loaded module before replacing it:

```sh
sudo test -f /usr/local/apache2/modules/mod_syn_sig_ra.so &&
  sudo cp /usr/local/apache2/modules/mod_syn_sig_ra.so \
    /usr/local/apache2/modules/mod_syn_sig_ra.so.before-upgrade
```

Install the module and helper binaries:

```sh
sudo install -m 0755 build/apache22/mod_syn_sig_ra.so \
  /usr/local/apache2/modules/mod_syn_sig_ra.so
sudo install -m 0755 build/apache22/syn_sig_ra_admin \
  /usr/local/bin/syn_sig_ra_admin
sudo install -m 0755 build/apache22/syn_sig_ra_worker \
  /usr/local/bin/syn_sig_ra_worker
```

Install packs:

```sh
sudo install -d -m 0755 /opt/signal_synth_saas/packs
sudo install -m 0644 packs/*.json /opt/signal_synth_saas/packs/
```

## Data directory and metadata database

Create the local state directory. The Apache 2.2 worker user on this VPS is
`apache:nogroup`.

```sh
sudo install -d -o apache -g nogroup -m 0750 /var/lib/syn_sig_ra
sudo install -d -o apache -g nogroup -m 0750 /var/lib/syn_sig_ra/work
sudo install -d -o apache -g nogroup -m 0750 /var/lib/syn_sig_ra/packages

sudo -u apache /usr/local/bin/syn_sig_ra_admin \
  init-db /var/lib/syn_sig_ra/db.sqlite3
sudo chmod 0600 /var/lib/syn_sig_ra/db.sqlite3
sudo chown apache:nogroup /var/lib/syn_sig_ra/db.sqlite3
```

Create an API key without committing or logging the plaintext secret:

```sh
sudo sh -c 'umask 077; openssl rand -base64 48 > /root/syn_sig_ra_api_key'
sudo sh -c 'cat /root/syn_sig_ra_api_key |
  /usr/local/bin/syn_sig_ra_admin bootstrap-owner \
    /var/lib/syn_sig_ra/db.sqlite3 \
    org_live \
    user_live \
    synsigra@gmail.com \
    "Kis Tamás" \
    key_live_$(date +%Y%m%d) \
    "live operator"'
```

Do not print `/root/syn_sig_ra_api_key` in shared logs.

## Apache configuration

Create `/usr/local/apache2/conf/extra/syn_sig_ra.conf`:

```apache
LoadModule syn_sig_ra_module modules/mod_syn_sig_ra.so

SynSigRaDataRoot /var/lib/syn_sig_ra
SynSigRaSignalSynthCli /opt/signal_synth/bin/signal-synth
SynSigRaPackRoot /opt/signal_synth_saas/packs
SynSigRaPublicBasePath /syn_sig_ra

# Enable only after the provider has verified the sender domain and the
# password file has been created with restrictive permissions.
# SynSigRaEmailTransport smtp
# SynSigRaEmailPublicOrigin https://www.timeonion.com
# SynSigRaEmailFrom noreply@timeonion.com
# SynSigRaEmailFromName "Synsigra"
# SynSigRaEmailSmtpUrl smtps://smtp.provider.example:465
# SynSigRaEmailSmtpUsername smtp-user
# SynSigRaEmailSmtpPasswordFile /etc/syn_sig_ra/smtp-password

<Location "/syn_sig_ra">
    SetHandler syn_sig_ra
    Order allow,deny
    Allow from all
</Location>
```

Include it from `/usr/local/apache2/conf/httpd.conf`:

```sh
sudo cp /usr/local/apache2/conf/httpd.conf \
  /usr/local/apache2/conf/httpd.conf.$(date +%Y%m%d)-before-syn-sig-ra
grep -q '^Include conf/extra/syn_sig_ra.conf$' \
  /usr/local/apache2/conf/httpd.conf ||
  echo 'Include conf/extra/syn_sig_ra.conf' |
    sudo tee -a /usr/local/apache2/conf/httpd.conf >/dev/null
```

Before uncommenting the SMTP directives, add the provider-supplied SPF and
DKIM records to the sender domain, wait for provider verification, and store
the generated SMTP password outside the repository:

```sh
sudo install -d -m 0750 -o root -g apache /etc/syn_sig_ra
sudo install -m 0640 -o root -g apache /dev/null /etc/syn_sig_ra/smtp-password
sudo sh -c 'umask 027; read -r SMTP_PASSWORD; printf "%s\n" "$SMTP_PASSWORD" > /etc/syn_sig_ra/smtp-password'
```

Replace `apache` with the actual group used by the custom Apache service. Do
not enable the test-only `capture_file` transport in production. For the
currently blocked direct-VPS IP, use the Gmail App Password workflow in
[`../ops/mail/README.md`](../ops/mail/README.md) instead of a direct relay.

Validate and reload:

```sh
sudo /usr/local/apache2/bin/httpd -t
sudo /usr/local/apache2/bin/apachectl graceful
```

## Worker service

Install the versioned service definition; do not maintain a second handwritten
copy of its command line:

```sh
sudo install -m 0644 ops/systemd/syn_sig_ra_worker.service \
  /etc/systemd/system/syn_sig_ra_worker.service
```

The unit passes the exact generator, curated-pack root, approved external-noise
root, challenge-artifact helper, and verifier wheel to the worker. It denies all
network access, makes the application trees read-only, and allows writes only
below `/var/lib/syn_sig_ra`. The normal release deployment installs this same
file automatically.

Enable and start it:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now syn_sig_ra_worker.service
sudo systemctl status syn_sig_ra_worker.service --no-pager -l
```

## Smoke tests on the live host

Public checks:

```sh
curl -fsS https://www.timeonion.com/syn_sig_ra/healthz
curl -fsS https://www.timeonion.com/syn_sig_ra/v1/packs
curl -fsS https://www.timeonion.com/syn_sig_ra/ | grep 'Challenge package generator'
```

Create and inspect a real job without printing the API key:

```sh
sudo sh -c 'key=$(cat /root/syn_sig_ra_api_key);
  curl -fsS \
    -H "Authorization: Bearer $key" \
    -H "Content-Type: application/json" \
    -d "{\"project_id\":\"org_live_default\",\"pack_id\":\"r_peak_rr_simple_stress_v1\"}" \
    https://www.timeonion.com/syn_sig_ra/v1/jobs'
```

Then poll the returned `job_id`:

```sh
sudo sh -c 'key=$(cat /root/syn_sig_ra_api_key);
  curl -fsS \
    -H "Authorization: Bearer $key" \
    https://www.timeonion.com/syn_sig_ra/v1/jobs/JOB_ID'
```

Download artifacts from the returned `package_id`:

```sh
sudo sh -c 'key=$(cat /root/syn_sig_ra_api_key);
  curl -fsS \
    -H "Authorization: Bearer $key" \
    -o /tmp/syn_sig_ra_manifest.json \
    https://www.timeonion.com/syn_sig_ra/v1/artifacts/PACKAGE_ID/manifest.json;
  curl -fsS \
    -H "Authorization: Bearer $key" \
    -o /tmp/syn_sig_ra_package.zip \
    https://www.timeonion.com/syn_sig_ra/v1/artifacts/PACKAGE_ID/package.zip;
  unzip -t /tmp/syn_sig_ra_package.zip'
```

The same flow is available through the browser UI:

```text
https://www.timeonion.com/syn_sig_ra/
```

Register or sign in with an e-mail address and password. The browser uses a
secure server-side session and never needs an API key. The UI can also delete
jobs from the visible job list. Job deletion is a soft delete: API downloads
stop working, but physical files remain until retention cleanup removes them.

Delete a job from the API:

```sh
sudo sh -c 'key=$(cat /root/syn_sig_ra_api_key);
  curl -fsS \
    -X DELETE \
    -H "Authorization: Bearer $key" \
    https://www.timeonion.com/syn_sig_ra/v1/jobs/JOB_ID'
```

## Logs and diagnostics

Apache:

```sh
sudo tail -n 120 /usr/local/apache2/logs/error_log
sudo systemctl status apache22 --no-pager -l
```

Worker:

```sh
sudo systemctl status syn_sig_ra_worker.service --no-pager -l
sudo journalctl -u syn_sig_ra_worker.service -n 200 --no-pager
```

API-key lifecycle:

```sh
sudo /usr/local/bin/syn_sig_ra_admin \
  list-api-keys /var/lib/syn_sig_ra/db.sqlite3
sudo /usr/local/bin/syn_sig_ra_admin \
  revoke-api-key /var/lib/syn_sig_ra/db.sqlite3 KEY_ID
```

Database and data root permissions:

```sh
sudo stat -c '%U:%G %a %n' \
  /var/lib/syn_sig_ra \
  /var/lib/syn_sig_ra/db.sqlite3 \
  /var/lib/syn_sig_ra/work \
  /var/lib/syn_sig_ra/packages
```

The current database is the only accepted schema. Any other non-empty schema
fails with an instruction to run the explicit destructive pre-beta reset; no
in-place compatibility migration exists. Use
`scripts/reset_prebeta_state.sh --confirm-destroy-prebeta-state`, optionally
with `--backup DIRECTORY`, instead of deleting individual files by hand.

## Rollback

The normal release path validates the artifact checksum and Apache ABI before
stopping services, captures every replaceable runtime file, validates both
Apache and nginx configuration, and executes the live smoke gate. Any failure
automatically restores the pre-deploy snapshot. No database or generated
customer package is changed by deployment or rollback.

Roll back the last successful release explicitly with:

```sh
scripts/rollback_live.sh
```

Or name a checksummed snapshot under the release rollback root:

```sh
scripts/rollback_live.sh \
  /opt/signal_synth_saas/rollback/pre-RELEASE_ID-TIMESTAMP
```

The rollback restores module, worker/admin tools, pinned generator, catalog,
verifier files, main landing site, nginx configuration, and log rotation as one
unit, then runs `verify_live.sh`. It first captures the current state, so a
failed rollback is automatically reversed and a successful rollback retains a
roll-forward snapshot.

Release archives and pointers are under `/opt/signal_synth_saas/releases/`.
Runtime snapshots are under `/opt/signal_synth_saas/rollback/`; each contains a
`SHA256SUMS` file. Do not edit either in place.

## Current production limitations

This deployment is suitable for private/live smoke usage, not a public
self-serve SaaS yet. Before external users receive credentials, add TLS, rate
limits, key rotation, job quotas, retention policy, monitoring alerts, and a
customer-facing onboarding/support process.
