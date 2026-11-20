---
id: TASK-016.04
title: Add sed (stream editor subset)
status: Done
assignee:
  - claude
created_date: '2026-11-17 03:29'
updated_date: '2026-11-20T18:02:00Z'
labels:
  - shell
  - coreutils
dependencies:
  - TASK-015.01
  - TASK-016.01
documentation:
  - AGENTS.md
  - context7 library /holzschu/a-shell
modified_files:
  - src/sshd_builtins.c
parent_task_id: TASK-016
priority: medium
ordinal: 38000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A deliberately small sed: line-oriented, one script, the substitutions and commands people actually use in a one-liner. Core functionality, not completeness -- the scope agreed with the user.

In scope: s/old/new/ (first match per line), s/old/new/g (all matches), an alternate delimiter such as s|old|new| for paths, d (delete the line), p (print it), q (stop), -n (suppress automatic printing), and repeated -e scripts applied in order. Out of scope and refused with a readable error: line-address ranges (1,5d), hold and space commands, labels and branches, and -i in place -- in particular -i must be refused, not quietly accepted, because silently rewriting a file the user did not ask to rewrite is the worst failure this family can have.

Same family rules as the parent task: cmd_sed() in src/sshd_builtins.c, alphabetical row, resolve_arg() for file arguments and for a script file, sh_printf() for the stream, sh_eprintf() for errors.

Regex: whatever grep settled on (POSIX basic via newlib regcomp, or literal) -- reuse one engine rather than writing a second matcher, and keep the two tools' pattern behavior identical so muscle memory does not betray anyone.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 sed 's/old/new/' FILE replaces the first match on each line and sed 's/old/new/g' replaces all of them, with unspecified lines passed through unchanged.
- [x] #2 An alternate delimiter works: sed 's|/ux0/a|/ux0/b|' edits a path without backslashes.
- [x] #3 -n suppresses automatic printing and p prints explicitly; d deletes the current line; q stops reading.
- [x] #4 Repeated -e scripts apply in the given order; a script that fails to parse reports sed: reason and exits nonzero without emitting a partial edit.
- [x] #5 sed reads stdin with no file argument, so cat f | sed 's/a/b/' works once pipes exist.
- [x] #6 -i is refused with a readable message and a nonzero exit; no file is written.
- [x] #7 Line-address ranges, hold-and-space commands and labels are refused with a readable unsupported message naming what was not understood.
- [x] #8 A missing file argument is reported as sed: FILE: reason with the other files still processed.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session
- [x] #3 README states which sed subset is supported
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Engine: add `sh_grep_span()` to src/sh_grep.c (leftmost-longest match span from a start offset, reusing the existing NFA walk in anchored mode; literal and empty-pattern fast paths). Host-test it in scripts/test_sh_grep.c first.
2. Pure sed core, no I/O: include/sh_sed.h + src/sh_sed.c. Parses -e/-f script text (s with any delimiter, flag g only; d, p, q; `;` and newline separators) into a bounded command list, refusing addresses, hold space, labels, y/a/i/c etc., backrefs `\1`-`\9` in the replacement, unknown s flags, with a readable reason. `sh_sed_apply()` runs the program over one line (s, d, p, q, -n). Replacement supports `&`, `\&`, `\\`, `\<delim>`. Empty-match rules follow GNU (`s/x*/-/g` on abc gives -a-b-c-).
3. Host test scripts/test_sh_sed.c/.py (ASan/UBSan, under timeout) written before the code: exact cases per acceptance criterion plus sweeps against a naive model.
4. cmd_sed in src/sshd_builtins.c: options -n, -e, -f (resolve_arg), refuse -i/--in-place/-E/-r; stdin via grep_fill when no files; per-file errors as `sed: FILE: reason` and continue; yields via sh_session_interrupted; alphabetical row. Add sh_sed.c to the server list in CMakeLists.txt and CLAUDE.md lists/test list.
5. README sed subset section, docs/shell-text-tools.md if it lists tools; smoke.sh sed checks; `task build`, then device install via `vpk install -r` and verification over ssh.
Known deviations to document: output always ends in a newline; no backrefs (engine has no captures); s flag p not supported.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Engine: added sh_grep_span() (leftmost-longest span) to the grep matcher, host tested against a brute-force model. Found and fixed two existing grep bugs while doing it: `^$` matched every line (compiled as matches-all) and `^a$` matched an empty line (node_can_be_empty treated BOL as empty regardless of its body). Both have tests in scripts/test_sh_grep.c; device check: `grep -c '^$'` on a file with one blank line now prints 1.

Core in include/sh_sed.h + src/sh_sed.c (pure, no I/O); scripts/test_sh_sed.py pins the worked examples and every {a,b} line up to 4 bytes against gsed (empty-match walk included). cmd_sed in src/sshd_builtins.c.

Device (the Vita, office network, 2026-11-20): installed with vpk install -r over ssh (promote package 0x00000000); scripts/smoke.sh exec: 174 pass, 0 fail, including 18 new sed checks (s, g, delimiter, &, empty matches, -n/p/d/q, -e order, pipe stdin, -i refused with the file untouched, range and hold space refused by name, bad script, missing file continues with status 2). -f script file with a comment line also checked by hand.

Deviations from GNU documented in README: output lines always end in a newline; no back-references (engine has no captures) so \1 is refused; s flags other than g are refused (p, i, N, w); \n and \t in a replacement are refused; a too-long line (4096) or a result over 8192 bytes stops the run instead of skipping a line.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Added a small sed: s///[g] with any delimiter and & in the replacement, d, p, q, -n, repeated -e, -f FILE, stdin or files. Refused by name before any input is read: -i, -E/-r, line addresses and ranges, hold space, labels and branches, blocks, other commands, back-references, other s flags. Implemented as a pure core (src/sh_sed.c) over a new sh_grep_span() in the grep engine, so patterns behave identically to grep. Fixed two grep anchor bugs found on the way (`^$`, `^a$`). Verified: host tests (grep, sed pinned to gsed), task build produces both VPKs, device smoke 174/0. Follow-up not created: s///p flag and backreferences (the latter needs captures in the engine).
<!-- SECTION:FINAL_SUMMARY:END -->
