---
id: TASK-041
title: Retry the listener rebuild as soon as the network address returns
status: To Do
assignee: []
created_date: '2026-11-19T22:25:00Z'
labels: []
dependencies:
  - TASK-040
references:
  - >-
    backlog/tasks/task-040 -
    Rebuild-the-SSH-listener-after-the-network-drops-and-returns.md
  - src/sh_listen.c
  - scripts/test_sh_listen.py
  - AGENTS.md
priority: medium
type: enhancement
ordinal: 76000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
TASK-040 rebuilds the SSH listener with a backoff of 0, then 500 ms doubling to a 30 s cap. While the network is down every retry fails with `network is not connected`, so the backoff climbs to the cap. When the address returns, the next retry can be up to 30 s away: in two of the three hotspot runs on the Vita (2026-11-19) the listener came back about 27 s after the address did, and a client that reconnects right away waits that long. Reset the backoff, and make a rebuild due now, when the address changes from none (`0.0.0.0`) to a usable one. The address is already re-read on the power tick and logged as `address changed: A -> B`. The schedule is the pure module `src/sh_listen.c`, host tested by `scripts/test_sh_listen.py` against a model; change the module and the model together. Device logs and addresses are in the device's sshd.log and CLAUDE.local.md, not here.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 When the displayed address changes from none to a usable address while the listener is down, the next rebuild attempt is due immediately and the backoff restarts from its first step.
- [ ] #2 The host test covers that event against the model over every short sequence, under ASan/UBSan, and the existing cases still pass.
- [ ] #3 A rebuild still waits for the network to report connected and still never runs from the listener's poll callback.
- [ ] #4 AGENTS.md's listener rule records the new behavior and the device result: after a network loss and return, a new ssh connection gets its banner within a few seconds of the address returning.
<!-- AC:END -->
