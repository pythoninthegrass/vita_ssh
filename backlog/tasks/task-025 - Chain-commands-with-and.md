---
id: TASK-025
title: 'Chain commands with ;, && and ||'
status: To Do
assignee: []
created_date: '2026-11-17 05:45'
labels:
  - shell
dependencies: []
references:
  - src/sshd_session.c
  - src/sshd_builtins.c
documentation:
  - AGENTS.md
  - README.md
modified_files:
  - src/sshd_session.c
  - src/sshd_builtins.c
  - README.md
priority: medium
ordinal: 56000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The shell runs one command per line. README.md says so: there are no pipes and no ;, && or || chaining, and those characters are ordinary text to the tokenizer. The cost is real. A one-shot ssh host "uname -a; pwd" prints the first command's output and silently ignores the rest, which already misled a device session, and ssh-copy-id cannot work because it sends a compound sh -c line (README.md says so under the key-install notes). Scripts and one-liners reach for these three operators first.

Wanted: a line may join commands with ; (run both), && (run the second only if the first exited 0) and || (run the second only if the first exited nonzero), evaluated left to right with equal precedence as a minimal sh subset.

Design points to settle in the open rather than paper over:
- Exit status: the line's status is that of the last command run, because exec mode reports it to the client with exit-status. A skipped command does not change it.
- Splitting must respect quoting the same way the tokenizer already does: an operator inside quotes stays literal, so echo "a; b" prints a; b. Today split_line() tracks one quoted bit meaning do not glob; reuse that rule rather than adding a second quoting model.
- Operators are recognised only as separate unquoted words or unambiguously between words; decide whether echo hi;pwd (no spaces) splits, and document the choice. README states that echo hi>x is just text for redirection, so the existing convention leans toward requiring spaces.
- Redirection is per command: a > file after one command does not apply to the next. Each command's redirect is opened before it runs and closed on every exit path, exactly as extract_redirect() and sh_session_redirect_start()/_end() do today for a single command.
- Nothing may block: the commands run one after another inside sh_session_pump(), each non-blocking, so a long chain cannot park the poll loop that every session shares. A bound on commands per line, with a readable error past it, keeps one line from monopolising the loop.
- exit and a client hangup mid-chain stop the chain: nothing after exit runs.
- history recalls the line as typed, not split.

Collision warning: TASK-015.02 (pipes), TASK-018 (aliases) and TASK-020 (variables) all edit the word handling in sh_execute_line(). Sequence this with them and settle the order explicitly: split on chain operators first, then pipe, then per command alias expansion, variable expansion, redirect extraction and globbing. A chain operator produced by expanding an alias or a variable must not be re-split, the same one-level rule those tasks already use.

This also unblocks ssh-copy-id only if the shell grows sh -c; chaining alone does not, so do not claim it does.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 echo a ; echo b prints a then b, and the line's exit status is that of echo b.
- [ ] #2 A command after && runs only when the previous command exited 0; a failing previous command skips it and leaves the failing status as the line's status.
- [ ] #3 A command after || runs only when the previous command exited nonzero; a succeeding previous command skips it and leaves status 0.
- [ ] #4 ls /nope && echo yes || echo no prints no, and ls /ux0 && echo yes || echo no prints yes without running the last command.
- [ ] #5 One-shot exec works: ssh host 'uname -a ; pwd' prints both outputs and exits with pwd's status.
- [ ] #6 An operator inside quotes stays literal: echo "a; b" prints a; b and echo 'x && y' prints x && y.
- [ ] #7 Redirection applies to its own command only: echo a > /ux0/data/x ; echo b leaves a in the file and prints b to the terminal.
- [ ] #8 exit in a chain stops it: exit ; echo never prints nothing after the session closes.
- [ ] #9 A chain longer than the stated per-line bound reports a readable error naming the limit and runs none of it, and the session keeps working.
- [ ] #10 A trailing or doubled operator (echo a ; ; echo b, echo a &&) reports a readable syntax error naming the operator and runs nothing.
- [ ] #11 history recalls the line exactly as typed, not split into its commands.
- [ ] #12 Whether operators need surrounding spaces is stated in help and README, and README no longer says there is no ; && or || chaining.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session, including a one-shot exec with ;, && and ||
<!-- DOD:END -->
