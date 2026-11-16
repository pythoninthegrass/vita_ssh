---
id: TASK-005.02
title: Check the server's host key before the client sends a password
status: To Do
assignee: []
created_date: '2026-11-16T00:00:00Z'
updated_date: '2026-11-16T02:20:00Z'
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

## Status Updates
<!-- STATUS:BEGIN -->
### Update: [2026-11-16 02:20:00 UTC]
- Confirmed on hardware, and worse than the description puts it. The client's first real connection — PCH-2001
  to `sshd` on a MacBook over Wi-Fi — completed the handshake and sent the password with the peer's identity
  never examined. `ssh_connect()` (`src/ssh_client.c:101`) goes `libssh2_session_handshake` (`:148`) straight
  to `libssh2_userauth_password` (`:160`). No call to `libssh2_session_hostkey`, `libssh2_hostkey_hash` or any
  `libssh2_knownhost_*` exists anywhere in `src/`. Anything on that network able to answer port 22, or to
  redirect it, is handed the password.
- That connection therefore evidences nothing for #1–#4: the client has never asked libssh2 for a server key,
  so it has not been shown able to receive one, let alone display or store it. Those criteria are untested,
  not failing.
- No new library work is needed. The bundled libssh2 is 1.11.1 and the installed headers already expose
  `libssh2_hostkey_hash()` and `libssh2_session_hostkey()` (`libssh2.h:684`, `:687`) plus the whole
  `libssh2_knownhost_init/add/addc/check/checkp/del` family (from `:1104`). What is missing is the prompt, the
  store and the changed-key stop — not the API.
- #2 has a precedent to copy rather than invent. `menu.c` already persists to
  `ux0:data/vitassh/profiles.dat` via `sceIoOpen` (`:248`, `:271`), so `ux0:data/vitassh/known_hosts` is the
  neighbouring path with a code path that is known to be reached. Two caveats the server already had to solve:
  on this PCH-2001 `ux0:` is not guaranteed present or writable, which is why `sshd_hostkey.c` walks an ordered
  candidate table instead of trusting one path; and that table is not reachable from the client — `network.c`
  is the only source in both `add_executable` lists — so reusing it means either duplicating the list or
  extracting it into a shared source added to both. That choice is this task's, not the server's.
- The material to compare against already exists on the server side: `sh_auth_prepare()` records a SHA256
  fingerprint and the console shows it, so a user has something to read off the Vita's screen. It is of no use
  against a Vita server yet: `ssh_server_vita` still has never drawn a frame, so #1 and #3 cannot be exercised
  end to end there. A devbox `sshd` can drive #1, #3 and #4 in the meantime, and that work does not wait on the
  server's launch failure.
- Until #1–#4 pass, no claim of a secure connection may appear in the docs: while password auth is offered to
  an unverified peer, the connection is authenticated in one direction only. Relevant to TASK-004.
<!-- STATUS:END -->
