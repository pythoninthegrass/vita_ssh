---
id: TASK-015.01
title: Give a session a bounded stdin source and < redirection
status: To Do
assignee: []
created_date: '2026-11-17 03:29'
labels:
  - shell
dependencies: []
documentation:
  - AGENTS.md
modified_files:
  - src/sshd_session.c
  - include/sshd.h
  - src/sshd_builtins.c
parent_task_id: TASK-015
priority: high
ordinal: 33000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Nothing in the server can read input that is not a named file: there is no stdin concept, so a command's only source is the path in its own argument. That blocks pipes (the sibling task) and makes cat useless in the position people expect.

Wanted: a session can be given a bounded stdin source that a command reads instead of a file, and cmd < FILE works end to end.

Provides to the pipe task: the buffer, the read interface, and the rule that a source with nothing in it reads as EOF immediately rather than waiting -- nothing may block, so an absent source is empty, never a stall.

Existing machinery to extend, not duplicate: the stdout side is extract_redirect() plus sh_session_redirect_start()/sh_session_redirect_end() in src/sshd_session.c, and the redirect file is opened before the builtin runs and closed on every exit path. Mirror that discipline for the read side. cat already writes whatever it reads through sh_printf(), which is what makes it the first consumer to test against.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 cat < FILE writes the file to the terminal byte for byte, including a file larger than one read chunk.
- [ ] #2 cat with no file argument reads the session's stdin source; with no source at all it reads nothing and exits 0 without waiting.
- [ ] #3 cmd < FILE where FILE does not exist prints sh: FILE: No such file or directory and runs no command.
- [ ] #4 cmd < DIR prints an Is a directory error; a path with a missing parent reports the same way other path arguments do.
- [ ] #5 A source larger than the stated bound fails with a readable error naming the limit instead of truncating silently or exhausting memory.
- [ ] #6 < and > are independent: cat a > b reads a and writes b, and a line with both reads one file and writes another.
- [ ] #7 A quoted < stays literal: echo "a < b" prints a < b rather than redirecting.
- [ ] #8 Errors still reach the terminal: a failing cmd < FILE reports its error on the terminal even though its stdin came from a file.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session
<!-- DOD:END -->
