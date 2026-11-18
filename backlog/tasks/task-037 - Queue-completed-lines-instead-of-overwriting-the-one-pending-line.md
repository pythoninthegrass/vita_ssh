---
id: TASK-037
title: Queue completed lines instead of overwriting the one pending line
status: To Do
assignee: []
created_date: '2026-11-18T09:16:00Z'
labels:
  - shell
dependencies: []
documentation:
  - AGENTS.md
  - src/sshd_session.c
priority: medium
type: bug
ordinal: 70000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The line editor holds one completed line at a time: src/sshd_session.c copies a finished line into `pending` and sets `pending_ready`, and `sh_session_pump()` runs it on the next poll-loop pass. If a second line completes before the pump consumes the first, it overwrites it and the first command never runs, with no message. The only guard is for the LF of a CRLF pair (a bare LF while a command is queued is swallowed).

Seen on a device on 2026-11-18 while testing TASK-016.06: an interactive `ssh -tt` session fed `cd /ux0/music` and `printenv PWD` in one write echoed both lines, then printed the home directory, so the `cd` was lost. The same thing happens to any pasted multi-line block, or a script piped into a pty session, and also when the pump is busy: a long command (`du`, a large `cat`) can be running while the user types ahead, and every line but the last typed during it is dropped.

Wanted: a completed line is never discarded. Decide in the plan, not assume: a small bounded queue of lines (how many, and what happens when it is full: refuse with a visible message rather than overwrite, since dropping silently is the failure being fixed), whether typed-ahead lines are echoed when they are typed or when they run, and how history, `!n` expansion and the prompt behave for a queued line. The exec path shares `pending` (`on_exec_request()`) and the sftp subsystem checks `pending_ready`, so both need to keep working. Memory is a constraint: SSHD_LINE_MAX bytes per queued line per session, with up to SSHD_MAX_SESSIONS sessions.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Two lines delivered in one chunk to a pty session both run, in order, and the second sees the first's effect (cd then printenv PWD)
- [ ] #2 Lines typed while a long command is running run after it finishes, in order
- [ ] #3 When the queue is full the editor refuses the line with a visible message and does not overwrite an earlier one
- [ ] #4 The CRLF case still works: a line ending \r\n runs once, and a scripted pty session loses no output lines
- [ ] #5 An exec session and an sftp session behave as before
- [ ] #6 The queue is bounded, and the per-session memory cost is stated in the task notes
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device with a pty session fed two lines in one write
- [ ] #3 arm-vita-eabi-size build/ssh_server_vita measured before and after against the size ceiling in AGENTS.md
<!-- DOD:END -->
