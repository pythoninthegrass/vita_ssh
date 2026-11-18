---
id: TASK-025
title: 'Chain commands with ;, && and ||'
status: Done
assignee:
  - Claude
created_date: '2026-11-17 05:45'
updated_date: '2026-11-18T06:49:00Z'
labels:
  - shell
dependencies:
  - TASK-030
references:
  - src/sshd_session.c
  - src/sshd_builtins.c
documentation:
  - AGENTS.md
  - README.md
modified_files:
  - src/sh_chain.c
  - include/sh_chain.h
  - scripts/test_sh_chain.c
  - scripts/test_sh_chain.py
  - src/sshd_builtins.c
  - CMakeLists.txt
  - README.md
  - AGENTS.md
  - scripts/smoke.sh
priority: medium
ordinal: 56000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The shell runs one command, or one pipeline, per line. README.md says so: there is no ;, && or || chaining, and those characters are ordinary text to the tokenizer. The cost is real. A one-shot ssh host "uname -a; pwd" prints the first command's output and silently ignores the rest, which already misled a device session, and ssh-copy-id cannot work because it sends a compound sh -c line (README.md says so under the key-install notes). Scripts and one-liners reach for these three operators first.

Wanted: a line may join commands with ; (run both), && (run the second only if the first exited 0) and || (run the second only if the first exited nonzero), evaluated left to right with equal precedence as a minimal sh subset.

Design points to settle in the open rather than paper over:
- Exit status: the line's status is that of the last command run, because exec mode reports it to the client with exit-status. A skipped command does not change it.
- Splitting must respect quoting the same way the tokenizer already does: an operator inside quotes stays literal, so echo "a; b" prints a; b. split_line() tracks one quoted bit per word meaning do not glob; reuse that rule rather than adding a second quoting model. TASK-015.02 did exactly this for `|` (is_pipe_word() in src/sshd_builtins.c: an unquoted word that is exactly the operator).
- Operators are recognized only as separate unquoted words or unambiguously between words; decide whether echo hi;pwd (no spaces) splits, and document the choice. The existing convention, kept by `>`, `<` and `|`, requires separate words: a glued echo hi>x or echo a|b is just text. Following it is the cheap choice and keeps one rule for every operator.
- Redirection is per command: a > file after one command does not apply to the next. Each command's redirect is opened before it runs and closed on every exit path, exactly as extract_redirect() and sh_session_redirect_start()/_end() do today for a single command.
- Nothing may block: the commands run one after another inside sh_session_pump(), each non-blocking, so a long chain cannot park the poll loop that every session shares. A bound on commands per line, with a readable error past it, keeps one line from monopolising the loop. Note the limit of that argument: a builtin that is CPU or I/O bound and never yields (du /ux0 takes about 170 s on a device) starves every other session however it is chained, so the bound limits how many commands run, not how long one takes.
- exit and a client hangup mid-chain stop the chain: nothing after exit runs.
- history recalls the line as typed, not split.

Where the code is now (TASK-015.02 has landed): sh_execute_line() splits the whole line into words once, then calls run_pipeline() when any unquoted `|` word is present and run_command() otherwise. run_command() takes ownership of one command's words (arrays of MAX_WORDS entries, because globbing grows the list in place), extracts redirects, expands globs and dispatches. run_pipeline() copies each segment into its own MAX_WORDS arrays, captures every segment's stdout but the last with sh_session_capture_start()/_end() and feeds it to the next as stdin. Reuse that shape.

Sequencing: chain operators bind looser than `|` (a | b && c is (a | b) && c), so split on ; && || first, then hand each item's words to the same pipeline/command path, then per command alias expansion (TASK-018), variable expansion (TASK-020), redirect extraction and globbing. TASK-018 and TASK-020 still edit the same word handling. A chain operator produced by expanding an alias or a variable must not be re-split, the same one-level rule those tasks already use.

Things the pipeline code already decided, so chaining stays consistent with it: a syntax error is reported as `sh: syntax error near unexpected token `X'` with status 2 and runs nothing (an empty segment today; a leading, trailing or doubled chain operator belongs here too, AC10). sh_session_interrupted() is checked before each segment and stops the rest. `exit` as a non-last pipeline segment is skipped, as in a subshell; as a chain item it must end the chain and the session (AC8). A capture or stdin left over from one item must never reach the next: run_pipeline() clears stdin after every segment, and a chain loop around it inherits that.

This also unblocks ssh-copy-id only if the shell grows sh -c; chaining alone does not, so do not claim it does.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 echo a ; echo b prints a then b, and the line's exit status is that of echo b.
- [x] #2 A command after && runs only when the previous command exited 0; a failing previous command skips it and leaves the failing status as the line's status.
- [x] #3 A command after || runs only when the previous command exited nonzero; a succeeding previous command skips it and leaves status 0.
- [x] #4 ls /nope && echo yes || echo no prints no, and ls /ux0 && echo yes || echo no prints yes without running the last command.
- [x] #5 One-shot exec works: ssh host 'uname -a ; pwd' prints both outputs and exits with pwd's status.
- [x] #6 An operator inside quotes stays literal: echo "a; b" prints a; b and echo 'x && y' prints x && y.
- [x] #7 Redirection applies to its own command only: echo a > /ux0/data/x ; echo b leaves a in the file and prints b to the terminal.
- [x] #8 exit in a chain stops it: exit ; echo never prints nothing after the session closes.
- [x] #9 A chain longer than the stated per-line bound reports a readable error naming the limit and runs none of it, and the session keeps working.
- [x] #10 A leading or doubled operator (echo a ; ; echo b) or a trailing && or || (echo a &&) reports a readable syntax error naming the operator and runs nothing; as in sh, one trailing ; is accepted and ignored.
- [x] #11 history recalls the line exactly as typed, not split into its commands.
- [x] #12 Whether operators need surrounding spaces is stated in help and README, and README no longer says there is no ; && or || chaining.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session, including a one-shot exec with ;, && and ||
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Refreshed 2026-11-17 after TASK-030 shipped; line numbers in the older plan had moved. Test approach chosen by the user: a pure module with a host test, written first (TDD), execution glue checked on the device.

Current code (src/sshd_builtins.c): sh_execute_line() splits once with split_line() (one quoted bit per word), counts unquoted `|` with is_pipe_word(), runs run_command() or run_pipeline(); both take MAX_WORDS (MAX_ARGS*2 = 128) arrays and own them. has_empty_segment() reports a bad `|`. sh_session_interrupted() is true when exit_requested or finished, so an `exit` item stops the chain with no extra flag.

1. Pure module: include/sh_chain.h and src/sh_chain.c, no Vita APIs. sh_chain_op(word, quoted) recognizes an unquoted word that is exactly `;`, `&&` or `||`; sh_chain_check(argv, quoted, argc, &bad) returns the item count, or SH_CHAIN_SYNTAX with the offending operator (leading, doubled, trailing) or SH_CHAIN_TOO_LONG past SH_CHAIN_MAX (16); sh_chain_should_run(op, status) is the run-or-skip rule. A trailing `;` is an error too, following AC10 literally (real sh accepts it); flag this to the user.
2. Host test first: scripts/test_sh_chain.c and scripts/test_sh_chain.py under ASan/UBSan, run under timeout, like test_sh_grep. Fixed cases from the ACs plus an exhaustive comparison against a naive reference over every word sequence up to length 6 from {word, quoted operator, ;, &&, ||}. Confirm it fails before the module exists.
3. Glue in sh_execute_line(): when sh_chain_check reports more than one item, copy each item's words into its own MAX_WORDS arrays (as run_pipeline copies segments), validate every item's `|` with has_empty_segment before running anything, then walk the items: sh_chain_should_run on the preceding operator, run via the existing pipeline-or-command path, keep status, skip leaves status unchanged, stop when sh_session_interrupted(). One-operator-free lines keep the current path untouched.
4. Register src/sh_chain.c in the server's add_executable list in CMakeLists.txt; document the host test in AGENTS.md Build.
5. help text and README: spaces required around operators, chaining now exists, the 16 item bound, and the trailing `;` rule; fix the ssh-copy-id sentence so it still says sh -c is missing.
6. scripts/smoke.sh exec: add chain checks including `uname -a ; pwd`.
7. Record arm-vita-eabi-size before (2297512 at b8b3d14, server text) and after.
8. Verify on the device over ssh (install through VitaShell over USB, not vpk install over ssh).

Risks: stack. sh_execute_line already holds two MAX_WORDS arrays and run_pipeline two more; a chain item adds a pair. The server thread has 512 KB, so this fits, but keep the chain arrays out of any deeper frame. TASK-018 and TASK-020 depend on this task so aliases and variables expand per chain item, and an operator produced by expansion is never re-split.

Final solution differs from step 1 in one place: a single trailing `;` is accepted and ignored, as in sh, on the user's decision. sh_chain_words() returns the command words without it. A leading or doubled operator and a trailing && or || remain errors.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-17: updated after TASK-015.02 was built and verified on a device (pipes work; see its notes). Description now reflects the as-built word-handling structure and the decisions the pipeline already made. README.md lines on chaining were reworded by 015.02, which still says `;`, `&&` and `||` are absent; AC12 here owns that last sentence.

2026-11-18: implemented per the plan. include/sh_chain.h and src/sh_chain.c are the pure module (operator words, syntax and length check, run-or-skip rule); scripts/test_sh_chain.c and test_sh_chain.py were written first and failed to build without the module, then passed; four deliberately broken versions of the module (inverted && rule, trailing check removed, leading check removed, off-by-one limit) each fail the test. sh_execute_line() now checks the chain, then runs one item through run_item() (the previous pipeline-or-command path, unchanged) or the items through run_chain(). run_chain() validates every item's `|` before running anything, skips an item by freeing its words and leaving the status alone, and stops when sh_session_interrupted() is true so `exit ; echo never` runs nothing. Decision flagged for the user: a trailing `;` is a syntax error, following AC10 literally; real sh accepts it. help footer, README (chaining paragraph, ssh-copy-id sentence), AGENTS.md (host test, server list) and scripts/smoke.sh exec checks updated. task build produces all VPKs; server text 2298408 bytes against 2297512 before (+896). Host tests sh_chain, sh_grep, sh_headtail and sh_textops pass. Not yet run on a device: needs the build installed through VitaShell over USB, then scripts/smoke.sh exec.

2026-11-18: trailing ; changed to match sh on the user's decision (it was an error, following AC10 literally). sh_chain_check accepts one trailing ; and counts no command for it; new sh_chain_words() returns the command words without it, and sh_execute_line() frees that word before running. A leading or doubled operator and a trailing && or || stay errors. AC10 reworded to say so. Host test updated first (failed to build without sh_chain_words), then passed; three broken versions of the new rule each fail it. README and smoke.sh updated (trailing ; accepted, trailing || rejected). Server text now 2298476 (+964 over 2297512), VPK sha256 cac7cc7e646a4ad1e702939507945c5acbb50fce85792dd8a6de7629c14946f8. Still not run on a device.

2026-11-18 device run (the one Vita, home network, installed through VitaShell over USB; the chaining build was confirmed by the new help footer and by the trailing-; check passing): scripts/smoke.sh exec, 75 pass 0 fail on three consecutive runs, covering AC1 to AC10 (; order and status, && and || run and skip with the failing or zero status kept, ls /nope && echo yes || echo no and the passing form, uname -a ; pwd one-shot with both outputs and exit 0, quoted and glued operators as text, redirect on its own command only, exit ; echo never prints nothing, doubled/leading/trailing operators exit 2 and run nothing with the error naming the operator, 17 commands exit 2 with 'limit 16' and none run, session still works after the errors, a bad | in a later item runs nothing). AC11 checked in an interactive pty session: history lists 'echo a ; echo b' and 'true && echo yes || echo no' exactly as typed, and the doubled-; line printed the syntax error. AC12: help footer and README. Two things learned: in exec mode the server sends sh_eprintf text on the same channel as stdout, so 2>/dev/null does not hide it and the smoke checks have to look for absence of command output instead of an empty stdout; and one early smoke run showed an empty result for echo "a; b" that did not reproduce in 8 repeats or three more full runs, cause not found (suspected connection churn from the many sequential ssh calls).
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
A line may now join commands with ;, && and ||, evaluated left to right with equal precedence and binding looser than |. The pure logic is in include/sh_chain.h and src/sh_chain.c (operator words, syntax and length check, run-or-skip rule), written test-first with scripts/test_sh_chain.py under ASan/UBSan: fixed cases from the ACs plus every word sequence up to seven words checked against a separately written model, and broken versions of the module each fail it. sh_execute_line() checks the chain, then runs one item through run_item() (the previous pipeline-or-command path) or the items through run_chain(), which validates every item's | before running anything, leaves the status alone for a skipped item, and stops on sh_session_interrupted() so exit ; echo never prints nothing. Operators are separate unquoted words, like | and >. A line takes at most 16 commands. A leading or doubled operator or a trailing && or || exits 2 with the operator named and runs nothing; one trailing ; is accepted, as in sh. help, README, AGENTS.md and scripts/smoke.sh exec are updated. Verified on the Vita over ssh: smoke.sh exec 75 pass 0 fail three runs in a row, and history lists chained lines as typed in a pty session. Server text grew 964 bytes. Caveats: chaining does not unblock ssh-copy-id (no sh -c); one smoke check returned empty once and did not reproduce, cause not found; AC wording for a trailing ; was changed from error to accepted.
<!-- SECTION:FINAL_SUMMARY:END -->
