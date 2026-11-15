---
id: TASK-001.03
title: Keep the host key and authorized_keys on writable storage
status: To Do
assignee: []
created_date: '2026-11-15T22:56:00Z'
labels: []
dependencies: []
documentation:
  - src/sshd_hostkey.c
  - README.md
modified_files:
  - src/sshd_hostkey.c
parent_task_id: TASK-001
type: task
ordinal: 4000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The server generates an Ed25519 host key on first run and looks for an authorized_keys file, but the paths it uses were chosen without checking which Vita partitions a homebrew app can actually write — ux0 is read-only on retail firmware, so a key saved there never persists and every reboot hands the user a new fingerprint and a known_hosts mismatch. The user's public key has to get onto the device somehow, so where it is looked for has to be a place they can actually put it. The failure mode to preserve is graceful: a key that cannot be saved must still leave a working server, which is why the key is imported into the bind rather than read from a path.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 the server starts on retail firmware where the app's ux0 is read-only, generating an in-memory host key without erroring
- [ ] #2 when a writable location is available, the generated host key is saved there and the same fingerprint is presented after a reboot
- [ ] #3 a host key that cannot be persisted still yields a working server and a visible fingerprint on the Vita screen
- [ ] #4 authorized_keys is read from a documented path the user can place a file in, and a malformed or missing file falls back to password auth rather than refusing all logins
- [ ] #5 the chosen paths and the search order are documented where a user will read them
- [ ] #6 the known_hosts mismatch a user sees after a key change is explained in the docs
<!-- AC:END -->
