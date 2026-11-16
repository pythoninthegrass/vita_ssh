---
id: TASK-001.04
title: Smoke-test interactive and exec sessions on the Vita
status: To Do
assignee: []
created_date: '2026-11-15T22:56:00Z'
updated_date: '2026-11-16T06:21:00Z'
labels: []
dependencies:
  - TASK-001.01
  - TASK-001.02
  - TASK-001.03
documentation:
  - src/sshd_session.c
  - src/sshd_builtins.c
  - src/sh_path.c
modified_files:
  - src/sshd_session.c
  - src/sshd_builtins.c
  - src/sh_path.c
parent_task_id: TASK-001
type: task
ordinal: 5000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The shell, path layer and session handling have only ever been reasoned about, never run: a build that links proves nothing about whether ls reaches a device, whether cd tracks the session's cwd, or whether a hung client leaves a dead session wedged in the poll loop. This is the on-device pass against a real OpenSSH client from both macOS and Linux. Two behaviours are design limits rather than bugs and should be reported as such, not silently fixed: a Ctrl-C during a running command cannot interrupt it (single-threaded, so the poll loop is inside the command), and long recursive walks are bounded by a depth limit instead.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 password login works with the password shown on the Vita screen, and any username is accepted
- [x] #2 public-key login works once the user's public key is installed, and a wrong key is refused
- [x] #3 ls /, ls on each device, cd, pwd and relative paths behave, and cd tracks per session rather than leaking between two concurrent sessions
- [ ] #4 du, recursive rm -r, recursive cp -r and a cross-device mv produce correct results and honest errors
- [x] #5 an untranslatable path (unknown device, traversal above a device root) reports No such file or directory rather than touching the wrong device
- [x] #6 exec mode (ssh vita ls -la ux0:) prints output and returns the command's exit status to the client so $? is usable in a shell script
- [x] #7 exit ends the session and a client that hangs up abruptly leaves no session stuck in the poll loop; a later connection still works
- [x] #8 tab completion, history recall and Ctrl-A/Ctrl-C/Ctrl-D/Ctrl-E/Ctrl-U/Ctrl-W/Ctrl-L behave at the prompt
- [ ] #9 results from a Linux devbox match macOS, and any divergence is recorded in the task notes
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Password login confirmed manually by Lance before the smoke script existed (AC1). Cross-device mv skipped in scripts/smoke.sh: no second writable device mounted (AC4 open). Linux devbox not yet run (AC9 open).

AC8 proven by scripts/smoke.sh pty on macOS: tab completion, !!, Ctrl-A/C/D/E/U/W/L, Left/Right, mid-line insert and backspace all pass (16/16).
<!-- SECTION:NOTES:END -->
