---
id: TASK-020
title: Make session variables settable and expand $NAME in the shell
status: To Do
assignee: []
created_date: '2026-11-17 03:41'
updated_date: '2026-11-17 03:41'
labels:
  - shell
  - coreutils
dependencies:
  - TASK-016.06
references:
  - 'https://github.com/holzschu/a-shell'
documentation:
  - AGENTS.md
  - context7 library /holzschu/a-shell
modified_files:
  - src/sshd_builtins.c
  - src/sshd_session.c
  - include/sshd.h
priority: medium
ordinal: 47000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Extends the environment table TASK-016.06 establishes so a person can set their own values and use them: NAME=value, unset NAME, and $NAME or the braced form inside unquoted words. That is what makes cat $HOME/x, cd $TMPDIR and grep $PATTERN f worth typing, and it is the last piece of everyday shell ergonomics in this family (TASK-018 aliases is the sibling).

Platform rule that decides the design, identical to the per-session cwd rule: libc's environ is a single process-wide static, so a variable set in one session must never be visible in another. The table lives with the session and nothing calls setenv(). Heap, with a stated bound on names, values and count per session -- a Vita thread's stack is whatever sceKernelCreateThread was given.

Honesty points to settle in the open rather than paper over:
- There are no child processes. export and plain assignment are therefore the same operation on the same table; if export exists as a word, it must be documented as a synonym rather than implying a distinction that does not exist.
- An unset variable expands to nothing, which is how sh behaves and which is safe here only because the path layer answers ENOENT for an unknown first component rather than falling back to a default device. State the word-elision rule explicitly: an unquoted reference to a missing variable contributes no word, a quoted one contributes an empty word, and that difference is what keeps rm with a missing argument from doing something nobody meant.
- Variable references are expansion, not evaluation: no command substitution, no arithmetic, no braced defaults. Unsupported forms report a readable error rather than being passed through as a literal dollar that silently survives.

Ordering in sh_execute_line() matters and is the one place this task can collide with others: word split, then alias expansion (TASK-018), then variable expansion, then redirect extraction, then glob expansion. A variable's value must not be glob-expanded or alias-expanded a second time -- the same one-level rule aliases use, and for the same reason: a hung poll loop takes every session with it. Coordinate with TASK-018, since both tasks edit the same function's word handling.

Server-owned names -- HOME, PWD, USER, LOGNAME, TMPDIR, and anything the client supplied through an env request -- refuse assignment with a readable error. Letting a typo move HOME would move ~, the SFTP landing directory and the download target out from under the session at once, which is exactly the kind of surprise this project refuses to ship.

Note on quoting: split_line() today tracks one quoted bit meaning "do not glob". Correct sh semantics distinguish single quotes (no expansion) from double quotes (expansion allowed). Either implement that distinction or treat all quoting as literal and document the difference in help -- a silent simplification is not acceptable.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 NAME=value sets a variable that env and printenv then report; unset NAME removes it; both exit 0 and setting an existing name replaces its value.
- [ ] #2 echo $NAME prints the value, the braced form works in the same position, and an unset name expands to nothing.
- [ ] #3 An unquoted reference to an unset variable contributes no word, so a command that would have received a stray empty argument receives none, while a quoted reference contributes an empty word.
- [ ] #4 A substituted value is not glob-expanded or alias-expanded afterwards: a value containing an asterisk stays a literal asterisk.
- [ ] #5 Expansion happens after alias expansion and before redirect extraction, so a redirect written with a variable in its target does the obvious thing exactly once.
- [ ] #6 Assignment to HOME, PWD, USER, LOGNAME, TMPDIR or a client-supplied name is refused with a readable error naming the variable, and ~ and the SFTP landing directory are unaffected either way.
- [ ] #7 Unsupported syntax -- command substitution, arithmetic, braced defaults -- reports a readable error naming the construct rather than passing a literal dollar sign through to the command.
- [ ] #8 A malformed braced reference reports a readable error and a nonzero exit rather than expanding to empty or to a literal brace.
- [ ] #9 Variables are per session and die with it; a second concurrent session never sees them, and history recalls the line as typed, not the expanded form.
- [ ] #10 A session that sets the stated maximum number of variables, or a value over the length bound, gets a readable error naming the limit and keeps working.
- [ ] #11 The assignment, unset and any export synonym have rows in builtins[] in alphabetical order, and which finds them.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session, including two concurrent sessions
- [ ] #3 The quoting decision (single versus double quotes) is documented in help and README rather than left implicit
<!-- DOD:END -->
