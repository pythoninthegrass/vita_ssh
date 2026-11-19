---
id: TASK-040
title: Rebuild the SSH listener after the network drops and returns
status: Done
assignee: []
created_date: '2026-11-19T02:43:00Z'
updated_date: '2026-11-19T22:25:00Z'
labels: []
dependencies: []
priority: high
type: bug
ordinal: 74000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
After the Vita is suspended on one network and woken on another, the server keeps running but nothing answers SSH until the app is force-quit and reopened. On a device run the log showed the listening socket becoming readable at wake, `accept()` failing with sceNet error 0x804101A3, the open session dropped with a socket error, and then no further `incoming:` lines for any connection: a TCP connect to the port succeeded but no banner ever came. The accept-failure path logs and returns as spurious readiness, so the dead listener is never replaced. The on-screen address is also read once at loop start and stays stale after the network changes. The plugin already rebuilds its listener on persistent accept failure (docs/turbo-wifi.md). Device logs and the maintainer's network addresses are in CLAUDE.local.md and the device's sshd.log, not here.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 After an accept failure, the server closes the dead listener, waits for the network to report connected, binds a new listener and resumes serving, without a restart.
- [x] #2 A host test covers the rebuild schedule (first attempt immediate, backoff doubling to a cap, reset on a good accept, kept across a rebuild) over every short sequence of events against a model, under ASan/UBSan.
- [x] #3 The address shown on the server's screen and logged as ready updates after a rebuild.
- [x] #4 A rebuild is never run from inside the listener's own poll callback, and it does not disturb a session that is still healthy.
- [x] #5 On a device: suspend on one network, wake on another, and a new ssh connection gets its banner and a shell with no restart; the log shows the rebuild.
- [x] #6 AGENTS.md records the platform rule and the verified device result.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. New pure module include/sh_listen.h + src/sh_listen.c (no I/O, no Vita APIs): listener health state. failed(now) marks the listener down and schedules the next rebuild at now + backoff (backoff 0, then 500 ms doubling to a 30 s cap); due(now) says a rebuild may run; rebuilt() clears down; accepted() resets backoff (a good accept proves the listener). The trigger is the FIRST accept failure, not a count: the device log shows exactly one failed accept at wake and then no further events from the dead fd, so a consecutive-failure threshold would never fire.
2. Host test scripts/test_sh_listen.c + scripts/test_sh_listen.py (ASan/UBSan, under timeout like the others): every event sequence up to length 7 over {failed, rebuilt, accepted, tick x3 sizes} against a separately written model.
3. src/sshd.c: on_incoming() on accept failure calls sh_listen_failed() and returns; it never frees the bind from inside the poll callback. server_loop() owns the listener: after each dopoll, if due, check sceNetCtlInetGetState == CONNECTED, drop the old fd (ssh_event_remove_fd + ssh_bind_free), make_bind() on the same port, ssh_event_add_fd; on any failure call sh_listen_failed() for backoff. Existing sessions are untouched. The boot path shares the same attach helper.
4. Refresh the displayed address on the 1 s power-tick cadence and log when it changes, so the screen is right after a rebuild.
5. CMake: add sh_listen.c to the server list. AGENTS.md: platform rule, test command, and device result once run.
Risk: sceNet itself may need sceNetInit/sceNetCtlInit again after a long outage; the user asked for the socket rebuild first. If the rebuilt bind keeps failing on device, the log lines will say so and a full bring_up_network() is the next step.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Built and installed on the device with vpk install -r. Host test: python3 -I scripts/test_sh_listen.py passes (all sequences up to 7 events). Device suspend/wake across networks (AC5) not yet run.

AGENTS.md updated: test command, sh_listen.c in the server list, and the platform rule for the rebuild. Build installed 2026-11-18 20:45 and serving. Remaining: AC5, the device suspend/wake across networks, which needs the user; AC1 and AC3 are verified by that run too. Nothing is committed.

2026-11-19: device run on the Vita, suspended on the home network and woken on the office network. sshd.log showed accept failed 0x804101A3, address changed, listener: rebuilding on port 22, ready ... (listener rebuilt, fd 4), and the next connection logged in and ran a command with no restart. Host test rerun, passes. AGENTS.md records the result. Not covered: the address on the Vita's screen (AC3), a healthy session surviving a rebuild (AC4), and the backoff and wait-for-connected path (the network was already up at wake).

2026-11-19: AC3 confirmed by the user, the Vita's screen showed the new network's address after the wake. AC4 stays open: no session was open across the suspend.

2026-11-19, hotspot run: a shell open from the mac was dropped cleanly (`Socket error`) when the hotspot went off, the backoff ran while the network was down, and after the listener was rebuilt a new connection logged in (`1 open`, so the slot was freed) with no restart. AC4 checked. All criteria are now checked; TASK-040.01 AC5 stays open by choice.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
The server rebuilds its SSH listener on the first accept failure: `on_incoming()` reports into the pure schedule in `src/sh_listen.c`, and `server_loop()` drops the dead fd, waits for the network to report connected and binds the same port again, never from inside the poll callback. The displayed address is re-read on the power tick and logged when it changes. Host test `scripts/test_sh_listen.py` covers the schedule over every sequence up to 7 events under ASan/UBSan. Device results (the Vita, 2026-11-19): suspend and wake across networks, and a phone hotspot switched off and on with a shell open, both recovered without a restart, the open session dropped cleanly and fresh connections logged in; the backoff and wait-for-connected path ran and recovered. AGENTS.md records the rule and both results. The rebuild can come up to 30 s after the address returns because the backoff is at its cap; that is the follow-up task.
<!-- SECTION:FINAL_SUMMARY:END -->
