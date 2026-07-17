# Synsigra Privacy and No-PHI Notice

Effective date: 2026-07-17
Notice version: `private-beta-2026-07-17-r4`

## Scope and operator contact

This notice describes the private-beta service operated through
`timeonion.com`. The data controller and service operator is Kis Tamás,
2040 Budaörs, Tátra u. 6, Hungary. Operational, account, deletion, and privacy
requests use the private address `synsigra@gmail.com`.

Use only the minimum account identifier needed for identity verification.
Never email a password, API key, account-action link, PHI, patient data,
generated signal file, proprietary detector output, or source code.

## Data processed by the service

Synsigra processes:

- account email address and display name;
- salted password hashes, session-token hashes and API-key hashes;
- organization, project, job, usage and quota metadata;
- synthetic scenario drafts and custom-pack descriptions submitted by users;
- generated synthetic packages and reproducibility metadata;
- limited request and worker operational logs, including method, route, status,
  duration and service events.

The service is designed not to log request bodies, API-key values, generated
signal content or detector output. Customer detector code and outputs stay
local in the intended workflow.

## Purposes

Data is used to create and secure accounts, deliver verification and password
reset email, generate and retain requested packages, enforce quotas, diagnose
failures, protect the service, and provide beta support. The service does not
sell personal data, run advertising, or use customer content to make decisions
about patients or individuals.

## No-PHI and data-minimization rule

Do not submit PHI, real patient data, patient identifiers, medical records,
clinical notes, real-person waveforms or annotations, or personal data about
another person. Scenario names, descriptions, project names and support reports
must use synthetic engineering language only.

If prohibited data is discovered, access may be suspended and the material may
be deleted to limit exposure. The beta is not offered as a HIPAA business
associate service and no business-associate agreement is provided.

## Retention

Generated artifacts are normally cached for 7 days and may be removed
immediately after a user deletes the related job. Reproducibility metadata may
remain after artifact expiry. Account and security records are retained while
the account is active. Self-service account deletion removes the single-member
workspace's identifiable account, project, job, package, scenario, custom-pack,
credential, legal-acceptance, quota, e-mail-token, and audit records, plus its
server-side artifact trees. The live database retains only a random anonymous
deletion receipt with aggregate deleted-object counts. Copies previously
downloaded by the user cannot be revoked. Operational backups are access
restricted and expire under the production backup policy.

## Sharing and infrastructure

Data is processed on the service VPS and by infrastructure providers needed for
hosting, DNS and transactional email. Account emails are sent through the
configured Gmail SMTP service. Information may also be disclosed when required
by law or necessary to protect users and the service.

## User choices

Users can delete eligible jobs, scenario drafts, custom packs and API keys in
the product. The Account page also supports profile correction, a portable JSON
account export, and password-confirmed self-service deletion for single-member
owner workspaces. Shared-workspace deletion requires members to be transferred
or removed first. Support requests can still be initiated privately through
the operator email above, and identity verification may be required.

The service uses an essential secure session cookie for browser sign-in. It
does not use advertising or cross-site tracking cookies.
