---
id: TASK-016.01
title: Add grep
status: To Do
assignee: []
created_date: '2026-11-17 03:29'
updated_date: '2026-11-17 03:29'
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
priority: medium
ordinal: 35000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
grep is the command that makes pipes worth having, and the first text tool in the coreutils family. Follow the family's rules in the parent task: cmd_grep() in src/sshd_builtins.c, an alphabetical row in builtins[], resolve_arg() for every path, sh_printf() for matches, sh_eprintf() for errors, no printf, no chdir, no invented device paths.

Pattern engine: prefer POSIX basic regular expressions through newlib's regcomp/regexec if the Vita toolchain provides them. If it does not, ship literal substring matching and say so plainly in the usage string and README -- a silent downgrade to literal matching that the user cannot see is exactly the dishonesty the family rule forbids. Record which one shipped.

Exit status follows the conventional contract, because scripts and the eventual && rely on it: 0 matched, 1 no match, 2 error.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 grep PATTERN FILE prints the matching lines; with several files each line is prefixed file:.
- [ ] #2 grep with no file argument reads the session's stdin, so ls | grep ux0 works once pipes exist.
- [ ] #3 -i matches case-insensitively, -v inverts, -n prefixes line numbers, -c prints only a count, -l prints only file names, -q prints nothing and sets only the exit status.
- [ ] #4 Exit status is 0 when a line matched, 1 when none did, 2 on error such as an unreadable file.
- [ ] #5 An unreadable file is reported as grep: FILE: reason on the terminal and the other files still searched.
- [ ] #6 Regex behaviour is stated: either the POSIX basic subset works (and a test like grep 'a.c' matches abc) or the tool matches literally and both help and README say so.
- [ ] #7 A line longer than the tool's bound is either matched correctly or reported as over the limit, never silently truncated into a wrong answer.
- [ ] #8 A nonexistent pattern argument (no PATTERN given) prints usage and exits nonzero rather than matching everything.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session
- [ ] #3 README states which pattern syntax grep supports
<!-- DOD:END -->
