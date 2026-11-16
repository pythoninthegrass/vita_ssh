---
id: TASK-002.03.03
title: Establish a repeatable SFTP throughput baseline and explain the stalls
status: To Do
assignee: []
created_date: '2026-11-16T18:24:00Z'
labels: []
dependencies: []
references:
  - scripts/smoke.sh
  - src/sftp_server.c
documentation:
  - README.md
  - AGENTS.md
parent_task_id: TASK-002.03
priority: low
type: task
ordinal: 23000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
scripts/smoke.sh sftp prints THROUGHPUT lines, but WiFi runs vary too much to use as a regression baseline: across runs on 2026-11-16 a 12 MB scp upload ran 0.53 to 1.14 MB/s and a 12 MB scp download 0.25 to 1.03 MB/s, and one sftp get took 111 s (0.11 MB/s) with 12 to 17 s gaps between 32-request log markers where about 1 s is normal. The cause of the stalls was not established; the transfer code was identical to a build that ran at 0.95 MB/s. Without a stable number, a later slowdown cannot be told from WiFi noise, which is what TASK-002.03 acceptance criterion 4 is meant to prevent. Reading is also done in 32 KB requests, so whether the card or the SSH crypto limits throughput is unknown.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A documented repeatable procedure produces a throughput figure for each of scp up, scp down, sftp put and sftp get, as a median and range over several runs
- [ ] #2 The conditions are recorded with the numbers: client, link, distance or channel, and the server build
- [ ] #3 The cause of a stall such as the 111 s get is identified, or recorded as a network effect with the evidence
- [ ] #4 Whether the memory card read speed or the SSH encryption limits throughput is stated with a measurement
- [ ] #5 The recorded baseline and the procedure are written down where a later run can compare against them
- [ ] #6 scripts/smoke.sh keeps printing parseable THROUGHPUT lines
<!-- AC:END -->
