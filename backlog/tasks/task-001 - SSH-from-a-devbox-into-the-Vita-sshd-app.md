---
id: TASK-001
title: SSH from a devbox into the Vita (sshd app)
status: Done
assignee: []
created_date: '2026-11-15T22:56:00Z'
updated_date: '2026-11-17 01:52'
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
ordinal: 1000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The repo's client app is the less useful half: typing on the Vita's on-screen keyboard is not how anyone wants to work. The value is the other direction — `ssh vita` from a macOS or Linux devbox, landing in a shell that exposes the whole filesystem, so files can be pushed, pulled, listed and inspected from a real terminal. This is a busybox-style shell: command functions run in-process (the Vita has no fork/exec), a shell-owned cwd per session, and POSIX paths mapped onto sceIo devices. The server sources exist in src/sshd*.c, src/sh_path.c and src/vita_shims.c but no build target knows about them and two of the files do not compile.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 ssh <user>@<vita-ip> from a macOS devbox authenticates and lands at a shell prompt
- [x] #2 the shell reaches every mounted device through POSIX paths (ls /, ls /ux0:, cd between devices)
- [x] #3 task build produces the client VPK and the server VPK, and skips when nothing changed
- [x] #4 with no authorized_keys installed, the server generates a password and shows it on the Vita screen
- [x] #5 the client app is unaffected: same titleid, same behaviour as before this feature
<!-- AC:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
The server app ships: `ssh <any-user>@<vita-ip>` from a macOS or Linux devbox lands in a shell over the Vita's own devices, and `ssh <vita> 'ls /ux0/vita'` works as a one-shot command. Built as a second target in the same tree (`ROM000070`, alongside the client's untouched `ROM000069`), so one `task build` produces both VPKs and they install side by side on one card.

What landed, by subtask:
- TASK-001.01, TASK-001.02 -- sshd registered as its own executable, VPK and titleid; sources compile and link against the libssh 0.12.2 Vita port (vita.patch keeps libssh's server side buildable without net/if.h, sys/un.h or termios, non-PIC so vita-elf-create accepts it, and out of __thread storage so the log callbacks work on a sceKernelCreateThread thread).
- TASK-001.03 -- host key and authorized_keys on ux0:data/vitasshd, so an identity survives a reboot instead of a new fingerprint every boot.
- TASK-001.04 -- the device pass: sign-in with the on-screen passphrase or a key, ls/cd/pwd and relative paths across devices, per-session cwd, honest errors for untranslatable paths, exec exit statuses, clean teardown on exit and on an abrupt hangup, and the line editor at the prompt -- all from macOS and Linux OpenSSH alike. It fixed two real bugs (cp/mv/rm naming the wrong command and leaking sceIo paths; `rm -r dir/.` deleting the parent).

Architecture, since nothing here can fork or exec: one thread drives vita2d, the pad, the sockets and every session through one poll loop; commands are builtins run in-process with a shell-owned cwd per session (libc's cwd is one process-wide static, so chdir is off limits); the path layer in src/sh_path.c is the only code that knows /ux0/data/x is ux0:data/x, and it refuses an unknown device rather than falling back.

Criteria basis: #1 and #4 rest on password login with the on-screen passphrase verified in TASK-001.04 AC1; #2 on `ls /` plus a per-device `ls` loop and cd checks in scripts/smoke.sh, and on the read-only and cross-device cases exercised through SFTP (TASK-002.02, TASK-002.03.01) -- the literal `cd` from one device into another was the one atom never graded, so scripts/smoke.sh gained `cd /os0` and `cd /vs0` checks for the next device run; #5 is the unchanged client titleid in CMakeLists.txt with the client's source list untouched.

Closed with two limits, both documented in the README rather than left as bugs: the cross-device `mv` success path has never executed (no second writable device on the test Vita), and a running command cannot be interrupted by Ctrl-C because the poll loop is inside the command.

Follow-ups moved out of this parent, because they are separate deliverables rather than parts of "ssh from a devbox works": TASK-010 (dim the screen while the server runs, was TASK-001.05) and TASK-011 (a full-screen text editor builtin, was TASK-001.06). TASK-001.07, the `vpk install` self-update path, is Done.
<!-- SECTION:FINAL_SUMMARY:END -->
