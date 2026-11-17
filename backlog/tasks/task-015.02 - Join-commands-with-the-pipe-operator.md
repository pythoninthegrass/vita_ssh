---
id: TASK-015.02
title: Join commands with the pipe operator
status: Done
assignee:
  - claude
created_date: '2026-11-17 03:29'
updated_date: '2026-11-17T17:45:00Z'
labels:
  - shell
dependencies:
  - TASK-015.01
references:
  - 'https://github.com/holzschu/a-shell'
documentation:
  - AGENTS.md
  - context7 library /holzschu/a-shell
modified_files:
  - src/sshd_builtins.c
  - src/sshd_session.c
  - src/sh_stdin.c
  - include/sshd.h
  - include/sh_stdin.h
  - scripts/test_sh_stdin.c
  - scripts/smoke.sh
  - README.md
  - AGENTS.md
parent_task_id: TASK-015
priority: high
ordinal: 34000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Wanted: cmd1 | cmd2 | cmd3 runs left to right, each segment reading the previous segment's captured output, the last segment writing to the terminal (or to a redirected file).

Design settled with the user, do not revisit: there is no fork and the poll loop may not block, so a pipeline is sequential in-process handoff through a bounded buffer -- segment 1 runs to completion, its output becomes segment 2's stdin, and so on. That is a-Shell's answer to the same no-fork constraint, and the only one that fits the platform rules. It is not a streaming pipe; a consumer cannot start before the producer finishes.

Depends on the stdin task, which provides the session's stdin source and its read interface; pipes feed that source. The stdout side reuses the same capture the existing > redirect already uses.

Split on | at the top level only: split_line() already tracks quoting, so a quoted pipe stays a literal argument and must not split the line.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 cat FILE | cat writes the file; echo x | cat writes x; a three-segment pipeline chains correctly.
- [x] #2 A pipe inside quotes is literal: echo "a|b" prints a|b and does not split the line.
- [x] #3 The last segment's stdout goes to the terminal, and honours a trailing > FILE / >> FILE on that segment.
- [x] #4 A redirect on an intermediate segment wins over the pipe for that segment, the way sh applies redirection after pipeline assignment.
- [x] #5 The pipeline's exit status is the last segment's; a failing middle segment still runs the segments after it and its error reaches the terminal.
- [x] #6 A segment other than the last cannot change session state: cd or exit mid-pipeline does not move the cwd or end the session.
- [x] #7 An unknown command mid-pipeline reports sh: name: command not found once, not once per segment.
- [x] #8 Exceeding the intermediate bound reports a readable error naming the limit; the session survives and other sessions keep being served.
- [x] #9 A pipeline adds no wait of its own: a quick pipeline does not hold up another session, and a segment that is itself long-running (du /ux0) starves the loop exactly as it does outside a pipeline. AGENTS.md states this and tells a new long-running builtin to yield to sh_server_poll_slice().
- [x] #10 The pipeline checks sh_session_interrupted() before every segment, so a closed channel or exit stops the remaining segments; Ctrl-C typed while a segment runs cannot be seen on this platform, and AGENTS.md and the sshd_session.c header say so.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Research: sh_execute_line() (src/sshd_builtins.c) splits the line once with split_line(), peels `>`/`>>` and `<` words, expands globs, then runs one builtin. sh_write() (src/sshd_session.c) sends to out_fp when a `>` redirect is open, else the channel. The stdin source is sh_stdin_t (src/sh_stdin.c, host-tested by scripts/test_sh_stdin.py, bound SSHD_STDIN_MAX = 1 MB).

Design (the sequential-capture design in the description, unchanged):
1. src/sh_stdin.c gains two pure functions, host-tested first: sh_stdin_append() (bounded, EFBIG past max, leaves the source empty like sh_stdin_load) and sh_stdin_move() (hand a captured buffer over as the next stdin). The struct gains a cap field so append can grow.
2. sshd_session.c: a capture buffer on the session. While capturing, sh_write() appends to it; the `>` redirect is checked first, so a redirect on a segment wins over the pipe (AC4). sh_eprintf is untouched, so errors always reach the terminal. API: sh_session_capture_start() / sh_session_capture_end() (moves the capture into the session's stdin, -1 when the bound was passed).
3. sshd_builtins.c: sh_execute_line() splits the word list at unquoted `|` words only, the same convention as `>` and `<` (a glued `a|b` stays one word), so a quoted pipe is literal (AC2). The existing body becomes run_command() on one segment's words. The pipeline loop runs each segment with capture on for every segment but the last, then feeds the capture to the next; stdin is cleared after each segment; stops on interrupt (AC10) or overflow (AC8, error names the limit). Exit status is the last segment's (AC5). On a non-last segment `cd` runs and the cwd is restored, and `exit` is skipped (AC6). Syntax errors (empty segment) run nothing.
4. Register nothing in the builtin table (the operator is not a command). No new source file, so no CMake list change.
5. Verify: host test for step 1, `task build` for both VPKs, then a device run over a real ssh session covering every AC including a concurrent second session (AC9). Update AGENTS.md (source list unchanged; add the pipeline rules) and scripts/smoke.sh with pipeline checks.

Risk: server is at the vita-elf-create size ceiling; watch `arm-vita-eabi-size build/ssh_server_vita` and the SCE-data overlap abort.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Built as planned. sh_stdin gained sh_stdin_append()/sh_stdin_move() (host test first: failed to compile, then passed, also under ASan/UBSan). The session gained sh_session_capture_start/_end. sh_execute_line() now splits the words, and either calls run_command() (the old body, taking ownership of its word arrays) or run_pipeline(). Server grew ~740 bytes; no size-ceiling problem.

Bug found on the first device run: capture_end moved the output into stdin and the loop then cleared stdin in the same iteration, so every consumer read nothing. Fixed by clearing stdin right after a segment runs, before capture_end. Device run after the fix, work Vita over ssh exec: scripts/smoke.sh exec 41 pass 0 fail, including 15 new pipe: checks (AC1-AC8).

AC7 wording: the unknown-command message was `name: command not found`; it is now `sh: name: command not found` as the AC states. Nothing tested or documented the old text.

AC9 NOT MET as written, and it is not caused by pipes: `du /ux0 | wc -l` (170 s) and plain `du /ux0` (170 s) both leave a second session unable to finish the SSH banner (ConnectTimeout 10 s). Any builtin that is CPU or I/O bound and never calls sh_server_poll_slice() starves the loop. Quick pipelines (16 segments over 800 KB, 0.5 s) are not affected in practice. Fixing it means making du (and any other long walk) yield, which is outside this task.

AC10 NOT MET as written: sh_session_interrupted() only reports a known state (channel closed, exit requested), a running command cannot see Ctrl-C (see the sshd_session.c header). The pipeline checks it before every segment, which is as far as the platform allows.

Syntax errors use status 2 and the text `sh: syntax error near unexpected token `|'`. Segments are checked for emptiness before anything runs.

AC9 and AC10 reworded (2026-11-17, Lance) to what the platform can deliver; the original text asked for a second session to be served during a long pipeline and for Ctrl-C to stop it. Evidence for AC9: du /ux0 | wc -l and plain du /ux0 both left a second session unable to finish the SSH banner for ~170 s (same behaviour, so pipes add no wait); a 16-segment pipeline over 800 KB took 0.5 s. AC10 is backed by the code check before each segment and the documented platform limit, not by a device run of a mid-pipeline hangup.

The `sh:` message forms (command not found / 127, syntax error / 2, redirect and < errors) are documented in README.md and in AGENTS.md under Adding a builtin.

Follow-ups filed for what the reworded AC9 and AC10 leave open: TASK-028 (long-running builtins yield so other sessions can connect) and TASK-029 (Ctrl-C stops a running command, depends on TASK-028).
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
`a | b | c` runs the segments in order inside the server's poll loop: each segment but the last has its stdout captured in memory (1 MB bound) and handed to the next as stdin. The `|` must be a separate, unquoted word; a `>` on a segment wins over the pipe; `exit` and `cd` before the last segment have no effect; the status is the last segment's; past the bound the pipeline stops with an error naming the limit. Added sh_stdin_append/sh_stdin_move (host-tested under ASan/UBSan), a session capture buffer, run_command()/run_pipeline() in sshd_builtins.c, and 15 pipeline checks in scripts/smoke.sh (exec half: 41 pass, 0 fail on the work Vita). The unknown-command message is now `sh: NAME: command not found`. AC9 and AC10 were reworded to the platform's limits: a long-running builtin such as du starves the loop with or without a pipe, and Ctrl-C cannot reach a running command.
<!-- SECTION:FINAL_SUMMARY:END -->
