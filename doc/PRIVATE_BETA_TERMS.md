# Synsigra Private Beta Terms

Effective date: 2026-07-17
Document version: `private-beta-2026-07-17-r4`

These terms govern access to the Synsigra private-beta service at
`timeonion.com`. By creating an account, the user confirms that they have read
and accepted this version of the terms and the linked Privacy and No-PHI Notice.

This is a practical private-beta agreement, not a representation that the
service has completed any medical-device, clinical, security, privacy, or other
regulatory certification. An organization evaluating Synsigra should obtain its
own legal and regulatory advice for its intended use.

## Operator and contact

The service operator is Kis Tamás, 2040 Budaörs, Tátra u. 6, Hungary.
Private support, privacy, and account contact: `synsigra@gmail.com`.

## 1. Intended and permitted use

Synsigra provides deterministic synthetic biosignal packages and local
engineering-verification tooling. A beta user may use downloaded packages and
the generator-free verifier for internal algorithm development, regression
testing, benchmarking, reproducibility, and engineering evaluation.

The service is not intended for diagnosis, prevention, monitoring, prediction,
prognosis or treatment of disease; clinical decision-making; patient monitoring;
clinical validation or certification; medical-device conformity assessment; or
processing data relating to an identifiable patient.

## 2. Synthetic data and no-PHI rule

Only synthetic engineering inputs are permitted. Apart from the account email
address and display name required to operate the account, users must not submit:

- protected health information (PHI), patient identifiers or medical records;
- real patient waveforms, annotations, clinical notes or case descriptions;
- personal data belonging to another person;
- confidential detector output or source code.

Synsigra does not run customer detector code. Detector output is intended to
remain on the customer's own system and be scored with the local verifier.
Users are responsible for reviewing their inputs before submission.

## 3. Accounts and acceptable use

Users must provide a working email address, protect passwords and API keys, and
promptly revoke credentials that may be compromised. Accounts may not be used
to interfere with the service, bypass limits, probe another organization,
distribute malware, infringe third-party rights, or perform unlawful activity.

Access may be rate-limited, suspended or terminated to protect the service,
other users, or the intended-use boundary.

## 4. Package use permission

Subject to these terms, the account holder receives a non-exclusive,
non-transferable, revocable permission during the beta to use, reproduce and
archive downloaded synthetic packages and verifier reports inside its
organization for the permitted engineering purposes above. Contractors may
receive a package only while acting for that organization and while bound by
equivalent confidentiality and use restrictions.

Packages and reports may not be sold, sublicensed, published as a standalone
dataset, used to identify or model a real person, or represented as clinical
evidence. Package fingerprints, manifests, provenance and claim-boundary notices
must remain with archived evidence. Customer algorithms and detector outputs
remain the customer's responsibility and are not acquired by the service.

## 5. Private-beta service level and support

The service is provided on a best-effort evaluation basis without an uptime or
response-time SLA. Maintenance, quota changes, data resets and breaking beta
changes may occur. When practicable, material planned interruptions will be
communicated in advance.

Support requests are accepted privately at `synsigra@gmail.com`. Include only a
safe job/package ID, UTC timestamp, browser version, and exact error code. Never
send passwords, API keys, account-action links, PHI, personal or patient data,
generated signal files, proprietary detector output, or source code. Support is
normally reviewed within three business days, but this is a target rather than
a contractual SLA.

## 6. Billing

The current private beta is free of charge. There is no automatic conversion to
a paid plan and no payment method is collected. Any future paid plan requires
separate notice, published pricing and the customer's explicit opt-in before
charges begin.

## 7. Retention and beta changes

Generated artifacts are normally cached for 7 days. Users must download and
archive evidence they need. Job metadata and reproducibility identifiers may
remain after an artifact expires. Beta databases may be reset when necessary for
development, security or schema changes.

The terms may change as the beta evolves. A new material version will require
fresh acceptance before new customer use.

## 8. Warranty and responsibility boundary

To the extent permitted by applicable law, the beta service and generated
materials are provided as-is and as-available, without warranties of
merchantability, fitness for a particular purpose, non-infringement, uninterrupted
availability, or suitability for regulated or clinical use. Users remain
responsible for their algorithms, validation plans, release decisions, backups,
legal obligations, and conclusions drawn from synthetic tests.

Nothing in these terms excludes liability that cannot lawfully be excluded.

## 9. Related notice

The Privacy and No-PHI Notice is part of this beta agreement:
`/syn_sig_ra/legal/privacy`.
