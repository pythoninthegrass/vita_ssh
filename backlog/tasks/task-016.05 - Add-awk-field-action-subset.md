---
id: TASK-016.05
title: Add awk (field-action subset)
status: Done
assignee:
  - claude
created_date: '2026-11-17 03:29'
updated_date: '2026-11-20T18:48:00Z'
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
  - src/sh_awk.c
  - include/sh_awk.h
  - src/sshd_builtins.c
  - CMakeLists.txt
  - scripts/test_sh_awk.c
  - scripts/test_sh_awk.py
  - scripts/smoke.sh
  - README.md
  - docs/shell-text-tools.md
  - AGENTS.md
parent_task_id: TASK-016
priority: low
ordinal: 39000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The largest lift in the family, scoped down to what a person types at a prompt: split each line into fields, gate on a pattern, print selected fields. awk '{print $1, $3}' after a pipe is the whole point.

In scope: -F to set the field separator (default whitespace), $0 through the last field, NF and NR, print with comma-separated arguments joined by OFS (a single space by default), and a pattern that gates the action -- either /regex/ or a comparison of a field or NR against a literal with == != < <= > >=.

Explicitly out of scope, refused with awk: unsupported: CONSTRUCT and a nonzero exit rather than being parsed and ignored: variables and assignment, arithmetic, if/while/for, functions, BEGIN/END, and multiple files with different separators. Parsing must fail loudly on anything it does not understand -- an awk that silently drops an unsupported statement reports numbers that look right and are wrong.

Same family rules as the parent task: cmd_awk() in src/sshd_builtins.c, alphabetical row, resolve_arg() for file arguments, sh_printf() for output, sh_eprintf() for errors, heap for the field table, bounded line length and field count.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 awk '{print $1, $3}' FILE prints those two fields of every line joined by a single space, and $0 prints the whole line.
- [x] #2 -F SEP splits on SEP instead of whitespace, including empty fields between consecutive separators.
- [x] #3 NF is the field count and NR is the record number, both usable in a pattern and as a print argument.
- [x] #4 A /regex/ pattern gates the action to matching lines, and a comparison such as $3 == "x" or NR > 5 gates it the same way (a bare word like x is a variable in awk and is refused).
- [x] #5 awk reads stdin with no file argument, so ls | awk '{print $1}' works once pipes exist.
- [x] #6 Unsupported syntax -- assignment, if, while, for, functions, BEGIN, END -- reports awk: unsupported: CONSTRUCT and exits nonzero without printing partial output.
- [x] #7 A malformed program reports awk: reason with a nonzero exit rather than treating it as a literal string.
- [x] #8 A line beyond the stated bound for length or field count is reported, not silently truncated.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session
- [x] #3 README states which awk subset is supported
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Approach: same split as sed. A pure module (no I/O, no Vita APIs) does parsing and per-line execution so it can be tested on the host under ASan/UBSan and pinned against real awk; cmd_awk() is only the frontend.

1. New include/sh_awk.h + src/sh_awk.c (added to the server's add_executable list in CMakeLists.txt). API: sh_awk_new/free, sh_awk_compile(prog, program_text, fs, why, whylen) (0, or -1 with the reason and nothing kept), sh_awk_apply(prog, line, emit, ctx) (increments NR, which runs across files like awk; -1 with errno ERANGE when the line has more than SH_AWK_FIELD_MAX fields or the printed line exceeds the output bound, ENOMEM).
2. Grammar, exactly one rule: [PATTERN] [{ print [ARG {, ARG}] [; print ...] }]. A missing action prints $0; a missing pattern matches every line. PATTERN is /RE/ (the grep engine, BRE, so wording matches grep and sed) or OPERAND RELOP OPERAND with == != < <= > >=. OPERAND/ARG is $N, $NF, NF, NR, a number literal or a "string" literal. Comparison is numeric when both sides look numeric (a field that looks like a number against a number literal), else string, as POSIX awk does.
3. -F: default splits on runs of blank/tab with ends trimmed; a single character splits literally and keeps empty fields; \t is a tab. A multi-character -F is a regex (ERE) in real awk and the engine is BRE, so it is refused as unsupported rather than silently differing.
4. Everything else is refused at compile time, before any line is read, so there is never partial output: awk: unsupported: CONSTRUCT for BEGIN, END, assignment, arithmetic (including $(NF-1)), if/while/for/do, functions and calls, getline, ~ and !~, !, &&, ||, a second rule, -v and -f. Anything not understood at all is awk: reason (syntax error), never a literal. Unknown identifiers are variables, hence unsupported: variables.
5. Bounds: line 4096 (SH_GREP_LINE_MAX, via sh_grep_read_line), fields 64, print args 16, output line 8192; each overflow is reported with the line number and stops the run, as sed does.
6. cmd_awk() in src/sshd_builtins.c: resolve_arg() for files, stdin with no file, grep_src_t/grep_fill, to_sink_t for output, alphabetical row in builtins[]. The per-file loop is the same as cmd_sed's; put it in one helper (filter_over_sources) that both awk and sed use; Lance approved converting cmd_sed.
7. TDD: scripts/test_sh_awk.c (checks plus a driver mode) and scripts/test_sh_awk.py (ASan/UBSan build, then worked examples and every {a,b} line up to a few bytes pinned against real awk, under timeout). Write both first and watch them fail before the module exists.
8. Docs and checks: README awk subset (DoD 3), docs/shell-text-tools.md rationale, AGENTS.md host-check list and server source list, scripts/smoke.sh awk section.
9. Verify: host tests, task build (both VPKs), then install with vpk install -r over ssh and run smoke.sh on the Vita (find the network per CLAUDE.local.md first).

Risks: awk number-versus-string comparison corners (pinned against real awk); the file-loop helper must not change sed's behavior (sed's non-regular-file message is the one intended change).

Plan adjustment (routine, within scope): awk regexes are POSIX extended and the grep engine is basic, so /a+/ or /(a|b)/ would silently mean something else. sh_awk.c translates ERE to the engine's BRE spelling outside bracket expressions (unescaped + ? | ( ) { } gain a backslash, escaped ones lose it) and the host test pins the result against real awk (BWK awk 20200816 and gawk 5.4.1 are both installed) rather than refusing. Also: a bare word in a comparison (the task's own `$3 == x` example) is an uninitialized variable in real awk, so it is refused as unsupported: variable; the string form `$3 == "x"` is what works. -Ft (tab in POSIX awks) is refused by name because a literal t split is never what was meant.

Final adjustments: field bound is 128, not 64. A missing field compares as the empty string, not 0 (both real awks, found by the pin run). cmd_awk and cmd_sed share filter_over_sources() (new), and sed_emit was renamed sink_emit_line since awk uses it too.
<!-- SECTION:PLAN:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Added awk as a field-action subset: new pure module include/sh_awk.h + src/sh_awk.c (compile one rule, run it per line), cmd_awk() and filter_over_sources() in src/sshd_builtins.c, CMake entry, builtins row. Grammar is [PATTERN] { print ARG, ... } with $N, $NF, NF, NR, number and string operands; PATTERN is /RE/ or a comparison; -F takes one character (or \t). Everything else (BEGIN/END, variables, assignment, arithmetic, control flow, functions, printf, ~, !, &&, ||, ranges, a second rule, redirection, concatenation, -v, -f, -Ft, multi-character -F) is refused by name at compile time, so no partial output. awk regexes are ERE and the grep engine is BRE, so the module translates the spelling and refuses the BRE-only rules that would differ. Number-versus-string comparison follows awk. Bounds: 4096-byte line, 128 fields, 8192-byte print; each stops the run with the line number.

Verification: scripts/test_sh_awk.py (ASan/UBSan, plus about 130 worked examples and every {a,b} line to four bytes pinned against BWK awk and gawk; the pin run caught a wrong assumption about missing fields). task build produces both VPKs, no warnings. Installed with vpk install -r on the Vita (office network); scripts/smoke.sh exec: 190 pass, 0 fail, including 16 new awk checks. Device check of bounds: a 200-field line and a 5000-byte line each stopped with the named error and status 2; NR-only programs are not stopped by a wide line. README, docs/shell-text-tools.md, AGENTS.md updated.

Decisions: AC4's `$3 == x` is written `$3 == "x"` because a bare word is a variable in awk (AC reworded); cmd_sed was converted to filter_over_sources() with Lance's approval.

Follow-up: cmd_sed now runs through filter_over_sources() too (sed_run adapter); sed's non-regular-file message is now 'sed: FILE: not a regular file'. Re-verified on the Vita: smoke.sh exec 190 pass, 0 fail.
<!-- SECTION:FINAL_SUMMARY:END -->
