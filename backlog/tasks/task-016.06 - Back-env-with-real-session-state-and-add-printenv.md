---
id: TASK-016.06
title: Back env with real session state and add printenv
status: To Do
assignee: []
created_date: '2026-11-17 03:40'
updated_date: '2026-11-17T18:43:00Z'
labels:
  - shell
  - coreutils
dependencies: []
documentation:
  - AGENTS.md
  - src/ssh_client.c
modified_files:
  - src/sshd_builtins.c
  - src/sshd_session.c
  - include/sshd.h
parent_task_id: TASK-016
priority: medium
ordinal: 46000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
env exists but is a printout, not an environment. cmd_env() walks a hardcoded static_env[] array (PATH=/bin:/usr/bin, SHELL=/bin/sh, TERM=xterm-256color, LANG=C, HOSTNAME=vita) and then prints HOME, PWD, USER and LOGNAME from the session. Nothing anywhere in the tree calls getenv or setenv, so there is no environment to inherit -- and env advertises TERM=xterm-256color whether or not the client said so, which is the sort of confident wrong answer this project's honesty rule exists to prevent.

Wanted: the two coreutils commands, env and printenv, reporting what is actually true for this session, with the values living where session state lives.

There is a real consumer waiting, already in the tree and currently ignored: src/ssh_client.c sends TERM, COLUMNS and LINES with libssh2_channel_setenv(), libssh exposes channel_env_request_function (libssh/callbacks.h), and sshd_session.c installs only the pty, shell, exec and subsystem callbacks. The client's declared terminal state arrives and goes on the floor. Whether to accept it is a decision, not an omission: either the values land in the session table or the server refuses them explicitly, and a silent drop is the failure being fixed here.

Platform rule that shapes the design, the same one behind the per-session cwd: libc's environ is one process-wide static, so a session that sets a variable must not move another session's view. The table belongs with the session (alongside sh_fsctx_t), not in setenv(). Design it so a later settable-variable task can extend the same table rather than build a second one.

Two things to check rather than assume, because getting them wrong costs a debugging cycle each:
- The return convention of an env-request callback. AGENTS.md records that pty/shell/exec/subsystem request callbacks return 0 to accept and that auth callbacks are different; read the declaration in the installed libssh header instead of guessing which family env requests belong to.
- COLUMNS and LINES duplicate the window size the pty request already carries, and sh_session_cols() already answers it. Pick one authority and keep the two views from disagreeing.

Client-supplied values are untrusted input: they must not be able to inject escape sequences into the Vita console or become a path that gets used without going through the path layer.

PATH stays what its own comment says it is -- decorative, since commands come from the builtin table and never from a search path. Document that rather than quietly honouring it.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 env prints the whole table for the session, including HOME, PWD, USER, LOGNAME, TERM and, once TASK-019.01 exists, TMPDIR, each reflecting real state rather than a constant.
- [ ] #2 TERM shows what the client actually requested, and the server default only when the client asked for nothing; it never claims a terminal the client did not ask for.
- [ ] #3 printenv NAME prints the bare value with no NAME= prefix; printenv NAME OTHER prints one per line; printenv with no arguments matches env.
- [ ] #4 printenv on a name that is not set prints nothing and exits 1, while env always exits 0.
- [ ] #5 Two concurrent sessions never see each other's values: different users show different USER and LOGNAME, and each PWD tracks its own session's directory.
- [ ] #6 A client COLUMNS or LINES request either lands in the table or is refused with a line on the console or log; nothing is dropped silently, and the chosen authority for window size is recorded in code comments.
- [ ] #7 A client-supplied variable value cannot put escape sequences on the Vita console and cannot reach a file call without going through the path layer.
- [ ] #8 env and printenv each have a row in builtins[] in alphabetical order with a usage string help prints, and which finds both.
- [ ] #9 PATH is still documented as decorative in the place a reader will find it, rather than being honoured as a search path.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device with two different clients (the Vita client and OpenSSH) showing different TERM values
- [ ] #3 The env-request callback return convention is quoted from the installed libssh header, not assumed
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
No dependencies; TASK-020 depends on this task's table and should not start before it lands.

Researched 2026-11-17 against src/sshd_builtins.c, src/sshd_session.c and the libssh header installed under $VITASDK:
- cmd_env() walks static_env[] and then prints HOME, PWD, USER and LOGNAME from the session; nothing calls getenv/setenv.
- sshd_session.c installs only the pty, shell, exec and subsystem request callbacks, so the env requests src/ssh_client.c sends (TERM, COLUMNS, LINES) are dropped silently.

Steps:
1. Read the declaration of the env-request callback in the installed libssh `callbacks.h` (`ssh_channel_env_request_function`) and quote its return convention in the task notes (DoD3); do not assume it matches the pty family.
2. Table: add a small per-session variable table next to sh_fsctx_t in src/sshd_session.c (heap, bounded count and lengths) with accessors in include/sshd.h, shaped so TASK-020 extends it with set/unset and read-only flags instead of building a second table.
3. Seed at session setup: HOME, USER, LOGNAME from the session; TERM only from the client's env request, with the server default (xterm-256color) used and marked as a default when the client sent nothing (AC2). PWD is not stored: read it from the session's fsctx at print time so it cannot disagree with `cd` (AC5).
4. Install the env-request callback. Accept an allowlist only (TERM, COLUMNS, LINES, LANG); every other name is refused with a line in sshd.log naming it (AC6). Strip bytes below 0x20 and 0x7F from values and cap length before storing or logging them (AC7); a value is never used as a path without sh_path_resolve().
5. Window-size authority: the pty request, via sh_session_cols(). A COLUMNS or LINES env request is logged and not stored, with a code comment saying so (AC6).
6. cmd_env() prints the table, always exit 0. Add cmd_printenv(): bare value for one name, one per line for several, no arguments matches env, exit 1 when a named variable is unset (AC3, AC4). Rows alphabetical (AC8).
7. Document in code and README that PATH is decorative (AC9); drop static_env[] once nothing reads it.
8. Verify with the Vita client and OpenSSH showing different TERM (DoD2); measure arm-vita-eabi-size before and after (size ceiling, AGENTS.md).
<!-- SECTION:PLAN:END -->
