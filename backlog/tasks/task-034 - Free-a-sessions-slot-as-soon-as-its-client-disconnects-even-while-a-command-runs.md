---
id: TASK-034
title: >-
  Free a session's slot as soon as its client disconnects, even while a command
  runs
status: To Do
assignee: []
created_date: '2026-11-17T23:30:00Z'
labels:
  - shell
  - sshd
dependencies:
  - TASK-028
references:
  - src/sshd.c
  - src/sshd_session.c
  - AGENTS.md
  - >-
    backlog/tasks/task-028 -
    Let-long-running-builtins-yield-to-the-poll-loop-so-other-sessions-can-connect.md
priority: medium
type: bug
ordinal: 65000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
While one command runs, a client that disconnects keeps its session slot until that command ends. Found on a device while verifying TASK-028 (the work Vita, 2026-11-17): a long `cp -r` was running and three ssh probes that authenticated, queued an exec command and were killed after 12 s each held a slot; sshd.log then showed `refused <peer>: 4 sessions already open` for every later connection (SSHD_MAX_SESSIONS is 4, a hardware limit), and the three `handshake failed ... Received SSH_MSG_DISCONNECT: 11:disconnected by user` lines appeared only when the copy finished, about 390 s later. Abandoned connections can therefore lock real clients out for as long as a long command runs, which undercuts the point of TASK-028 (the server answers and accepts during a long command).

An exec session deliberately ignores the client's EOF (AGENTS.md: `ssh host cmd` with no stdin sends EOF right behind the request), so a closed socket has to be told apart from an EOF on an open connection; the transport closing is the signal, not the channel EOF. Related to TASK-029 (Ctrl-C), which also needs the server to notice what a client sent mid-command, but this is about the connection going away, not a keystroke, and needs no pty.

The sshd.log watchdog line `poll: loop stalled for N ms (suspend or starvation)` also fires after a long yielding command, because it measures the gap between outer loop passes and does not know the yield polled meanwhile; whether that should be fixed here or left is for the worker to settle and record.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 With a long command running on session A, a second client that authenticates, queues a command and is then killed has its slot freed within a stated time (measured), not when A's command ends
- [ ] #2 With a long command running on session A, five consecutive abandon-after-auth probes never cause a `refused ...: 4 sessions already open` for a sixth, real client, measured on a device
- [ ] #3 A session whose client disconnected while its queued command had not started never runs that command
- [ ] #4 An exec session whose client sends EOF but keeps the connection open still runs its command and returns the output (existing behaviour kept)
- [ ] #5 The running command's own session is unaffected by other sessions being reaped, and sshd.log records each reaped session with the reason
- [ ] #6 AGENTS.md says when a session's slot is freed relative to a running command, matching the code
- [ ] #7 The decision on the `poll: loop stalled` log line after a yielding command is recorded in the task notes
<!-- AC:END -->
