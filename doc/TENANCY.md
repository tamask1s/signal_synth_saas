# Tenancy and authorization

The pre-beta API uses API keys as user credentials. Every active key resolves
to one user membership in one organization and carries the membership role.

## Resource hierarchy

```text
organization
├── memberships: owner, admin, developer, viewer
└── projects
    ├── jobs
    └── immutable packages
```

Built-in packs are a global, read-only catalog. Jobs and packages always store
both `organization_id` and `project_id`. A user can read all projects, jobs,
and packages in their organization. Requests for another organization's job,
project, or package return `404` so resource existence is not disclosed.

## Roles

| Operation | owner | admin | developer | viewer |
|---|---:|---:|---:|---:|
| List projects/jobs and download packages | yes | yes | yes | yes |
| Create/delete jobs | yes | yes | yes | no |
| Create projects | yes | yes | no | no |
| Manage organization membership | operator only | operator only | no | no |

Membership administration remains an operator action during private beta.
`bootstrap-owner` creates the first real owner only in a fresh empty database;
`create-api-key` can attach a key only to an existing membership and accepts an
optional role that must match that membership.

## Project API

- `GET /syn_sig_ra/v1/projects` lists projects and returns the caller's role.
- `POST /syn_sig_ra/v1/projects` creates a project for owners and admins.
- `POST /syn_sig_ra/v1/jobs` requires `project_id`.

Owner bootstrap creates the `Default` project. The schema intentionally uses
clean database resets instead of compatibility migrations.
