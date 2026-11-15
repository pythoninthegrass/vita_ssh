---
id: TASK-002
title: Serve SFTP from the Vita sshd app
status: To Do
assignee: []
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-15T22:57:00Z'
labels: []
dependencies:
  - TASK-001.01
  - TASK-001.02
references:
  - 'https://api.libssh.org/stable/group__libssh__sftp.html'
documentation:
  - src/sshd_session.c
  - src/sh_path.c
  - include/sh_path.h
modified_files:
  - src/sshd_session.c
  - src/sftp_server.c
  - CMakeLists.txt
priority: high
type: feature
ordinal: 6000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
An interactive shell is half the workflow; moving files in and out is the other half, and doing it over SSH means SFTP so ordinary tooling (sftp, scp, and the SFTP-mounted filesystems people already use from a devbox) works without a second protocol or a custom helper. The subsystem is currently refused at the channel callback. The intent is to use libssh's stock SFTP server handlers rather than reimplementing the protocol, and to override only the opcodes that misbehave on Vita, so the protocol work stays upstream's. All file access must go through the same POSIX-to-sceIo translation layer the shell uses — one place decides what a path means.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 sftp <user>@<vita-ip> from a devbox opens a session against the running server
- [ ] #2 directory listing, download, upload, mkdir, rmdir, rm and rename work through sftp
- [ ] #3 every byte of file access goes through the shared path layer, so devices and permissions behave exactly as they do in the shell
- [ ] #4 operations the Vita genuinely cannot perform fail with a correct SFTP status instead of hanging or reporting success
- [ ] #5 the shell and sftp can be used at the same time against the same server
<!-- AC:END -->
