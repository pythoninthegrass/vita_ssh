---
id: TASK-015
title: Give the shell stdin and pipes
status: To Do
assignee: []
created_date: '2026-11-17 03:28'
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
  - include/sshd.h
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
- [ ] #1 A pipeline such as ls | grep ux0 prints the matching lines once grep exists, and cat file | head -n 5 prints five lines.
- [ ] #2 A pipeline never blocks the poll loop: while one runs, another session's command still executes and the Vita console still updates.
- [ ] #3 The intermediate data stays within a stated bound, and exceeding it reports a readable error naming the limit rather than corrupting output or exhausting memory.
<!-- AC:END -->
