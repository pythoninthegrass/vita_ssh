---
id: TASK-016.08
title: Add the p flag to sed's s command
status: To Do
assignee: []
created_date: '2026-11-20T18:06:00Z'
labels:
  - shell
  - coreutils
dependencies:
  - TASK-016.04
modified_files:
  - src/sh_sed.c
  - include/sh_sed.h
  - scripts/test_sh_sed.c
  - scripts/test_sh_sed.py
  - scripts/smoke.sh
  - README.md
parent_task_id: TASK-016
priority: low
type: enhancement
ordinal: 78000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`sed -n 's/old/new/p'` is a common one-liner: print the line only when the substitution changed it. TASK-016.04 refuses the flag by name (`flag 'p' of `s' is not supported`) because it was outside the agreed subset. The refusal, the README sed paragraph and the smoke check that asserts it must change together.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 s/RE/TEXT/p prints the pattern space once more when, and only when, the substitution changed it; s/RE/TEXT/gp combines with g in either order.
- [ ] #2 With -n, sed -n s/a/b/p prints only the changed lines; without -n a changed line prints twice and an unchanged one once.
- [ ] #3 Host test cases and the gsed pinning in scripts/test_sh_sed.py cover the flag, including the empty-match and no-match cases.
- [ ] #4 README states p as supported; the smoke check that expects the refusal is replaced by one that expects the behavior; the other s flags (i, a number, w) are still refused by name.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 Verified on a device over a real ssh session
<!-- DOD:END -->
