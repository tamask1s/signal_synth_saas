# Independent services on timeonion.com

Nginx owns public HTTPS. The shared vhost owns certificates, domain redirects,
ACME, security headers and shared limits. It includes
`/etc/nginx/timeonion-services/*.conf` inside the `www.timeonion.com` HTTPS server.
`ops/nginx/timeonion.conf` is an infrastructure reference, not a release payload.

Each service owns exactly one snippet and its own process, data and release files:

| Service | Nginx snippet | Routes | Backend |
| --- | --- | --- | --- |
| Synsigra | `synsigra.conf` | `/` fallback, `/syn_sig_ra/`, `/tamaskis` | `127.0.0.1:8080` Apache |
| Mesemondó | `mesemondo.conf` | exact `/mesemondo`, prefix `^~ /mesemondo/` | separate free localhost port |

Mesemondó files must live outside Synsigra's frontend, `/opt/signal_synth_saas`,
`/opt/signal_synth` and `/var/lib/syn_sig_ra`. Neither service deploys the shared
vhost or deletes the snippets directory. Neither restarts the other's backend.
More specific Mesemondó locations win over Synsigra's `/` fallback.

Run `bash scripts/setup_shared_nginx.sh` once on the existing host. It checks the
known old configuration, backs it up outside nginx's include globs, installs the
split configuration and gracefully reloads nginx. It restores the old vhost if
validation/reload fails. Re-running a successful setup preserves service snippets.
On a new host, provision the shared vhost, TLS and lock as infrastructure first.

Service deployments and rollbacks acquire the same advisory lock before taking
snapshots or changing proxy configuration: `exec 9>/etc/nginx/timeonion-services/.deploy.lock`
then `flock -x 9`. The root-owned lock is writable by the trusted `sudo` group;
never delete or replace its inode. Hold it through validation, reload and any
automatic rollback. Preserve it when deploying either service.

Snapshot/replace/restore only your own snippet; run `nginx -t` before a graceful
`systemctl reload nginx.service`. If validation or deployment fails, restore only
your own snippet and service. Code-only Mesemondó deploys need only its backend
restart when its proxy configuration does not change; no Synsigra rebuild is needed.
Nginx remains shared infrastructure, so syntax validation and the lock are required
for proxy changes. Custom location-level headers, body limits and logging must be
reviewed against the inherited host settings.

Synsigra artifacts now contain only `ops/nginx/synsigra.conf`. Its runtime
snapshots store only `ops/synsigra.conf`. Whole-vhost legacy artifacts/snapshots
are rejected before stopping the service. Use current deployment scripts, not
old checkouts, which do not honor this ownership contract.

Mesemondó's external API base is `https://www.timeonion.com/mesemondo/api/v1/`;
strip `/mesemondo` at the proxy and configure the application's external root path.
Validate client URL restrictions, generated downloads and OpenAPI against that
prefix. Preserve Authorization and Range, prevent ticket URLs from entering logs,
and keep API calls redirect-free. After infrastructure changes check `/`,
`/syn_sig_ra/healthz`, `/syn_sig_ra/readyz` and the Mesemondó health endpoint.
