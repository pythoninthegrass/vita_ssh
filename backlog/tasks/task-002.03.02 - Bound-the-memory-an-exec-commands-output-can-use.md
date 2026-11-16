---
id: TASK-002.03.02
title: Bound the memory an exec command's output can use
status: To Do
assignee: []
created_date: '2026-11-16T18:24:00Z'
labels: []
dependencies: []
references:
  - src/sshd_session.c
  - src/sshd_builtins.c
  - scripts/smoke.sh
documentation:
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
