---
id: TASK-040
title: Rebuild the SSH listener after the network drops and returns
status: In Progress
assignee: []
created_date: '2026-11-19T02:43:00Z'
updated_date: '2026-11-19T02:48:00Z'
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
- [ ] #1 After an accept failure, the server closes the dead listener, waits for the network to report connected, binds a new listener and resumes serving, without a restart.
- [ ] #2 A host test covers the rebuild schedule (first attempt immediate, backoff doubling to a cap, reset on a good accept, kept across a rebuild) over every short sequence of events against a model, under ASan/UBSan.
- [ ] #3 The address shown on the server's screen and logged as ready updates after a rebuild.
- [ ] #4 A rebuild is never run from inside the listener's own poll callback, and it does not disturb a session that is still healthy.
- [ ] #5 On a device: suspend on one network, wake on another, and a new ssh connection gets its banner and a shell with no restart; the log shows the rebuild.
- [ ] #6 AGENTS.md records the platform rule and the verified device result.
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
<!-- SECTION:NOTES:END -->
