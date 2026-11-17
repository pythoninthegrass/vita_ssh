---
id: TASK-016.01
title: Add grep
status: Done
assignee: []
created_date: '2026-11-17 03:29'
updated_date: '2026-11-17 03:36'
labels:
  - shell
  - coreutils
dependencies:
  - TASK-015.01
documentation:
  - AGENTS.md
modified_files:
  - src/sshd_builtins.c
parent_task_id: TASK-016
priority: medium
ordinal: 35000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
grep is the command that makes pipes worth having, and the first text tool in the coreutils family. Follow the family's rules in the parent task: cmd_grep() in src/sshd_builtins.c, an alphabetical row in builtins[], resolve_arg() for every path, sh_printf() for matches, sh_eprintf() for errors, no printf, no chdir, no invented device paths.

Pattern engine: prefer POSIX basic regular expressions through newlib's regcomp/regexec if the Vita toolchain provides them. If it does not, ship literal substring matching and say so plainly in the usage string and README -- a silent downgrade to literal matching that the user cannot see is exactly the dishonesty the family rule forbids. Record which one shipped.

Exit status follows the conventional contract, because scripts and the eventual && rely on it: 0 matched, 1 no match, 2 error.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 grep PATTERN FILE prints the matching lines; with several files each line is prefixed file:.
- [x] #2 grep with no file argument reads the session's stdin, so ls | grep ux0 works once pipes exist.
- [x] #3 -i matches case-insensitively, -v inverts, -n prefixes line numbers, -c prints only a count, -l prints only file names, -q prints nothing and sets only the exit status.
- [x] #4 Exit status is 0 when a line matched, 1 when none did, 2 on error such as an unreadable file.
- [x] #5 An unreadable file is reported as grep: FILE: reason on the terminal and the other files still searched.
- [x] #6 Regex behaviour is stated: either the POSIX basic subset works (and a test like grep 'a.c' matches abc) or the tool matches literally and both help and README say so.
- [x] #7 A line longer than the tool's bound is either matched correctly or reported as over the limit, never silently truncated into a wrong answer.
- [x] #8 A nonexistent pattern argument (no PATTERN given) prints usage and exits nonzero rather than matching everything.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session
- [x] #3 README states which pattern syntax grep supports
<!-- DOD:END -->

## Notes

The pattern engine is src/sh_grep.c (include/sh_grep.h): a bounded POSIX basic
engine, no Vita APIs, so scripts/test_sh_grep.py runs it on the host under ASan/
UBSan. It ships real BRE -- not the literal-matching fallback -- so AC #6 and the
"which one shipped" instruction resolve to "POSIX basic, stated in help and README."
The line bound and the work budget are in that header; a line over SH_GREP_LINE_MAX
is reported rather than truncated (AC #7), and a pattern too expensive to match is
refused with errno EAGAIN rather than parking the poll loop.

Why an engine at all: the SDK ships <regex.h> (Henry Spencer's BSD header, declaring
regcomp/regexec/regerror/regfree over a re_guts-opaque regex_t) but no library in its
sysroot defines them -- a scan of 23789 objects across every archive finds no
regcomp/regexec definition, only unresolvable references from rpmatch(). So the header
certifies that BRE is the right shape here, not that one is available. The entry points
are named sh_grep_* rather than regcomp/regexec so a libc that does provide them is
never shadowed and no call here looks like one that will not link. The engine is real
rather than substring matching in regex clothing because a caller cannot tell a literal
from a metacharacter by feel, and a silent downgrade is the dishonesty the family rule
forbids -- help, README and the usage string all say BRE, and it is BRE.

Shape: compile to a small tree, then match by walking it with an explicit continuation
rather than recursion, so depth is bounded by the live states; cost is O(bytes x live
states) and the scaled budget in SH_GREP_STEP_LIMIT bounds it. Every step charges that
budget, which is what turns a pathological pattern into a reported failure instead of a
frozen device -- this runs in the server's one poll loop, where a match that does not
return is the same outage as a stalled socket write. A budget exhausted answers EAGAIN;
it never answers "no match", which would be a guess dressed as a result.

Syntax is BRE, not ERE: + ? ( ) { } | are literal bytes unless backslash-escaped, and
\( \) \{m,n\} \| \+ \? are the operators. Matching is unanchored unless the pattern
begins with ^. A dot matches any byte including NUL, because a line is a length-
delimited buffer and stopping at NUL would say "no match" about text that visibly
contains the pattern. Case folding is ASCII only: there is no locale on this platform.

src/sshd_builtins.c adds cmd_grep and the builtins[] row, and wires src/sh_grep.c
into the server target in CMakeLists.txt. The line reader is sh_grep_read_line so wc
and sed (TASK-016.03/.04) reuse it. Exit status is 0 matched, 1 none, 2 error
(bad pattern, unreadable file, overlong line), with an error outranking a match.

Verified: host tests for the engine and line reader pass; task build produces both
VPKs clean, no warnings. AC #6 (BRE works, stated in help/README) and #7 (overlong line
reported, not truncated) are covered by scripts/test_sh_grep.py.

Device run on the home Vita, server built 2026-11-17 03:34, installed with `vpk install` and exercised over real ssh exec sessions. Passing: AC1 one file and two files (`file:` prefix); AC2 `grep abc < file` reads the redirected stdin, and bare `grep abc` returns in about 1 s (the pipe form waits on TASK-015.02); AC3 `-i`, `-v`, `-n`, `-c`, `-l`, `-q`; AC4 exit 0 on a match, 1 on none, 2 on an unreadable file, a bad pattern (`[`) and a directory; AC5 `grep: FILE: No such file or directory` with the remaining file still searched (exit 2); AC6 `a.c`, `^al` and `a\(b\)c` match; AC7 a 5000-byte line reports `line 1: too long (limit 4096 bytes)`, still searches the later lines and exits 2; AC8 no PATTERN prints the usage line and `missing pattern operand`, exit 2; `help grep` and `which grep` answer.

The device run found one defect, fixed in this task: `grep -c PATTERN FILE` printed `FILE:N` for a single file. The count line now carries the name only when more than one file is given, matching the match-line rule. Re-checked on the device after the fix.

Known gaps, not fixed here: `--` and `-e PATTERN` are rejected (`grep: illegal option -- -`), so a pattern that begins with `-` cannot be given. An unknown flag prints `grep: illegal option -- z`, the same wording `ls` uses, whereas the TASK-016 parent AC #4 asks for `name: unsupported option: FLAG`; no builtin meets that wording yet, so the parent AC needs a decision on whether to change the shared option error or the AC.

## Later finding

2026-11-17, found during the TASK-016.03 device run: grep was unusable on large inputs even though every criterion above had passed on small fixtures. sh_grep_read_line pulls its input one byte per call and grep_fill turned each into its own sceIoRead, so `grep -c line` over a 20000-line, 175 KB file took 45 s on the home Vita; matched lines were also written to the channel in two or three pieces each. Commit 55e60db on branch TASK-016.03 reads a file in 4096-byte blocks and batches the output into one buffer, flushed before any error message and at the end. `grep -c line` on the same file now takes 1 s, `-v 1` 1 s and `-i LINE` 2 s, and every checked case is byte-identical to the host grep; printing all 20000 matches took 8 to 12 s, which is the device's large-output transport stalls and not the command. The lesson for the rest of the family: a host test cannot see device performance, so each builtin needs a device run on a 20000-line file.

