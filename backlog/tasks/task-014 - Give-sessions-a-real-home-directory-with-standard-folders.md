---
id: TASK-014
title: Give sessions a real home directory with standard folders
status: To Do
assignee: []
created_date: '2026-11-17 03:28'
labels:
  - shell
  - storage
  - path-layer
dependencies: []
documentation:
  - AGENTS.md
modified_files:
  - src/sshd_hostkey.c
  - src/sshd_session.c
  - include/sshd.h
priority: high
ordinal: 28000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Today a session's $HOME and its initial cwd are both what sh_server_home_posix() returns: the server's own data directory (ux0:data/vitasshd), the one holding the host key, authorized_keys and sshd.log. So ~ behaves like ~/.ssh on unix -- there is nowhere obvious to put a download, and a user's files share a directory with the server's private material.

Wanted: $HOME is a normal user home on writable storage; the server's SSH files live under $HOME/.ssh the way they do on unix; boot creates the standard folders so a fresh device can cd ~/Downloads without mkdir first. This is the foundation the curl download task needs somewhere to put files.

Decisions already made, do not re-litigate: the SSH files move rather than being duplicated; the host key must keep its fingerprint across an upgrade so existing known_hosts entries stay valid; sshd.log stays with the server rather than in the user's home; the Vita has no symlinks (platform rule), so the standard folders are real directories created at boot, not links.

Related: TASK-013.01 (~/.ssh/authorized_keys for ssh-copy-id) should land on this task's location rather than inventing its own.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A new session starts in $HOME: pwd prints it and bare cd lands there.
- [ ] #2 $HOME is a user-facing directory on writable storage, separate from the directory holding sshd.log.
- [ ] #3 The host key and authorized_keys are read from $HOME/.ssh, and the console reports the paths it actually opened.
- [ ] #4 A device that already has a host key or authorized_keys in the old server directory keeps working after an upgrade: the file is moved into $HOME/.ssh once at boot and the host key fingerprint is unchanged.
- [ ] #5 ~/Downloads, ~/Documents and ~/Pictures exist after the server boots, on a device that never had them, with no error when they already exist.
- [ ] #6 ls ~, cd ~/Downloads and cd ../Documents resolve through the path layer in POSIX spelling, and SFTP sees the same directories.
- [ ] #7 A session whose home device is unmounted or absent still starts, landing somewhere usable, and says so on the console instead of failing the connection.
- [ ] #8 env prints HOME as the new home directory, and the prompt still agrees with it.
- [ ] #9 The host key directory search rules in AGENTS.md match where the files actually live now.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device, including the upgrade case where the host key moves and the fingerprint does not change
<!-- DOD:END -->
