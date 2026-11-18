---
id: TASK-038
title: Strip control bytes from the client's login name before the server prints it
status: To Do
assignee: []
created_date: '2026-11-18T09:20:00Z'
labels:
  - shell
  - security
dependencies: []
references:
  - src/sshd_session.c
  - src/sh_env.c
priority: low
type: bug
ordinal: 71000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The server accepts any login name (sh_auth_check_pubkey and sh_auth_check_password ignore it), and remember_user() in src/sshd_session.c copies it into the session's `user[64]` with only a length cap. Everything that prints it prints the raw bytes: the shell prompt, the connection banner, `whoami`, the `Connected from ... as <user>` line, and the console and sshd.log lines for logins. TASK-016.06 sanitized the copy that goes into the env table (sh_env_set drops bytes below 0x20 and 0x7F), so USER and LOGNAME are safe; the other consumers are not.

If a login name can carry an escape sequence, it can put one on the user's own terminal, on the Vita's console, and in sshd.log. Not yet checked: whether libssh passes such a name through to the auth callbacks or rejects it first (SSH protocol allows arbitrary UTF-8 in the user name, and OpenSSH's client will send what `-l` is given). Reproduce that before changing anything, and record the result in the task notes. If libssh already rejects it, close this task with that evidence.

Wanted: one place that cleans the name when it is remembered, so every consumer is covered, with the same rule sh_env_set uses (bytes below 0x20 and 0x7F dropped) and a decision about an empty result (the existing fallback is `vita`). Do not add a user list or any user-to-privilege mapping: the Vita has no ownership model, and USER is only the name the client logged in with.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Whether libssh delivers a login name containing ESC or other control bytes to the auth callbacks is determined on a device and recorded in the task notes
- [ ] #2 A login name containing control bytes never reaches the prompt, banner, whoami output, console or sshd.log with those bytes intact
- [ ] #3 A login name that is empty after cleaning falls back to the existing default and the session still works
- [ ] #4 Ordinary names (any, root, alice, names with dots, dashes and UTF-8 letters) are shown unchanged
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device with a login name that carries an escape sequence
<!-- DOD:END -->
