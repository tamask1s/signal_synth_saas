# Secret rotation runbook

Never paste a live secret into a command argument, GitHub issue, chat, shell
history, build log, or audit event. Store secret files outside the repository
with owner/group permissions matching the consuming service.

## Personal API key

In the web UI open **Account**, find the active integration key, and choose
**Rotate**. Copy the replacement secret immediately. Rotation is atomic: the
replacement is inserted and the prior key is revoked in the same database
transaction. Update the client secret store, verify one harmless API call, and
delete any temporary copy.

API clients can do the same operation while authenticated with a browser
session or another bearer key:

```http
POST /syn_sig_ra/v1/api-keys/{api_key_id}/rotate
```

The response returns the replacement `api_key` exactly once. If the rotating
request authenticates with the key being replaced, that request completes but
the old key fails every subsequent request.

## Browser sessions and passwords

Use password reset to change a possibly exposed account password. Successful
reset deletes all prior sessions before issuing the new session. Use **Sign
out** for ordinary single-session invalidation.

## Gmail SMTP App Password

1. Generate a replacement Google App Password for the Synsigra sender account.
2. Put it in the external password file described by
   [`ops/mail/README.md`](../ops/mail/README.md), using the same restrictive
   ownership/mode. Do not print or commit it.
3. Validate and reload Apache, then run the mail status/verification wrapper.
4. Complete a registration or recovery delivery test.
5. Revoke the previous App Password in the Google account.

Keep the old password active only for the short validation window. If exposure
is suspected, revoke first, accept a brief mail outage, then install the new
value.

## Legacy bootstrap/live integration key

The root-owned `/root/syn_sig_ra_api_key` is an operational bootstrap key, not
a browser-user key. To rotate it, generate a new value into a root-only
temporary file, provision a new key ID through `syn_sig_ra_admin`, atomically
replace the root file, validate `/v1/projects`, and revoke the old key ID. Keep
the old value long enough for rollback only when there is no suspected leak.

After every rotation, export the organization audit trail and confirm the
expected create/rotate/revoke and authentication events. Record only key IDs,
UTC timestamps, and outcome—never plaintext values.
