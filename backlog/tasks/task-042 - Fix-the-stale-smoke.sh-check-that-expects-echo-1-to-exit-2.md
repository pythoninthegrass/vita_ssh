---
id: TASK-042
title: Fix the stale smoke.sh check that expects echo $1 to exit 2
status: To Do
assignee: []
created_date: '2026-11-19T22:29:00Z'
labels: []
dependencies: []
references:
  - scripts/smoke.sh
  - backlog/tasks/task-032 - Expand-to-the-exit-status-of-the-last-command.md
priority: low
type: bug
ordinal: 77000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`scripts/smoke.sh` has a loop (`run_exec`, the `var:` checks, near the line `for BAD in 'echo $(date)' 'echo $((1+1))' 'echo ${A:-x}' 'echo $1'`) that expects every listed line to exit 2 as unsupported syntax. TASK-032 made `$1` a supported special parameter, which expands to nothing, so `echo $1` now exits 0 and the check `var: unsupported syntax exits 2 (echo $1)` fails with 0 where 2 is wanted. The server is right and the check is stale. Remove `echo $1` from that list, and cover the new behavior instead: `echo [$1]` printing `[]` and exiting 0, in the same group as the other special-parameter checks if one exists. Found while verifying TASK-033; left alone there. Run it on a device with `scripts/smoke.sh exec` (endpoint in CLAUDE.local.md).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `echo $1` is no longer in the list of lines expected to exit 2 in scripts/smoke.sh.
- [ ] #2 A check asserts that `echo [$1]` prints `[]` and exits 0.
- [ ] #3 `scripts/smoke.sh exec` on a device reports no failure from the var: group, and the result is recorded in the task notes.
<!-- AC:END -->
