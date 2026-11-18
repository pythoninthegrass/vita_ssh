---
id: TASK-030
title: 'Add the true, false and : builtins'
status: Done
assignee:
  - Claude
created_date: '2026-11-17T18:42:00Z'
updated_date: '2026-11-18T05:55:00Z'
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
- [x] #1 `true` and `:` print nothing and exit 0; `false` prints nothing and exits 1; each ignores any arguments it is given, as POSIX specifies.
- [x] #2 `ssh host true`, `ssh host false` and `ssh host :` return exit statuses 0, 1 and 0 to the client.
- [x] #3 `:` is recognized as a command name by the tokenizer and dispatcher, so `: foo bar` and `:` alone both run it instead of answering command not found.
- [x] #4 help lists all three with a one-line usage string, in alphabetical position in builtins[], and which reports each of them.
- [x] #5 A pipeline segment that is one of them behaves as in sh: `true | cat` and `false | cat` both print nothing and take the status of cat.
- [x] #6 README's builtin list names the three commands.
- [x] #7 The server still links, and the change in server text size, read with arm-vita-eabi-size before and after, is recorded in the task notes.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session, including a one-shot exec
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Add cmd_true and cmd_false to src/sshd_builtins.c and three builtins[] rows (":" sharing cmd_true, "false", "true") in table order; list them in README's builtin list. No tokenizer change was needed: ":" is an ordinary word. Verified on a device over ssh; the server size change is recorded in the notes.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-17 device run over ssh (the one Vita, home network, build installed through VitaShell over USB, eboot.bin byte-identical to the working tree at b8b3d14): true, false, ':', ': foo bar', 'true extra args' and 'false extra args' exit 0, 1, 0, 0, 0, 1 and print nothing; 'true | cat' and 'false | cat' print nothing and exit 0 (cat's status; cat on empty stdin cannot tell a swallowed nonzero from cat's own, so only the zero case is shown); 'which true false :' reports each as a shell builtin; 'help true', 'help false' and 'help :' print their usage; bare help lists ':' first and false, true in alphabetical position. Sizes of the server ELF text from arm-vita-eabi-size: 2297612 at 6fb5c0e, 2297824 with the three builtins on top of the same code (+212 bytes), 2297512 after TASK-035 removed the power locks. AC7 stays open: the byte headroom before the vita-elf-create ceiling was not measured, only the text size, so a later builtin still does not know what is left.

AC7 reworded from 'byte headroom before the size ceiling' to 'change in server text size' on the user's decision: the headroom would have needed padding the build until vita-elf-create failed, and the ceiling figure in AGENTS.md is stale. The size readings are in the notes above (+212 bytes for the three builtins).
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Added the true, false and : builtins (0b3d828). true and : exit 0, false exits 1, each ignores its arguments; ":" shares cmd_true. They give chained commands a known status to test && and || against (TASK-025). Verified on a device over ssh against an installed build identical to the working tree: exit statuses 0/1/0, arguments ignored, true|cat and false|cat print nothing, which and help report them, bare help lists them in order. Server text grew 212 bytes. AC7 was reworded from byte headroom to the size change, because measuring the headroom needs the build padded until vita-elf-create fails. Caveat: `false | cat` exits 0 because cat does, so the pipeline test does not show a nonzero status being swallowed. The device runs for this task exposed a separate regression in the power locks, tracked and fixed in TASK-035.
<!-- SECTION:FINAL_SUMMARY:END -->
