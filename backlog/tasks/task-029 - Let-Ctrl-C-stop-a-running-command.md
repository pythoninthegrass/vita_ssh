---
id: TASK-029
title: Let Ctrl-C stop a running command
status: To Do
assignee: []
created_date: '2026-11-17T17:45:00Z'
updated_date: '2026-11-17T17:45:00Z'
labels:
  - shell
  - sshd
dependencies:
  - TASK-028
references:
  - src/sshd_session.c
  - src/sshd_builtins.c
  - AGENTS.md
priority: low
type: feature
ordinal: 60000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A command already running cannot be interrupted with Ctrl-C today: noticing the keystroke needs the session's socket to be read, which is exactly what the running command is blocking. `sh_session_interrupted()` therefore only reports state the server already knew (channel closed, `exit` requested), as the header of src/sshd_session.c explains. TASK-015.02's AC10 had to be reworded for that reason: a pipeline checks the flag before every segment, but a Ctrl-C typed while a segment runs is never seen.

Depends on the yielding task: once a long-running builtin calls `sh_server_poll_slice()` periodically, the session's channel data callback runs during the command and can see byte 0x03. That callback must then only set a flag (nothing may run a command from a libssh callback), and `sh_session_interrupted()` must report it so the walks that already check it stop, and so must `run_pipeline()` between segments and, once it exists, the chain loop of TASK-025.

Settle in the open: Ctrl-C must reach the command only in a pty session (an exec session has no keyboard), the flag must be cleared before the next line runs so one Ctrl-C does not kill the following command, and a command that never yields still cannot be interrupted, which AGENTS.md must keep saying for those.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Ctrl-C typed in a pty session while `du /ux0` runs stops the walk within a stated time and returns to the prompt, measured on a device.
- [ ] #2 Ctrl-C during a multi-segment pipeline stops it before the next segment starts, and the session survives.
- [ ] #3 The interrupted flag is cleared before the next command line runs: a command typed right after a Ctrl-C runs to completion.
- [ ] #4 The flag is set only from the channel data callback and never runs a command from it.
- [ ] #5 An exec session (no pty) is unaffected, and a command that never yields is documented as still uninterruptible in AGENTS.md and the sshd_session.c header.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real pty ssh session
<!-- DOD:END -->
