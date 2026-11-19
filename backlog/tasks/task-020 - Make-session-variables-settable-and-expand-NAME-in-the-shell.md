---
id: TASK-020
title: Make session variables settable and expand $NAME in the shell
status: Done
assignee: []
created_date: '2026-11-17 03:41'
updated_date: '2026-11-19T06:07:00Z'
labels:
  - shell
  - coreutils
dependencies:
  - TASK-016.06
  - TASK-025
references:
  - 'https://github.com/holzschu/a-shell'
documentation:
  - AGENTS.md
  - context7 library /holzschu/a-shell
modified_files:
  - src/sshd_builtins.c
  - src/sshd_session.c
  - src/sh_env.c
  - src/sh_expand.c
  - include/sh_env.h
  - include/sh_expand.h
  - scripts/test_sh_env.c
  - scripts/test_sh_expand.c
  - scripts/test_sh_expand.py
  - scripts/smoke.sh
  - CMakeLists.txt
  - README.md
  - AGENTS.md
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

Ordering in sh_execute_line() matters and is the one place this task can collide with others: word split, then alias expansion (TASK-018), then variable expansion, then redirect extraction, then glob expansion. A variable's value is split into fields at IFS and the fields are glob-expanded like any unquoted word, as in sh, but a value must never be alias-expanded or taken for an operator -- the same one-level rule aliases use, and for the same reason: a hung poll loop takes every session with it. Coordinate with TASK-018, since both tasks edit the same function's word handling.

Server-owned names -- HOME, PWD, USER, LOGNAME, TMPDIR, and anything the client supplied through an env request -- refuse assignment with a readable error. Letting a typo move HOME would move ~, the SFTP landing directory and the download target out from under the session at once, which is exactly the kind of surprise this project refuses to ship.

Note on quoting: split_line() today tracks one quoted bit meaning "do not glob". Correct sh semantics distinguish single quotes (no expansion) from double quotes (expansion allowed). Either implement that distinction or treat all quoting as literal and document the difference in help -- a silent simplification is not acceptable.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 NAME=value sets a variable that env and printenv then report; unset NAME removes it; both exit 0 and setting an existing name replaces its value.
- [x] #2 echo $NAME prints the value, the braced form works in the same position, and an unset name expands to nothing.
- [x] #3 An unquoted reference to an unset variable contributes no word, so a command that would have received a stray empty argument receives none, while a quoted reference contributes an empty word.
- [x] #4 An unquoted substituted value is split at IFS and each field is glob-expanded like any unquoted word, as in sh; a quoted reference is neither split nor globbed; no field is ever alias-expanded or read as an operator, so a value of `>`, `;` or `|` is an ordinary argument.
- [x] #5 Expansion happens after alias expansion and before redirect extraction, so a redirect written with a variable in its target does the obvious thing exactly once.
- [x] #6 Assignment to HOME, PWD, USER, LOGNAME, TMPDIR or a client-supplied name is refused with a readable error naming the variable, and ~ and the SFTP landing directory are unaffected either way.
- [x] #7 Unsupported syntax -- command substitution, arithmetic, braced defaults -- reports a readable error naming the construct rather than passing a literal dollar sign through to the command.
- [x] #8 A malformed braced reference reports a readable error and a nonzero exit rather than expanding to empty or to a literal brace.
- [x] #9 Variables are per session and die with it; a second concurrent session never sees them, and history recalls the line as typed, not the expanded form.
- [x] #10 A session that sets the stated maximum number of variables, or a value over the length bound, gets a readable error naming the limit and keeps working.
- [x] #11 The assignment, unset and any export synonym have rows in builtins[] in alphabetical order, and which finds them.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session, including two concurrent sessions
- [x] #3 The quoting decision (single versus double quotes) is documented in help and README rather than left implicit
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Depends on TASK-016.06 (the per-session environment table this extends) and TASK-025 (chain splitting), so expansion runs once per chain item and a `;`, `&&`, `||` or `|` produced by a variable's value is never split again.

Researched 2026-11-17 against src/sshd_builtins.c at a3a9224:
- split_line() (~439) gives one `quoted` bit per word meaning "do not glob". Its readers are has_glob_chars use in run_command (~3657) and is_pipe_word (~3753); grep every reader of `quoted[]` before changing its meaning.
- run_command() (~3626) is where redirects are extracted, then globs expanded, then dispatched; variable expansion belongs between word split (and TASK-018 alias expansion) and extract_redirect.

Steps:
1. Table: extend the structure TASK-016.06 adds next to sh_fsctx_t in src/sshd_session.c; add set/unset/get accessors to include/sshd.h with stated bounds on name length, value length and count, heap allocated. Server-owned names (HOME, PWD, USER, LOGNAME, TMPDIR, client-supplied) are flagged read-only in the table.
2. Quoting: decide single versus double quotes (DoD3). Recommended: change `quoted[]` from a bool to flags (QUOTE_SINGLE, QUOTE_DOUBLE) set by split_line, with every existing `quoted[i]` test becoming `quoted[i] != 0` so globbing and operator rules are unchanged; only expansion distinguishes the two.
3. Expansion pass in run_command before extract_redirect: for each word without QUOTE_SINGLE, replace `$NAME` and `${NAME}`; an unquoted reference to an unset name drops the word, a double-quoted one yields an empty word; mark the result quoted so it is not globbed or alias-expanded again (AC4). Unsupported `$(`, `$((`, `${X:-}` and a malformed brace print a readable error and return nonzero (AC7, AC8).
4. Assignment: in command position, an unquoted word of the form identifier `=` value is handled before builtin lookup; add `unset` and `export` (documented synonym of assignment) rows to builtins[] alphabetically.
5. A name given to assignment that is server-owned prints `NAME: readonly variable` and returns 1 (AC6).
6. history stores the raw line already; verify AC9 on a device with two concurrent sessions.
7. smoke.sh exec: add variable checks; README and help document the quoting rule.
8. Measure with arm-vita-eabi-size before and after (size ceiling, AGENTS.md, Build).

Open question for the user, not decided here: `$?` (last exit status) is the most-used special parameter and the thing that makes `&&`/`||` results inspectable, but it needs a per-session last-status field that TASK-025 and TASK-031 also want. Either add `$?` to this task (needs AC) or file it separately.

Update 2026-11-17: the `$?` question above is answered. It is filed as TASK-032, which depends on this task. Until that lands, `$?` in a word falls under the unsupported-construct error (AC7) rather than expanding, so write the expansion pass with a clear place to add one special parameter.
<!-- SECTION:PLAN:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Session variables and $NAME expansion. Pure module src/sh_expand.c (host test scripts/test_sh_expand.py under ASan/UBSan: replacement, literal and quoted dollar markers, field splitting at IFS, unsupported and malformed forms, bounds, token and hostile sweeps). sh_env gained unset, readonly (HOME, PWD, USER, LOGNAME, TMPDIR plus client-supplied TERM/LANG via sh_env_lock) and snapshot/restore. split_line() writes markers for a `$` that must stay text and for one inside double quotes, and encodes the unquoted prefix length in quoted[] so `X="a b"` is an assignment; expand_variables() runs in run_command() after alias expansion and before redirects and globs. NAME=value, unset and export (synonym) added; a non-last pipeline segment restores the table like it restores the cwd.

Expansion follows sh: an unquoted reference is split at $IFS (default space, tab, newline; custom IFS keeps empty fields; empty IFS disables splitting), the fields are glob-expanded, and an unquoted unset reference leaves no word. Typed text, "$X" and assignment values are never split. A field from a value is marked QUOTE_VALUE (nonzero, no QUOTE_NOGLOB), so it is never an operator or an alias but still globs. History: the first cut gave a value one word and no globbing; the user rejected both, and AC4 was rewritten to the sh behavior.

Decisions to review: `rm $P` with P="*" removes every file in the cwd, as in sh; a word with typed quotes next to a reference (`x"$P"`) is not globbed (per-word granularity); `$` before a quote stays `$`; `A=1 cmd` is refused; `$?` is an unsupported-syntax error until TASK-032; backticks are not recognized; an unquoted `> $F` with spaces in F passes the extra fields as arguments instead of reporting an ambiguous redirect.

Device (the Vita, home network, 2026-11-19): scripts/smoke.sh exec 152 pass, 0 fail, including checks for IFS splitting, globbing of unquoted values, literal operators and alias values; two concurrent sessions kept separate values (2026-11-18).
<!-- SECTION:FINAL_SUMMARY:END -->
