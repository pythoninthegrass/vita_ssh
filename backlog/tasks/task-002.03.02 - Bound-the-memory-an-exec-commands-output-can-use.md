---
id: TASK-002.03.02
title: Bound the memory an exec command's output can use
status: In Progress
assignee:
  - claude
created_date: '2026-11-16T18:24:00Z'
updated_date: '2026-11-16T19:42:00Z'
labels: []
dependencies: []
references:
  - src/sshd_session.c
  - src/sshd_builtins.c
  - scripts/smoke.sh
documentation:
  - AGENTS.md
modified_files:
  - src/sshd_session.c
  - src/sshd.c
  - include/sshd.h
  - scripts/smoke.sh
  - AGENTS.md
parent_task_id: TASK-002.03
priority: medium
type: bug
ordinal: 22000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
An ssh exec command such as `ssh host "cat bigfile"` runs inside the server's single poll loop and writes all of its output before the loop can send any of it, so libssh queues the whole output in RAM. A multi-megabyte file works today (a 1.35 MB cat was verified on a device), but a file of tens or hundreds of megabytes could exhaust the Vita's memory and take the server down for every session. SFTP transfers do not have this problem because replies are held back once a bounded amount is queued. Found while fixing the 64 KB exec output cutoff in TASK-002.03; this limit was not tested and not fixed there.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An exec cat of a file much larger than the free memory completes byte-identical, or fails with a clear error, without taking the server down
- [ ] #2 Memory held for one exec command's output stays bounded regardless of file size
- [ ] #3 Other sessions keep responding while a large exec output is being sent
- [ ] #4 A 1 MB exec cat still returns every byte
- [ ] #5 A client that stops reading mid-output does not hold the server up indefinitely
- [ ] #6 AGENTS.md records how exec output is paced
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Research findings (libssh 0.12.2 src/channels.c channel_write_common, src/session.c ssh_handle_packets_termination):
- exec sessions are blocking (no ssh_set_blocking call outside sftp). With a blocking session and no session timeout, a write that exceeds the client's remote window waits with SSH_TIMEOUT_INFINITE for a window adjust. So libssh does NOT queue the whole output: in-flight data is capped by the client's window. The description's premise is only partly right.
- Real defects: (a) a client that stops reading parks the server inside ssh_channel_write forever (AC5 fails); (b) while parked, only libssh's own packet handling runs, so other sessions' pump (commands, sftp) is stalled (AC3 at risk); (c) chan_write_raw drops the rest of the output on n<=0 with no error.
- Neither the memory bound (AC1/2) nor AC3/AC5 can be checked from the build sandbox; device run needed.

Proposed approach (awaiting approval, material: touches the one-thread model):
1. Exec channel writes become window-paced: write at most ssh_channel_window_size() bytes per call, never blocking inside libssh.
2. When the window is 0, service the poll event (one ssh_event_dopoll slice) until it reopens, with a stall deadline (no window progress for N s) after which the command is aborted with an stderr message, exit status 1, and the session dropped.
3. Export a small poll-slice hook from sshd.c for sshd_session.c to call.
4. smoke.sh: add a large exec cat check plus a stalled-reader check; AGENTS.md records the pacing.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented the approved approach (nested poll slice): sh_server_poll_slice() in src/sshd.c, in_pump flag, wait_for_window() and window-capped writes in chan_write_raw() in src/sshd_session.c, WINDOW_STALL_S = 30. task build produces both VPKs. smoke.sh sftp half gained a 12 MB exec cat compare and a stalled-reader check; AGENTS.md records the pacing (AC6). Not yet run on a device, so AC1-5 stay unchecked. Known limit: while one command waits for the window, other sessions' sockets are read but their command and sftp pumps run only after it ends, so AC3 holds for socket service, not for concurrent command execution.

Device run 2026-11-16 on the work LAN (10.5.121.57:2222), after the user restarted the server: smoke.sh sftp half passed up to and including 'exec cat of 12 MB arrives byte-identical' (OpenSSH_10.6p1); 1.35 MB VPK exec cat byte-identical. Stalled reader (ssh cat 12 MB piped to sleep 45): a pwd probe 8 s in got no answer within 12 s (other sessions' pumps wait while a command waits on its client, as documented); probes from t+25 s onward answered, and the stalled session was gone by then. An earlier run, killed by my own 540 s cap mid-stall, left the server unresponsive for about 2 minutes before it recovered; the cause was not established (old build vs this build is unknown), so that hang is unexplained. Not exercised: a file larger than free memory, and server memory use during a transfer.
<!-- SECTION:NOTES:END -->
