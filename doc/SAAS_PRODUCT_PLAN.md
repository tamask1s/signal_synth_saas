# SaaS product readiness plan

This repo now has a working private SaaS core:

- Apache 2 module under `/syn_sig_ra/...`;
- public health and pack catalog endpoints;
- API-key protected job and artifact endpoints;
- SQLite metadata store;
- local worker loop around `signal-synth pack challenge`;
- immutable local artifact storage;
- end-to-end smoke test;
- Apache 2.2 VPS deployment runbook.

That is enough for controlled live smoke usage. It is not yet enough for real
external users without close operator involvement.

## Product boundary

The product should remain a B2B developer-tool platform for synthetic
biosignal QA packages. It should not become a medical diagnostic workflow, a
PHI intake system, or a generic ECG datastore.

The user-facing promise should be:

1. choose a curated synthetic biosignal test pack;
2. generate a deterministic package with explicit ground truth;
3. download machine-readable and human-readable evidence;
4. run the customer algorithm locally;
5. reproduce and audit the package later by pack, generator, and fingerprint.

## Gap analysis

### Security and exposure

Current state:

- HTTP is live and functional.
- API keys are stored hashed, but onboarding is manual.
- Apache route scope is constrained to `/syn_sig_ra/...`.

Missing before real users:

- HTTPS and certificate renewal;
- explicit HTTP redirect/deny policy;
- key rotation/revocation workflow;
- rate limiting and quotas;
- security headers where applicable;
- threat model and audit coverage review.

### Product usability

Current state:

- A developer can use curl with a manually issued API key.
- Pack metadata is available.
- Jobs can be created and fetched by known ID.

Missing before real users:

- customer-facing API docs;
- onboarding/offboarding process;
- job list/status history;
- cancellation/retry semantics;
- minimal web UI for non-curl usage;
- clear pack descriptions, versioning, changelog, and deprecation behavior.

### Operations

Current state:

- A systemd worker can run jobs.
- Apache and worker logs exist.
- Local immutable storage works.

Missing before real users:

- structured logs and metrics;
- queue depth/disk/worker alerts;
- backup and restore drill;
- retention/cleanup policy;
- release artifact versioning;
- repeatable deployment and rollback automation.

### Commercial and legal readiness

Current state:

- README and `signal_synth` docs clearly state synthetic engineering-test
  boundaries.

Missing before real users:

- beta terms and support expectations;
- no-PHI/no-diagnostic-use policy shown to customers;
- provenance/license notices in generated packages where needed;
- billing or at least usage-plan placeholders.

## Recommended phases

### Phase 0: private live smoke

Goal: keep validating the core API on the live VPS with trusted internal keys.

Already mostly done:

- `/syn_sig_ra/healthz` live;
- pack catalog live;
- job creation, worker completion, manifest/archive download live;
- E2E test and deployment runbook in this repo.

Exit criteria:

- #9 and #10 closed;
- no regressions to the existing `timeonion.com` site;
- live worker and Apache checks documented.

### Phase 1: private beta API

Goal: a small number of known external technical users can use the API safely.

Required issues:

- [#12 Public HTTPS and HTTP hardening](https://github.com/tamask1s/signal_synth_saas/issues/12)
- [#13 Customer onboarding and API-key lifecycle](https://github.com/tamask1s/signal_synth_saas/issues/13)
- [#15 Job lifecycle API: list, cancel, retry, retention](https://github.com/tamask1s/signal_synth_saas/issues/15)
- [#16 Quotas, rate limits, and usage metering](https://github.com/tamask1s/signal_synth_saas/issues/16)
- [#17 Observability: structured logs, metrics, alerts](https://github.com/tamask1s/signal_synth_saas/issues/17)
- [#18 Backup, restore, and artifact retention policy](https://github.com/tamask1s/signal_synth_saas/issues/18)
- [#20 Customer API docs, examples, and SDK smoke clients](https://github.com/tamask1s/signal_synth_saas/issues/20)
- [#23 Security baseline: threat model, secret rotation, audit export](https://github.com/tamask1s/signal_synth_saas/issues/23)
- [#24 Commercial and legal readiness for private beta](https://github.com/tamask1s/signal_synth_saas/issues/24)

Phase 1 can remain API-only. A web UI is useful, but not required if beta users
are developers and the docs/client examples are good.

### Phase 2: usable hosted MVP

Goal: users can self-serve common flows without direct shell/operator support.

Required issues:

- [#14 Organizations, projects, roles, and tenancy model](https://github.com/tamask1s/signal_synth_saas/issues/14)
- [#19 Pack catalog productization and versioning](https://github.com/tamask1s/signal_synth_saas/issues/19)
- [#21 Minimal web UI for packs, jobs, and downloads](https://github.com/tamask1s/signal_synth_saas/issues/21)
- [#22 Release, CI/CD, and deployment automation](https://github.com/tamask1s/signal_synth_saas/issues/22)

At this stage, the service should have:

- stable customer-facing documentation;
- at least one non-curl UI path;
- a repeatable deployment process;
- visible pack/version semantics;
- quota and retention controls;
- operational alerts.

### Phase 3: enterprise-ready service

Goal: support teams and regulated engineering organizations.

Likely follow-up work after Phase 2:

- SSO or enterprise identity provider integration;
- role-based audit export;
- per-organization export policy;
- object storage or on-prem deployment option;
- controlled release channels tied to `signal_synth` generator versions;
- validation evidence bundle per release.

These should not be implemented before the private beta proves which workflows
customers actually need.

## Architecture direction

For the next increment, keep the Apache module and local worker. The current
VPS is already serving the domain, and the route prefix is isolated. Replacing
the runtime stack now would create migration risk without improving the core
product feedback loop.

The first major architecture pressure points will be:

- SQLite concurrency and backup limits;
- local disk retention;
- manual API-key operations;
- lack of job search/listing;
- lack of observability.

When those become painful, migrate deliberately:

```text
Apache module API
  -> explicit service boundary
  -> PostgreSQL metadata
  -> object storage for artifacts
  -> external queue/workers
  -> optional frontend/API gateway
```

Do not move to a larger stack until quotas, retention, and customer workflows
are better understood.

## Task index

Roadmap tracking:

- [#11 SaaS readiness roadmap and backlog](https://github.com/tamask1s/signal_synth_saas/issues/11)

Follow-up tasks:

- [#12 Public HTTPS and HTTP hardening](https://github.com/tamask1s/signal_synth_saas/issues/12)
- [#13 Customer onboarding and API-key lifecycle](https://github.com/tamask1s/signal_synth_saas/issues/13)
- [#14 Organizations, projects, roles, and tenancy model](https://github.com/tamask1s/signal_synth_saas/issues/14)
- [#15 Job lifecycle API: list, cancel, retry, retention](https://github.com/tamask1s/signal_synth_saas/issues/15)
- [#16 Quotas, rate limits, and usage metering](https://github.com/tamask1s/signal_synth_saas/issues/16)
- [#17 Observability: structured logs, metrics, alerts](https://github.com/tamask1s/signal_synth_saas/issues/17)
- [#18 Backup, restore, and artifact retention policy](https://github.com/tamask1s/signal_synth_saas/issues/18)
- [#19 Pack catalog productization and versioning](https://github.com/tamask1s/signal_synth_saas/issues/19)
- [#20 Customer API docs, examples, and SDK smoke clients](https://github.com/tamask1s/signal_synth_saas/issues/20)
- [#21 Minimal web UI for packs, jobs, and downloads](https://github.com/tamask1s/signal_synth_saas/issues/21)
- [#22 Release, CI/CD, and deployment automation](https://github.com/tamask1s/signal_synth_saas/issues/22)
- [#23 Security baseline: threat model, secret rotation, audit export](https://github.com/tamask1s/signal_synth_saas/issues/23)
- [#24 Commercial and legal readiness for private beta](https://github.com/tamask1s/signal_synth_saas/issues/24)
