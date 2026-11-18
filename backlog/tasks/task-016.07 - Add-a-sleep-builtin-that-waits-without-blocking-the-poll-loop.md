---
id: TASK-016.07
title: Add a sleep builtin that waits without blocking the poll loop
status: To Do
assignee: []
created_date: '2026-11-18T09:13:00Z'
labels:
  - shell
  - coreutils
dependencies: []
documentation:
  - AGENTS.md
  - docs/vcmd.md
parent_task_id: TASK-016
priority: low
type: enhancement
ordinal: 69000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
There is no sleep, so `sleep 3 && cmd` and `cmd ; sleep 1 ; cmd` fail with `sh: sleep: command not found` (status 127). It came up while testing TASK-016.06 on a device: holding one exec session open long enough to overlap another had to be done from the Mac side with a local sleep.

The platform rule that shapes it: everything runs on one poll-loop thread, so a bare sleep or sceKernelDelayThread inside a builtin freezes every other session, the accept path and the console. The wait has to be a deadline loop that runs sh_server_poll_slice() (or sh_session_yield()) each pass, the way vcmd's net_wait() does, and stops when sh_session_interrupted() says the session is finished. docs/vcmd.md describes that shape.

Questions to settle in the plan, not assume: whether fractional seconds are accepted (GNU sleep takes 0.5; busybox may not), whether suffixes s/m/h/d are accepted, whether several arguments add up as GNU does, and what upper bound is sensible given that a running command cannot be interrupted by Ctrl-C and a stalled session is only reclaimed by WINDOW_STALL_S. The sh_chain.c operator words (`;`, `&&`, `||`) need no change; they already run the next command after this one returns.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 sleep N waits about N seconds and exits 0; sleep with no argument, a non-number or a negative number prints a usage line on stderr and exits 1
- [ ] #2 While one session is in sleep, a second session connects, gets its banner and runs a command without waiting for the first to finish
- [ ] #3 A sleep whose client has disconnected stops early and tears the session down instead of running to the end
- [ ] #4 The accepted argument forms (fractions, suffixes, multiple arguments) and the upper bound are decided, documented in the usage string and covered by the host test if the parsing is a pure function
- [ ] #5 sleep has a row in builtins[] in alphabetical order with a usage string that help prints, and which finds it
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device with two overlapping sessions
- [ ] #3 arm-vita-eabi-size build/ssh_server_vita measured before and after against the size ceiling in AGENTS.md
<!-- DOD:END -->
