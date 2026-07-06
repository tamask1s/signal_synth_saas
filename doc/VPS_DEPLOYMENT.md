# VPS deployment runbook

This document describes the current `timeonion.com` deployment shape. The
server runs the old custom Apache 2.2 installation from `/usr/local/apache2`.
Keep using that runtime for now; do not switch traffic to the packaged Apache
2.4 service until the existing `mod_ts` site behavior is migrated and tested.

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
`ops/nginx/timeonion.conf`. HTTP serves only ACME challenges and redirects all
other requests to the canonical `https://www.timeonion.com` origin. The apex
HTTPS origin redirects there as well, which keeps future login cookies on one
origin. Unknown hostnames are rejected. The edge also limits request bodies to
1 MiB, each source IP to 30 concurrent connections, and request rates to
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

The current built-in SaaS pack uses relative scenario paths that resolve from
`/opt/signal_synth_saas/packs` to `/opt/signal_synth/examples/...`, so the
scenario tree must be installed with the CLI.

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
sudo install -d -o apache -g nogroup -m 0750 /var/lib/syn_sig_ra/jobs
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
  /usr/local/bin/syn_sig_ra_admin create-api-key \
    /var/lib/syn_sig_ra/db.sqlite3 \
    org_live \
    user_live \
    key_live_$(date +%Y%m%d) \
    "live API key"'
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

Validate and reload:

```sh
sudo /usr/local/apache2/bin/httpd -t
sudo /usr/local/apache2/bin/apachectl graceful
```

## Worker service

Create `/etc/systemd/system/syn_sig_ra_worker.service`:

```systemd
[Unit]
Description=SynSigRa SaaS worker
After=network.target apache22.service

[Service]
Type=simple
User=apache
Group=nogroup
ExecStart=/usr/local/bin/syn_sig_ra_worker run-loop /var/lib/syn_sig_ra/db.sqlite3 /opt/signal_synth/bin/signal-synth /opt/signal_synth_saas/packs /var/lib/syn_sig_ra
Restart=always
RestartSec=2
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/syn_sig_ra
UMask=0077

[Install]
WantedBy=multi-user.target
```

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
    -d "{\"project_id\":\"org_live_default\",\"pack_id\":\"r_peak_stress_v1\"}" \
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

The pre-beta module deliberately does not migrate obsolete schema versions.
Before a schema-changing deployment, stop the worker, back up and remove
`/var/lib/syn_sig_ra/db.sqlite3`, initialize a fresh database, and provision a
new API key. This keeps schema development clean while there are no users.

## Rollback

If Apache fails configuration validation, do not reload. Fix the config or
restore the previous module first.

Rollback sequence:

```sh
sudo systemctl stop syn_sig_ra_worker.service
sudo cp /usr/local/apache2/modules/mod_syn_sig_ra.so.before-upgrade \
  /usr/local/apache2/modules/mod_syn_sig_ra.so
sudo /usr/local/apache2/bin/httpd -t
sudo /usr/local/apache2/bin/apachectl graceful
sudo systemctl start syn_sig_ra_worker.service
```

If the include itself must be disabled:

```sh
sudo sed -i.bak '/^Include conf\/extra\/syn_sig_ra.conf$/s/^/#/' \
  /usr/local/apache2/conf/httpd.conf
sudo /usr/local/apache2/bin/httpd -t
sudo /usr/local/apache2/bin/apachectl graceful
```

This removes only the `/syn_sig_ra/...` API. The existing non-SaaS
`timeonion.com` routes continue to be served by Apache 2.2.

## Current production limitations

This deployment is suitable for private/live smoke usage, not a public
self-serve SaaS yet. Before external users receive credentials, add TLS, rate
limits, key rotation, job quotas, retention policy, monitoring alerts, and a
customer-facing onboarding/support process.
