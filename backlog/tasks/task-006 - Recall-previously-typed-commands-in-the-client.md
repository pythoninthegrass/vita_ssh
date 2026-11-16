---
id: TASK-006
title: Recall previously typed commands in the client
status: To Do
assignee: []
created_date: '2026-11-16T00:00:00Z'
labels: []
dependencies: []
documentation:
  - src/shell.c
  - src/main.c
  - src/menu.c
  - include/sshd.h
type: task
ordinal: 13000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The client keeps no record of what you typed. Single-command mode opens the IME on an empty field every time, so retyping the same path on a small on-screen keyboard is the daily cost of using the client at all, and the profiles file proves the app can already persist per-user state to ux0:data. The interactive pty path is different: shell.c forwards the shoulder buttons as raw arrow escapes, so history there belongs to the remote shell and should stay that way. The sshd app already keeps a 12-line per-session history (SSHD_HISTORY in include/sshd.h), which is the precedent for how much to keep.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 single-command mode offers the previous command in the IME field and walks newest-first through earlier ones
- [ ] #2 history survives disconnecting and relaunching the app, and is bounded to a fixed number of entries
- [ ] #3 repeating a command and submitting a blank line do not add duplicate or empty entries
- [ ] #4 passwords entered in the credential fields are never written to the history file
- [ ] #5 the interactive shell is unchanged: arrows still reach the remote shell untouched
- [ ] #6 task build produces both VPKs
<!-- AC:END -->
