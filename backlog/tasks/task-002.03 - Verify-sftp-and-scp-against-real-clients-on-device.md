---
id: TASK-002.03
title: Verify sftp and scp against real clients on device
status: To Do
assignee: []
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-15T22:57:00Z'
labels: []
dependencies:
  - TASK-002.01
  - TASK-002.02
documentation:
  - src/sshd_session.c
  - README.md
modified_files:
  - README.md
parent_task_id: TASK-002
type: task
ordinal: 9000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The SFTP path has one implementation and one set of assumptions behind it, and the client that actually exercises them is OpenSSH on a devbox. This is the evidence pass: byte-for-byte comparison both directions, the clients people really use, and the failures people actually hit. It also has to pin down the practical answer to 'how do I get my build onto the Vita', which is the reason the feature exists.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 scp of a file up and down matches sha256 in both directions, including a file over 10 MB
- [ ] #2 sftp recursive put and get reproduce a directory tree with the same file contents and sizes
- [ ] #3 operations on a read-only device and on a nonexistent path report a clear error to the client rather than an unhelpful status code
- [ ] #4 transfer throughput is recorded so a later regression is visible
- [ ] #5 the README documents a working copy-to-Vita and copy-from-Vita command line a user can paste
- [ ] #6 divergences between the macOS and Linux clients are recorded in the task notes
- [ ] #7 a multi-megabyte VPK put from OpenSSH lands byte-identical (cmp against the source) over WiFi in one command
- [ ] #8 legacy scp (-O) fails with a clear message instead of hanging
- [ ] #9 two concurrent sessions transfer at once without corrupting each other
<!-- AC:END -->
