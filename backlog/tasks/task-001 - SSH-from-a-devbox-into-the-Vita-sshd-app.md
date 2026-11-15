---
id: TASK-001
title: SSH from a devbox into the Vita (sshd app)
status: To Do
assignee: []
created_date: '2026-11-15T22:56:00Z'
labels: []
dependencies: []
references:
  - 'https://github.com/holzschu/a-shell'
  - 'https://github.com/theofficialflow/vitashell'
documentation:
  - src/sshd.c
  - src/sshd_session.c
  - src/sshd_builtins.c
  - src/sh_path.c
  - include/sshd.h
  - include/sh_path.h
  - AGENTS.md
modified_files:
  - src/sshd.c
  - src/sshd_session.c
  - src/sshd_builtins.c
  - src/sshd_hostkey.c
  - src/sh_path.c
  - src/vita_shims.c
  - include/sshd.h
  - include/sh_path.h
  - CMakeLists.txt
priority: high
type: feature
ordinal: 1000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The repo's client app is the less useful half: typing on the Vita's on-screen keyboard is not how anyone wants to work. The value is the other direction — `ssh vita` from a macOS or Linux devbox, landing in a shell that exposes the whole filesystem, so files can be pushed, pulled, listed and inspected from a real terminal. This is a busybox-style shell: command functions run in-process (the Vita has no fork/exec), a shell-owned cwd per session, and POSIX paths mapped onto sceIo devices. The server sources exist in src/sshd*.c, src/sh_path.c and src/vita_shims.c but no build target knows about them and two of the files do not compile.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 ssh <user>@<vita-ip> from a macOS devbox authenticates and lands at a shell prompt
- [ ] #2 the shell reaches every mounted device through POSIX paths (ls /, ls /ux0:, cd between devices)
- [ ] #3 task build produces the client VPK and the server VPK, and skips when nothing changed
- [ ] #4 with no authorized_keys installed, the server generates a password and shows it on the Vita screen
- [ ] #5 the client app is unaffected: same titleid, same behaviour as before this feature
<!-- AC:END -->
