---
id: TASK-016.04
title: Add sed (stream editor subset)
status: To Do
assignee: []
created_date: '2026-11-17 03:29'
updated_date: '2026-11-17 03:29'
labels:
  - shell
  - coreutils
dependencies:
  - TASK-015.01
  - TASK-016.01
documentation:
  - AGENTS.md
  - context7 library /holzschu/a-shell
modified_files:
  - src/sshd_builtins.c
parent_task_id: TASK-016
priority: medium
ordinal: 38000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A deliberately small sed: line-oriented, one script, the substitutions and commands people actually use in a one-liner. Core functionality, not completeness -- the scope agreed with the user.

In scope: s/old/new/ (first match per line), s/old/new/g (all matches), an alternate delimiter such as s|old|new| for paths, d (delete the line), p (print it), q (stop), -n (suppress automatic printing), and repeated -e scripts applied in order. Out of scope and refused with a readable error: line-address ranges (1,5d), hold and space commands, labels and branches, and -i in place -- in particular -i must be refused, not quietly accepted, because silently rewriting a file the user did not ask to rewrite is the worst failure this family can have.

Same family rules as the parent task: cmd_sed() in src/sshd_builtins.c, alphabetical row, resolve_arg() for file arguments and for a script file, sh_printf() for the stream, sh_eprintf() for errors.

Regex: whatever grep settled on (POSIX basic via newlib regcomp, or literal) -- reuse one engine rather than writing a second matcher, and keep the two tools' pattern behaviour identical so muscle memory does not betray anyone.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 sed 's/old/new/' FILE replaces the first match on each line and sed 's/old/new/g' replaces all of them, with unspecified lines passed through unchanged.
- [ ] #2 An alternate delimiter works: sed 's|/ux0/a|/ux0/b|' edits a path without backslashes.
- [ ] #3 -n suppresses automatic printing and p prints explicitly; d deletes the current line; q stops reading.
- [ ] #4 Repeated -e scripts apply in the given order; a script that fails to parse reports sed: reason and exits nonzero without emitting a partial edit.
- [ ] #5 sed reads stdin with no file argument, so cat f | sed 's/a/b/' works once pipes exist.
- [ ] #6 -i is refused with a readable message and a nonzero exit; no file is written.
- [ ] #7 Line-address ranges, hold-and-space commands and labels are refused with a readable unsupported message naming what was not understood.
- [ ] #8 A missing file argument is reported as sed: FILE: reason with the other files still processed.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session
- [ ] #3 README states which sed subset is supported
<!-- DOD:END -->
