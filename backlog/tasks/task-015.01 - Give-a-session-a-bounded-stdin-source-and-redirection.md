---
id: TASK-015.01
title: Give a session a bounded stdin source and < redirection
status: Done
assignee: []
created_date: '2026-11-17 03:29'
updated_date: '2026-11-17 08:06'
labels:
  - shell
dependencies: []
documentation:
  - AGENTS.md
modified_files:
  - include/sshd.h
  - include/sh_stdin.h
  - src/sh_stdin.c
  - src/sshd_session.c
  - src/sshd_builtins.c
  - CMakeLists.txt
  - scripts/test_sh_stdin.c
  - scripts/test_sh_stdin.py
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
- [x] #1 cat < FILE writes the file to the terminal byte for byte, including a file larger than one read chunk.
- [x] #2 cat with no file argument reads the session's stdin source; with no source at all it reads nothing and exits 0 without waiting.
- [x] #3 cmd < FILE where FILE does not exist prints sh: FILE: No such file or directory and runs no command.
- [x] #4 cmd < DIR prints an Is a directory error; a path with a missing parent reports the same way other path arguments do.
- [x] #5 A source larger than the stated bound fails with a readable error naming the limit instead of truncating silently or exhausting memory.
- [x] #6 < and > are independent: cat a > b reads a and writes b, and a line with both reads one file and writes another.
- [x] #7 A quoted < stays literal: echo "a < b" prints a < b rather than redirecting.
- [x] #8 Errors still reach the terminal: a failing cmd < FILE reports its error on the terminal even though its stdin came from a file.
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Design: the stdin source is `sh_stdin_t` (include/sh_stdin.h, src/sh_stdin.c), a bounded buffer filled completely before the command runs from a reader callback, so it works for real files and pseudo files alike. `sh_stdin_read()` returns 0 at the end and for a source that was never loaded, so a reader never waits. The session owns one (`sh_session_stdin_start/_read/_end`), limit SSHD_STDIN_MAX = 1 MiB, and `sh_execute_line` loads it before the stdout redirect is opened so a failed `<` runs no command and creates no output file. `<` and `>` share `drop_operator()` for removing the operator and its file name. `cat` with no file operand now reads the session's stdin instead of failing with `missing operand`.

Host check: `python3 -I scripts/test_sh_stdin.py` (byte-exact over 200000 bytes read through small chunks, empty and never-loaded sources, the limit boundary, double free).

Device run on the home Vita, server built 2026-11-17 01:52:46, over real ssh exec sessions, all passing: AC1 `cat < small` and `cat < big` (200000 bytes, past one 64 KB chunk) byte-exact by cmp; AC2 bare `cat` exits 0 in 1 s; AC3 `cat < nope` prints `sh: /ux0/data/t015/nope: No such file or directory`, exit 1, and `echo ran > shouldnt < nope` created no `shouldnt`; AC4 `cat < <dir>` prints `Is a directory`, a missing parent prints `No such file or directory`; AC5 a 2 MiB source prints `too large for stdin (limit 1048576 bytes)`; AC6 `cat a > b`, `cat < a > b` and a 200000-byte `cat < big > b3` all produce byte-identical files; AC7 `echo "a < b"` prints `a < b`; AC8 errors reach the terminal for both `cat < nope > out` and `cat nope < small`.

Not covered: the pipe task will reuse sh_stdin_t as its buffer; a `cat < FILE` interrupted by ^C was not exercised.
<!-- SECTION:NOTES:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session
<!-- DOD:END -->
