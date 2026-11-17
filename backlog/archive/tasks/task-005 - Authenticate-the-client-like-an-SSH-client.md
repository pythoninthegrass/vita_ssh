---
id: TASK-005
title: Authenticate the client like an SSH client
status: To Do
assignee: []
created_date: '2026-11-16T00:00:00Z'
updated_date: '2026-11-17 03:14'
labels: []
dependencies: []
documentation:
  - src/ssh_client.c
  - src/main.c
  - src/menu.c
  - include/display.h
ordinal: 12000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The client has one credential and no trust model. libssh2_userauth_password is the only auth call in src/ssh_client.c, so every connection needs a password typed through the IME and stored in the clear in ux0:data/vitassh/profiles.dat, and libssh2's knownhost API is never touched, so a password goes out to whatever machine answered on the port. Both halves matter: keys take the secret off the memory card, host verification makes 'secure shell' mean something on a network you do not own. The sshd app already prints its SHA256 fingerprint on the Vita's screen, so there is something real for a user to compare against.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 the client can authenticate with a private key with no password typed or stored
- [ ] #2 the client will not send a password to a host key it has not accepted
- [ ] #3 password authentication against an ordinary server still works unchanged
- [ ] #4 task build produces both VPKs
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-16: Archived at Lance's direction along with both subtasks. The client is not being developed further; the sshd server is the project's focus. What remains true of the client and untracked: password auth only, no host key verification.
<!-- SECTION:NOTES:END -->
