---
id: TASK-032
title: Expand $? to the exit status of the last command
status: To Do
assignee: []
created_date: '2026-11-17T20:44:00Z'
labels:
  - shell
dependencies:
  - TASK-020
references:
  - src/sshd_builtins.c
  - src/sshd_session.c
documentation:
  - AGENTS.md
priority: medium
type: enhancement
ordinal: 63000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The shell reports a command's exit status to an exec client but gives a person or a one-liner no way to look at it: there is no `$?`. After `grep foo f` or `cp a b` there is nothing to ask what happened except to chain on it with `&&`/`||` (TASK-025), which only branches and cannot print or compare the number. `$?` is the most used special parameter in sh, and `echo $?` is the first thing anyone types to check a command.

Needs a per-session record of the last command's status, which TASK-031 (`exit` with no argument and EOF both report the last status) also wants. Whichever of the two lands first adds the field; the other reuses it rather than adding a second one. The field is per session like the cwd, never a libc static, so one session's `$?` cannot be seen from another.

Scope is this one parameter. Other special parameters (`$$`, `$#`, `$@`, `$1`) are out: there are no scripts or processes for them to describe.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `echo $?` prints 0 after a command that succeeded and the command's nonzero status after one that failed: `ls /nope` then `echo $?` prints the status ls returned.
- [ ] #2 Within one chained line, `$?` reflects the previous item: `false ; echo $?` prints 1 and `true && echo $?` prints 0, once TASK-025 and TASK-030 exist.
- [ ] #3 A pipeline sets `$?` to its last segment's status, matching what the line reports to an exec client.
- [ ] #4 A syntax error leaves `$?` at 2 and an unknown command leaves it at 127, matching the statuses the shell already returns for them.
- [ ] #5 `$?` is 0 at the start of a fresh session, and `echo $?` does not change the value it reads for the next command beyond its own status of 0.
- [ ] #6 `${?}` works in the same positions as `$?`; a single-quoted `'$?'` stays literal and a double-quoted `"$?"` expands.
- [ ] #7 Each session has its own value: a failure in one session is never visible as `$?` in another.
- [ ] #8 history recalls the line as typed, not with the number substituted.
- [ ] #9 README and help list `$?` as the one special parameter the shell supports and say the others are unsupported.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session, including two concurrent sessions
<!-- DOD:END -->
