---
id: TASK-016.02
title: Add head and tail
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
ordinal: 36000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The two simplest members of the family, and the ones that make a long cat survivable on a small screen. Same rules as the parent task: cmd_head()/cmd_tail() in src/sshd_builtins.c, alphabetical rows in builtins[], resolve_arg() for paths, sh_printf()/sh_eprintf() for output and errors.

tail is the only one with a real design question: reading the last N lines of a file means knowing where the end is. Keep it bounded -- stat the file, read backwards in chunks, and refuse with a readable error past a stated size bound rather than loading a huge file into memory. A Vita thread's stack is whatever sceKernelCreateThread was given, so the line accumulation lives on the heap.

Note that tail -f cannot exist: nothing may block in the poll loop, so a follow option is refused with a clear message rather than hanging every session.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 head FILE prints the first 10 lines; -n N prints the first N, including -n 0 printing nothing; -c N prints the first N bytes.
- [ ] #2 tail FILE prints the last 10 lines; -n N prints the last N; -c N prints the last N bytes.
- [ ] #3 Both read the session's stdin when given no file, so cat big | head -n 5 works once pipes exist.
- [ ] #4 Several file arguments print the ==> name <== headers between files, as the coreutils convention expects.
- [ ] #5 A missing file is reported as head: FILE: No such file or directory with a nonzero exit, and the remaining files are still printed.
- [ ] #6 tail refuses -f (follow) with a readable message instead of blocking the poll loop.
- [ ] #7 A file beyond tail's stated size bound fails with a readable error naming the bound rather than exhausting memory.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session
<!-- DOD:END -->
