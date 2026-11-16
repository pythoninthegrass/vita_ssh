---
id: TASK-005.01
title: Log in from the client with a private key
status: To Do
assignee: []
created_date: '2026-11-16T00:00:00Z'
labels: []
dependencies: []
documentation:
  - src/ssh_client.c
  - src/menu.c
  - src/input.c
  - include/display.h
  - taskfiles/setup.yml
parent_task_id: TASK-005
type: task
ordinal: 13000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
libssh2_userauth_password is the only authentication the client performs, so a connection always needs a password typed on the on-screen keyboard and kept in plaintext in ux0:data/vitassh/profiles.dat next to the host and username. libssh2 can authenticate from a key instead; the work is where the private key lives on the device, how a profile says to use it, and how a passphrase reaches the user through the same IME path the password uses. The Vita has no ~/.ssh to scan and no terminal to answer a prompt in, and the only OpenSSL available is the 1.1.1 port task setup installs, which bounds the key formats that can be expected to load.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 a profile that names a private key authenticates against a server with password authentication disabled
- [ ] #2 key material is read from the device filesystem and never written to profiles.dat
- [ ] #3 an encrypted key prompts for its passphrase through the IME in password mode, and a wrong passphrase reports a failure instead of hanging
- [ ] #4 an unreadable or unsupported key names the key and the reason, and a profile that also has a password can still fall back to it
- [ ] #5 password-only profiles connect exactly as they do today
- [ ] #6 task build produces both VPKs
<!-- AC:END -->
