---
id: TASK-016
title: Give the shell a coreutils command set
status: To Do
assignee: []
created_date: '2026-11-17 03:28'
labels:
  - shell
  - coreutils
dependencies: []
references:
  - 'https://github.com/holzschu/a-shell'
documentation:
  - AGENTS.md
  - context7 library /holzschu/a-shell
modified_files:
  - src/sshd_builtins.c
priority: medium
ordinal: 30000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Wanted: the commands people reach for first on any unix box, as entries in the shell's existing builtin table. The bar agreed with the user is core functionality, not feature completeness: grep, sed, awk, head, tail, plus wc, sort and uniq as the companions that make pipes worth having.

Every command in this family lands in src/sshd_builtins.c following the five-step recipe in AGENTS.md ("Adding a builtin command"): a cmd_<name>() in that file, a row in builtins[] in alphabetical order with a one-line usage string that help prints, paths only through resolve_arg() and the sh_target_t it fills, check_writable() before any mutation, output through sh_printf() and errors through sh_eprintf() in the form name: arg: strerror(errno). The file is already in the server's add_executable list; a new file would have to be added to that list by hand.

Non-negotiables, each of which has already cost a debugging cycle elsewhere in this tree: no printf/puts/stdout (that is the debug screen, not the user); no chdir()/getcwd() (libc's cwd is one static for the whole process); no hand-built ux0: strings; no faking support the platform lacks.

The honesty rule for the whole family: a flag or construct this tool does not implement answers with a readable error and a nonzero exit. A tool that silently ignores an option is worse than one that refuses it -- the user believes something happened.

Each command reads its file arguments and, when given none, the session's stdin, so the pipe task can drive it.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 grep, sed, awk, head, tail, wc, sort and uniq are all reachable from the shell and each appears in help with a one-line usage string.
- [ ] #2 which reports each of them and no longer reports them as unknown.
- [ ] #3 Every one of them reads the session's stdin when given no file argument, so a pipe can drive it.
- [ ] #4 Every one of them reports an unsupported flag as "name: unsupported option: FLAG" with a nonzero exit rather than ignoring it.
- [ ] #5 Every one of them bounds line length and working memory, and a bound that is hit says so instead of truncating silently.
- [ ] #6 No command in this family uses printf, puts, chdir() or a hand-built device path (AGENTS.md platform rules).
- [ ] #7 README lists the commands with the flags that actually work, and says plainly which ones are subsets.
<!-- AC:END -->
