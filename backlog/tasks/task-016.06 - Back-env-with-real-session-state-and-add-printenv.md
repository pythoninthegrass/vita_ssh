---
id: TASK-016.06
title: Back env with real session state and add printenv
status: Done
assignee:
  - Claude
created_date: '2026-11-17 03:40'
updated_date: '2026-11-18T09:18:00Z'
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

PATH stays what its own comment says it is -- decorative, since commands come from the builtin table and never from a search path. Document that rather than quietly honoring it.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 env prints the whole table for the session, including HOME, PWD, USER, LOGNAME, TERM and, once TASK-019.01 exists, TMPDIR, each reflecting real state rather than a constant.
- [x] #2 TERM shows what the client actually requested, and the server default only when the client asked for nothing; it never claims a terminal the client did not ask for.
- [x] #3 printenv NAME prints the bare value with no NAME= prefix; printenv NAME OTHER prints one per line; printenv with no arguments matches env.
- [x] #4 printenv on a name that is not set prints nothing and exits 1, while env always exits 0.
- [x] #5 Two concurrent sessions never see each other's values: different users show different USER and LOGNAME, and each PWD tracks its own session's directory.
- [x] #6 A client COLUMNS or LINES request either lands in the table or is refused with a line on the console or log; nothing is dropped silently, and the chosen authority for window size is recorded in code comments.
- [x] #7 A client-supplied variable value cannot put escape sequences on the Vita console and cannot reach a file call without going through the path layer.
- [x] #8 env and printenv each have a row in builtins[] in alphabetical order with a usage string help prints, and which finds both.
- [x] #9 PATH is still documented as decorative in the place a reader will find it, rather than being honored as a search path.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 The env-request callback return convention is quoted from the installed libssh header, not assumed
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

Adjustments made while implementing (2026-11-18): (a) TERM is seeded from the pty request's term argument as well as the env request, because OpenSSH sends TERM only in pty-req; before either, TERM=dumb (no terminal claimed) rather than xterm-256color. (b) The table is a pure module, src/sh_env.c / include/sh_env.h, with a host test scripts/test_sh_env.py, mirroring sh_alias; the session owns it (sh_session_env()). (c) Env allowlist is TERM and LANG; COLUMNS, LINES and everything else are refused with a sshd console/log line. Value sanitizing (drop bytes <0x20 and 0x7F, reject over 256 bytes) lives in sh_env_set.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
DoD3: libssh/callbacks.h, ssh_channel_env_request_callback: '@returns 0 if the env request is accepted / @returns 1 if the request is denied'. Same convention as pty/shell/exec; on_env_request returns 1 for refusals.

Build: task build produces both VPKs; ssh_server_vita text 2303234 -> 2304652 (+1418 bytes) with sh_env.c added and static_env[] removed. Host test python3 -I scripts/test_sh_env.py passes under ASan/UBSan. Not yet verified on a device: AC1-AC6 behavior and DoD2 (Vita client vs OpenSSH TERM) need an install via VitaShell over USB per CLAUDE.local.md.

Device run 2026-11-18 (home Vita, installed with vpk install at the user's explicit request; promote package 0x00000000, server answered again in about 9 s). OpenSSH exec: env prints PATH SHELL HOSTNAME LANG HOME USER LOGNAME TERM PWD, rc 0. TERM is dumb with no pty, xterm-256color with TERM=xterm-256color -tt, vt100 with TERM=vt100 -tt. printenv TERM USER PWD HOME prints four bare values; printenv NOPE prints nothing, rc 1; printenv NOPE TERM prints dumb, rc 1; printenv with no arguments is byte-identical to env. cd /ux0/data then printenv PWD prints /ux0/data. Two overlapping pty sessions printed /ux0/data and /ux0/music. Env requests: LANG accepted; LC_ALL, LC_X, COLUMNS, LINES, FOO each logged as 'env NAME from <peer> refused: not accepted'. LANG carrying ESC[2J came back as 'a[2Jb' (ESC dropped). help printenv, help env and which env printenv all find both.

Not verified: the Vita client's TERM (DoD2) and different USER/LOGNAME across two sessions (AC5), since the device has one user and the Vita client needs the handset. AC1 names TMPDIR only once TASK-019.01 exists. Observed, unrelated to env: a pty session that sends two lines in one chunk runs only the second, since the line editor holds one pending line; and there is no sleep builtin (TASK-016.07).

2026-11-18: AC5 verified. The server accepts any login name (sh_auth_check_pubkey and sh_auth_check_password ignore the user; the key or passphrase is the credential), and USER and LOGNAME echo the name the client logged in with: ssh root@, any@ and alice@ printed root, any and alice. DoD2 removed at the user's direction: the Vita client is deprioritized, and the TERM difference was shown with two OpenSSH terminals (xterm-256color, vt100) plus dumb with no pty.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
env and printenv now report per-session state. A new pure module, src/sh_env.c, holds a bounded variable table (32 names, 256-byte values, control bytes stripped) that each session owns and reaches through sh_session_env(); the static_env[] constants are gone. PWD is read from the session's cwd at print time. TERM comes from the pty request or an env request and is dumb until a client names a terminal, so env never claims one nobody asked for. An env-request callback accepts TERM and LANG and refuses everything else, COLUMNS and LINES included, with a log line; the pty request stays the single authority for window size. PATH is documented as decorative in the code and README.

Verified: python3 -I scripts/test_sh_env.py (ASan/UBSan) and task build (both VPKs; server text +1418 bytes). On the home Vita, installed with vpk install: env and printenv output and exit statuses, TERM dumb / xterm-256color / vt100 from OpenSSH, refused env names in sshd.log, an ESC[2J in LANG stripped, two overlapping sessions with different PWD, and USER/LOGNAME echoing root, any and alice (the server accepts any login name).

Not verified: the Vita client's TERM (DoD item dropped at the user's direction; the client is deprioritized). Follow-ups filed: TASK-016.07 (sleep) and TASK-037 (line editor overwrites a queued line). TASK-020 can extend the same table with set/unset.
<!-- SECTION:FINAL_SUMMARY:END -->
