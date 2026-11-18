---
id: TASK-014
title: Give sessions a real home directory with standard folders
status: In Progress
assignee:
  - claude
created_date: '2026-11-17 03:28'
updated_date: '2026-11-18T23:19:00Z'
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
- [x] #1 A new session starts in $HOME: pwd prints it and bare cd lands there.
- [x] #2 $HOME is a user-facing directory on writable storage, separate from the directory holding sshd.log.
- [x] #3 The host key and authorized_keys are read from $HOME/.ssh, and the console reports the paths it actually opened.
- [x] #4 A device that already has a host key or authorized_keys in the old server directory keeps working after an upgrade: the file is moved into $HOME/.ssh once at boot and the host key fingerprint is unchanged.
- [x] #5 ~/Downloads, ~/Documents and ~/Pictures exist after the server boots, on a device that never had them, with no error when they already exist.
- [x] #6 ls ~, cd ~/Downloads and cd ../Documents resolve through the path layer in POSIX spelling, and SFTP sees the same directories.
- [ ] #7 A session whose home device is unmounted or absent still starts, landing somewhere usable, and says so on the console instead of failing the connection.
- [x] #8 env prints HOME as the new home directory, and the prompt still agrees with it.
- [x] #9 The host key directory search rules in AGENTS.md match where the files actually live now.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [ ] #2 Verified on a device, including the upgrade case where the host key moves and the fingerprint does not change
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Decision (Lance, 2026-11-18): $HOME is `<device>:/data/home`; the SSH files move to `<device>:/data/home/.ssh`; sshd.log stays in `<device>:/data/vitasshd`.

Design, all in src/sshd_hostkey.c (data_dirs[] keeps its device order ux0, uma0, xmc0, ur0; each entry gains `home` and `ssh` paths beside the existing server data dir):
1. ensure_dir() creates every level, not just one parent, since `data/home/.ssh` is two below `data`.
2. Boot migration, before the host key search: per candidate, move ssh_host_ed25519_key, its .pub and authorized_keys from the old server dir into `<home>/.ssh` when the destination lacks them. Rename on one device, so the bytes and fingerprint cannot change. One console line per move; a failed move is logged and not fatal.
3. The three search rules look in `<home>/.ssh` first and the old server dir second, so a move that failed (or a card from before the upgrade) still finds the same key. Without this, a failed move would generate a new host key and change the fingerprint. New keys are only ever saved to `<home>/.ssh`.
4. choose_home() is decoupled from the key directory: the first candidate whose home dir can be created, then ~/.ssh, ~/Downloads, ~/Documents, ~/Pictures are created (EEXIST is fine). If no candidate works, fall back to the existing logic (an existing server dir, else /ux0) and log that home was unavailable.
5. key_path / keys_path report the paths actually opened; update the comments in sshd.h and sshd_session.c that call home the server data dir.
6. Docs: AGENTS.md (search rules, if mentioned), README "Where its files live" and the `ux0:data/vitasshd` mentions for authorized_keys, docs/development.md if it names the key location.

Verification: no host test seam exists for this file (needs libssh and sceIo), so `task build` for both VPKs, then a device run: fresh-boot folders, upgrade from a device holding keys in the old dir with the fingerprint compared before and after, ls ~, cd ~/Downloads, SFTP listing, env HOME.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Device run, the Vita on the office network, 2026-11-18, build installed with vpk install -r. Before: key in ux0:/data/vitasshd, fingerprint SHA256:WruC7i/zfLV7z3YVjuP5WfZ194xfEXfh2d7QhGdY9y8. After: the same fingerprint from ~/.ssh/ssh_host_ed25519_key.pub; sshd.log shows 'home: moved ... to ux0:/data/home/.ssh/...' for the .pub and authorized_keys, 'authorized_keys: 2 key(s) from ux0:/data/home/.ssh/authorized_keys' and 'home: ux0:/data/home'. pwd is /ux0/data/home, ls -a ~ shows .ssh Documents Downloads Pictures (none existed before), env HOME=/ux0/data/home, sftp ls /ux0/data/home lists the three folders, cd ~/Downloads and ls ~/Downloads/../Documents succeed, /ux0/data/vitasshd keeps only sshd.log and sshd.log.old.

Not verified on a device: AC #7 (home device absent), because the card cannot be unmounted while the server runs; the fallback is the old choose_home() path plus a console line. DoD #2 stays open for the same reason only. The host key's own 'moved' line was not read from the log; the fingerprint match shows it is the same file. No host test exists for sshd_hostkey.c (needs libssh and sceIo).
<!-- SECTION:NOTES:END -->
