---
id: TASK-005.01
title: Log in from the client with a private key
status: To Do
assignee: []
created_date: '2026-11-16T00:00:00Z'
updated_date: '2026-11-17 03:14'
labels: []
dependencies: []
documentation:
  - src/ssh_client.c
  - src/menu.c
  - src/input.c
  - include/display.h
  - taskfiles/setup.yml
parent_task_id: TASK-005
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

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-16: Returned to To Do and unassigned before any code was written. Lance is not taking client-side work right now — the sshd server only — so this task is not being executed. Nothing under src/ changed. The gap it was meant to close (a key reaching the Vita, and a public key reaching a server) is a server-side problem anyway: ssh-copy-id still does not work against sshd (README: it sends exec sh -c with ; and &&), so installing a key means VitaShell or USB. Tracked separately as server work.

2026-11-16: Archived at Lance's direction. Client-side authentication is not on the roadmap; the server is the project's focus. No code was written for this task.
<!-- SECTION:NOTES:END -->
