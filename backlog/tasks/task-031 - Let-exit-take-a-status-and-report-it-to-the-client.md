---
id: TASK-031
title: Let exit take a status and report it to the client
status: Done
assignee:
  - claude
created_date: '2026-11-17T18:42:00Z'
updated_date: '2026-11-19T08:00:00Z'
labels:
  - shell
dependencies: []
references:
  - src/sshd_session.c
  - src/sshd_builtins.c
documentation:
  - AGENTS.md
priority: medium
type: enhancement
ordinal: 62000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`exit` ignores its argument and the session always reports exit status 0 when it ends through `exit`. So `ssh host exit 3` returns 0, and a script cannot use the shell to signal failure the way every POSIX shell lets it. Exec mode already reports the real status of an ordinary command to the client; only the `exit` path is hard-wired to 0. A related gap: when an interactive shell ends because the client sent EOF, the status should be that of the last command, as in sh, rather than a constant.

Wanted: `exit [N]` as POSIX defines it for the cases that apply to a shell with no jobs and no traps.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 `ssh host "exit 3"` returns exit status 3 to the client, and `ssh host exit` with no argument returns the status of the last command run in that session.
- [x] #2 N is reduced modulo 256 as POSIX specifies, so `exit 256` returns 0 and `exit 257` returns 1.
- [x] #3 A non-numeric or negative argument prints `exit: ARG: numeric argument required` and ends the session with status 2, as bash and dash do.
- [x] #4 More than one argument prints `exit: too many arguments`, does not end the session, and exits 1.
- [x] #5 In a pipeline `exit 3` as a non-last segment is still skipped as in a subshell, and as the last segment ends the session with status 3.
- [x] #6 An interactive shell session that ends on EOF at an empty line reports the status of the last command it ran.
- [x] #7 help's usage string for exit shows the optional status, and README states the behavior.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session, including a one-shot exec
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. New pure module `src/sh_exit.c` / `include/sh_exit.h`: `sh_exit_parse(argc, argv, last_status, &status)` returns whether the session ends and what status to report: no argument gives last_status; digits only, reduced modulo 256; non-numeric, negative or overflowing gives status 2 and ends (message `exit: ARG: numeric argument required`); more than one argument gives status 1 and does not end (message `exit: too many arguments`).
2. Host test `scripts/test_sh_exit.c` plus `scripts/test_sh_exit.py` (ASan/UBSan, under timeout), written first and run to confirm it fails.
3. `sh_session_request_exit(s, status)` stores `exit_status`; `sh_session_pump()` sends it instead of the hard-wired 0. Ctrl-D on an empty line and the drained-EOF path use `last_status`.
4. `cmd_exit` calls the parser and prints the messages. The pipeline's skipped non-last `exit` is unchanged, and a last-segment `exit N` goes through `cmd_exit`.
5. Update the `exit` usage string, README, the AGENTS.md test list and source list, and CMakeLists (server list).
6. `task build`, then a device run over ssh (one-shot exec and interactive) using `vpk install -r`.

Added during implementation: the shell `exit N` path never sent the status at all. After a command set exit_requested the pump returned 1 and the slot was freed without exit-status, EOF or close, so an interactive `exit 5` made the client's ssh exit 255. The post-command path now returns 0 and the next pump sends the status, and that branch waits for the client's close ack (as the sftp path does, up to 3 s) because freeing early raced the status and gave an occasional 255. `sftp_closing` was renamed `close_started` since both paths share it.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-17: TASK-032 (`$?`) needs the same per-session last-status field this task needs for `exit` with no argument and for EOF. Whichever lands first adds the field in src/sshd_session.c; the other reuses it. TASK-032 depends on TASK-020, so this task will normally land first and should create the field.

2026-11-19 device results (the Vita, home network, server rebuilt and installed with `vpk install -r`, promote result 0x00000000). One-shot exec: `exit 3` gave 3, `exit` after `true` gave 0, `false ; exit` gave 1 (10 of 10 runs), `exit 256` gave 0, `exit 257` gave 1, `exit abc` and `exit -1` printed `exit: ARG: numeric argument required` and gave 2 (6 of 6), `exit 1 2 ; echo still-here` printed the too-many-arguments error, kept going and printed still-here, `exit 3 | cat ; echo after` skipped the exit and printed after, `echo a | exit 4` gave 4, `exit 3 ; echo no` gave 3 with no output. Interactive shell over a real pty: `exit 7` gave 7, `exit 3` gave 3 (6 of 6), `exit abc` gave 2 (8 of 8), `false` then Ctrl-D gave 1, `true` then Ctrl-D gave 0. `help exit` shows `exit [N] -- end this session with status N (default: the last command's)`. Before the close-ack wait, interactive `exit abc` returned 255 in 1 of 4 runs. Piping into `ssh -tt` does not deliver a real Ctrl-D (the session only ended when stdin closed), so Ctrl-D was checked through a pty.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
`exit [N]` now reports its status. The argument rules live in a new pure module, `src/sh_exit.c`, with a host test (`scripts/test_sh_exit.py`, ASan/UBSan, written first and seen failing): no argument takes the last status, digits are reduced modulo 256, a sign, space, text or int overflow prints `exit: ARG: numeric argument required` and ends with status 2, and extra arguments print `exit: too many arguments`, keep the session and return 1. `sh_session_request_exit()` takes the status and the pump sends it instead of 0; Ctrl-D on an empty line uses the last command's status. While verifying on the device I found that a shell session ending through `exit` never sent a status at all (the client's ssh exited 255), and that freeing the session right after sending it raced the status; both are fixed in `sh_session_pump()` by deferring to the top-of-pump branch and waiting up to 3 s for the client's close ack, shared with the sftp path (`close_started`). README, the `exit` usage string, AGENTS.md (test and source lists) and CMakeLists are updated. Verified on the device over real ssh, exec and pty. Risk: the exit path now holds a session up to 3 s if a client never acknowledges the close. Not committed.
<!-- SECTION:FINAL_SUMMARY:END -->
