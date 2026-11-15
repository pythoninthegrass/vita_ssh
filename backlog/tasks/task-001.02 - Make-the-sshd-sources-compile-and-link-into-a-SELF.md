---
id: TASK-001.02
title: Make the sshd sources compile and link into a SELF
status: To Do
assignee: []
created_date: '2026-11-15T22:56:00Z'
labels: []
dependencies: []
documentation:
  - src/sshd.c
  - src/sshd_session.c
  - include/sshd.h
  - src/sshd_hostkey.c
  - AGENTS.md
modified_files:
  - src/sshd.c
  - src/sshd_session.c
  - include/sshd.h
  - src/sshd_hostkey.c
parent_task_id: TASK-001
priority: high
type: bug
ordinal: 3000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The server sources were written against libssh 0.12.2's API from documentation and were never compiled. Three files (sh_path.c, sshd_builtins.c, vita_shims.c) already compile clean under arm-vita-eabi-gcc -Wall; sshd.c and sshd_session.c do not. Known breakage: sshd.c includes psp2/kernel/thread.h, which does not exist under that name in this SDK; sshd_session.c is missing the header that declares the server-callback structs and the channel-callback setter, so those types come back incomplete; sshd.h still declares a console-draw function that no longer exists, and its forward declaration of the bind type leaks into a parameter list; sshd_hostkey.c passes an unsigned pointer where libssh wants a signed one. Beyond those, expect the ordinary first-compile fallout in a file written untested. Constraints that must survive the fixes: output goes to the SSH channel and the vita2d overlay, never printf (there is no stdout and no sceDebugScreenInit in this SDK); the shell's cwd is per-session state, never the libc process-global one; the server is single-threaded, so callbacks must not block.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 every sshd app source compiles with -Wall and no warnings that indicate a real defect
- [ ] #2 the app links into a SELF and VPK with libssh's server symbols resolved
- [ ] #3 no printf/puts/stdout use anywhere in the server sources; status text goes to the vita2d overlay and shell output goes to the channel
- [ ] #4 no libc chdir() or getcwd() call anywhere in the server sources
- [ ] #5 the server runs on its own kernel thread with an explicit stack size rather than in main()
- [ ] #6 the poll loop takes exactly one accept per readable-listening-fd event
<!-- AC:END -->
