---
id: TASK-040.01
title: Verify the listener rebuild on a device across a network change
status: To Do
assignee: []
created_date: '2026-11-19T02:50:00Z'
updated_date: '2026-11-19T21:58:00Z'
labels: []
dependencies: []
references:
  - >-
    backlog/tasks/task-040 -
    Rebuild-the-SSH-listener-after-the-network-drops-and-returns.md
  - AGENTS.md
  - CLAUDE.local.md
parent_task_id: TASK-040
priority: high
type: task
ordinal: 75000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
TASK-040 added a rebuild of the SSH listening socket after an accept failure, plus a periodic refresh of the displayed address. It builds, installs and passes its host test, but the failure it fixes only happens when the Vita is suspended on one network and woken on another, so it has not been exercised. The device is used on two networks (the home and the office, addresses in CLAUDE.local.md); the user will test it in normal use when moving between them. The device-run criteria of TASK-040 (1, 3, 5) and its AGENTS.md result (6) close on this task. Install the build with the server's own `vpk install -r` over ssh first if the device is not already running it. Use the system `/usr/bin/ssh`, not Homebrew's.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 After the device is suspended on one network and woken on the other, a new ssh connection gets its banner and a shell with no restart of the app.
- [x] #2 sshd.log from the wake onward shows the `accept failed` line, then `listener: rebuilding on port`, then a `ready: ssh any@<ip>` line with the new address and `listener rebuilt`.
- [ ] #3 The address on the Vita's screen shows the new network's address after the wake, and the log has an `address changed: A -> B` line.
- [ ] #4 A session that was open before the suspend is either dropped cleanly or still works; it does not block the new connection.
- [ ] #5 If the rebuilt bind fails or the log shows repeated `could not bind` or `network is not connected` retries that never recover, the outcome and log lines are recorded and the follow-up (running bring_up_network() again) is raised with the user.
- [ ] #6 TASK-040's criteria 1, 3, 5 and 6 are checked, and AGENTS.md's listener rule records the verified device result with the date.
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-19: AC1 and AC2 verified on the Vita (home to office). Log sequence: accept failed 0x804101A3, address changed A -> B, listener: rebuilding on port 22, ready: ssh any@<ip> -p 22 (listener rebuilt, fd 4); a new connection then logged in and ran a command. Open: AC3 needs the on-screen address read, AC4 had no session open across the suspend, AC5 did not occur, AC6 waits on TASK-040 AC3 and AC4.

AC5 stays unchecked on purpose: the failure it covers (the rebuilt bind failing, or repeated `could not bind` / `network is not connected` retries) is intermittent, and the 2026-11-19 run did not hit it. The network was already up at wake, so the backoff and the wait for `sceNetCtlInetGetState` have not run on a device. A phone hotspot with auto-join turned off is a way to force the no-network-at-wake case.
<!-- SECTION:NOTES:END -->
