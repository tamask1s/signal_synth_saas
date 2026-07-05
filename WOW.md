# Way of Working

This repository uses task-first implementation.

## Rules

1. Every code or documentation change must start from a task.
2. The task must define scope, acceptance criteria, and expected test evidence.
3. The first line of every commit message must start with a link to the task.
4. After implementation, the task must be updated with a link to the implementation commit or PR.
5. Keep unrelated changes out of the task.
6. If the implementation discovers new scope, create a new task instead of silently expanding the current one.
7. Update documentation when behavior, configuration, API shape, or deployment changes.
8. The SaaS repository must use `../signal_synth` directly for now. Do not add it as a submodule or vendored dependency.
9. All public HTTP routes must live under `/syn_sig_ra/...`.
10. Do not add clinical claims, patient-data workflows, or server-side user-algorithm execution to v1.

## Commit message format

First line:

```text
<task-url> <short imperative summary>
```

Example:

```text
https://github.com/tamask1s/signal_synth_saas/issues/12 Add health endpoint for Apache module
```

Body, when useful:

```text
- summarize implementation choices
- list tests run
- note follow-up tasks
```

## Task closure checklist

Before closing a task, add a comment containing:

- implementation link: commit or PR URL;
- test evidence: commands run and results;
- documentation updated: yes/no, with reason if no;
- follow-up tasks, if any.

## Minimum task template

```md
## Scope

What should change.

## Acceptance criteria

- Observable behavior or files changed.
- Required route/config/test behavior.

## Test evidence

Commands that must pass before closing.

## Implementation link

To be filled after merge or commit.
```
