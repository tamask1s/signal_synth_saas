# SaaS product readiness plan

Updated: 2026-07-06

Synsigra SaaS is now a working private-beta hosted workflow for synthetic
biosignal algorithm QA. The product remains intentionally thin: generation,
pack validation, challenge packaging, and local verifier semantics stay in the
sibling `signal_synth` project. This repo owns browser access, projects,
curated/custom pack selection, job orchestration, artifact access, usage
limits, docs, and live operations.

## Current implemented baseline

- Apache module mounted under `/syn_sig_ra/...`, with nginx as the HTTPS edge.
- Open registration with browser session cookies.
- Personal API keys for scripts and CI; secrets are shown once and stored
  hashed.
- Organizations, projects, roles, and tenant isolation.
- Public health/readiness endpoints.
- Rich curated catalog for ten beta packs imported from
  `signal_synth/examples/catalog/curated_pack_metadata_v1.json`.
- Catalog UI/API exposes scoreable targets, reference-only targets, detector
  schemas, verifier profiles, difficulty tags, duration, sampling rates,
  channel counts, package-size estimates, and recommended-use guidance.
- Scenario draft editor with core template/form-assisted authoring, curated
  scenario clone/fork, validation, clickable validation paths, and package
  preview.
- Immutable custom pack composer from valid scenario drafts.
- Job creation, pagination, cancel, retry, soft delete, worker execution, and
  artifact downloads.
- Completed-job recipe panel with copyable `synsigra-verify` commands.
- Detector-output template ZIP for completed curated jobs.
- Authenticated generator-free verifier bundle/wheel downloads for local
  `synsigra-verify` installation without cloning the generator repository.
- Quotas, request rate limits, usage view, and admin metrics.
- Retention cleanup, backup, restore drill, build/deploy/verify scripts, and
  live deployment workflow.
- User manual, rendered in-app quickstart/API/troubleshooting docs, raw
  OpenAPI YAML, curl examples, and a Python smoke client.

This is enough for trusted private beta usage and product iteration. It is not
yet enough for unknown external users without completing the remaining release,
security, account-lifecycle, and legal work.

## Product boundary

The user-facing promise is:

1. choose a curated or custom synthetic biosignal challenge pack;
2. generate a deterministic package with explicit ground truth;
3. download package, manifest, and detector-output templates;
4. run the customer algorithm locally;
5. verify local outputs with `synsigra-verify`;
6. archive package, manifest, detector build/configuration, detections, and
   verification reports together.

The service must not become:

- clinical decision support;
- patient monitoring;
- PHI or clinical-note storage;
- regulated medical-device validation;
- server-side execution of customer detector code;
- a generic ECG/PPG datastore.

## Current beta limitations

- E-mail ownership verification and password recovery are implemented, but
  production registration/recovery sending remains disabled until a
  transactional SMTP provider and verified sender domain are configured.
- Legal/commercial terms are not ready for less-trusted users.
- Security baseline needs a focused threat model, secret rotation review, and
  audit export story before broader beta.
- Release automation exists as scripts, but full CI/CD/release governance is
  still pending.
- API hardening work remains: idempotency keys, request IDs, richer artifact
  metadata, HEAD support, and stable error examples.
- Detector-output templates cover curated scoreable targets; custom-pack
  template generation is deferred until custom scoring metadata is richer.
- Privacy-preserving activation telemetry is not implemented.

## Remaining roadmap issues

Before broader external beta:

- [#22 Release, CI/CD, and deployment automation](https://github.com/tamask1s/signal_synth_saas/issues/22)
- [#23 Security baseline: threat model, secret rotation, audit export](https://github.com/tamask1s/signal_synth_saas/issues/23)
- [#24 Commercial and legal readiness for private beta](https://github.com/tamask1s/signal_synth_saas/issues/24)

Product/API hardening after the default verification path:

- [#36 Add API usability hardening: idempotency, request IDs, artifact metadata](https://github.com/tamask1s/signal_synth_saas/issues/36)
- [#38 Add privacy-preserving beta activation telemetry](https://github.com/tamask1s/signal_synth_saas/issues/38)

Issues #31, #32, #33, #35, and #37 are the current documentation/onboarding
and curated verification path closure set.

## Architecture direction

Keep the current Apache module and local worker while the product is still in
trusted private beta. The VPS already serves the domain, the route prefix is
isolated, and the live deploy loop is fast.

The first likely scaling pressure points are:

- SQLite concurrency and backup windows;
- local disk artifact retention;
- manual account/support operations;
- API request tracing and customer support diagnostics;
- release governance tied to `signal_synth` versions.

When those become real constraints, migrate deliberately:

```text
Apache module API
  -> explicit service boundary
  -> PostgreSQL metadata
  -> object/object-storage artifact backend
  -> external queue/workers
  -> optional frontend/API gateway
```

Do not move to a larger stack until quotas, retention, customer workflows, and
support volume justify it.
