---
id: TASK-002.02
title: Override the SFTP opcodes the Vita cannot honour
status: To Do
assignee: []
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-15T22:57:00Z'
labels: []
dependencies:
  - TASK-002.01
documentation:
  - src/sshd_session.c
  - src/sh_path.c
modified_files:
  - src/sftp_server.c
parent_task_id: TASK-002
type: task
ordinal: 8000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Stock handlers answer opcodes the Vita cannot back, and the wrong answer here is worse than an error: a client that is told chmod/chown/utimes succeeded will believe permissions and timestamps it did not get, and scp's typical chmod-after-upload will either silently corrupt the user's expectations or fail the transfer depending on how it is answered. Vita's sceIo has no uid/gid model, no symlink support, and limited attribute control, and libssh's handlers also want to map numeric ids to user and group names, which this platform has no source of. The override mechanism is libssh's own callback surface for SFTP operations — the point is to override only the misbehaving handlers and leave the rest of the protocol to the library.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 chmod, chown and utimes answer with an honest status instead of a false success, and the choice for each is recorded with its reason
- [ ] #2 uid and gid are reported consistently for every file so clients stop trying to resolve names that do not exist
- [ ] #3 no handler reports a symlink for any path
- [ ] #4 clients that probe before acting (scp, sftp batch mode) complete their transfers rather than aborting on the unsupported calls
- [ ] #5 removing and renaming across devices behaves honestly: refused with a clear status, not silently copied
- [ ] #6 which opcodes are overridden, and why, is documented in the code and in AGENTS.md
<!-- AC:END -->
