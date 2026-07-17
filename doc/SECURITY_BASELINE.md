# Synsigra security baseline

Reviewed: 2026-07-17. This baseline applies to the private-beta service at
`https://www.timeonion.com/syn_sig_ra`.

## Assets and trust boundaries

The assets are account credentials and sessions, personal API-key secrets,
SMTP credentials, tenant metadata, immutable generator recipes, generated
packages, audit history, and the generator/verifier binaries. Generated data
is synthetic engineering data, but a private pack or detector output can still
be confidential.

The public trust boundary is nginx. It terminates TLS, rejects unknown hosts,
enforces security headers, connection/rate limits, and a 64 KiB request-body
limit. It proxies only to Apache on `127.0.0.1:8080`. The custom Apache 2.2
process loads `mod_syn_sig_ra`; the worker and SQLite/artifact store are local
services and are not publicly reachable. The sibling `signal_synth` generator
is server-only. Users receive only generated artifacts and the generator-free
verifier distribution.

## Threat model and controls

| Threat | Principal controls | Residual risk / response |
|---|---|---|
| Credential stuffing and account enumeration | Generic recovery/duplicate-registration responses, password hashing, verified email, nginx auth throttling, 12-character minimum | No MFA in beta; investigate abnormal auth volume and rotate affected sessions/keys |
| Stolen browser session | Secure, HttpOnly, SameSite=Lax cookie; seven-day expiry; logout and password reset invalidate sessions | A compromised browser can act until invalidation |
| Leaked API key | Only SHA-256 hashes are stored; secret shown once; atomic self-service rotation/revocation; per-key quota; audit trail | Bearer keys remain replayable until rotated |
| Cross-tenant access | Every job/package/draft/key query is scoped by authenticated organization/user; artifact paths are reconstructed from the configured data root | Authorization regressions require route/E2E tests and audit review |
| Path traversal or symlink substitution | Strict identifiers, fixed filenames, real-file/no-symlink checks, immutable recipes, atomic artifact construction | Local root compromise is outside the application boundary |
| Oversized input or resource exhaustion | nginx and module 64 KiB bodies, bounded JSON fields, bounded pagination/viewer windows, per-key request/job quotas, disk reserve, seven-day retention | Valid large generation can still consume CPU/disk; readiness and metrics must be watched |
| Artifact corruption or partial delivery | File-backed immutable artifacts, SHA-256 checksum/ETag, atomic publication, HEAD/Range resume, expiry metadata | Client disk/network failures remain client-visible and resumable |
| Secret or data disclosure through logs/support | Request logs contain method/path/status/duration only; no headers, query, body, cookies, tokens, or signal content; private support warns against PHI/credentials/proprietary output | Internal error strings must never be constructed from secrets |
| Supply-chain/runtime compromise | Pinned local source checkout, release build/tests, deployed binary fingerprint, server-only generator, TLS edge | Legacy Apache 2.2 is an accepted beta risk; keep it loopback-only and minimize modules |

## Audit coverage and export

Owner/admin users can inspect recent events in **Account → Security & audit** or
export them with:

```sh
curl -fsS -H "Authorization: Bearer $SYN_SIG_RA_API_KEY" \
  'https://www.timeonion.com/syn_sig_ra/v1/audit-events?format=csv&limit=1000' \
  -o synsigra-audit-events.csv
```

Events include successful key/session authentication, API-key create/rotate/
revoke, job create/retry/cancel/delete/exact rebuild, and artifact inspection/
download. The export is organization-scoped, newest first, bounded to 1,000
rows per page, and is served with `Cache-Control: no-store`. Secrets, request
bodies, signal samples, email addresses, and password/token hashes are never
audit fields.

## Input and file limits

- Request body: 64 KiB at nginx and the Apache module.
- API-key/project labels: 100 characters; passwords: 12–128 characters.
- Job list: 100 rows/page; audit export: 1,000 rows/page.
- Viewer window: 16,384 display buckets and 10,000 overlay items/request.
- Artifact delivery: one byte range/request with 64-bit offsets; no multipart
  ranges. Files remain downloadable for seven days.
- Generated paths and filenames come from validated IDs, not arbitrary user
  filesystem input.

## Dependency and exposure review

The application links OpenSSL, SQLite, Jansson, libcurl, zlib/minizip, APR and
the custom Apache runtime, plus the sibling `signal_synth` core. Before a
release, run the complete build/E2E suite, inspect `ldd` for unexpected dynamic
libraries, check the OS security-update backlog, and verify that Apache still
listens only on loopback. nginx is the only public application listener.

The legacy Apache 2.2 runtime is not treated as a firewall or TLS endpoint. Its
accepted beta exposure is reduced by loopback binding, nginx request controls,
the single application handler, minimal filesystem permissions, and absence of
direct public access. Replacing it remains a separate compatibility project.

## Incident checklist

1. Preserve the UTC time window, affected organization/key/job IDs, audit CSV,
   nginx/Apache logs, worker journal, build identity, and relevant checksums.
   Do not copy secrets or synthetic/proprietary outputs into tickets.
2. Contain: revoke/rotate affected API keys, reset the password to invalidate
   sessions, rotate SMTP credentials if implicated, and disable generation or
   edge traffic when continued processing could worsen impact.
3. Scope: correlate audit event IDs with HTTP status/path logs, package IDs,
   generator identity, database backup, and filesystem ownership. Check for
   cross-tenant reads and unexpected artifact downloads.
4. Recover from a known build/configuration, run `scripts/verify_live.sh`, and
   issue replacement secrets through the approved private channel.
5. Notify affected beta users with facts, impact, containment, and next steps.
   Record follow-up controls and update this threat model.

Secret-specific procedures are in [SECRET_ROTATION.md](SECRET_ROTATION.md).
