---
id: TASK-028
title: Let long-running builtins yield to the poll loop so other sessions can connect
status: To Do
assignee: []
created_date: '2026-11-17T17:45:00Z'
labels:
  - shell
  - sshd
dependencies: []
references:
  - src/sshd_builtins.c
  - src/sshd_session.c
  - src/sshd.c
  - AGENTS.md
  - docs/vcmd.md
priority: medium
type: bug
ordinal: 59000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Found while verifying TASK-015.02 on a device (the work Vita, 2026-11-17): while `du /ux0` runs (about 170 s there), a second ssh session cannot even finish the SSH banner exchange (ConnectTimeout 10 s expired). The same happens with `du /ux0 | wc -l`, so pipes did not cause it. A builtin that is CPU or I/O bound and never calls `sh_server_poll_slice()` stops the single poll-loop thread, and with it every other session, the accept path and the Vita console.

AGENTS.md already states the rule for output (`wait_for_room()` runs `sh_server_poll_slice()` so every session's socket is still read) and `vcmd` does it for the network (`net_wait()`), but the tree walkers do not: `du`, `cp -r`, `rm -r`, and `grep` over many files are the candidates, and `du` is the one measured. Only `du` has a measurement; the others are suspects until timed.

Limits to settle in the open, not paper over: `sh_server_poll_slice()` is one `ssh_event_dopoll` and is not reentrant from inside a libssh callback, so it is legal only from the pump (`in_pump`), the same restriction `chan_write_raw()` has. A slice reads sockets and completes handshakes; per AGENTS.md, other sessions' commands and sftp pumps still run only when the running command finishes, so this task makes the server answer and accept during a long command, not run two commands at once. Decide how often a walk yields (every N entries or every N ms) so the cost is small, and whether the 170 s itself is a defect worth a separate look (it may be the sceIo stat per entry on a card with many files).

Do not widen this to Ctrl-C handling; that is its own task and depends on this one.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 While `du /ux0` runs on a device, a second ssh connection completes its banner and authenticates within the ConnectTimeout the smoke script uses (5 s), measured and recorded.
- [ ] #2 The same holds for the other tree walkers that are timed and found to starve the loop (cp -r, rm -r, grep over a tree), each named in the task notes with its measurement.
- [ ] #3 A walk yields only from the pump; a call from a libssh callback never runs a poll slice.
- [ ] #4 A walk's total time grows by a stated, measured amount at most, recorded before and after.
- [ ] #5 AGENTS.md says which builtins yield and how often, and the rule for a new long-running builtin matches what the code does.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session with a second session connecting mid-command
<!-- DOD:END -->
