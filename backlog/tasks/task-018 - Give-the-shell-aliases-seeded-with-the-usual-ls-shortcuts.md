---
id: TASK-018
title: 'Give the shell aliases, seeded with the usual ls shortcuts'
status: To Do
assignee: []
created_date: '2026-11-17 03:28'
updated_date: '2026-11-17T18:43:00Z'
labels:
  - shell
dependencies:
  - TASK-025
documentation:
  - AGENTS.md
modified_files:
  - src/sshd_builtins.c
  - src/sshd_session.c
  - include/sshd.h
priority: low
ordinal: 32000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Common boxes ship l and ll; the shell has neither. Wanted: an alias/unalias builtin over per-session aliases, seeded at session setup with the ls shortcuts people expect.

Aliases are the only short-name mechanism available here: the Vita has no symlinks and the platform rules forbid faking one (readlink and symlink answer ENOSYS on purpose), so there is nothing to ln -s a directory with.

Expansion design already settled: expansion happens once, in command position only, after word-splitting and before redirect extraction -- so l > file still redirects, and grep ll does not expand. One level only, with a guard against an alias cycle, because a hung poll loop takes every session with it.

State lives with the session (like cwd does in sh_fsctx_t), never in libc statics: two people logged in must not see each other's aliases.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 alias lists the current name=value pairs; alias name=value defines one; unalias name removes one and reports unalias: name: not found with a nonzero exit for a name that was never defined.
- [ ] #2 A fresh session can run l, ll and la for ls -l, ls -la and ls -a, and .. and ... move up one and two directories.
- [ ] #3 Aliases expand only in command position: grep ll searches for the word ll, and an argument that happens to name an alias is untouched.
- [ ] #4 An alias cycle such as alias l=ll with alias ll=l is refused or broken with a readable message and never hangs the session.
- [ ] #5 A value with spaces survives quoting: alias two=echo a b then two prints a b.
- [ ] #6 l > /ux0/data/x writes the listing to that file: expansion happens before redirect extraction.
- [ ] #7 Aliases are per session and die with it; a second session never sees them.
- [ ] #8 history recalls the line as the user typed it, not the expanded form.
- [ ] #9 A user alias shadows a builtin of the same name, and help says which one wins.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Depends on TASK-025: alias expansion is per command position, and with chaining every chain item and every pipeline segment has its own command position, so the expansion point has to be the per-item path TASK-025 creates rather than the whole line. Coordinate order with TASK-020 (aliases first, then variables).

Researched 2026-11-17 against src/sshd_builtins.c at a3a9224:
- run_command() (~3626) takes one command's words and extracts redirects before globbing; alias expansion must run at its top, before extract_redirect, so `l > file` still redirects (AC6).
- A split value's words come from split_line() (~439); reuse it on the alias value rather than a second tokenizer.

Steps:
1. Table: a small per-session array of name/value pairs in src/sshd_session.c next to sh_fsctx_t, heap allocated, with a stated bound on count, name and value length; accessors in include/sshd.h.
2. Seed in sh_session_new(): l, ll, la, `..` and `...` (AC2). `..` and `...` are aliases for `cd ..` and `cd ../..`, so they need word-splitting to accept those names.
3. Expansion: in run_command, if argv[0] is unquoted and names an alias, split the value with split_line(), splice its words in front of the remaining arguments, and do not look at the result's first word again (one level, which also makes `alias l=ll` with `alias ll=l` terminate; still print the cycle message AC4 asks for when the definition would form a cycle).
4. Lookup order: alias first, then builtin, so a user alias shadows a builtin (AC9); help says which wins.
5. Builtins: `alias` (list, define, and the `name=value` form with quoted values, AC5) and `unalias` (`unalias: NAME: not found`, status 1), rows alphabetical.
6. history keeps the typed line already; confirm AC8.
7. smoke.sh exec checks, README and help text; measure arm-vita-eabi-size before and after (size ceiling, AGENTS.md).
<!-- SECTION:PLAN:END -->
