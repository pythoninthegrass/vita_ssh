---
id: TASK-042
title: Fix the stale smoke.sh check that expects echo $1 to exit 2
status: Done
assignee:
  - '@Claude'
created_date: '2026-11-19T22:29:00Z'
updated_date: '2026-11-19T23:37:00Z'
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
- [x] #1 `echo $1` is no longer in the list of lines expected to exit 2 in scripts/smoke.sh.
- [x] #2 A check asserts that `echo [$1]` prints `[]` and exits 0.
- [x] #3 `scripts/smoke.sh exec` on a device reports no failure from the var: group, and the result is recorded in the task notes.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Drop `echo $1` from the unsupported-syntax loop in scripts/smoke.sh.
2. Add two var: checks after it: `echo [$1]` prints `[]`, and exits 0.
3. Run `scripts/smoke.sh exec` on the Vita and record the result.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Device (the Vita, work network, 2026-11-19): `scripts/smoke.sh exec` against the work Vita exited 0 with 156 PASS lines and no FAIL line. The two new checks (`var: an unset positional parameter expands to nothing`, `... exits 0`) pass, and the former `var: unsupported syntax exits 2 (echo $1)` failure is gone.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Removed `echo $1` from the lines smoke.sh expects to exit 2, since TASK-032 made `$1` a supported special parameter. Added two checks in the same var: group asserting `echo [$1]` prints `[]` and exits 0. `scripts/smoke.sh exec` on the Vita: 156 PASS, no FAIL.
<!-- SECTION:FINAL_SUMMARY:END -->
