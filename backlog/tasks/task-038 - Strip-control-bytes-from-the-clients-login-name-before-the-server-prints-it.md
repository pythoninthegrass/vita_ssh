---
id: TASK-038
title: Strip control bytes from the client's login name before the server prints it
status: Done
assignee: []
created_date: '2026-11-18T09:20:00Z'
updated_date: '2026-11-18T21:58:00Z'
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
- [x] #1 Whether libssh delivers a login name containing ESC or other control bytes to the auth callbacks is determined on a device and recorded in the task notes
- [x] #2 A login name containing control bytes never reaches the prompt, banner, whoami output, console or sshd.log with those bytes intact
- [x] #3 A login name that is empty after cleaning falls back to the existing default and the session still works
- [x] #4 Ordinary names (any, root, alice, names with dots, dashes and UTF-8 letters) are shown unchanged
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device with a login name that carries an escape sequence
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Add a pure helper in src/sh_env.c / include/sh_env.h that copies a string into a bounded buffer dropping bytes below 0x20 and 0x7F (the sh_env_set rule). 2. TDD: extend scripts/test_sh_env.py first, see it fail, then implement. 3. remember_user() uses it, so s->user is clean for every consumer; an empty result keeps the existing `vita` fallback. 4. The three auth log lines print the raw `user` argument, so they log the cleaned name instead. 5. task build, then device check with paramiko probe (escape name, empty-after-clean name, ordinary and UTF-8 names)."
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Reproduced 2026-11-18 on the Vita (office network), pre-change build. OpenSSH's client refuses a control-byte login name locally (`remote username contains invalid characters`), so the probe used paramiko (scratchpad script). libssh delivers the name to the auth callbacks intact: login with name `zz<ESC>[31mRED<ESC>[0m` succeeded and `whoami` returned those raw bytes (ESC intact). AC #1 satisfied; the bug is real.

Implemented (uncommitted): sh_env_strip_ctrl() in src/sh_env.c with host tests in scripts/test_sh_env.c (pass under ASan/UBSan); remember_user() cleans the name, so the prompt, banner, whoami and the Connected-from line read the clean copy; the six auth log lines use loggable_user(). An empty result leaves s->user empty, so sh_session_user() returns `vita`. task build produces all three VPKs. Device verification (AC #2-#4, DoD #2) is pending an install of the new server build.

Device run 2026-11-18 after `scp -s` + `vpk install -r` (result.txt: promote package 0x00000000), paramiko probe: `zz<ESC>[31mRED<ESC>[0m` -> whoami `zz[31mRED[0m`; `<ESC><BEL>` (empty after cleaning) -> `vita`, session works; `any`, `root`, `al.ice-1`, `café` unchanged. sshd.log lines for the new logins carry no ESC (`zz[31mRED[0m@...`, `?@...` for the empty one); the only ESC lines are the pre-fix repro at 15:21-15:22. The prompt, banner and Connected-from line read the same s->user as whoami; they were not captured separately.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
remember_user() now stores the login name through the new sh_env_strip_ctrl() (bytes below 0x20 and 0x7F dropped, same rule as sh_env_set), so every consumer of s->user is covered; an empty result keeps the `vita` fallback. The six auth log lines print the cleaned name via loggable_user(). libssh does deliver control bytes to the auth callbacks (reproduced with paramiko; OpenSSH's client refuses to send them). Host tests in scripts/test_sh_env.c; device-verified.
<!-- SECTION:FINAL_SUMMARY:END -->
