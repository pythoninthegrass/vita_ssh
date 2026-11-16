---
id: TASK-001.07
title: Wire the SFTP subsystem so scp and sftp work against the Vita
status: To Do
assignee: []
created_date: '2026-11-16T04:30:00Z'
labels:
  - sftp
  - server
  - file-transfer
dependencies: []
references:
  - TASK-001.08
modified_files:
  - src/sshd_session.c
  - src/sshd_sftp.c
  - src/sshd_hostkey.c
  - CMakeLists.txt
parent_task_id: TASK-001
priority: medium
type: enhancement
ordinal: 19000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Getting a build onto the device today means USB mass storage via VitaShell, and the SSH session that installs files cannot receive one: sftp subsystem requests are rejected outright (on_subsystem_request says so and names this exact task), and legacy scp is no good either because it needs a remote scp binary to exec, which this platform cannot have (no fork/exec). So the SSH server can send anything (cat) and receive nothing.

The build already carries what is needed: libssh is compiled WITH_SFTP and exports the sftp_server_* API, and the server already drives everything else from its single poll loop. The work is implementing the SFTP protocol server-side over the shell's VFS (open/read/write/rename/remove/mkdir/readdir/stat), which also serves any future 'move files without VitaShell' want, not just deploys.

Second-order value: once scp works, a build lands in /ux0/downloads from any machine in one command, which is the input the install task (TASK-001.08) consumes.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 sftp put of a multi-megabyte VPK from OpenSSH lands byte-identical on the Vita (cmp against the source file), over WiFi, in one command.
- [ ] #2 sftp get, ls, mkdir, rm and rename work through the same session, against the device surface the shell already has (data dirs; read-only devices refuse writes with a clear error).
- [ ] #3 scp of a file to and from the Vita works (OpenSSH's default SFTP path), so no new client habits are needed.
- [ ] #4 A failed or interrupted transfer leaves no partial file with the target name (temp name then move, or equivalent).
- [ ] #5 Legacy scp (-O) fails with a clear message, not a hang: there is no remote scp binary to exec.
- [ ] #6 Two concurrent sessions can transfer at once without corrupting each other or stalling the poll loop beyond what the single-thread design already implies.
<!-- AC:END -->
