---
id: TASK-030
title: 'Add the true, false and : builtins'
status: To Do
assignee: []
created_date: '2026-11-17T18:42:00Z'
labels:
  - shell
  - coreutils
dependencies: []
references:
  - scripts/silent_sockets.sh
  - src/sshd_builtins.c
documentation:
  - AGENTS.md
priority: medium
type: enhancement
ordinal: 61000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The shell has no command whose only job is to succeed or fail. A one-shot `ssh host true` answers `sh: true: command not found` (status 127), which is how scripts/silent_sockets.sh broke on its first device run, and the chaining work (TASK-025) has no way to produce a known 0 or nonzero status to test `&&` and `||` against except by relying on a side effect such as `ls /nope`. `:` is the POSIX no-op people write as a placeholder. All three are the smallest commands a shell can have and belong in the builtin table.

Context for whoever picks this up: the server is close to the size ceiling `vita-elf-create` enforces and fails as an abort, not a link error (AGENTS.md, Build). Measure the headroom with `arm-vita-eabi-size build/ssh_server_vita` before and after, and record both numbers in the task notes, so a later builtin knows what is left.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `true` and `:` print nothing and exit 0; `false` prints nothing and exits 1; each ignores any arguments it is given, as POSIX specifies.
- [ ] #2 `ssh host true`, `ssh host false` and `ssh host :` return exit statuses 0, 1 and 0 to the client.
- [ ] #3 `:` is recognised as a command name by the tokenizer and dispatcher, so `: foo bar` and `:` alone both run it instead of answering command not found.
- [ ] #4 help lists all three with a one-line usage string, in alphabetical position in builtins[], and which reports each of them.
- [ ] #5 A pipeline segment that is one of them behaves as in sh: `true | cat` and `false | cat` both print nothing and take the status of cat.
- [ ] #6 README's builtin list names the three commands.
- [ ] #7 The server still links, and the byte headroom before the size ceiling, read with arm-vita-eabi-size before and after, is recorded in the task notes.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session, including a one-shot exec
<!-- DOD:END -->
