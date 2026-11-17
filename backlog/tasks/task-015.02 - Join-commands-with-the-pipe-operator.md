---
id: TASK-015.02
title: Join commands with the pipe operator
status: To Do
assignee: []
created_date: '2026-11-17 03:29'
updated_date: '2026-11-17 03:29'
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
- [ ] #1 cat FILE | cat writes the file; echo x | cat writes x; a three-segment pipeline chains correctly.
- [ ] #2 A pipe inside quotes is literal: echo "a|b" prints a|b and does not split the line.
- [ ] #3 The last segment's stdout goes to the terminal, and honours a trailing > FILE / >> FILE on that segment.
- [ ] #4 A redirect on an intermediate segment wins over the pipe for that segment, the way sh applies redirection after pipeline assignment.
- [ ] #5 The pipeline's exit status is the last segment's; a failing middle segment still runs the segments after it and its error reaches the terminal.
- [ ] #6 A segment other than the last cannot change session state: cd or exit mid-pipeline does not move the cwd or end the session.
- [ ] #7 An unknown command mid-pipeline reports sh: name: command not found once, not once per segment.
- [ ] #8 Exceeding the intermediate bound reports a readable error naming the limit; the session survives and other sessions keep being served.
- [ ] #9 While a pipeline runs the poll loop keeps serving: another session's command executes and the Vita console still updates.
- [ ] #10 Ctrl-C during a pipeline stops it; sh_session_interrupted() is honoured by every segment.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session
<!-- DOD:END -->
