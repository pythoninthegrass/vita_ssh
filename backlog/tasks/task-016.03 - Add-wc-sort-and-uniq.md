---
id: TASK-016.03
title: 'Add wc, sort and uniq'
status: To Do
assignee: []
created_date: '2026-11-17 03:29'
updated_date: '2026-11-17 03:38'
labels:
  - shell
  - coreutils
dependencies:
  - TASK-015.01
documentation:
  - AGENTS.md
modified_files:
  - src/sshd_builtins.c
parent_task_id: TASK-016
priority: low
ordinal: 37000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The pipeline companions: wc counts, sort orders, uniq collapses adjacent duplicates. Without them a pipe can only be read by eye. Same rules as the parent task -- cmd_wc()/cmd_sort()/cmd_uniq() in src/sshd_builtins.c, alphabetical rows, resolve_arg(), sh_printf()/sh_eprintf(), no printf, no chdir, no invented device paths.

sort is the memory-sensitive one: it must accumulate everything before it can emit anything, so it needs a stated bound with a readable error past it (the exec-output bound in TASK-002.03.02 is the precedent for that style of limit), and the accumulation belongs on the heap. Comparison is byte order (C locale), stated in help.

uniq collapses only adjacent duplicates -- say so in the usage string rather than letting people assume it deduplicates a whole file.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 wc FILE prints lines, words and bytes in that order; -l, -w and -c print only their own column; several files print one row each plus a total row.
- [ ] #2 wc reads stdin with no file argument, so wc -l after a pipe counts what the pipe produced.
- [ ] #3 sort FILE orders lines by byte value; -n sorts numerically; -r reverses; -u drops duplicate lines after sorting.
- [ ] #4 sort with -t or -k refuses with a readable unsupported-option message and a nonzero exit rather than ignoring them.
- [ ] #5 sort past its stated memory bound reports a readable error naming the bound and leaves the session usable.
- [ ] #6 uniq collapses adjacent duplicate lines; -c prefixes a count; -d prints only lines that were duplicated; -u prints only lines that appeared once.
- [ ] #7 All three read stdin when given no file argument, and all three report a missing file as name: FILE: reason.
- [ ] #8 help shows a one-line usage for each that names the subset it implements.
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Scratch space: when TASK-019.01 lands, sort's accumulation spool resolves through the path layer to /tmp (the ur0: RAM disk) rather than a hand-picked directory, and the memory bound reported in the error should name the same limit the tool actually enforces.
<!-- SECTION:NOTES:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session
<!-- DOD:END -->
