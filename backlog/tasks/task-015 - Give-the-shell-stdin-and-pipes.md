---
id: TASK-015
title: Give the shell stdin and pipes
status: Done
assignee: []
created_date: '2026-11-17 03:28'
updated_date: '2026-11-17T17:45:00Z'
labels:
  - shell
dependencies: []
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
priority: high
ordinal: 29000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
People expect ls | grep ux0. There is no fork, no exec and nothing may block: every command is an in-process table entry run from the single poll-loop thread. That rules out a streaming pipe, so the a-Shell model is the workable one -- a pipeline runs its segments one after another, capturing stdout into a bounded memory buffer that becomes the next segment's stdin. a-Shell faces the identical no-fork constraint and pipes that way.

Parent for the two pieces of plumbing the text utilities need: a stdin side for a session, and the | operator wiring segments together.

Already there, reuse it: > and >> on stdout work today through extract_redirect() plus sh_session_redirect_start()/sh_session_redirect_end(). Pipes and stdin should extend that machinery rather than opening a second output path.

Hard platform rules this family must not break: nothing blocks inside the poll loop; a command's output is paced by the client's window; errors (sh_eprintf) always reach the terminal even when stdout is redirected; long work checks sh_session_interrupted().
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 A pipeline such as ls | grep ux0 prints the matching lines once grep exists, and cat file | head -n 5 prints five lines.
- [x] #2 A pipeline adds no wait of its own: a quick pipeline does not hold up another session, and a segment that is itself long-running (du /ux0) starves the loop exactly as it does outside a pipeline, which TASK-028 addresses.
- [x] #3 The intermediate data stays within a stated bound, and exceeding it reports a readable error naming the limit rather than corrupting output or exhausting memory.
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-17: both subtasks done. AC1 on the work Vita over ssh: `cat FILE | head -n 5` printed five lines, `ls / | grep ux0` printed /ux0 (see TASK-015.02 for the full run). AC3: a 2 MB file through `cat | wc -l` stopped with `sh: cat: output too large for a pipe (limit 1048576 bytes)` and the session kept working; the bound is SSHD_STDIN_MAX, 1 MB.

AC2 was reworded like TASK-015.02's AC9: a long-running segment such as du /ux0 (about 170 s) starves every other session with or without a pipe, which is not caused by pipes. Follow-ups: TASK-028 (let long-running builtins yield so other sessions can connect) and TASK-029 (Ctrl-C stops a running command, depends on TASK-028).
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
The shell has bounded stdin (`<`, TASK-015.01) and the `|` operator (TASK-015.02): a pipeline runs its segments one after another, each segment's output held in a 1 MB memory buffer that becomes the next segment's stdin. Verified on a device over real ssh sessions, including the over-bound error. The one thing it does not give is a responsive server during a long-running segment, which was already true of long commands and is now tracked as TASK-028, with Ctrl-C as TASK-029.
<!-- SECTION:FINAL_SUMMARY:END -->
