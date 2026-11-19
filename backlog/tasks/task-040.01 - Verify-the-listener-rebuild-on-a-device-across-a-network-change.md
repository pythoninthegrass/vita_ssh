---
id: TASK-040.01
title: Verify the listener rebuild on a device across a network change
status: Done
assignee: []
created_date: '2026-11-19T02:50:00Z'
updated_date: '2026-11-19T22:25:00Z'
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
- [x] #3 The address on the Vita's screen shows the new network's address after the wake, and the log has an `address changed: A -> B` line.
- [x] #4 A session that was open before the suspend is either dropped cleanly or still works; it does not block the new connection.
- [ ] #5 If the rebuilt bind fails or the log shows repeated `could not bind` or `network is not connected` retries that never recover, the outcome and log lines are recorded and the follow-up (running bring_up_network() again) is raised with the user.
- [x] #6 TASK-040's criteria 1, 3, 5 and 6 are checked, and AGENTS.md's listener rule records the verified device result with the date.
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-19: AC1 and AC2 verified on the Vita (home to office). Log sequence: accept failed 0x804101A3, address changed A -> B, listener: rebuilding on port 22, ready: ssh any@<ip> -p 22 (listener rebuilt, fd 4); a new connection then logged in and ran a command. Open: AC3 needs the on-screen address read, AC4 had no session open across the suspend, AC5 did not occur, AC6 waits on TASK-040 AC3 and AC4.

AC5 stays unchecked on purpose: the failure it covers (the rebuilt bind failing, or repeated `could not bind` / `network is not connected` retries) is intermittent, and the 2026-11-19 run did not hit it. The network was already up at wake, so the backoff and the wait for `sceNetCtlInetGetState` have not run on a device. A phone hotspot with auto-join turned off is a way to force the no-network-at-wake case.

2026-11-19: AC3 confirmed by the user, the Vita's screen showed the new network's address after the wake, and the log has the `address changed: A -> B` line.

2026-11-19, hotspot run, from the log. Run 1: hotspot turned off with a shell open from the mac. `accept failed 0x804101A3`, the open session dropped with a socket error, the address went to 0.0.0.0, and `listener: network is not connected; will retry` repeated at 0, 1, 2, 4 and 8 s. The Vita rejoined the office network, the address changed, and the listener was rebuilt (`ready ... (listener rebuilt, fd 4)`). This is the first device run of the backoff and wait-for-connected path, and it recovered. No new connection was made afterward, so AC4's 'does not block the new connection' is not shown. Run 2: office to hotspot. The same sequence; the rebuild came about 27 s after the address returned because the backoff had reached its 30 s cap, and the log says `listener rebuilt`. The user's one connection attempt in that window came from the mac while it was not on the hotspot, so it says nothing about the listener; the user then restarted the app by mistake. Not yet shown: a new connection through a rebuilt listener after a network loss with no restart.

2026-11-19, third hotspot run (hotspot off with a shell open, then on, no app restart): the session dropped cleanly, `network is not connected; will retry` repeated at 1, 2, 4 and 8 s and then at the 30 s cap while the network was down, the address returned, the listener was rebuilt about 27 s later, and the next connections logged in with `1 open` (the dead session's slot was freed) and again with `2 open`. This supersedes the 'not yet shown' line above. AC4 and AC6 checked. AC5 stays unchecked: the failure it covers (a bind that never recovers) did not occur, because the rebuild recovered each time.

Closed 2026-11-19 with AC5 unchecked on purpose. AC5 covers a rebuilt bind that never recovers; it is intermittent and did not occur in any of the three runs (suspend and wake across networks, and two hotspot off/on runs), where every rebuild recovered. The device-run criteria of TASK-040 are checked, AGENTS.md records the results, and the 27 s rebuild delay seen after the address returns is tracked as a separate follow-up task.
<!-- SECTION:NOTES:END -->
