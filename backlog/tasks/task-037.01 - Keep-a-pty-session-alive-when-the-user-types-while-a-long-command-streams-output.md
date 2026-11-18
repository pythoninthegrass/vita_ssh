---
id: TASK-037.01
title: >-
  Keep a pty session alive when the user types while a long command streams
  output
status: To Do
assignee: []
created_date: '2026-11-18T15:43:00Z'
labels:
  - shell
dependencies: []
documentation:
  - AGENTS.md
  - src/sshd_session.c
  - scripts/smoke.sh
parent_task_id: TASK-037
priority: medium
type: bug
ordinal: 72000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
In an interactive pty session, typing a line while a command is still streaming a large output wedges the session: the output stops at roughly 65 to 125 KB and nothing more arrives until the server logs `stopped reading for 30 s; dropping the session` and closes it. Found on a device on 2026-11-18 while verifying TASK-037, and reproduced on the committed HEAD build from before that task, so it is not a regression of the line queue. Repro: in `ssh -tt` run `cat` on a file of about 1 MB or more (the 1.4 MB server VPK in `ux0:data` works, as does a 236 KB `sshd.log`) and type a line, for example `echo MID1`, about a second after the cat starts; a line typed after the output finished works, and so does a line typed during a command that prints little (`du /ux0/app`).\n\nIt also makes `scripts/smoke.sh pty` unreliable: its first step is `cat sshd.log` and it types the next lines while that streams, so the pty half passes 16 of 16 on HEAD with a 92 KB log and fails the same 12 checks with a 236 KB log. The log grows with every run and is not rotated on restart, so the smoke result depends on how much has been logged. A user pasting text or typing ahead during a long listing hits the same wedge.\n\nThe cause is not confirmed. One candidate: the echo of typed bytes is written from inside the channel data callback while `in_pump` is set, so `wait_for_room()` runs a nested `sh_server_poll_slice()`, which AGENTS.md says is not legal from inside a libssh callback. A related loss: echo and the queue-full message written from the callback while a command streams. Nothing in this task presumes that cause; confirm it first.\n\nThe same device run showed 12 MB sftp/scp transfers and a 12 MB exec `cat` failing on HEAD as well (`sftp get 12 MB`, `sftp get -r tree`, `exec cat of 12 MB`). Those may share a cause or may not; this task covers them only if the root cause turns out to be common.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A pty session running `cat` on a file of at least 1.4 MB with a line typed a second in streams the whole file and then runs the typed line, with no `stopped reading` entry in sshd.log
- [ ] #2 The typed line's echo appears and the line runs after the cat finishes, in order, with the TASK-037 queue behavior unchanged (refusal when full, no overwrite)
- [ ] #3 `scripts/smoke.sh pty` passes 16 of 16 with an sshd.log of at least 250 KB
- [ ] #4 The cause is identified and recorded in the task notes, including whether the 12 MB sftp, scp and exec transfer failures share it
- [ ] #5 AGENTS.md states whatever rule the fix establishes for writing from a libssh callback while a command is running
- [ ] #6 A host test covers any new pure logic the fix adds, and `task build` produces both VPKs with `arm-vita-eabi-size build/ssh_server_vita` recorded against the size ceiling
<!-- AC:END -->
