---
id: TASK-002.03.01
title: Tell sftp clients when a write is refused because the device is read-only
status: To Do
assignee: []
created_date: '2026-11-16T18:19:00Z'
labels: []
dependencies: []
references:
  - scripts/smoke.sh
  - src/sftp_server.c
documentation:
  - AGENTS.md
parent_task_id: TASK-002.03
priority: low
type: bug
ordinal: 21000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A sftp or scp user who writes to a read-only device such as /os0 sees a bare 'Failure' (for example `dest open "/os0/smoke.txt": Failure`, `remote mkdir "/os0/smoke": Failure`, `remote delete /os0/smoke.txt: Failure`), with nothing saying why. The shell already says 'Read-only file system' for the same writes, and the server already refuses them before touching the device, so the cause is known and only the SFTP reply drops it. Missing paths already report clearly ('No such file or directory', 'not found'), so this is the one gap left in TASK-002.03 acceptance criterion 3. Found by the AC3 check in scripts/smoke.sh sftp, which currently fails for put, mkdir and rm on /os0.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 sftp put, mkdir and rm on a read-only device each print an error that names the cause (read-only or permission denied), not a bare 'Failure'
- [ ] #2 The same writes through scp -s report the same cause
- [ ] #3 Errors for a nonexistent path or a missing device keep their current clear messages
- [ ] #4 Renaming onto or within a read-only device keeps its current refusal and message
- [ ] #5 scripts/smoke.sh sftp passes the three '/os0' message checks against a device
- [ ] #6 AGENTS.md notes which status the server answers for a read-only refusal
<!-- AC:END -->
