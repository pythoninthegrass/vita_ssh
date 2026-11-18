---
id: TASK-034
title: >-
  Free a session's slot as soon as its client disconnects, even while a command
  runs
status: Done
assignee:
  - Claude
created_date: '2026-11-17T23:30:00Z'
updated_date: '2026-11-18T21:16:00Z'
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
- [x] #1 With a long command running on session A, a second client that authenticates, queues a command and is then killed has its slot freed within a stated time (measured), not when A's command ends
- [x] #2 With a long command running on session A, five consecutive abandon-after-auth probes never cause a `refused ...: 4 sessions already open` for a sixth, real client, measured on a device
- [x] #3 A session whose client disconnected while its queued command had not started never runs that command
- [x] #4 An exec session whose client sends EOF but keeps the connection open still runs its command and returns the output (existing behavior kept)
- [x] #5 The running command's own session is unaffected by other sessions being reaped, and sshd.log records each reaped session with the reason
- [x] #6 AGENTS.md says when a session's slot is freed relative to a running command, matching the code
- [x] #7 The decision on the `poll: loop stalled` log line after a yielding command is recorded in the task notes
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Cause: the closed-transport check (`SSH_CLOSED | SSH_CLOSED_ERROR`) and the session drop run only in `server_loop()`'s outer pass. A yielding command only calls `sh_server_poll_slice()` (one `ssh_event_dopoll`), which lets libssh notice the closed socket but never frees the slot, so it is freed when the command ends.

Plan:
1. In `src/sshd.c`, extract the per-slot closed-transport check into `slot_reap_closed(i, how)`, used by `server_loop()` (same log line as today) and by the poll slice, so there is one copy of the log format.
2. `sh_server_poll_slice()` runs it after `ssh_event_dopoll` returns (never inside a callback) for every used slot except the one currently inside `sh_session_pump()` (tracked as `pumping_slot`, set around the pump call in `server_loop()`). Reaping the running session from inside its own command would free state it is using. A reaped slot logs `session <peer> dropped: ... (while a command ran)`, and its queued line is freed with it, so it never runs (AC3).
3. Only a closed transport reaps. Channel EOF never does, so an exec client that half-closes stdin keeps its command (AC4).
4. Watchdog: set `last_wake` after the slot loop so the `loop stalled` line measures only the poll/draw part of a pass, not a yielding command. Record the decision in notes.
5. No host test is possible (sshd.c is Vita-only). Add a device probe `scripts/abandon_probes.sh` (long command on session A, N authenticate-queue-kill probes, then a real client), measure on the device, and update the AGENTS.md/CLAUDE.md slot-lifetime text and README.
6. Verify: `task build` for both VPKs; device run needs the VPK installed (see CLAUDE.local.md install notes).
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-18: implemented in src/sshd.c (slot_reap_closed, pumping_slot, reaping in sh_server_poll_slice, closed-transport check before the pump so a queued line of a gone client never starts). `task build` produces both VPKs. Added scripts/abandon_probes.sh. AGENTS.md and README updated.

Decision on `poll: loop stalled` (AC7): fixed here. server_loop() resets last_wake after a pump that took at least RESUME_GAP_US, so a yielding command no longer logs a stall; shorter passes and a suspend during the 50 ms poll are still measured. A suspend in the middle of a long command goes unlogged, accepted because the command's own yields keep the sockets alive.

Device run, 2026-11-18, the Vita on the office network, build installed with `vpk install` over ssh (promote result 0x00000000, no card problems). `scripts/abandon_probes.sh`: long `du /ux0` on session A, five probes (authenticate, queue `sleep 1`, kill -9), then a sixth client: 2 pass 0 fail. sshd.log: each probe logged `session <peer> dropped: error 0x4 -- Socket error: disconnected (while a command ran)` in the same log second as the kill (log resolution is 1 s), while `du` was still running, and no `refused ... sessions already open` line appeared. The sixth client was accepted and waited behind `du`, and its later kill logged `dropped ... disconnected by user` the same way. None of the probes' queued `sleep 1` ran (no exec-done line for them). A later exec (`ls /ux0/data`, client sends EOF right behind the request) ran and returned its output once `du` finished, so exec-ignores-EOF is unchanged.

Observed, not changed: session A's own `du` kept running about 2 minutes after the script killed A's client, until its output waited for room and logged `transport closed while output waited for room`. A running command's own session is only noticed as gone when its output stalls, not by the slice. That is the running session, outside this task's scope.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
A session whose client disconnects is now freed while another session's command runs. `sh_server_poll_slice()` reaps every other slot whose transport closed (`slot_reap_closed()`, never the pumping slot), the closed check also runs before the pump so a gone client's queued line never starts, and only a closed transport frees a slot, so an exec client's EOF is unchanged. The `poll: loop stalled` line skips a pump longer than its threshold. Added `scripts/abandon_probes.sh`; AGENTS.md and README describe the rule. Measured on the Vita: five abandon-after-auth probes during a long `du` were each dropped in the same log second as the kill, with no `refused` line, and a sixth client was accepted.
<!-- SECTION:FINAL_SUMMARY:END -->
