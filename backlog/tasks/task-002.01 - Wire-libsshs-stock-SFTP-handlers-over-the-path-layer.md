---
id: TASK-002.01
title: Wire libssh's stock SFTP handlers over the path layer
status: To Do
assignee: []
created_date: '2026-11-15T22:57:00Z'
labels: []
dependencies: []
documentation:
  - src/sshd_session.c
  - src/sh_path.c
  - include/sh_path.h
modified_files:
  - src/sshd_session.c
  - src/sftp_server.c
parent_task_id: TASK-002
priority: high
type: task
ordinal: 7000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The channel subsystem callback currently prints 'subsystem not supported' and refuses. libssh ships a server-side SFTP implementation that handles the packet protocol itself; the job is to construct it for the accepted session, drive it from the same single-threaded poll loop that already pumps the session, and have it tear down cleanly when the channel closes. The path semantics live in src/sh_path.c: normalise in POSIX space and translate once immediately before the sceIo call, never guess a device, and never fake symlinks. libssh's own handlers assume POSIX filesystem semantics underneath (stat attributes, directory iteration, path handling), so part of the work is showing what they do with Vita's device-prefixed names and adapting at the boundary rather than forking the library.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 sftp opens a subsystem session and lands in the session's current directory
- [ ] #2 ls, cd, pwd, get, put, mkdir, rmdir, rm, rename and realpath work against a real device
- [ ] #3 large downloads and uploads complete without truncation, including files larger than the transfer buffer
- [ ] #4 closing the sftp session leaves the server responsive and the shell session still usable
- [ ] #5 a rejected subsystem request still leaves the session able to accept a later one
- [ ] #6 directory iteration handles a device root and an empty directory without an infinite loop
<!-- AC:END -->
