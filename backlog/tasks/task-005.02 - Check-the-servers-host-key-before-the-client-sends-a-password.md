---
id: TASK-005.02
title: Check the server's host key before the client sends a password
status: To Do
assignee: []
created_date: '2026-11-16T00:00:00Z'
labels: []
dependencies: []
documentation:
  - src/ssh_client.c
  - src/sshd_hostkey.c
  - src/menu.c
parent_task_id: TASK-005
type: task
ordinal: 14000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
ssh_client.c does libssh2_session_handshake and goes straight to authentication. libssh2_knownhost_* is never called, so the client will hand a password to any host that completes the handshake, and the README cannot promise a secure connection on a network the user does not control. The sshd app already shows its SHA256 fingerprint on the Vita's screen, so a user has something to compare. The decisions that make this task non-trivial are where accepted keys are stored on the card and what the user sees when a known host's key changes.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 connecting to a host with no stored key shows the key type and SHA256 fingerprint and asks before authenticating
- [ ] #2 an accepted key is stored on the card and a later connection to the same host authenticates without asking
- [ ] #3 a changed key for a known host stops the connection with a clear reason, not a prompt that can be dismissed
- [ ] #4 declining an unknown key sends no password and leaves the app usable
- [ ] #5 task build produces both VPKs
<!-- AC:END -->
